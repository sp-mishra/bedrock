#pragma once

// crank/host.hpp — macro-free C++ host embedding surface for crank.
//
// C++23, header-only, no virtual, no macros. Namespace: crank
//
// Three-stage lifecycle:
//   context_builder   — register functions/types/containers/resources/backends/annotations
//   finalized_context — immutable registry snapshot + fingerprints (after builder.finalize())
//   runtime_instance  — finalized_context + scheduler/cancellation/observability
//
// Fast path: free functions use direct typed thunks (no std::any, no std::function).
// Dynamic boundary: capturing callables + plugins use explicit erased storage.
//
// Key types:
//   stable_entity_id / stable_function_id / stable_type_id — cross-compilation-stable hashes
//   function_descriptor  — §5.2: typed flags, effects, capabilities, direct thunk
//   host_type_descriptor — §7.2: layout, ownership, operations
//   field_descriptor     — §7.3: stable ID, access, mutability
//   container_descriptor — §9.2: capabilities bitmask
//   resource_descriptor  — §10.1: lifetime, threading, tx integration
//   backend_descriptor   — §11.2: kind, capabilities, limits
//   host_error / error_adapter<E> — §13: boundary error conversion
//   host_value / owned_host_value — §16: controlled dynamic values (replaces std::any in hot paths)
//   observability_options — §15: pay-for-use event sink
//   context_builder / finalized_context — §3: build→finalize lifecycle
//   build_result = std::expected<finalized_context, std::vector<build_diagnostic>>

#include "lithe/lithe_extension.hpp"        // lithe::fixed_string
#include "medha/resource_traits.hpp"       // medha::resource_traits<R>, medha::commit_capability
#include "containers/descriptor_registry.hpp" // containers::desc_name_hash
#include "languages/crank/effects.hpp"     // effect_mask, capability_mask
#include "languages/crank/annotation.hpp"  // annotation_registry, CrankExtension, etc.

#include <any>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <mdspan>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace crank {
    // ============================================================================
    // callable_traits — extract return type, parameter pack, and arity.
    // Supports: free functions, non-capturing lambdas, const member operators,
    //           noexcept variants.
    // ============================================================================

    namespace detail {
        template <class F, class = void>
        struct callable_traits : callable_traits<decltype(&std::decay_t<F>::operator())> {};

        template <class R, class... Args>
        struct callable_traits<R(*)(Args...), void> {
            using return_type = R;
            using param_types = std::tuple<std::decay_t<Args>...>;
            static constexpr std::size_t arity = sizeof...(Args);
            static constexpr bool is_noexcept = false;
        };

        template <class R, class... Args>
        struct callable_traits<R(*)(Args...) noexcept, void> {
            using return_type = R;
            using param_types = std::tuple<std::decay_t<Args>...>;
            static constexpr std::size_t arity = sizeof...(Args);
            static constexpr bool is_noexcept = true;
        };

        template <class C, class R, class... Args>
        struct callable_traits<R(C::*)(Args...) const, void> {
            using return_type = R;
            using param_types = std::tuple<std::decay_t<Args>...>;
            static constexpr std::size_t arity = sizeof...(Args);
            static constexpr bool is_noexcept = false;
        };

        template <class C, class R, class... Args>
        struct callable_traits<R(C::*)(Args...) const noexcept, void> {
            using return_type = R;
            using param_types = std::tuple<std::decay_t<Args>...>;
            static constexpr std::size_t arity = sizeof...(Args);
            static constexpr bool is_noexcept = true;
        };
    } // namespace detail

    // ============================================================================
    // HostCallable — detects free functions or types with operator()
    // ============================================================================

    template <class F>
    concept HostCallable =
        std::is_function_v<std::remove_pointer_t<F>> ||
        requires(F& f) { &std::decay_t<F>::operator(); };

    // ============================================================================
    // field<Name, Ptr> — compile-time field descriptor for type_descriptor
    // ============================================================================

    template <lithe::fixed_string Name, auto Ptr>
    struct field {
        static constexpr auto name = Name;
        static constexpr auto pointer = Ptr;
    };

    // ============================================================================
    // type_descriptor<T> — seam for C++ type → crank type reflection.
    // Specialise to expose T to crank:
    //   template<> struct crank::type_descriptor<Vec3> {
    //     static constexpr std::string_view name = "Vec3";
    //     static constexpr auto fields = std::tuple{
    //         field<"x", &Vec3::x>{}, field<"y", &Vec3::y>{}, field<"z", &Vec3::z>{}
    //     };
    //   };
    // Default: undefined (no exposure).
    // ============================================================================

    template <class T>
    struct type_descriptor; // user specialises

    // ============================================================================
    // has_type_descriptor<T> — detect if T has a crank::type_descriptor
    // ============================================================================

    template <class T, class = void>
    inline constexpr bool has_type_descriptor = false;

    template <class T>
    inline constexpr bool has_type_descriptor<
        T, std::void_t<decltype(crank::type_descriptor<T>::name)>> = true;

    // ============================================================================
    // Stable entity identity (§4)
    //
    // Cross-compilation-stable: derived from qualified name + kind + schema version
    // via FNV-1a (containers::desc_name_hash). Independent of pointer values, RTTI,
    // static init order, or registration order.
    // ============================================================================

    struct stable_entity_id {
        std::uint64_t namespace_hash = 0;
        std::uint64_t name_hash = 0;
        std::uint32_t kind = 0;
        std::uint32_t schema_version = 1;

        [[nodiscard]] bool operator==(const stable_entity_id&) const noexcept = default;
    };

    // Convenience typed aliases — same struct, different semantic intent.
    using stable_function_id = stable_entity_id;
    using stable_type_id = stable_entity_id;
    using stable_field_id = stable_entity_id;
    using stable_resource_id = stable_entity_id;
    // host_backend_id: stable identity for registered host backends.
    // Named host_backend_id to avoid clash with capability.hpp's backend_id = uint32_t.
    using host_backend_id = stable_entity_id;

    // descriptor_fingerprint — stronger hash over full canonical descriptor fields.
    using descriptor_fingerprint = std::uint64_t;

    // Entity kind constants for stable_entity_id::kind.
    inline constexpr std::uint32_t kKindFunction = 1u;
    inline constexpr std::uint32_t kKindType = 2u;
    inline constexpr std::uint32_t kKindField = 3u;
    inline constexpr std::uint32_t kKindResource = 4u;
    inline constexpr std::uint32_t kKindBackend = 5u;
    inline constexpr std::uint32_t kKindContainer = 6u;

    namespace detail {
        // FNV-1a 64-bit fold over a string — deterministic, cross-platform.
        [[nodiscard]] constexpr std::uint64_t fnv1a(std::string_view s) noexcept {
            std::uint64_t h = 14695981039346656037ULL;
            for (unsigned char c : s) {
                h ^= c;
                h *= 1099511628211ULL;
            }
            return h;
        }

        // Split "namespace.name" at the last '.'; namespace_hash covers all but last segment.
        [[nodiscard]] constexpr stable_entity_id make_id(std::string_view qualified,
                                                         std::uint32_t kind,
                                                         std::uint32_t version = 1) noexcept {
            auto dot = qualified.rfind('.');
            std::string_view ns = (dot == std::string_view::npos) ? "" : qualified.substr(0, dot);
            std::string_view name = (dot == std::string_view::npos) ? qualified : qualified.substr(dot + 1);
            return {fnv1a(ns), fnv1a(name), kind, version};
        }

        // Combine two fingerprint values (FNV-1a fold over bytes).
        [[nodiscard]] constexpr descriptor_fingerprint fp_combine(
            descriptor_fingerprint a, descriptor_fingerprint b) noexcept {
            std::uint64_t h = 14695981039346656037ULL;
            for (int i = 0; i < 8; ++i) {
                h ^= (a & 0xFF);
                h *= 1099511628211ULL;
                a >>= 8;
            }
            for (int i = 0; i < 8; ++i) {
                h ^= (b & 0xFF);
                h *= 1099511628211ULL;
                b >>= 8;
            }
            return h;
        }
    } // namespace detail

    // ============================================================================
    // blocking_class — scheduling hint for the async/coroutine planner (§14.5)
    // ============================================================================

    enum class blocking_class : std::uint8_t {
        non_blocking, // returns promptly; safe on any worker
        potentially_blocking, // may block (I/O, locks) — offload to a blocking pool
        async, // returns a future/awaitable — integrates with await
        thread_affine, // must run on a specific/owning thread
    };

    [[nodiscard]] constexpr std::string_view to_string(blocking_class b) noexcept {
        switch (b) {
        case blocking_class::non_blocking: return "non_blocking";
        case blocking_class::potentially_blocking: return "potentially_blocking";
        case blocking_class::async: return "async";
        case blocking_class::thread_affine: return "thread_affine";
        }
        return "unknown";
    }

    // ============================================================================
    // function_flag / function_flags (§5.2)
    // ============================================================================

    enum class function_flag : std::uint32_t {
        pure = 1u << 0,
        thread_safe = 1u << 1,
        deterministic = 1u << 2,
        blocking = 1u << 3,
        asynchronous = 1u << 4,
        cancellation_aware = 1u << 5,
        gpu_compatible = 1u << 6,
        transaction_safe = 1u << 7,
        may_throw_cpp = 1u << 8,
    };

    using function_flags = std::uint32_t;

    // ============================================================================
    // source_boundary_policy — exception handling at the host/crank boundary (§13.4)
    // ============================================================================

    enum class source_boundary_policy : std::uint8_t {
        require_noexcept, // function must be noexcept; throwing = UB
        convert_registered, // catch registered error types; rethrow unknown
        convert_all, // catch std::exception + ...; convert to host_error
        terminate, // std::terminate on any exception
    };

    // ============================================================================
    // injected_parameter — runtime parameters stripped from Crank-visible arity (§14.2)
    // ============================================================================

    enum class injected_parameter : std::uint8_t {
        cancellation_token,
        deadline,
        transaction_context,
        execution_context,
        logger,
        trace_span,
    };

    // ============================================================================
    // cancellation_behavior — how a host function responds to cancellation (§14.3)
    // ============================================================================

    enum class cancellation_behavior : std::uint8_t {
        unsupported,
        cooperative,
        interruptible,
        completes_in_background,
    };

    // ============================================================================
    // function_descriptor — full descriptor for a registered host function (§5.2)
    //
    // Typed fast path: typed_thunk is a direct function pointer (no std::any).
    // Capturing callable path: erased_invoke holds the closure (std::function only
    // at the explicit dynamic boundary — not in hot loops).
    // ============================================================================

    struct function_descriptor {
        stable_function_id id;
        std::string name; // qualified name, e.g. "math.dot"
        std::size_t arity = 0;

        stable_type_id return_type;
        std::vector<stable_type_id> parameter_types;

        effect_mask effects = 0;
        capability_mask capabilities = 0;
        function_flags flags = 0;
        source_boundary_policy boundary = source_boundary_policy::require_noexcept;
        blocking_class blocking = blocking_class::non_blocking;
        cancellation_behavior cancellation = cancellation_behavior::unsupported;

        // Direct typed thunk: args[i] are const void* pointing to typed C++ values.
        // result is void* pointing to storage for the return value.
        // nullptr for capturing callables (use erased_invoke instead).
        void (*typed_thunk)(const void* const* args, void* result) = nullptr;

        // Capturing callable path (explicit dynamic boundary only, §5.5).
        std::function<std::any(std::span<const std::any>)> trampoline;

        descriptor_fingerprint fingerprint = 0;
    };

    // function_options — optional per-registration overrides
    struct function_options {
        effect_mask effects = 0;
        capability_mask capabilities = 0;
        function_flags flags = 0;
        source_boundary_policy boundary = source_boundary_policy::require_noexcept;
        blocking_class blocking = blocking_class::non_blocking;
        cancellation_behavior cancellation = cancellation_behavior::unsupported;
    };

    // ============================================================================
    // Typed thunk generation (§5.4)
    //
    // detail::make_typed_thunk<Fn>() returns a function pointer that unpacks
    // const void* const* args into typed C++ values, calls Fn, and writes the
    // return value to *result. No std::any, no heap allocation.
    // ============================================================================

    namespace detail {
        template <auto Fn, class Params, std::size_t... Is>
        void invoke_typed_impl(const void* const* args, void* result,
                               std::index_sequence<Is...>) {
            using Traits = callable_traits<decltype(Fn)>;
            using R = typename Traits::return_type;
            if constexpr (std::is_void_v<R>) {
                Fn(*static_cast<const std::tuple_element_t<Is, Params>*>(args[Is])...);
            }
            else {
                *static_cast<R*>(result) =
                    Fn(*static_cast<const std::tuple_element_t<Is, Params>*>(args[Is])...);
            }
        }

        template <auto Fn>
        [[nodiscard]] constexpr auto make_typed_thunk() noexcept {
            using Traits = callable_traits<decltype(Fn)>;
            return +[](const void* const* args, void* result) {
                invoke_typed_impl<Fn, typename Traits::param_types>(
                    args, result, std::make_index_sequence < Traits::arity >
                {}
                )
                ;
            };
        }

        // Legacy std::any trampoline (compat path for capturing callables).
        template <auto Fn, class Params, std::size_t... Is>
        [[nodiscard]] std::function<std::any(std::span<const std::any>)>
        make_erased_invoke_impl(std::index_sequence<Is...>) {
            return [](std::span<const std::any> a) -> std::any {
                using Traits = callable_traits<decltype(Fn)>;
                using R = typename Traits::return_type;
                if constexpr (std::is_void_v<R>) {
                    Fn(std::any_cast<std::tuple_element_t<Is, Params>>(a[Is])...);
                    return std::any{};
                }
                else {
                    return std::any{Fn(std::any_cast<std::tuple_element_t<Is, Params>>(a[Is])...)};
                }
            };
        }
    } // namespace detail

    // ============================================================================
    // make_host_fn_descriptor<Name, Fn> — build a function_descriptor with both
    // the direct typed thunk and the legacy std::any trampoline.
    // Fn must be a non-capturing lambda or free function.
    // ============================================================================

    template <lithe::fixed_string Name, auto Fn>
    [[nodiscard]] function_descriptor make_host_fn_descriptor(
        function_options opts = {}) {
        using Traits = detail::callable_traits<decltype(Fn)>;
        static_assert(Traits::arity <= 8,
                      "make_host_fn_descriptor: arity > 8 not supported via zero-overhead path");

        function_descriptor d;
        d.name = std::string(Name.view());
        d.id = detail::make_id(Name.view(), kKindFunction);
        d.arity = Traits::arity;
        d.effects = opts.effects;
        d.capabilities = opts.capabilities;
        d.flags = opts.flags;
        d.boundary = opts.boundary;
        d.blocking = opts.blocking;
        d.cancellation = opts.cancellation;
        d.typed_thunk = detail::make_typed_thunk<Fn>();
        d.trampoline = detail::make_erased_invoke_impl<Fn, typename Traits::param_types>(
            std::make_index_sequence < Traits::arity >
        {}
        )
        ;
        // Fingerprint: fold name_hash + arity + flags.
        d.fingerprint = detail::fp_combine(d.id.name_hash,
                                           detail::fp_combine(d.arity, d.flags));
        return d;
    }

    template <class R, class... Args>
        requires (!std::is_void_v<R>)
    [[nodiscard]] std::optional<R> invoke_typed(const function_descriptor& desc, const Args&... args) {
        if (desc.typed_thunk == nullptr || desc.arity != sizeof...(Args)) {
            return std::nullopt;
        }
        std::array<const void*, sizeof...(Args)> raw_args{
            static_cast<const void*>(std::addressof(args))...
        };
        R out{};
        desc.typed_thunk(raw_args.data(), static_cast<void*>(&out));
        return out;
    }

    template <class... Args>
    [[nodiscard]] bool invoke_typed_void(const function_descriptor& desc, const Args&... args) {
        if (desc.typed_thunk == nullptr || desc.arity != sizeof...(Args)) {
            return false;
        }
        std::array<const void*, sizeof...(Args)> raw_args{
            static_cast<const void*>(std::addressof(args))...
        };
        desc.typed_thunk(raw_args.data(), nullptr);
        return true;
    }

    // Backward-compatible alias: existing code uses host_fn_descriptor.
    // Provided as a typedef so old tests and context.hpp continue to compile.
    using host_fn_descriptor = function_descriptor;

    // ============================================================================
    // is_injected_param<T> — detect parameters injected by runtime, not by Crank (§14.2)
    // Forward-declared here; specialised after cancellation.hpp types are visible.
    // ============================================================================

    namespace detail {
        template <class T>
        inline constexpr bool is_injected_param = false;
    } // namespace detail

    // ============================================================================
    // host_error — typed error crossing the host/crank boundary (§13.2)
    // ============================================================================

    struct host_error {
        std::uint32_t code = 0;
        std::string message;
        std::string category;
    };

    // ============================================================================
    // error_adapter<E> — user specialises to convert C++ exception type E (§13.2)
    // ============================================================================

    template <class E>
    struct error_adapter; // user specialises

    template <class E, class = void>
    inline constexpr bool has_error_adapter = false;

    template <class E>
    inline constexpr bool has_error_adapter<
        E, std::void_t<decltype(error_adapter<E>::convert(std::declval<const E&>()))>> = true;

    // ============================================================================
    // exception_boundary_policy — §13.4 (alias — also in source_boundary_policy)
    // Kept as a separate enum for clarity at call-sites that only care about exceptions.
    // ============================================================================

    using exception_boundary_policy = source_boundary_policy;

    // ============================================================================
    // layout_policy / ownership_policy / field_access (§7)
    // ============================================================================

    enum class layout_policy : std::uint8_t {
        opaque, // Crank can hold/pass but cannot inspect fields
        stable_native, // host asserts native layout is part of ABI
        crank_canonical, // serialization follows Crank rules
        gpu_compatible, // layout satisfies declared GPU address-space rules
    };

    enum class ownership_policy : std::uint8_t {
        value,
        shared,
        unique_owned,
        borrowed,
        pinned,
        host_managed,
    };

    enum class field_access : std::uint8_t {
        read_write,
        read_only,
        write_only,
        computed,
    };

    // type_category mirrors vakya::types categories but kept local to avoid coupling.
    enum class host_type_category : std::uint8_t {
        value, tensor, effect, capability, language, custom
    };

    // ============================================================================
    // type_operation_table — populated from C++ traits (§8)
    // ============================================================================

    struct type_operation_table {
        void (*copy)(const void* src, void* dst) = nullptr;
        void (*move)(void* src, void* dst) = nullptr;
        void (*destroy)(void* ptr) = nullptr;
        bool (*equal)(const void* a, const void* b) = nullptr;
        std::size_t (*hash)(const void* ptr) = nullptr;
    };

    // ============================================================================
    // field_descriptor — runtime record for a registered type field (§7.3)
    // ============================================================================

    struct field_descriptor {
        stable_field_id id;
        std::string name;
        stable_type_id type;
        std::size_t offset = 0;
        std::size_t byte_size = 0;
        field_access access = field_access::read_write;
        bool mutable_ = true;
        bool nullable = false;
        bool transient = false;
    };

    // Legacy alias — existing code uses host_field_info.
    struct host_field_info {
        std::string name;
        std::size_t byte_offset = 0;
        std::size_t byte_size = 0;
    };

    // ============================================================================
    // host_type_descriptor — runtime type reflection record (§7.2)
    // ============================================================================

    struct host_type_descriptor {
        stable_type_id id;
        std::string name;
        std::size_t byte_size = 0;
        std::size_t alignment = 0;
        host_type_category category = host_type_category::value;
        ownership_policy ownership = ownership_policy::value;
        layout_policy layout = layout_policy::stable_native;
        std::vector<field_descriptor> fields;
        type_operation_table operations;
        descriptor_fingerprint fingerprint = 0;
    };

    // ============================================================================
    // detail::type_ops_for<T> — populate type_operation_table from C++ traits
    // ============================================================================

    namespace detail {
        template <class T>
        [[nodiscard]] type_operation_table type_ops_for() noexcept {
            type_operation_table ops;
            if constexpr (std::is_trivially_copyable_v<T>)
                ops.copy = [](const void* s, void* d) {
                    std::memcpy(d, s, sizeof(T));
                };
            else if constexpr (std::is_copy_constructible_v<T>)
                ops.copy = [](const void* s, void* d) {
                    ::new(d) T(*static_cast<const T*>(s));
                };

            if constexpr (std::is_move_constructible_v<T>)
                ops.move = [](void* s, void* d) {
                    ::new(d) T(std::move(*static_cast<T*>(s)));
                };

            if constexpr (std::is_destructible_v<T>)
                ops.destroy = [](void* p) {
                    static_cast<T*>(p)->~T();
                };

            if constexpr (requires(const T& a, const T& b) { { a == b } -> std::convertible_to<bool>; })
                ops.equal = [](const void* a, const void* b) -> bool {
                    return *static_cast<const T*>(a) == *static_cast<const T*>(b);
                };

            if constexpr (requires(const T& t) { std::hash<T>{}(t); })
                ops.hash = [](const void* p) -> std::size_t {
                    return std::hash<T>{}(*static_cast<const T*>(p));
                };
            return ops;
        }

        // Collect field_descriptor list from crank::type_descriptor<T> specialisation.
        template <class T, class Tuple, std::size_t... Is>
        void collect_field_descriptors(std::vector<field_descriptor>& out,
                                       std::index_sequence<Is...>) {
            (out.push_back([&]() -> field_descriptor {
                using F = std::tuple_element_t<Is, Tuple>;
                field_descriptor fd;
                fd.name = std::string(F::name.view());
                fd.id = make_id(F::name.view(), kKindField);
                // byte_size from the pointed-to member type.
                using MemberT = std::remove_reference_t<
                    decltype(std::declval<T>().*F::pointer)>;
                fd.byte_size = sizeof(MemberT);
                // Stable native layout: use offsetof-equivalent via pointer arithmetic.
                if constexpr (std::is_standard_layout_v<T>) {
                    T dummy{};
                    fd.offset = static_cast<std::size_t>(
                        reinterpret_cast<const char*>(&(dummy.*F::pointer)) -
                        reinterpret_cast<const char*>(&dummy));
                }
                return fd;
            }()), ...);
        }

        // Legacy helper for old collect_fields signature.
        template <class T, class Tuple, std::size_t... Is>
        void collect_fields(std::vector<host_field_info>& out, std::index_sequence<Is...>) {
            (out.push_back({
                std::string(std::tuple_element_t < Is, Tuple > ::name.view()),
                0u, 0u
            }), ...);
        }
    } // namespace detail

    // ============================================================================
    // make_host_type_descriptor<T> — build a host_type_descriptor from
    // a specialised crank::type_descriptor<T>.
    // ============================================================================

    template <class T>
        requires has_type_descriptor<T>
    [[nodiscard]] host_type_descriptor make_host_type_descriptor() {
        using Desc = crank::type_descriptor<T>;
        host_type_descriptor d;
        d.name = std::string(Desc::name);
        d.id = detail::make_id(Desc::name, kKindType);
        d.byte_size = sizeof(T);
        d.alignment = alignof(T);
        d.operations = detail::type_ops_for<T>();
        d.layout = std::is_standard_layout_v<T>
                       ? layout_policy::stable_native
                       : layout_policy::opaque;
        using Fields = std::decay_t<decltype(Desc::fields)>;
        detail::collect_field_descriptors<T, Fields>(
            d.fields, std::make_index_sequence<std::tuple_size_v<Fields>>{});
        // Fingerprint: fold type name hash + size + alignment.
        d.fingerprint = detail::fp_combine(d.id.name_hash,
                                           detail::fp_combine(d.byte_size, d.alignment));
        return d;
    }

    // ============================================================================
    // container_traits<C> — contiguous/resizable container layout
    // ============================================================================

    template <class C>
    struct container_traits; // user specialises; built-ins below

    // std::span<T>
    template <class T, std::size_t Extent>
    struct container_traits<std::span<T, Extent>> {
        static constexpr bool is_gpu_visible = false;
        static constexpr bool is_resizable = false;
        using element_type = T;
        static T* data(std::span<T, Extent>& c) noexcept { return c.data(); }
        static const T* data(const std::span<T, Extent>& c) noexcept { return c.data(); }
        static std::size_t size(const std::span<T, Extent>& c) noexcept { return c.size(); }
    };

    // std::vector<T>
    template <class T, class Alloc>
    struct container_traits<std::vector<T, Alloc>> {
        static constexpr bool is_gpu_visible = false;
        static constexpr bool is_resizable = true;
        using element_type = T;
        static T* data(std::vector<T, Alloc>& c) noexcept { return c.data(); }
        static const T* data(const std::vector<T, Alloc>& c) noexcept { return c.data(); }
        static std::size_t size(const std::vector<T, Alloc>& c) noexcept { return c.size(); }
    };

    // std::array<T, N>
    template <class T, std::size_t N>
    struct container_traits<std::array<T, N>> {
        static constexpr bool is_gpu_visible = false;
        static constexpr bool is_resizable = false;
        using element_type = T;
        static T* data(std::array<T, N>& c) noexcept { return c.data(); }
        static const T* data(const std::array<T, N>& c) noexcept { return c.data(); }
        static std::size_t size(const std::array<T, N>&) noexcept { return N; }
    };

    // std::string
    template <>
    struct container_traits<std::string> {
        static constexpr bool is_gpu_visible = false;
        static constexpr bool is_resizable = true;
        using element_type = char;
        static char* data(std::string& c) noexcept { return c.data(); }
        static const char* data(const std::string& c) noexcept { return c.data(); }
        static std::size_t size(const std::string& c) noexcept { return c.size(); }
    };

    // std::string_view
    template <>
    struct container_traits<std::string_view> {
        static constexpr bool is_gpu_visible = false;
        static constexpr bool is_resizable = false;
        using element_type = const char;
        static const char* data(std::string_view c) noexcept { return c.data(); }
        static std::size_t size(std::string_view c) noexcept { return c.size(); }
    };

    // std::optional<T> → Option[T]
    template <class T>
    struct container_traits<std::optional<T>> {
        static constexpr bool is_gpu_visible = false;
        static constexpr bool is_resizable = false;
        using element_type = T;

        static T* data(std::optional<T>& c) noexcept {
            return c.has_value() ? &*c : nullptr;
        }

        static const T* data(const std::optional<T>& c) noexcept {
            return c.has_value() ? &*c : nullptr;
        }

        static std::size_t size(const std::optional<T>& c) noexcept {
            return c.has_value() ? 1u : 0u;
        }
    };

    // std::expected<T,E> → Result[T,E]
    template <class T, class E>
    struct container_traits<std::expected<T, E>> {
        static constexpr bool is_gpu_visible = false;
        static constexpr bool is_resizable = false;
        using element_type = T;

        static T* data(std::expected<T, E>& c) noexcept {
            return c.has_value() ? &*c : nullptr;
        }

        static const T* data(const std::expected<T, E>& c) noexcept {
            return c.has_value() ? &*c : nullptr;
        }

        static std::size_t size(const std::expected<T, E>& c) noexcept {
            return c.has_value() ? 1u : 0u;
        }
    };

    // std::mdspan<T, Extents, Layout, Accessor>
    template <class T, class Extents, class Layout, class Accessor>
    struct container_traits<std::mdspan<T, Extents, Layout, Accessor>> {
        static constexpr bool is_gpu_visible = true;
        static constexpr bool is_resizable = false;
        using element_type = T;

        static T* data(std::mdspan<T, Extents, Layout, Accessor>& c) noexcept {
            return c.data_handle();
        }

        static const T* data(const std::mdspan<T, Extents, Layout, Accessor>& c) noexcept {
            return c.data_handle();
        }

        static std::size_t size(const std::mdspan<T, Extents, Layout, Accessor>& c) noexcept {
            return c.size();
        }
    };

    // ============================================================================
    // has_container_traits<C>
    // ============================================================================

    template <class C, class = void>
    inline constexpr bool has_container_traits = false;

    template <class C>
    inline constexpr bool has_container_traits<
        C, std::void_t < typename container_traits<C>::element_type>
    >
    =
    true;

    // ============================================================================
    // Container capability concepts (§9.1)
    // ============================================================================

    template <class C>
    concept IndexableContainer = requires(C& c, std::size_t i) { c[i]; };

    template <class C>
    concept ResizableContainer = requires(C& c, std::size_t n) { c.resize(n); };

    // ============================================================================
    // container_cap / container_capabilities (§9.2)
    // ============================================================================

    enum class container_cap : std::uint32_t {
        sized = 1u << 0,
        indexed = 1u << 1,
        mutable_ = 1u << 2,
        resizable = 1u << 3,
        contiguous = 1u << 4,
        strided = 1u << 5,
        associative = 1u << 6,
        ordered = 1u << 7,
        thread_safe = 1u << 8,
        gpu_visible = 1u << 9,
        transactional = 1u << 10,
        borrowed_view = 1u << 11,
    };

    using container_capabilities = std::uint32_t;

    // ============================================================================
    // container_descriptor — full descriptor for a registered host container (§9.2)
    // ============================================================================

    struct container_descriptor {
        stable_type_id container_type;
        stable_type_id element_type;
        std::string name;
        container_capabilities capabilities = 0;
        std::optional<stable_type_id> key_type;
        std::optional<stable_type_id> mapped_type;
        // Legacy fields preserved for existing accessors.
        bool is_gpu_visible = false;
        bool is_resizable = false;
        std::size_t element_size = 0;
        descriptor_fingerprint fingerprint = 0;
    };

    // Legacy alias.
    using host_container_descriptor = container_descriptor;

    namespace detail {
        template <class C>
        [[nodiscard]] container_capabilities compute_capabilities() noexcept {
            container_capabilities caps = static_cast<std::uint32_t>(container_cap::sized);
            if constexpr (has_container_traits<C>) {
                using CT = container_traits<C>;
                if (CT::is_gpu_visible)
                    caps |= static_cast<std::uint32_t>(container_cap::gpu_visible);
                if (CT::is_resizable)
                    caps |= static_cast<std::uint32_t>(container_cap::resizable)
                        | static_cast<std::uint32_t>(container_cap::mutable_);
                // std::span and string_view are borrowed views.
                if (!CT::is_resizable && !CT::is_gpu_visible)
                    caps |= static_cast<std::uint32_t>(container_cap::borrowed_view);
            }
            if constexpr (IndexableContainer<C>)
                caps |= static_cast<std::uint32_t>(container_cap::indexed);
            if constexpr (ResizableContainer<C>)
                caps |= static_cast<std::uint32_t>(container_cap::resizable)
                    | static_cast<std::uint32_t>(container_cap::mutable_);
            // Detect contiguous: data() + size() with pointer arithmetic.
            if constexpr (requires(C& c) { c.data(); c.size(); })
                caps |= static_cast<std::uint32_t>(container_cap::contiguous);
            return caps;
        }
    } // namespace detail

    template <class C>
        requires has_container_traits<C>
    [[nodiscard]] container_descriptor make_host_container_descriptor(std::string name) {
        using CT = container_traits<C>;
        container_descriptor d;
        d.name = std::move(name);
        d.is_gpu_visible = CT::is_gpu_visible;
        d.is_resizable = CT::is_resizable;
        d.element_size = sizeof(typename CT::element_type);
        d.capabilities = detail::compute_capabilities<C>();
        d.container_type = detail::make_id(d.name, kKindContainer);
        d.fingerprint = detail::fp_combine(d.container_type.name_hash, d.capabilities);
        return d;
    }

    // ============================================================================
    // resource_lifetime / resource_threading (§10)
    // ============================================================================

    enum class resource_lifetime : std::uint8_t {
        context, // lives as long as the finalized_context
        runtime, // lives as long as the runtime_instance
        request, // per-execution-request scope
        scoped, // RAII scope — released explicitly
    };

    enum class resource_threading : std::uint8_t {
        single_thread,
        thread_affine,
        externally_synchronized,
        concurrent,
    };

    // resource_options — options for register_resource<R>()
    struct resource_options {
        resource_lifetime lifetime = resource_lifetime::runtime;
        resource_threading threading = resource_threading::concurrent;
        capability_mask required_caps = 0;
    };

    // ============================================================================
    // transactional_resource_descriptor (preserved from v1 for Medha integration)
    // ============================================================================

    struct transactional_resource_descriptor {
        std::string name;
        bool is_transactional = false;
        bool supports_snapshot = false;
        bool aba_safe = true;
        std::uint8_t commit_protocol_ordinal = 0;
        std::uint64_t traits_hash = 0;
        bool value_trivially_copyable = false;
        bool value_move_only = false;
        bool resource_stages_values = false;
        bool supports_rollback = false;
    };

    // ============================================================================
    // resource_descriptor — runtime record for a registered host resource (§10.1)
    // ============================================================================

    struct resource_descriptor {
        stable_resource_id id;
        std::string name;
        stable_type_id resource_type;
        resource_lifetime lifetime = resource_lifetime::runtime;
        resource_threading threading = resource_threading::concurrent;
        capability_mask required_caps = 0;
        bool transactional = false;
        std::optional<transactional_resource_descriptor> tx;
        // Factory: returns heap-allocated instance (nullptr = use bound reference).
        std::function<void*()> factory;
        void (*destroy_fn)(void*) = nullptr;
        descriptor_fingerprint fingerprint = 0;
    };

    // ============================================================================
    // register_transactional<R>("Name") — register a C++ transactional resource.
    // Returns a transactional_resource_descriptor (Medha integration, v1 compat).
    // ============================================================================

    template <class R>
        requires (medha::resource_traits<R>::transactional)
    [[nodiscard]] transactional_resource_descriptor
    register_transactional(std::string_view name) {
        using RT = medha::resource_traits<R>;
        transactional_resource_descriptor d;
        d.name = std::string(name);
        d.is_transactional = RT::transactional;
        d.supports_snapshot = RT::supports_snapshot;
        d.aba_safe = RT::aba_safe;
        d.commit_protocol_ordinal = static_cast<std::uint8_t>(RT::commit_protocol);
        d.traits_hash = 0; // caller fills via extend_aot_key_with_resource<R>
        d.value_trivially_copyable = RT::value_trivially_copyable;
        d.value_move_only = RT::value_move_only;
        d.resource_stages_values = RT::resource_stages_values;
        d.supports_rollback = RT::supports_rollback;
        return d;
    }

    // ============================================================================
    // coordinator_descriptor — named cross-resource commit coordinator (§v2.11)
    // ============================================================================

    struct coordinator_descriptor {
        std::string name;
        std::vector<transactional_resource_descriptor> participants;

        [[nodiscard]] bool all_participants_transactional() const noexcept {
            for (const auto& p : participants)
                if (!p.is_transactional) return false;
            return true;
        }

        [[nodiscard]] std::uint32_t nontransactional_participant_count() const noexcept {
            std::uint32_t n = 0;
            for (const auto& p : participants)
                if (!p.is_transactional) ++n;
            return n;
        }

        coordinator_descriptor& enroll(transactional_resource_descriptor r) {
            participants.push_back(std::move(r));
            return *this;
        }
    };

    template <class C>
    concept multi_resource_coordinator = requires {
        { C::commit_protocol } -> std::convertible_to<medha::commit_capability>;
    };

    [[nodiscard]] inline coordinator_descriptor
    register_coordinator(std::string_view name) {
        coordinator_descriptor d;
        d.name = std::string(name);
        return d;
    }

    template <class C>
        requires multi_resource_coordinator<C>
    [[nodiscard]] coordinator_descriptor
    register_coordinator_for(std::string_view name) {
        return register_coordinator(name);
    }

    // ============================================================================
    // backend_kind / backend_limits / backend_descriptor (§11)
    // ============================================================================

    // host_backend_kind — execution family for a registered host backend.
    // Named host_backend_kind to avoid clash with capability.hpp's execution_kind.
    enum class host_backend_kind : std::uint8_t {
        interpreter,
        simd_cpu,
        gpu_metal,
        gpu_vulkan,
        custom,
    };

    struct host_backend_limits {
        std::uint32_t max_function_args = 256;
        std::uint32_t max_locals = 4096;
        std::uint64_t max_memory_bytes = 0; // 0 = unlimited
    };

    // host_backend_descriptor — identity + capabilities for a registered host backend (§11.2).
    // Named host_backend_descriptor to avoid clash with capability.hpp's backend_descriptor.
    struct host_backend_descriptor {
        host_backend_id id;
        std::string name;
        host_backend_kind kind = host_backend_kind::interpreter;
        std::uint32_t version = 1;
        capability_mask capabilities = 0;
        host_backend_limits limits;
        bool supports_async = false;
        bool supports_cancellation = false;
        bool supports_deadlines = false;
        bool supports_deterministic_reduction = false;
        descriptor_fingerprint fingerprint = 0;
    };

    // CrankBackend concept (§11.1) — forward-declares execution_context.
    // Full constraint verified at instantiation; kept minimal here to avoid
    // pulling execution.hpp into the host embedding header.
    namespace detail {
        struct execution_context_placeholder;
    }

    template <class B>
    concept HostBackend = requires {
        { B::descriptor() } -> std::same_as<host_backend_descriptor>;
    };

    // ============================================================================
    // observability_options — pay-for-use event sink (§15)
    // ============================================================================

    enum class event_kind : std::uint16_t {
        host_call_entry, host_call_exit,
        module_parse, module_analyse,
        backend_selected, backend_fallback,
        task_scheduled, task_cancelled,
        tx_retry, tx_commit, tx_abort,
        compilation_cached, artifact_loaded,
        runtime_failure,
    };

    enum class event_severity : std::uint8_t {
        trace, debug, info, warn, error_, fatal
    };

    enum class field_sensitivity : std::uint8_t {
        public_,
        internal,
        secret,
        personally_identifying,
    };

    struct runtime_event {
        event_kind kind;
        event_severity severity = event_severity::info;
        std::uint64_t timestamp_ns = 0;
        std::optional<std::string> module_name;
        std::optional<std::string> function_name;
        std::optional<std::string> backend_name;
    };

    template <class S>
    concept EventSink = requires(S& sink, const runtime_event& ev) {
        sink.on_event(ev);
    };

    struct observability_options {
        bool enabled = false;
        std::function<void(const runtime_event&)> sink; // type-erased; OK at boundary
    };

    // ============================================================================
    // lifetime_token — epoch for borrowed container/resource views (§9.5, §19.2)
    // ============================================================================

    struct lifetime_token {
        std::uint64_t epoch = 0;
    };

    // ============================================================================
    // host_value — borrowed reference to a typed host value (§16.1)
    // ============================================================================

    struct host_value {
        stable_type_id type;
        void* data = nullptr;
        const host_type_descriptor* descriptor = nullptr;
        lifetime_token lifetime;
    };

    // ============================================================================
    // detail::k_ops_for<T> — static per-type op-table instance (§8)
    // Avoids allocating a new op-table per owned_host_value instance.
    // ============================================================================

    namespace detail {
        template <class T>
        inline const type_operation_table k_ops_for = type_ops_for<T>();
    } // namespace detail

    // ============================================================================
    // owned_host_value — owned typed host value with SBO (§16.1)
    //
    // SBO threshold: 24 bytes inline. Larger values heap-allocate.
    // No virtual, no std::any.
    // ============================================================================

    class owned_host_value {
    public:
        static constexpr std::size_t kSBOSize = 24;

        owned_host_value() noexcept = default;

        template <class T>
        [[nodiscard]] static owned_host_value make(T&& val) {
            owned_host_value v;
            v.type_ = detail::make_id(
                std::string_view{}, kKindType); // type set by caller if needed
            if constexpr (sizeof(T) <= kSBOSize &&
                std::is_trivially_copyable_v<T>) {
                std::memcpy(v.sbo_, &val, sizeof(T));
                v.data_ = v.sbo_;
            }
            else {
                v.heap_ = ::operator new(sizeof(T));
                ::new(v.heap_) T(std::forward<T>(val));
                v.data_ = v.heap_;
            }
            v.ops_ = &detail::k_ops_for<std::decay_t<T>>;
            return v;
        }

        owned_host_value(const owned_host_value&) = delete;
        owned_host_value& operator=(const owned_host_value&) = delete;

        owned_host_value(owned_host_value&& o) noexcept
            : type_(o.type_), ops_(o.ops_), heap_(o.heap_) {
            if (o.data_ == o.sbo_) {
                std::memcpy(sbo_, o.sbo_, kSBOSize);
                data_ = sbo_;
            }
            else {
                data_ = heap_;
            }
            o.data_ = nullptr;
            o.heap_ = nullptr;
            o.ops_ = nullptr;
        }

        ~owned_host_value() {
            if (data_ && ops_ && ops_->destroy) ops_->destroy(data_);
            if (heap_) ::operator delete(heap_);
        }

        [[nodiscard]] bool has_value() const noexcept { return data_ != nullptr; }
        [[nodiscard]] void* get() noexcept { return data_; }
        [[nodiscard]] const void* get() const noexcept { return data_; }
        [[nodiscard]] stable_type_id type_id() const noexcept { return type_; }

    private:
        stable_type_id type_;
        alignas(std::max_align_t) std::byte sbo_[kSBOSize]{};
        void* data_ = nullptr;
        void* heap_ = nullptr;
        const type_operation_table* ops_ = nullptr;
    };

    // ============================================================================
    // dynamic_callable — controlled alternative to std::function (§16.2)
    // Stable signature metadata + explicit state ownership + no hidden exceptions.
    // ============================================================================

    struct dynamic_callable {
        function_descriptor descriptor;
        void* state = nullptr;
        void (*invoke)(void* state, const void* const* args, void* result) = nullptr;
        void (*destroy)(void* state) = nullptr;
    };

    // ============================================================================
    // build_diagnostic / build_result (§3.2)
    // ============================================================================

    struct build_diagnostic {
        enum class kind : std::uint8_t { error, warning } severity = kind::error;

        std::string message;
    };

    // Forward declare finalized_context before context_builder references it.
    class finalized_context;
    // host_build_result: named host_build_result to avoid clash with frontend.hpp's build_result.
    using host_build_result = std::expected<finalized_context, std::vector<build_diagnostic>>;

    // ============================================================================
    // finalized_context — immutable registry snapshot after finalization (§3.3)
    // ============================================================================

    class finalized_context {
    public:
        [[nodiscard]] const std::vector<function_descriptor>& functions() const noexcept { return functions_; }
        [[nodiscard]] const std::vector<host_type_descriptor>& types() const noexcept { return types_; }
        [[nodiscard]] const std::vector<container_descriptor>& containers() const noexcept { return containers_; }
        [[nodiscard]] const std::vector<resource_descriptor>& resources() const noexcept { return resources_; }
        [[nodiscard]] const std::vector<host_backend_descriptor>& backends() const noexcept { return backends_; }
        [[nodiscard]] const annotation_registry& annotations() const noexcept { return annotations_; }
        [[nodiscard]] descriptor_fingerprint fingerprint() const noexcept { return fingerprint_; }

        // Internal factory used by detail::finalize_builder.
        struct make_tag {};

        finalized_context(make_tag,
                          std::vector<function_descriptor> fns,
                          std::vector<host_type_descriptor> tys,
                          std::vector<container_descriptor> ctrs,
                          std::vector<resource_descriptor> res,
                          std::vector<host_backend_descriptor> bks,
                          annotation_registry ann,
                          std::unordered_set<std::string> coords,
                          descriptor_fingerprint fp)
            : functions_(std::move(fns))
              , types_(std::move(tys))
              , containers_(std::move(ctrs))
              , resources_(std::move(res))
              , backends_(std::move(bks))
              , annotations_(std::move(ann))
              , coordinators_(std::move(coords))
              , fingerprint_(fp) {}

    private:
        std::vector<function_descriptor> functions_;
        std::vector<host_type_descriptor> types_;
        std::vector<container_descriptor> containers_;
        std::vector<resource_descriptor> resources_;
        std::vector<host_backend_descriptor> backends_;
        annotation_registry annotations_;
        std::unordered_set<std::string> coordinators_;
        descriptor_fingerprint fingerprint_ = 0;
    };

    // ============================================================================
    // detail::finalize_builder — §18 finalization algorithm
    // ============================================================================

    namespace detail {
        [[nodiscard]] inline host_build_result finalize_builder(
            std::vector<function_descriptor> fns,
            std::vector<host_type_descriptor> types,
            std::vector<container_descriptor> containers,
            std::vector<resource_descriptor> resources,
            std::vector<host_backend_descriptor> backends,
            annotation_registry ann_reg,
            std::unordered_set<std::string> coordinators) {
            std::vector<build_diagnostic> diags;

            // Step 3: detect stable-name collisions among functions.
            std::unordered_map<std::string, std::size_t> fn_name_count;
            for (auto& fn : fns) fn_name_count[fn.name]++;
            for (auto& [name, count] : fn_name_count) {
                if (count > 1) {
                    // Duplicate: check if overloads have distinct parameter types.
                    // Collect all with this name.
                    std::vector<function_descriptor*> group;
                    for (auto& fn : fns)
                        if (fn.name == name) group.push_back(&fn);
                    // If any two have identical parameter_types → collision error.
                    for (std::size_t i = 0; i < group.size(); ++i) {
                        for (std::size_t j = i + 1; j < group.size(); ++j) {
                            if (group[i]->parameter_types == group[j]->parameter_types) {
                                diags.push_back({
                                    build_diagnostic::kind::error,
                                    "duplicate function registration: '" + name + "'"
                                });
                            }
                        }
                    }
                }
            }

            // Step 3: detect stable-name collisions among types.
            {
                std::unordered_map<std::string, std::size_t> cnt;
                for (auto& t : types) cnt[t.name]++;
                for (auto& [nm, c] : cnt)
                    if (c > 1)
                        diags.push_back({
                            build_diagnostic::kind::error,
                            "duplicate type registration: '" + nm + "'"
                        });
            }

            // Step 3: detect stable-name collisions among resources.
            {
                std::unordered_map<std::string, std::size_t> cnt;
                for (auto& r : resources) cnt[r.name]++;
                for (auto& [nm, c] : cnt)
                    if (c > 1)
                        diags.push_back({
                            build_diagnostic::kind::error,
                            "duplicate resource registration: '" + nm + "'"
                        });
            }

            if (!diags.empty()) return std::unexpected<std::vector<build_diagnostic>>(std::move(diags));

            // Steps 13–15: compute registry fingerprint.
            descriptor_fingerprint reg_fp = 0;
            for (const auto& fn : fns) reg_fp = fp_combine(reg_fp, fn.fingerprint);
            for (const auto& t : types) reg_fp = fp_combine(reg_fp, t.fingerprint);
            for (const auto& c : containers) reg_fp = fp_combine(reg_fp, c.fingerprint);
            for (const auto& r : resources) reg_fp = fp_combine(reg_fp, r.fingerprint);
            for (const auto& b : backends) reg_fp = fp_combine(reg_fp, b.fingerprint);

            return finalized_context{
                finalized_context::make_tag{},
                std::move(fns),
                std::move(types),
                std::move(containers),
                std::move(resources),
                std::move(backends),
                std::move(ann_reg),
                std::move(coordinators),
                reg_fp
            };
        }
    } // namespace detail

    // ============================================================================
    // context_builder — mutable builder before finalization (§3.1)
    // ============================================================================

    class context_builder {
    public:
        context_builder()
            : annotation_registry_(make_crank_annotation_registry()) {}

        // ── function registration ─────────────────────────────────────────────────

        template <lithe::fixed_string Name, auto Fn>
        context_builder& register_function(function_options opts = {}) {
            pending_functions_.push_back(make_host_fn_descriptor<Name, Fn>(opts));
            return *this;
        }

        template <class C>
            requires HostCallable<C>
        context_builder& register_callable(std::string name, C&& callable,
                                           function_options opts = {}) {
            function_descriptor d;
            d.name = name;
            d.id = detail::make_id(name, kKindFunction);
            d.effects = opts.effects;
            d.capabilities = opts.capabilities;
            d.flags = opts.flags;
            d.boundary = opts.boundary;
            d.blocking = opts.blocking;
            d.cancellation = opts.cancellation;
            // Capturing callable — erased invoke only, no direct typed thunk.
            d.trampoline = [c = std::forward<C>(callable)](
                std::span<const std::any> args) -> std::any {
                    using Traits = detail::callable_traits<std::decay_t<C>>;
                    static_assert(Traits::arity <= 8,
                                  "register_callable: arity > 8 not supported");
                    return [&]<std::size_t... Is>(std::index_sequence<Is...>) -> std::any {
                        return std::any{
                            c(std::any_cast<
                                std::tuple_element_t<Is, typename Traits::param_types>>(args[Is])...)
                        };
                    }(std::make_index_sequence < Traits::arity >
                    {}
                    )
                    ;
                };
            d.arity = detail::callable_traits<std::decay_t<C>>::arity;
            d.fingerprint = detail::fp_combine(d.id.name_hash, d.arity);
            pending_functions_.push_back(std::move(d));
            return *this;
        }

        // ── type registration ─────────────────────────────────────────────────────

        template <class T>
            requires has_type_descriptor<T>
        context_builder& register_type() {
            pending_types_.push_back(make_host_type_descriptor<T>());
            return *this;
        }

        // ── container registration ────────────────────────────────────────────────

        template <class C>
            requires has_container_traits<C>
        context_builder& register_container(std::string name) {
            pending_containers_.push_back(make_host_container_descriptor<C>(std::move(name)));
            return *this;
        }

        // ── resource registration ─────────────────────────────────────────────────

        template <class R>
        context_builder& register_resource(std::string name, resource_options opts = {}) {
            resource_descriptor d;
            d.name = name;
            d.id = detail::make_id(name, kKindResource);
            d.lifetime = opts.lifetime;
            d.threading = opts.threading;
            d.required_caps = opts.required_caps;
            if constexpr (requires { medha::resource_traits<R>::transactional; }) {
                if constexpr (medha::resource_traits<R>::transactional) {
                    d.transactional = true;
                    d.tx = register_transactional<R>(name);
                }
            }
            d.fingerprint = detail::fp_combine(d.id.name_hash, d.id.kind);
            pending_resources_.push_back(std::move(d));
            return *this;
        }

        context_builder& bind_resource_borrowed(std::string name, void* ptr,
                                                resource_options opts = {}) {
            resource_descriptor d;
            d.name = std::move(name);
            d.id = detail::make_id(d.name, kKindResource);
            d.lifetime = opts.lifetime;
            d.threading = opts.threading;
            // Borrowed: factory returns the existing pointer; no destroy.
            d.factory = [ptr]() -> void* { return ptr; };
            d.destroy_fn = nullptr;
            d.fingerprint = detail::fp_combine(d.id.name_hash, d.id.kind);
            pending_resources_.push_back(std::move(d));
            return *this;
        }

        // ── backend registration ──────────────────────────────────────────────────

        template <class B>
            requires HostBackend<B>
        context_builder& register_backend() {
            pending_backends_.push_back(B::descriptor());
            return *this;
        }

        // ── annotation registration ───────────────────────────────────────────────

        template <lithe::fixed_string Name, class Schema>
        context_builder& register_annotation(annotation_kind kind,
                                             annotation_strength strength,
                                             std::uint32_t stable_id,
                                             effect_mask effects = 0,
                                             capability_mask caps = 0) {
            constexpr auto fields = crank::detail::flatten_schema<Schema>::make();
            annotation_descriptor d;
            d.name = std::string_view{Name.data(), Name.size() - 1};
            d.kind = kind;
            d.default_strength = strength;
            d.effects = effects;
            d.capabilities = caps;
            d.stable_id = stable_id;
            d.version = 1;
            d.name_hash = containers::desc_name_hash(d.name);
            annotation_registry_.register_desc(d, std::span<const schema_field>{fields});
            return *this;
        }

        template <CrankExtension E>
        context_builder& install_extension(E&& ext) {
            crank::install_extension(annotation_registry_, std::forward < E > (ext));
            return *this;
        }

        // ── coordinators ─────────────────────────────────────────────────────────

        context_builder& register_coordinator_name(std::string name) {
            coordinators_.insert(std::move(name));
            return *this;
        }

        [[nodiscard]] bool has_coordinator(std::string_view name) const {
            return coordinators_.find(std::string(name)) != coordinators_.end();
        }

        // ── finalize ──────────────────────────────────────────────────────────────

        [[nodiscard]] host_build_result finalize() && {
            return detail::finalize_builder(
                std::move(pending_functions_),
                std::move(pending_types_),
                std::move(pending_containers_),
                std::move(pending_resources_),
                std::move(pending_backends_),
                std::move(annotation_registry_),
                std::move(coordinators_));
        }

        // ── read-back (for context backward-compat) ───────────────────────────────

        [[nodiscard]] const std::vector<function_descriptor>& pending_functions() const noexcept {
            return pending_functions_;
        }

        [[nodiscard]] const std::vector<host_type_descriptor>& pending_types() const noexcept { return pending_types_; }

        [[nodiscard]] const std::vector<container_descriptor>& pending_containers() const noexcept {
            return pending_containers_;
        }

        [[nodiscard]] const std::vector<resource_descriptor>& pending_resources() const noexcept {
            return pending_resources_;
        }

        [[nodiscard]] annotation_registry& annotations() noexcept { return annotation_registry_; }
        [[nodiscard]] const annotation_registry& annotations() const noexcept { return annotation_registry_; }
        [[nodiscard]] const std::unordered_set<std::string>& coordinators() const noexcept { return coordinators_; }

    private:
        std::vector<function_descriptor> pending_functions_;
        std::vector<host_type_descriptor> pending_types_;
        std::vector<container_descriptor> pending_containers_;
        std::vector<resource_descriptor> pending_resources_;
        std::vector<host_backend_descriptor> pending_backends_;
        annotation_registry annotation_registry_;
        std::unordered_set<std::string> coordinators_;
    };
} // namespace crank
