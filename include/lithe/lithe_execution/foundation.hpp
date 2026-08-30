#pragma once

// =============================================================================
// lithe_execution/foundation.hpp — single source of truth for execution-layer
// identity, capability, IR-kind, artifact classification, stage errors,
// execution-mode, target-descriptor seams, and neutral policy defaults.
//
// DEPENDENCY RULE (): This header MUST be includable without pulling in
//   lithe::ir, lithe_rt/engine.hpp, or any backend header.
// All types defined here are backend-neutral and execution-layer vocabulary.
//
// lithe::codegen re-exports everything it previously defined here as aliases
// (see lithe_codegen_pipeline.hpp compatibility block) so no existing codegen
// user needs to change an include path.
//
// No virtual, no macros.  Header-only C++23.
// =============================================================================

#include <array>
#include <atomic>
#include <bitset>
#include <concepts>
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <variant>

namespace lithe::execution {
    // =========================================================================
    //  Backend identity — four distinct notions
    // =========================================================================

    // persisted_backend_id — deterministic string/hash form.  Safe to store in
    // config files, caches, and serialized artifacts.  MUST NOT be used to cast
    // or dispatch to a concrete C++ type at runtime — that is type_token's role.
    struct persisted_backend_id {
        std::string_view value; // points into static storage (plugin descriptor)

        [[nodiscard]] constexpr bool operator==(const persisted_backend_id&) const noexcept
        = default;
        [[nodiscard]] constexpr bool empty() const noexcept { return value.empty(); }
    };

    // backend_display_name — human-readable label.  May change across versions.
    // Not a stable identity; never compare two backends by display name.
    struct backend_display_name {
        std::string_view value;
        [[nodiscard]] constexpr bool operator==(const backend_display_name&) const noexcept
        = default;
    };

    // in_process_type_token — process-local identity derived from typeid() or
    // an NTTP-computed hash.  Used for safe same-process type dispatch.
    // MUST NOT be persisted across processes or stored in serialized form.
    struct in_process_type_token {
        std::uint64_t value = 0;

        [[nodiscard]] constexpr bool operator==(const in_process_type_token&) const noexcept
        = default;
        [[nodiscard]] constexpr bool valid() const noexcept { return value != 0; }
    };

    // Prevent accidental construction of type_token from persisted id numeric.
    static_assert(!std::is_constructible_v<in_process_type_token, std::string_view>,
                  "type_token must not be constructible from persisted id string");

    // =========================================================================
    // backend_capability_set — moved here from lithe::codegen
    //
    // lithe::codegen re-exports this via `using` alias so existing code compiles.
    // =========================================================================

    enum class backend_feature : std::uint8_t {
        integer_arithmetic,
        floating_arithmetic,
        spill_load_store,
        branches,
        calls,
        memory_operands,
        stack_frame,
        interpreter_execution,
        tensor_arithmetic,
        symbolic_arithmetic,
    };

    struct backend_capability_set {
        std::uint64_t bits = 0;

        constexpr backend_capability_set() = default;
        constexpr explicit backend_capability_set(const std::uint64_t raw) : bits(raw) {}

        [[nodiscard]] static constexpr backend_capability_set from(
            std::initializer_list<backend_feature> feats) noexcept {
            backend_capability_set out;
            for (const auto f : feats) out.add(f);
            return out;
        }

        constexpr void add(const backend_feature f) noexcept {
            bits |= (std::uint64_t{1} << static_cast<std::uint8_t>(f));
        }

        [[nodiscard]] constexpr bool has(const backend_feature f) const noexcept {
            return (bits & (std::uint64_t{1} << static_cast<std::uint8_t>(f))) != 0;
        }

        [[nodiscard]] constexpr bool contains_all(const backend_capability_set& other) const noexcept {
            return (bits & other.bits) == other.bits;
        }

        [[nodiscard]] constexpr backend_capability_set
        missing(const backend_capability_set& provided) const noexcept {
            return backend_capability_set{bits & ~provided.bits};
        }

        [[nodiscard]] constexpr bool empty() const noexcept { return bits == 0; }

        [[nodiscard]] friend constexpr backend_capability_set
        operator|(const backend_capability_set a, const backend_capability_set b) noexcept {
            return backend_capability_set{a.bits | b.bits};
        }

        [[nodiscard]] friend constexpr backend_capability_set
        operator&(const backend_capability_set a, const backend_capability_set b) noexcept {
            return backend_capability_set{a.bits & b.bits};
        }

        [[nodiscard]] friend constexpr backend_capability_set
        operator~(const backend_capability_set a) noexcept {
            return backend_capability_set{~a.bits};
        }

        [[nodiscard]] friend constexpr bool
        operator==(const backend_capability_set a, const backend_capability_set b) noexcept {
            return a.bits == b.bits;
        }
    };

    static_assert(std::is_trivially_copyable_v<backend_capability_set>);

    // =========================================================================
    // Execution admission vocabulary (Phase 1 adapter surface)
    // =========================================================================

    enum class backend_provider : std::uint8_t {
        unknown = 0,
        host,
        simd,
        metal,
        vulkan,
    };

    enum class backend_admission_reason : std::uint8_t {
        admitted = 0,
        plan_rejected,
        provider_unavailable,
        policy_restricted,
        installation_failed,
        dispatch_failed,
        unknown,
    };

    struct backend_admission_state {
        backend_provider provider = backend_provider::unknown;
        bool plan_admitted = false;
        bool provider_available = false;
        backend_admission_reason reason = backend_admission_reason::unknown;

        [[nodiscard]] constexpr bool usable() const noexcept {
            return plan_admitted && provider_available;
        }
    };

    // =========================================================================
    // IR-kind identity + artifact_class classification
    // =========================================================================

    enum class ir_kind : std::uint8_t {
        unknown = 0,
        surface_ast, // lithe expression tree (pre-optimization)
        canonical_ast, // after canonicalization
        optimized_ast, // after optimization passes
        hl_mir, // high-level structured MIR (hl::hl_mir_function)
        physical_mir, // flat register MIR (mir::physical_mir_function)
        managed_mir, // physical_mir annotated for managed runtime
        text_assembly, // emitted text assembly (target-dependent)
        object_code, // relocatable binary
        aot_image, // ahead-of-time executable image
        jit_code, // live JIT-compiled native code
    };

    // artifact_class — broader classification of pipeline output.
    // Orthogonal to artifact_kind (which describes format); this describes role.
    enum class artifact_class : std::uint8_t {
        none = 0,
        diagnostic, // only diagnostics, no executable content
        interpreter_plan, // bytecode/interpreter-executable plan
        native_code, // machine code installed in executable memory
        text_report, // human-readable text (debug_text, assembly listing)
        binary_object, // relocatable or AOT binary
        metadata_only, // metadata bundle with no executable content
    };

    // =========================================================================
    // Stage errors
    //
    // Each is a distinct type — compile_install_error is NOT constructible from
    // a pair of compile_error + install_error (fused-backend failure invariant).
    // =========================================================================

    struct compile_error {
        std::string_view detail;
        constexpr explicit compile_error(const std::string_view d = {}) noexcept : detail(d) {}
    };

    struct install_error {
        std::string_view detail;
        constexpr explicit install_error(const std::string_view d = {}) noexcept : detail(d) {}
    };

    // Fused-backend failure: the backend cannot separate compilation from
    // installation (e.g. AsmJIT emitting directly to executable memory).
    // Distinct from compile_error or install_error; must not be implicitly
    // constructible from either.
    struct compile_install_error {
        std::string_view detail;

        constexpr explicit compile_install_error(const std::string_view d = {}) noexcept
            : detail(d) {}
    };

    struct selection_error {
        std::string_view detail;
        constexpr explicit selection_error(const std::string_view d = {}) noexcept : detail(d) {}
    };

    struct execution_error {
        std::string_view detail;
        constexpr explicit execution_error(const std::string_view d = {}) noexcept : detail(d) {}
    };

    struct ir_error {
        std::string_view detail;
        constexpr explicit ir_error(const std::string_view d = {}) noexcept : detail(d) {}
    };

    // P0B: native executable-code installation unavailable (JIT backend not wired).
    struct native_install_unavailable {
        std::string_view detail;

        constexpr explicit native_install_unavailable(const std::string_view d = {}) noexcept
            : detail(d) {}
    };

    // Verify distinct-failure invariant at compile time.
    static_assert(!std::is_constructible_v<compile_install_error, compile_error>,
                  "compile_install_error must not be constructible from compile_error");
    static_assert(!std::is_constructible_v<compile_install_error, install_error>,
                  "compile_install_error must not be constructible from install_error");

    // =========================================================================
    // execution_mode enum + execution_mode_set bitset ()
    //
    // Describes backend compilation/invocation strategy: HOW code is compiled
    // and installed for execution.  Orthogonal to lithe::exec::execution_kind
    // (in lithe_exec/exec_kinds.hpp) which describes the parallelism model used
    // for analysis & planning.  Use lithe_exec/exec_bridge.hpp to convert.
    // =========================================================================

    enum class execution_mode : std::uint8_t {
        interpret = 0, // software interpreter execution
        jit_tier1, // fast/baseline JIT (low compile cost, modest speed)
        jit_tier2, // optimizing JIT (higher compile cost, peak speed)
        aot, // ahead-of-time compiled code
        native_inline, // inline native code emitted at build time
        device, // GPU / device compute tier (Vulkan/MoltenVK)
        out_of_proc, // isolated / out-of-process plugin execution
    };

    inline constexpr std::size_t execution_mode_count = 7;

    struct execution_mode_set {
        std::bitset<execution_mode_count> bits;

        constexpr execution_mode_set() noexcept = default;

        constexpr void set(const execution_mode m) noexcept {
            bits.set(static_cast<std::size_t>(m));
        }

        constexpr void reset(const execution_mode m) noexcept {
            bits.reset(static_cast<std::size_t>(m));
        }

        [[nodiscard]] constexpr bool test(const execution_mode m) const noexcept {
            return bits.test(static_cast<std::size_t>(m));
        }

        [[nodiscard]] constexpr bool any() const noexcept { return bits.any(); }
        [[nodiscard]] constexpr bool none() const noexcept { return bits.none(); }
        [[nodiscard]] constexpr bool all() const noexcept { return bits.all(); }
    };

    // =========================================================================
    // Target-descriptor seams
    // =========================================================================

    enum class memory_domain : std::uint8_t {
        host_cpu = 0, // standard CPU-accessible heap
        device_gpu, // GPU-local memory (e.g. Metal, CUDA)
        shared_unified, // CPU+GPU unified memory
        guest_sandbox, // sandboxed guest linear memory
    };

    // buffer — type seam for a typed memory region; no ownership semantics.
    struct buffer {
        void* base = nullptr;
        std::size_t size_bytes = 0;
        memory_domain domain = memory_domain::host_cpu;

        [[nodiscard]] constexpr bool valid() const noexcept {
            return base != nullptr && size_bytes > 0;
        }
    };

    // kernel_launch — type seam for an async dispatch descriptor.
    // Full async wiring is out of scope for ; this seam lets backends
    // name the type without a circular include.
    struct kernel_launch {
        void* fn_ptr = nullptr;
        std::uint32_t grid_x = 1;
        std::uint32_t grid_y = 1;
        std::uint32_t block_x = 1;
        std::uint32_t block_y = 1;
        buffer args{};
    };

    // execution_event — opaque async completion token.
    struct execution_event {
        std::uint64_t id = 0;
        [[nodiscard]] constexpr bool valid() const noexcept { return id != 0; }
    };

    // =========================================================================
    //  backend_slot_state + backend_lifetime — per-slot lifecycle control
    //
    // Defined here (not in registry.hpp) so resource.hpp can hold a
    // shared_ptr<backend_lifetime> with a complete type without including
    // the full registry header.
    // =========================================================================

    enum class backend_slot_state : std::uint8_t {
        live = 0, // normal; new leases can be acquired
        retiring = 1, // unregister requested; no new leases; wait for refcount
    };

    struct backend_lifetime {
        std::atomic<std::uint64_t> refcount{0};
        std::atomic<backend_slot_state> state{backend_slot_state::live};

        [[nodiscard]] bool is_live() const noexcept {
            return state.load(std::memory_order_acquire) == backend_slot_state::live;
        }

        [[nodiscard]] std::uint64_t live_refs() const noexcept {
            return refcount.load(std::memory_order_acquire);
        }

        [[nodiscard]] bool try_pin() noexcept {
            if (state.load(std::memory_order_acquire) != backend_slot_state::live)
                return false;
            refcount.fetch_add(1, std::memory_order_acq_rel);
            return true;
        }

        void unpin() noexcept {
            refcount.fetch_sub(1, std::memory_order_acq_rel);
        }
    };

    // =========================================================================
    // Neutral policy defaults
    //
    // These empty structs are the default template parameters for the core
    // headers so they never need to name a lithe::ir or pipeline-hooks type.
    // A consumer can specialize them by providing a non-empty policy struct.
    // =========================================================================

    struct no_ir_integration {};

    struct no_pipeline_hooks {};

    static_assert(std::is_empty_v<no_ir_integration>,
                  "no_ir_integration must be empty (zero-cost default)");
    static_assert(std::is_empty_v<no_pipeline_hooks>,
                  "no_pipeline_hooks must be empty (zero-cost default)");

    // =========================================================================
    //  Core engine error types (G8 — moved here from lithe_engine.hpp)
    //
    // Placed in foundation.hpp so lithe_ir/integration.hpp can reference them
    // without a circular include through lithe_engine.hpp.
    //
    // INVARIANT: neither type may include ir_error as an alternative.
    //   ir_error is an IR-layer concern; engine compile errors are execution-layer.
    //
    // Both types wrap a variant of stage errors and expose a .detail accessor
    // that returns the active alternative's detail string (for diagnostics).
    // =========================================================================

    namespace impl {
        // Extract .detail from any stage-error alternative.
        struct detail_visitor {
            constexpr std::string_view operator()(const selection_error& e) const noexcept { return e.detail; }
            constexpr std::string_view operator()(const compile_error& e) const noexcept { return e.detail; }
            constexpr std::string_view operator()(const install_error& e) const noexcept { return e.detail; }
            constexpr std::string_view operator()(const compile_install_error& e) const noexcept { return e.detail; }
            constexpr std::string_view operator()(const execution_error& e) const noexcept { return e.detail; }
        };
    } // namespace impl

    struct engine_compile_error {
        using cause_t = std::variant<
            selection_error,
            compile_error,
            install_error,
            compile_install_error>;

        cause_t cause;
        std::string_view detail; // mirrors the active alternative's detail field

        engine_compile_error() = default;

        // Construct from any alternative: store cause and cache detail.
        template <class T>
            requires std::constructible_from<cause_t, T>
        constexpr explicit(false) engine_compile_error(T&& v)
            noexcept(std::is_nothrow_constructible_v<cause_t, T>)
            : cause(std::forward<T>(v))
              , detail(std::visit(impl::detail_visitor{}, cause)) {}
    };

    struct engine_compile_invoke_error {
        using cause_t = std::variant<
            selection_error,
            compile_error,
            install_error,
            compile_install_error,
            execution_error>;

        cause_t cause;
        std::string_view detail; // mirrors the active alternative's detail field

        engine_compile_invoke_error() = default;

        template <class T>
            requires std::constructible_from<cause_t, T>
        constexpr explicit(false) engine_compile_invoke_error(T&& v)
            noexcept(std::is_nothrow_constructible_v<cause_t, T>)
            : cause(std::forward<T>(v))
              , detail(std::visit(impl::detail_visitor{}, cause)) {}
    };

    // Compile-time invariant: core engine errors must never embed ir_error.
    static_assert(!std::disjunction_v<
                      std::is_same<ir_error, selection_error>,
                      std::is_same<ir_error, compile_error>,
                      std::is_same<ir_error, install_error>,
                      std::is_same<ir_error, compile_install_error>>,
                  "engine_compile_error alternatives must not be ir_error");

    // no_observer — zero-cost default for the engine's Observer slot.
    // Distinct role from no_pipeline_hooks (which is the pipeline's IR hook slot).
    struct no_observer {};

    static_assert(std::is_empty_v<no_observer>,
                  "no_observer must be empty (zero-cost default)");
} // namespace lithe::execution
