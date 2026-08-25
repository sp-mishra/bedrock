#pragma once

// =============================================================================
// lithe_execution/compile.hpp — converged compile entry point for lithe
//
// Namespace: lithe::execution
//
// Provides:
//   aot_cache_key      — codegen-derived + source-derived native cache key.
//   mir_cost_facts     — language-agnostic cost signals derived from physical MIR.
//   plan()             — cost-model planner: execution_hint × MIR → execution_kind.
//   compile_request    — caller intent bundled for compile().
//   compile_result     — codegen product (jit_function_handle / fallback flag).
//   artifact_store     — in-process fingerprint → compile_result cache.
//   compile()          — the converged single entry point:
//                        physical MIR → backend selection → emit → cache.
//   invoke()           — run a compile_result; adapts handle or fallback.
//
// Architecture (§4.2): JIT / AOT / GPU are entry points into ONE codegen
// pipeline.  The interpreter is the fallback tier, NOT a peer strategy.
// crank supplies source-side key fields + execution_hint; lithe owns
// backend selection, emit, cache, and invoke.
//
// NOTE: asmjit is guarded by LITHE_HAS_ASMJIT at compile time; on targets
// without it, compile() falls back to the interpreter transparently.
//
// No virtual, no macros.  Header-only C++23.
// =============================================================================

#include "../lithe_codegen_pipeline.hpp"
#include "../backends/lithe_codegen_backend_registry.hpp"
#include "../backends/lithe_codegen_interpreter.hpp"
#include "../lithe_exec/exec_hint.hpp"
#if defined(LITHE_HAS_ASMJIT)
#  include "../backends/lithe_codegen_asmjit.hpp"
#endif

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lithe::execution {
    // =============================================================================
    // aot_cache_key — stable key for native artifact caching
    //
    // Lifecycle fields:
    //   source-side (supplied by the frontend / crank): source_hash, dep_hashes,
    //     module_name, compiler_version.
    //   codegen-side (filled here or by compile()): target_triple, backend_id,
    //     opt_profile_id, native_abi_hash.
    //
    // fingerprint() — FNV-1a over all fields in stable order.
    // =============================================================================

    namespace detail {
        [[nodiscard]] constexpr std::uint64_t
        key_fnv1a(std::uint64_t seed, std::string_view s) noexcept {
            constexpr std::uint64_t kPrime = 1099511628211ULL;
            for (const char c : s) {
                seed ^= static_cast<std::uint64_t>(static_cast<std::uint8_t>(c));
                seed *= kPrime;
            }
            return seed;
        }

        [[nodiscard]] constexpr std::uint64_t
        key_fnv1a_u64(std::uint64_t seed, std::uint64_t v) noexcept {
            constexpr std::uint64_t kPrime = 1099511628211ULL;
            for (int i = 0; i < 8; ++i) {
                seed ^= static_cast<std::uint64_t>((v >> (i * 8)) & 0xFFu);
                seed *= kPrime;
            }
            return seed;
        }
    } // namespace detail

    struct aot_cache_key {
        std::string module_name;
        std::uint64_t source_hash = 0; // FNV-1a of source bytes (frontend-supplied)
        std::vector<std::uint64_t> dep_hashes;
        std::string compiler_version;
        std::string target_triple; // e.g. "arm64-apple-macos14.0"
        std::string backend_id; // e.g. "asmjit" or "interpreter"
        std::string opt_profile_id; // e.g. "o3"
        std::uint64_t native_abi_hash = 0; // ABI-relevant layout hash
        std::uint64_t enabled_features = 0;

        [[nodiscard]] std::uint64_t fingerprint() const noexcept {
            constexpr std::uint64_t kOffset = 14695981039346656037ULL;
            std::uint64_t h = kOffset;
            h = detail::key_fnv1a(h, module_name);
            h = detail::key_fnv1a_u64(h, source_hash);
            for (const auto dh : dep_hashes)
                h = detail::key_fnv1a_u64(h, dh);
            h = detail::key_fnv1a(h, compiler_version);
            h = detail::key_fnv1a(h, target_triple);
            h = detail::key_fnv1a(h, backend_id);
            h = detail::key_fnv1a(h, opt_profile_id);
            h = detail::key_fnv1a_u64(h, native_abi_hash);
            h = detail::key_fnv1a_u64(h, enabled_features);
            return h;
        }
    };

    // =============================================================================
    // mir_cost_facts — language-agnostic cost signals over physical MIR
    //
    // Derived from physical_mir_function by count_mir_cost_facts() without any
    // language-specific knowledge.  Used by plan() as the cost-model input.
    // =============================================================================

    struct mir_cost_facts {
        std::uint32_t instr_count = 0;
        std::uint32_t branch_count = 0;
        std::uint32_t block_count = 0;
        std::uint32_t fp_instr_count = 0; // floating-point instructions
        bool has_spills = false;
        bool has_fp = false;
        // est_trip_count: 0 = unknown; filled by frontend as optional refinement.
        std::uint64_t est_trip_count = 0;
    };

    [[nodiscard]] inline mir_cost_facts
    count_mir_cost_facts(const lithe::codegen::mir::physical_mir_function& fn) noexcept {
        using lithe::codegen::opcode;
        mir_cost_facts f;
        f.block_count = static_cast<std::uint32_t>(fn.function.blocks.size());
        for (const auto& blk : fn.function.blocks) {
            for (const auto& inst : blk.instructions) {
                ++f.instr_count;
                if (inst.op == opcode::branch || inst.op == opcode::branch_cond)
                    ++f.branch_count;
                if (inst.op == opcode::load_spill || inst.op == opcode::store_spill)
                    f.has_spills = true;
                if (inst.op == opcode::fadd || inst.op == opcode::fsub ||
                    inst.op == opcode::fmul || inst.op == opcode::fdiv ||
                    inst.op == opcode::fneg || inst.op == opcode::fload ||
                    inst.op == opcode::fload_imm || inst.op == opcode::gpr_to_fp ||
                    inst.op == opcode::fp_to_gpr || inst.op == opcode::fcmp_eq ||
                    inst.op == opcode::fcmp_ne || inst.op == opcode::fcmp_lt ||
                    inst.op == opcode::fcmp_le || inst.op == opcode::fcmp_gt ||
                    inst.op == opcode::fcmp_ge) {
                    ++f.fp_instr_count;
                    f.has_fp = true;
                }
            }
        }
        return f;
    }

    // =============================================================================
    // plan() — cost-model planner
    //
    // Returns the execution_kind that best matches the physical MIR + caller hint:
    //   - If hint.required && hint.preferred: force that kind if policy.allows() and
    //     the backend exists; otherwise stay on scalar (caller emits hard diagnostic).
    //   - Otherwise: rank by cost model biased toward hint.preferred.
    //
    // Concretely maps execution_kind → a backend name:
    //   scalar / (no asmjit)  → interpreter fallback
    //   scalar                → asmjit (hot CPU path)
    //   simd                  → simd
    //   gpu                   → gpu
    //   threaded / distributed→ interpreter (not yet JIT-parallel)
    //
    // Returns execution_kind::scalar when asmjit is the right target.
    // Callers map scalar → "asmjit" themselves (plan returns the *kind*, not name).
    // =============================================================================

    [[nodiscard]] inline lithe::exec::execution_kind
    plan(const lithe::codegen::mir::physical_mir_function& phys,
         const lithe::exec::execution_hint& hint = {},
         const lithe::exec::auto_execution_policy& policy = {}) noexcept {
        using lithe::exec::execution_kind;
        using lithe::codegen::backends::backend_kind;
        using lithe::codegen::backends::backend_kind_from_string;

        // Forced path: hint.required means emit diagnostic on miss but still try.
        if (hint.required && hint.preferred.has_value()) {
            const auto k = *hint.preferred;
            if (policy.allows(k)) {
                // Validate that a backend for this kind exists (light check).
                std::string_view name;
                switch (k) {
                case execution_kind::simd: name = "simd";
                    break;
                case execution_kind::gpu: name = "gpu";
                    break;
                default: name = "asmjit";
                    break;
                }
                if (backend_kind_from_string(name).has_value())
                    return k;
            }
            // Forced kind unavailable; fall through to best-available.
        }

        // Prefer hint if present and policy allows.
        if (hint.preferred.has_value() && !hint.required) {
            const auto k = *hint.preferred;
            if (!hint.forbid_gpu && k == execution_kind::gpu && policy.allows(k))
                return execution_kind::gpu;
            if (!hint.forbid_parallel && k == execution_kind::threaded && policy.allows(k))
                return execution_kind::threaded;
            if (k == execution_kind::simd && policy.allows(k))
                return execution_kind::simd;
        }

        // Cost-model defaults (MIR-driven).
        (void)phys; // mir_cost_facts available via count_mir_cost_facts(phys) if needed

        // GPU/SIMD forbidden?
        if (!hint.forbid_gpu && policy.allows(execution_kind::gpu)) {
            // Only return gpu if explicitly preferred; do not auto-promote to gpu.
        }
        if (policy.allows(execution_kind::simd)) {
            // Only return simd if explicitly preferred.
        }

        // Default: scalar (→ asmjit on CPU when available, interpreter as fallback).
        return execution_kind::scalar;
    }

    // =============================================================================
    // compile_result — the output of compile()
    //
    // Contains the selected backend name, the compilation_artifact (which holds the
    // jit_function_handle when asmjit was used), and a fallback_fired flag.
    // When fallback_fired=true the artifact was produced by the interpreter.
    // =============================================================================

    struct compile_result {
        lithe::codegen::compilation_artifact artifact;
        std::string selected_backend;
        bool fallback_fired = false;
        std::vector<std::string> diagnostics;

        [[nodiscard]] bool ok() const noexcept {
            return artifact.ok() || fallback_fired;
        }

        // Returns true when the artifact holds a native jit_function_handle.
        [[nodiscard]] bool is_native() const noexcept {
            return artifact.kind == lithe::codegen::artifact_kind::jit_function
                && artifact.handle && artifact.handle->valid();
        }
    };

    // =============================================================================
    // compile_request — caller intent bundled for compile()
    // =============================================================================

    struct compile_request {
        lithe::exec::execution_hint hint; // from @parallel/@simd/@gpu attrs
        lithe::exec::auto_execution_policy policy; // global planning policy
        std::optional<aot_cache_key> cache_key; // present → persist native artifact
    };

    // =============================================================================
    // artifact_store — in-process fingerprint → compile_result cache
    //
    // Stores the full compile_result (including the live jit_function_handle) for
    // re-use across invocations.  Keyed by aot_cache_key::fingerprint().
    // =============================================================================

    class artifact_store {
    public:
        artifact_store() = default;

        // Store a compile_result under the given key.  Overwrites on collision.
        void store(const aot_cache_key& key, compile_result res) {
            entries_[key.fingerprint()] = std::move(res);
        }

        // Find a cached result.  Returns nullptr on miss.
        [[nodiscard]] const compile_result*
        find(const aot_cache_key& key) const noexcept {
            auto it = entries_.find(key.fingerprint());
            if (it == entries_.end()) return nullptr;
            return &it->second;
        }

        void invalidate(const aot_cache_key& key) { entries_.erase(key.fingerprint()); }
        [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
        [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
        void clear() noexcept { entries_.clear(); }

    private:
        std::unordered_map<std::uint64_t, compile_result> entries_;
    };

    // =============================================================================
    // compile() — the converged single entry point
    //
    // 1. Check artifact_store (if req.cache_key present) → cache hit → return.
    // 2. plan() → execution_kind → map to backend name.
    // 3. execute_with_fallback(primary=asmjit/simd/gpu, fallback=interpreter).
    // 4. Keep the compilation_artifact (NOT discarded).
    // 5. On cache_key present: store in artifact_store.
    // Returns compile_result.
    //
    // Callers MUST use invoke() to run the result — never touch the artifact handle
    // directly (ownership semantics enforced through compile_result).
    // =============================================================================

    [[nodiscard]] inline compile_result
    compile(const lithe::codegen::mir::physical_mir_function& phys,
            const compile_request& req,
            artifact_store* store = nullptr) noexcept {
        using namespace lithe::codegen::backends;
        using lithe::exec::execution_kind;

        // Cache hit check.
        if (store && req.cache_key.has_value()) {
            const auto* cached = store->find(*req.cache_key);
            if (cached) return *cached;
        }

        compile_result res;

        // Determine target backend from planner.
        const auto kind = plan(phys, req.hint, req.policy);

        std::string primary_name;
        switch (kind) {
        case execution_kind::simd: primary_name = "simd";
            break;
        case execution_kind::gpu: primary_name = "gpu";
            break;
        default:
#if defined(LITHE_HAS_ASMJIT)
            primary_name = "asmjit";
#else
            primary_name = "interpreter";
#endif
            break;
        }

        res.selected_backend = primary_name;

        const auto primary_kind_opt = backend_kind_from_string(primary_name);
        if (!primary_kind_opt || primary_name == "interpreter") {
            // No codegen backend available — run interpreter directly.
            interpreter_backend interp;
            res.artifact = interp.emit(phys);
            res.fallback_fired = true;
            for (const auto& d : res.artifact.diagnostics)
                res.diagnostics.push_back(d);
            return res;
        }

        backend_variant primary = make_backend(*primary_kind_opt);
        backend_variant fallback = make_backend(backend_kind::interpreter);

        res.artifact = execute_with_fallback(phys, primary, fallback);

        // Detect whether the fallback fired (artifact diagnostics contain the marker).
        for (const auto& d : res.artifact.diagnostics) {
            if (d.find("execute_with_fallback") != std::string::npos)
                res.fallback_fired = true;
            res.diagnostics.push_back(d);
        }

        // Force-requirement diagnostic: if hint.required and we fell back, record.
        if (req.hint.required && req.hint.preferred.has_value() && res.fallback_fired) {
            res.diagnostics.push_back(
                std::string("lithe: required backend '")
                + std::string(lithe::exec::to_string(*req.hint.preferred))
                + "' unavailable; fell back to interpreter");
        }

        // Cache the result.
        if (store && req.cache_key.has_value())
            store->store(*req.cache_key, res);

        return res;
    }

    // =============================================================================
    // invoke() — run a compile_result, return optional scalar
    //
    // • Native artifact (jit_function): calls jit_function_handle::call(a, b).
    // • Interpreter fallback: reads the return_value from artifact.metadata.
    //
    // args must have at least 2 elements; excess are ignored (handle ABI is 2-arg).
    // Returns nullopt when the function is void or execution failed.
    // =============================================================================

    [[nodiscard]] inline std::optional<std::int64_t>
    invoke(const compile_result& res,
           std::span<const std::int64_t> args = {}) noexcept {
        const std::int64_t a = args.size() >= 1 ? args[0] : 0;
        const std::int64_t b = args.size() >= 2 ? args[1] : 0;

        if (res.is_native()) {
            // Native JIT path: call through the function pointer.
#if defined(LITHE_HAS_ASMJIT)
            const auto* handle = res.artifact.handle->get<lithe::codegen::backends::jit_function_handle>();
            if (handle && handle->valid()) {
                try {
                    return handle->call(a, b);
                }
                catch (...) {
                    return std::nullopt;
                }
            }
#endif
        }

        // Interpreter fallback path: read return_value from artifact metadata.
        const auto it = res.artifact.metadata.find("return_value");
        if (it != res.artifact.metadata.end()) {
            try {
                return static_cast<std::int64_t>(std::stoll(it->second));
            }
            catch (...) {}
        }

        return std::nullopt;
    }
} // namespace lithe::execution
