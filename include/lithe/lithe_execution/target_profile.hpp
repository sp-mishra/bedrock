#pragma once

// =============================================================================
// lithe_execution/target_profile.hpp — host capability discovery + fingerprint
//
// Arch §6 / §11.M4.2: backend selection requires a description of the current
// execution target; the fingerprint of that description feeds the impl-3
// executable_key so a cached artifact is only reused on a matching target.
//
// Provides:
//   cpu_features              — SIMD/ISA extension bits
//   simd_width                — widest available SIMD register
//   gpu_descriptor            — GPU identity + compute version
//   endianness                — byte order
//   target_profile            — aggregate of host discovery fields
//   discover_target_profile() — query host once; result cached via function-local static
//   fingerprint()             — stable capability_fingerprint from target_profile
//                               uses canonical_codec + meta::schema_hash
//
// Fingerprint = content_digest(canonical_encode(profile)) XOR schema_hash<target_profile>()
//   • One digest space with portable module identity (impl-1 uses same codec).
//   • ABI/layout change of target_profile auto-invalidates all stale executable keys.
//
// No virtual, no macros.  Header-only C++23.
// =============================================================================

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "foundation.hpp"                             // backend_capability_set, backend_feature
#include "store/artifact_record.hpp"                  // capability_fingerprint
#include "containers/canonical_codec.hpp"       // canonical_writer, content_digest, sha256_digest_policy
#include "meta/meta.hpp"

// Platform detection for hardware discovery
#if defined(__APPLE__)
#  include <sys/types.h>
#  include <sys/sysctl.h>
#elif defined(__linux__)
#  include <sys/auxv.h>
#endif

namespace lithe::execution {
    // =============================================================================
    // cpu_features — ISA extension capability bits
    // =============================================================================

    enum class cpu_feature_bit : std::uint64_t {
        sse2 = 1ull << 0,
        sse4_1 = 1ull << 1,
        avx = 1ull << 2,
        avx2 = 1ull << 3,
        avx512f = 1ull << 4,
        neon = 1ull << 5, // ARM NEON / AdvSIMD
        sve = 1ull << 6, // ARM SVE
        amx = 1ull << 7, // Apple AMX / x86 AMX
        fp16 = 1ull << 8, // native FP16 compute
        dot_prod = 1ull << 9, // int8 dot product units
        bf16 = 1ull << 10, // bfloat16
    };

    struct cpu_features {
        std::uint64_t bits = 0;

        [[nodiscard]] constexpr bool has(cpu_feature_bit f) const noexcept {
            return (bits & static_cast<std::uint64_t>(f)) != 0;
        }

        constexpr void set(cpu_feature_bit f) noexcept {
            bits |= static_cast<std::uint64_t>(f);
        }

        [[nodiscard]] friend constexpr bool operator==(cpu_features a, cpu_features b) noexcept {
            return a.bits == b.bits;
        }
    };

    // =============================================================================
    // simd_width — widest available SIMD register in bits
    // =============================================================================

    enum class simd_width : std::uint16_t {
        scalar = 0,
        w64 = 64,
        w128 = 128,
        w256 = 256,
        w512 = 512,
        scalable = 0xFFFF, // ARM SVE scalable vectors
    };

    // =============================================================================
    // gpu_descriptor — GPU identity and compute tier
    // =============================================================================

    struct gpu_descriptor {
        std::uint32_t vendor_id = 0; // PCI vendor id (0 = unknown/Metal)
        std::uint32_t device_id = 0;
        std::uint16_t compute_major = 0;
        std::uint16_t compute_minor = 0;
        bool metal_supported = false;
        bool vulkan_supported = false;

        [[nodiscard]] bool operator==(const gpu_descriptor&) const noexcept = default;
    };

    // =============================================================================
    // endianness
    // =============================================================================

    enum class endianness : std::uint8_t {
        little = 0,
        big = 1,
    };

    // =============================================================================
    // target_profile — aggregate host capability description
    //
    // All fields are fixed-width scalars or small aggregates: canonically encodable
    // without pointer chasing.
    // =============================================================================

    struct target_profile {
        cpu_features cpu;
        simd_width max_simd = simd_width::scalar;
        std::optional<gpu_descriptor> gpu;
        std::uint32_t page_size = 4096;
        std::size_t cache_line = 64;
        endianness byte_order = endianness::little;
        std::uint16_t pointer_width = 64; // bits

        [[nodiscard]] bool operator==(const target_profile&) const noexcept = default;
    };

    // =============================================================================
    // canonical_encode_profile — deterministic byte preimage for fingerprinting
    //
    // Field order is fixed; adding a field here WILL change the fingerprint for
    // existing profiles (intentional: the layout hash guards against silent staleness).
    // =============================================================================

    namespace detail {
        [[nodiscard]] inline std::vector<std::uint8_t>
        canonical_encode_profile(const target_profile& p) {
            containers::canonical_writer w;
            w.write_u64(p.cpu.bits);
            w.write_u16(static_cast<std::uint16_t>(p.max_simd));
            w.write_u8(p.gpu.has_value() ? 1 : 0);
            if (p.gpu) {
                w.write_u32(p.gpu->vendor_id);
                w.write_u32(p.gpu->device_id);
                w.write_u16(p.gpu->compute_major);
                w.write_u16(p.gpu->compute_minor);
                w.write_u8(p.gpu->metal_supported ? 1 : 0);
                w.write_u8(p.gpu->vulkan_supported ? 1 : 0);
            }
            w.write_u32(p.page_size);
            w.write_u64(static_cast<std::uint64_t>(p.cache_line));
            w.write_u8(static_cast<std::uint8_t>(p.byte_order));
            w.write_u16(p.pointer_width);
            return w.emit();
        }
    } // namespace detail

    // =============================================================================
    // fingerprint() — stable 32-byte hash of a target_profile
    //
    // Digest = SHA-256(canonical_encode_profile(p)) XOR schema_hash<target_profile>()
    //
    // The schema_hash mixes in a compile-time hash of target_profile's layout so
    // that any ABI change (reorder, add, remove field) changes the fingerprint and
    // auto-invalidates stale impl-3 executable_key entries.
    // =============================================================================

    [[nodiscard]] inline store::capability_fingerprint
    fingerprint(const target_profile& p) {
        const auto bytes = detail::canonical_encode_profile(p);
        const std::span<const std::uint8_t> sp{bytes.data(), bytes.size()};
        const auto raw = containers::content_digest<containers::sha256_digest_policy>(sp);

        // Mix in the compile-time layout hash of target_profile using meta::schema_hash.
        // schema_hash returns a std::uint64_t compile-time constant.
        constexpr std::uint64_t layout_hash = ::meta::schema_hash<target_profile>();

        // XOR the layout hash into the first 8 bytes of the digest.
        store::capability_fingerprint fp;
        fp.digest = raw;
        for (std::size_t i = 0; i < 8; ++i) {
            fp.digest[i] ^= static_cast<std::uint8_t>((layout_hash >> (i * 8)) & 0xFF);
        }
        return fp;
    }

    // =============================================================================
    // discover_target_profile() — query the host once, cache permanently
    //
    // Thread-safe via function-local static. The discovery is idempotent and
    // cheap after the first call.
    // =============================================================================

    [[nodiscard]] inline const target_profile& discover_target_profile() {
        static const target_profile profile = []() -> target_profile {
            target_profile p;

            // ---- endianness ----
            if constexpr (std::endian::native == std::endian::little)
                p.byte_order = endianness::little;
            else
                p.byte_order = endianness::big;

            // ---- pointer width ----
            p.pointer_width = static_cast<std::uint16_t>(sizeof(void*) * 8);

            // ---- cache line ----
#if defined(__APPLE__)
            {
                std::size_t val = 64;
                std::size_t len = sizeof(val);
                ::sysctlbyname("hw.cachelinesize", &val, &len, nullptr, 0);
                p.cache_line = val;
            }
#else
            p.cache_line = 64;
#endif

            // ---- page size ----
#if defined(_SC_PAGESIZE)
            {
                const long ps = ::sysconf(_SC_PAGESIZE);
                if (ps > 0) p.page_size = static_cast<std::uint32_t>(ps);
            }
#endif

            // ---- CPU features (compile-time NTTP probe where possible) ----
#if defined(__AVX512F__)
            p.cpu.set(cpu_feature_bit::avx512f);
            p.cpu.set(cpu_feature_bit::avx2);
            p.cpu.set(cpu_feature_bit::avx);
            p.cpu.set(cpu_feature_bit::sse4_1);
            p.cpu.set(cpu_feature_bit::sse2);
            p.max_simd = simd_width::w512;
#elif defined(__AVX2__)
            p.cpu.set(cpu_feature_bit::avx2);
            p.cpu.set(cpu_feature_bit::avx);
            p.cpu.set(cpu_feature_bit::sse4_1);
            p.cpu.set(cpu_feature_bit::sse2);
            p.max_simd = simd_width::w256;
#elif defined(__AVX__)
            p.cpu.set(cpu_feature_bit::avx);
            p.cpu.set(cpu_feature_bit::sse4_1);
            p.cpu.set(cpu_feature_bit::sse2);
            p.max_simd = simd_width::w256;
#elif defined(__SSE4_1__)
            p.cpu.set(cpu_feature_bit::sse4_1);
            p.cpu.set(cpu_feature_bit::sse2);
            p.max_simd = simd_width::w128;
#elif defined(__SSE2__)
            p.cpu.set(cpu_feature_bit::sse2);
            p.max_simd = simd_width::w128;
#elif defined(__ARM_NEON) || defined(__aarch64__)
            p.cpu.set(cpu_feature_bit::neon);
            p.max_simd = simd_width::w128;
#  if defined(__ARM_FEATURE_SVE)
            p.cpu.set(cpu_feature_bit::sve);
            p.max_simd = simd_width::scalable;
#  endif
#  if defined(__ARM_FEATURE_FP16_VECTOR_ARITHMETIC)
            p.cpu.set(cpu_feature_bit::fp16);
#  endif
#  if defined(__ARM_FEATURE_DOTPROD)
            p.cpu.set(cpu_feature_bit::dot_prod);
#  endif
#  if defined(__ARM_FEATURE_BF16)
            p.cpu.set(cpu_feature_bit::bf16);
#  endif
#endif

            // ---- GPU (Metal on Apple) ----
#if defined(__APPLE__)
            {
                gpu_descriptor g;
                g.metal_supported = true;
                // Vulkan is available via MoltenVK; detect conservatively as false.
                // Full GPU enumeration requires ObjC/Metal headers — not pulled here.
                p.gpu = g;
            }
#endif

            return p;
        }();
        return profile;
    }

    // =============================================================================
    // to_backend_capability_set() — convert cpu_features to execution capability bits
    //
    // Used by the planner to express what the host target provides in the unified
    // backend_capability_set vocabulary.
    // =============================================================================

    [[nodiscard]] inline backend_capability_set
    to_backend_capability_set(const target_profile& p) noexcept {
        backend_capability_set caps;
        caps.add(backend_feature::integer_arithmetic);
        caps.add(backend_feature::floating_arithmetic);
        caps.add(backend_feature::branches);
        caps.add(backend_feature::calls);
        caps.add(backend_feature::memory_operands);
        caps.add(backend_feature::stack_frame);
        caps.add(backend_feature::spill_load_store);
        caps.add(backend_feature::interpreter_execution);
        if (p.max_simd >= simd_width::w128)
            caps.add(backend_feature::tensor_arithmetic);
        return caps;
    }
} // namespace lithe::execution
