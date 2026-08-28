#pragma once

#include "lithe_core.hpp"

namespace lithe {
    // =========================================================================
    // fixed_string — structural NTTP string for use as template argument.
    //
    // Satisfies the C++20 structural type requirement: all members are public,
    // the type is literal, and equality is value-based.  Strings of different
    // lengths are different types.
    //
    // Usage:
    //   template<fixed_string Name> struct my_tag {};
    //   using my_add = my_tag<"add">;
    // =========================================================================

    template <std::size_t N>
    struct fixed_string {
        char data[N]{};

        // Implicit construction from a string literal of exactly the right size.
        consteval fixed_string(const char (&src)[N]) noexcept {
            for (std::size_t i = 0; i < N; ++i) data[i] = src[i];
        }

        [[nodiscard]] constexpr std::string_view view() const noexcept {
            return {data, N - 1}; // exclude null terminator
        }

        [[nodiscard]] constexpr const char* c_str() const noexcept { return data; }

        [[nodiscard]] constexpr std::size_t size() const noexcept { return N - 1; }

        constexpr bool operator==(const fixed_string&) const noexcept = default;

        template <std::size_t M>
        constexpr bool operator==(const fixed_string<M>&) const noexcept { return false; }
    };

    // Deduction guide: fixed_string{"add"} deduces fixed_string<4>.
    template <std::size_t N>
    fixed_string(const char (&)[N]) -> fixed_string<N>;

    // =========================================================================
    // version_triple — compile-time semantic version (major.minor.patch).
    //
    // Fully structural: usable as an NTTP.  Comparison is lexicographic on
    // the three unsigned fields, matching semver ordering semantics.
    //
    // Usage:
    //   static constexpr version_triple version{1, 2, 0};
    // =========================================================================

    struct version_triple {
        std::uint32_t major = 0;
        std::uint32_t minor = 0;
        std::uint32_t patch = 0;

        constexpr bool operator==(const version_triple&) const noexcept = default;

        [[nodiscard]] constexpr bool operator<(const version_triple& o) const noexcept {
            if (major != o.major) return major < o.major;
            if (minor != o.minor) return minor < o.minor;
            return patch < o.patch;
        }

        [[nodiscard]] constexpr bool operator<=(const version_triple& o) const noexcept {
            return !(o < *this);
        }
    };

    // =========================================================================
    // plugin_descriptor — static constexpr identity block for any Lithe
    // extension (pass, backend, analysis, etc.).
    //
    // Structural type: all fields are literal / fixed_string so a
    // plugin_descriptor value may itself be used as an NTTP.
    //
    // Fields:
    //   id            — unique reverse-DNS style identifier, e.g. "lithe.opt.dce"
    //   version       — semantic version of this plugin
    //   author        — free-form author/owner string
    //   domain        — OR-combined semantic::domain_type bits this plugin
    //                   operates on (e.g. arithmetic | tensor)
    //
    // Usage:
    //   struct my_pass {
    //       static constexpr lithe::plugin_descriptor descriptor{
    //           .id      = "acme.passes.my_pass",
    //           .version = {1, 0, 0},
    //           .author  = "Acme Corp",
    //           .domain  = lithe::semantic::domain_type::arithmetic,
    //       };
    //       // ... pass implementation ...
    //   };
    // =========================================================================

    // Forward-declare domain_type to avoid a hard dependency on lithe_semantic.hpp.
    // lithe_extension.hpp is included before lithe_semantic.hpp, so the full enum
    // definition is not yet visible here.  Callers that need domain-aware queries
    // (has_domain, operator|, etc.) must include lithe_semantic.hpp separately.
    namespace semantic {
        enum class domain_type : std::uint16_t;
    }

    template <std::size_t IdN, std::size_t AuthorN>
    struct plugin_descriptor {
        fixed_string<IdN> id;
        version_triple version{};
        fixed_string<AuthorN> author;
        semantic::domain_type domain{};

        constexpr bool operator==(const plugin_descriptor&) const noexcept = default;

        [[nodiscard]] constexpr std::string_view id_view() const noexcept { return id.view(); }
        [[nodiscard]] constexpr std::string_view author_view() const noexcept { return author.view(); }
    };

    // Deduction guide: plugin_descriptor{"lithe.opt.dce", {1,0,0}, "Lithe"}.
    template <std::size_t IdN, std::size_t AuthorN>
    plugin_descriptor(const char (&)[IdN], version_triple, const char (&)[AuthorN])
        -> plugin_descriptor<IdN, AuthorN>;

    // =========================================================================
    // LitheExtension concept — the single gate-check every Lithe plugin
    // (pass, backend, analysis) must satisfy.
    //
    // A type T models LitheExtension iff it exposes:
    //   static constexpr <some plugin_descriptor specialisation> descriptor;
    //
    // The descriptor type is intentionally unconstrained beyond being a
    // plugin_descriptor specialisation so that callers can choose any
    // (IdN, AuthorN) combination without the concept hardcoding sizes.
    //
    // Usage:
    //   static_assert(lithe::LitheExtension<my_pass>);
    // =========================================================================

    template <class T>
    concept LitheExtension =
        // descriptor must be a static constexpr member accessible without an instance
        requires {
            typename std::remove_cvref_t<decltype(T::descriptor)>;
        } &&
        // descriptor type must expose all required plugin_descriptor members/methods
        requires(const decltype(T::descriptor)& d) {
            d.id;
            d.version;
            d.author;
            d.domain;
            { d.id_view() } -> std::same_as<std::string_view>;
            { d.author_view() } -> std::same_as<std::string_view>;
            { d.version } -> std::convertible_to<version_triple>;
        };

    // =========================================================================
    // DSL Extension Infrastructure
    // =========================================================================
    namespace dsl_extension {
        // ---------------------------------------------------------------------
        // tag_registry — constexpr metadata for operation tags.
        //
        // Old interface (macro-based):
        //   LITHE_REGISTER_TAG(MyTag, "my", 5, true, false, 2)
        //
        // New interface (NTTP structural):
        //   using my_tag = extension_tag<"my">;
        //   // metadata comes from the descriptor registered in an
        //   // extension_registry, or from a custom_tag_info<> specialization.
        // ---------------------------------------------------------------------
        namespace tag_registry {
            struct tag_metadata {
                const char* name;
                int precedence;
                bool is_associative;
                bool is_commutative;
                std::size_t arity;

                constexpr explicit tag_metadata(
                    const char* n,
                    const int prec = 0,
                    const bool assoc = false,
                    const bool comm = false,
                    const std::size_t ar = 2
                ) noexcept
                    : name(n), precedence(prec), is_associative(assoc),
                      is_commutative(comm), arity(ar) {}
            };

            // Primary template: falls back to a "custom" stub.
            // Specialize this for any Tag type that is not an extension_tag<>
            // but still needs override metadata.
            template <class Tag>
            struct custom_tag_info {
                static constexpr tag_metadata metadata{"custom", 0, false, false, 2};
            };

            // Query helpers — unchanged semantics, unchanged call sites.

            template <class Tag>
            constexpr const char* get_tag_name() {
                if constexpr (requires { custom_tag_info<Tag>::metadata.name; }) {
                    return custom_tag_info<Tag>::metadata.name;
                }
                else {
                    return emit::tag_name<Tag>::value;
                }
            }

            template <class Tag>
            constexpr int get_precedence() {
                if constexpr (requires { custom_tag_info<Tag>::metadata.precedence; }) {
                    return custom_tag_info<Tag>::metadata.precedence;
                }
                else {
                    return 0;
                }
            }

            template <class Tag>
            constexpr bool is_associative() {
                if constexpr (requires { custom_tag_info<Tag>::metadata.is_associative; }) {
                    return custom_tag_info<Tag>::metadata.is_associative;
                }
                else {
                    return false;
                }
            }

            template <class Tag>
            constexpr bool is_commutative() {
                if constexpr (requires { custom_tag_info<Tag>::metadata.is_commutative; }) {
                    return custom_tag_info<Tag>::metadata.is_commutative;
                }
                else if constexpr (std::is_same_v<Tag, add_tag> || std::is_same_v<Tag, mul_tag>) {
                    return true;
                }
                else {
                    return false;
                }
            }
        } // namespace tag_registry

        // =====================================================================
        // extension_tag<Name> — structural tag type identified by a fixed_string
        // NTTP.
        //
        // Satisfies all requirements of a lithe tag type:
        //   - default-constructible
        //   - structural (all data encoded in the type, not in instances)
        //
        // Usage:
        //   using my_pow_tag = extension_tag<"pow">;
        //   auto expr = make_node<my_pow_tag>(base, exp);
        //
        // The tag's name is the view() of the fixed_string; no macro needed.
        //
        // custom_tag_info<extension_tag<Name>> delegates to extension_tag_traits<Name>
        // (see below).  Override traits to supply precedence, associativity, etc.
        // =====================================================================

        template <fixed_string Name>
        struct extension_tag {
            static constexpr auto tag_name = Name;

            constexpr bool operator==(const extension_tag&) const noexcept = default;
        };

        // =====================================================================
        // extension_descriptor<Name, Metadata> — constexpr descriptor bundling
        // a structural name with its full metadata.
        //
        // Metadata must be default-constructible; its type is part of the
        // descriptor type so registries can carry heterogeneous metadata.
        //
        // Usage:
        //   struct pow_meta { int precedence = 8; bool is_associative = false; };
        //   using pow_desc = extension_descriptor<"pow", pow_meta>;
        // =====================================================================

        template <fixed_string Name, class Metadata = tag_registry::tag_metadata>
        struct extension_descriptor {
            static constexpr auto name = Name;
            using metadata_type = Metadata;

            Metadata metadata{};

            constexpr explicit extension_descriptor(Metadata m = {}) noexcept
                : metadata(std::move(m)) {}

            [[nodiscard]] constexpr std::string_view name_view() const noexcept {
                return Name.view();
            }
        };

        // Specialization for the common case where Metadata IS tag_metadata:
        // allow construction from individual fields for ergonomic registration.
        template <fixed_string Name>
        struct extension_descriptor<Name, tag_registry::tag_metadata> {
            static constexpr auto name = Name;
            using metadata_type = tag_registry::tag_metadata;

            tag_registry::tag_metadata metadata;

            constexpr explicit extension_descriptor(
                const int precedence = 0,
                const bool is_associative = false,
                const bool is_commutative = false,
                const std::size_t arity = 2
            ) noexcept
                : metadata{Name.c_str(), precedence, is_associative, is_commutative, arity} {}

            constexpr explicit extension_descriptor(tag_registry::tag_metadata m) noexcept
                : metadata(m) {}

            [[nodiscard]] constexpr std::string_view name_view() const noexcept {
                return Name.view();
            }
        };

        // =====================================================================
        // extension_registry<Descriptors...> — constexpr ordered registry of
        // extension_descriptor instantiations.
        //
        // Provides compile-time lookup by name (fixed_string NTTP) and
        // index-based access.  Registering a descriptor automatically wires
        // custom_tag_info for the corresponding extension_tag<Name>.
        //
        // Usage:
        //   inline constexpr auto my_registry = make_extension_registry(
        //       extension_descriptor<"pow">{8, false, false, 2},
        //       extension_descriptor<"min">{0, false, true,  2}
        //   );
        //
        //   // Lookup at compile time:
        //   constexpr auto &d = my_registry.get<"pow">();
        //   static_assert(d.metadata.precedence == 8);
        //
        //   // After registering, the query helpers reflect the metadata:
        //   static_assert(tag_registry::get_precedence<extension_tag<"pow">>() == 8);
        //
        // Registering a descriptor with the same Name as a previous one in the
        // same registry is a compile-time error (duplicate detected via
        // static_assert).
        // =====================================================================

        namespace detail {
            // Cleaner: iterate at compile time via fold.
            template <fixed_string Target, std::size_t I, class Head, class... Tail>
            struct find_idx_impl {
                static constexpr std::size_t value =
                    (Head::name == Target)
                        ? I
                        : find_idx_impl<Target, I + 1, Tail...>::value;
            };

            template <fixed_string Target, std::size_t I, class Head>
            struct find_idx_impl<Target, I, Head> {
                static constexpr std::size_t value =
                    (Head::name == Target) ? I : static_cast<std::size_t>(-1);
            };

            template <fixed_string Target, class... Ds>
            inline constexpr std::size_t descriptor_index_v =
                find_idx_impl<Target, 0, Ds...>::value;

            // Uniqueness check: all Names in Ds... are distinct.
            template <class... Ds>
            inline constexpr bool all_names_unique_v = true; // specialised below

            template <class D0, class D1, class... Rest>
            inline constexpr bool all_names_unique_v<D0, D1, Rest...> =
                (D0::name != D1::name) &&
                all_names_unique_v<D0, Rest...> &&
                all_names_unique_v<D1, Rest...>;
        } // namespace detail

        template <class... Descriptors>
        struct extension_registry {
            static constexpr std::size_t size = sizeof...(Descriptors);

            std::tuple<Descriptors...> entries;

            constexpr explicit extension_registry(Descriptors... ds) noexcept
                : entries(std::move(ds)...) {
                static_assert(
                    detail::all_names_unique_v<Descriptors...>,
                    "extension_registry: duplicate descriptor names detected"
                );
            }

            // Index-based access.
            template <std::size_t I>
            [[nodiscard]] constexpr auto const& at() const noexcept {
                return std::get < I > (entries);
            }

            // Name-based access: get<"pow">() returns the matching descriptor.
            // Compile-time error if Name is not registered.
            template <fixed_string Name>
            [[nodiscard]] constexpr auto const& get() const noexcept {
                constexpr std::size_t idx =
                    detail::descriptor_index_v<Name, Descriptors...>;
                static_assert(
                    idx != static_cast<std::size_t>(-1),
                    "extension_registry::get<Name>(): name not found in registry"
                );
                return std::get < idx > (entries);
            }

            // Predicate: returns true if Name is registered.
            template <fixed_string Name>
            [[nodiscard]] static constexpr bool contains() noexcept {
                return detail::descriptor_index_v<Name, Descriptors...>
                    != static_cast<std::size_t>(-1);
            }
        };

        // Factory function — deduces Descriptors... from arguments.
        template <class... Descriptors>
        [[nodiscard]] constexpr auto make_extension_registry(Descriptors&&... ds) noexcept {
            return extension_registry<std::decay_t<Descriptors>...>(
                std::forward<Descriptors>(ds)...
            );
        }

        // =====================================================================
        // Wire extension_registry entries into custom_tag_info.
        //
        // When a descriptor is added to a registry, its metadata should be
        // reflected by the tag_registry query helpers.  Because registries are
        // values (not types), the per-descriptor wiring is done by specializing
        // custom_tag_info<extension_tag<Name>> at the point of descriptor
        // construction.  The automatic specialization (above) uses zero-defaults;
        // callers can override by providing a full-metadata descriptor whose
        // values replace those defaults.
        //
        // override_tag_metadata<Name>(descriptor) explicitly writes the
        // descriptor metadata back into the custom_tag_info specialization.
        // Since custom_tag_info is a template and C++ does not allow runtime
        // specialization, the real binding is done via constexpr_tag_binding<>
        // described below.
        //
        // constexpr_tag_binding<Name, Desc>: a structural type that carries a
        // descriptor as a constexpr static member.  Specializing
        // custom_tag_info<extension_tag<Name>> to delegate to a binding enables
        // arbitrary metadata to reach the query helpers without macros.
        //
        // Usage:
        //   // 1. Define descriptor with full metadata.
        //   inline constexpr auto pow_desc =
        //       extension_descriptor<"pow">{8, false, false, 2};
        //
        //   // 2. Specialize extension_tag_traits to supply full metadata:
        //   template<> struct lithe::dsl_extension::extension_tag_traits<"pow"> {
        //       static constexpr int  precedence     = 8;
        //       static constexpr bool is_associative = false;
        //       static constexpr bool is_commutative = false;
        //       static constexpr std::size_t arity   = 2;
        //   };
        //
        // Since "no macros" is the goal, the preferred idiom uses
        // extension_registry and calls register_extension<Name>(registry) which
        // does the binding at compile time for every entry.  See below.
        // =====================================================================

        // constexpr_tag_binding<Name, meta> — structural holder for per-name
        // metadata that drives custom_tag_info.
        template <fixed_string Name, tag_registry::tag_metadata Meta>
        struct constexpr_tag_binding {
            static constexpr tag_registry::tag_metadata metadata = Meta;
        };

        // Partial specialization: when custom_tag_info<extension_tag<Name>> is
        // re-specialized to delegate to a constexpr_tag_binding, the metadata is
        // fully structural.  The extension_tag_traits path below is the preferred
        // ergonomic alternative.

        // The idiomatic no-macro registration pattern (verbose but fully explicit):
        //
        //   template<>
        //   struct lithe::dsl_extension::tag_registry::custom_tag_info<
        //       lithe::dsl_extension::extension_tag<"pow">>
        //       : lithe::dsl_extension::constexpr_tag_binding<"pow",
        //             {.name="pow", .precedence=8, .is_associative=false,
        //              .is_commutative=false, .arity=2}> {};
        //
        // That is verbose but fully macro-free.  The helper alias below reduces
        // boilerplate for the common case.

        // extension_tag_traits<Name> — traits class that extension authors
        // specialize instead of writing the full custom_tag_info path.
        //
        // Default: zero-metadata (defers to the automatic extension_tag<Name>
        // specialization of custom_tag_info, which uses Name.c_str()).
        template <fixed_string Name>
        struct extension_tag_traits {
            static constexpr int precedence = 0;
            static constexpr bool is_associative = false;
            static constexpr bool is_commutative = false;
            static constexpr std::size_t arity = 2;
        };

        // Hook: custom_tag_info<extension_tag<Name>> defers to
        // extension_tag_traits<Name> so users only need to specialize
        // extension_tag_traits, not the more deeply nested custom_tag_info.
        namespace tag_registry {
            template <fixed_string Name>
            struct custom_tag_info<extension_tag<Name>> {
                static constexpr tag_metadata metadata{
                    Name.c_str(),
                    extension_tag_traits<Name>::precedence,
                    extension_tag_traits<Name>::is_associative,
                    extension_tag_traits<Name>::is_commutative,
                    extension_tag_traits<Name>::arity
                };
            };
        } // namespace tag_registry

        // =====================================================================
        // Symbolic, operator_hooks, functional — unchanged.
        // =====================================================================

        namespace symbolic {
            template <class T>
            struct symbolic_var {
                std::string name;
                std::size_t id;

                constexpr explicit symbolic_var(std::string n, const std::size_t var_id = 0)
                    : name(std::move(n)), id(var_id) {}

                using value_type = T;

                constexpr bool operator==(const symbolic_var& other) const {
                    return id == other.id && name == other.name;
                }

                constexpr bool operator<(const symbolic_var& other) const {
                    return id < other.id || (id == other.id && name < other.name);
                }
            };

            class symbolic_factory {
                mutable std::size_t next_id = 1;

            public:
                template <class T>
                constexpr auto create(std::string name) const {
                    return symbolic_var<T>{std::move(name), next_id++};
                }

                template <class T>
                constexpr auto operator()(std::string name) const {
                    return create<T>(std::move(name));
                }
            };

            inline constexpr symbolic_factory symbol{};

            using int_var = symbolic_var<int>;
            using double_var = symbolic_var<double>;
            using bool_var = symbolic_var<bool>;
        } // namespace symbolic

        namespace operator_hooks {
            template <class T>
            struct has_custom_operators : std::false_type {};

            template <class Derived>
            struct custom_operator_mixin {
                using has_custom_ops = std::true_type;

                template <class R>
                constexpr auto custom_add(R&& rhs) const {
                    return static_cast<const Derived*>(this)->template operator_impl<add_tag>(
                        std::forward<R>(rhs));
                }

                template <class R>
                constexpr auto custom_mul(R&& rhs) const {
                    return static_cast<const Derived*>(this)->template operator_impl<mul_tag>(
                        std::forward<R>(rhs));
                }

                template <class R>
                constexpr auto custom_sub(R&& rhs) const {
                    return static_cast<const Derived*>(this)->template operator_impl<sub_tag>(
                        std::forward<R>(rhs));
                }

                template <class Tag, class R>
                constexpr auto operator_impl(R&& rhs) const {
                    return make_node<Tag>(static_cast<const Derived&>(*this), std::forward<R>(rhs));
                }
            };

            template <class T>
                requires requires { typename T::has_custom_ops; }
            struct has_custom_operators<T> : std::true_type {};

            template <class T>
            inline constexpr bool has_custom_operators_v = has_custom_operators<T>::value;

            template <class L, class R, class Tag>
            constexpr auto dispatch_binary_op(L&& lhs, R&& rhs, Tag) {
                if constexpr (has_custom_operators_v<std::decay_t<L>>) {
                    if constexpr (std::is_same_v<Tag, add_tag>) {
                        return std::forward<L>(lhs).custom_add(std::forward<R>(rhs));
                    }
                    else if constexpr (std::is_same_v<Tag, mul_tag>) {
                        return std::forward<L>(lhs).custom_mul(std::forward<R>(rhs));
                    }
                    else if constexpr (std::is_same_v<Tag, sub_tag>) {
                        return std::forward<L>(lhs).custom_sub(std::forward<R>(rhs));
                    }
                    else {
                        return make_node<Tag>(std::forward<L>(lhs), std::forward<R>(rhs));
                    }
                }
                else {
                    return make_node<Tag>(std::forward<L>(lhs), std::forward<R>(rhs));
                }
            }
        } // namespace operator_hooks

        namespace functional {
            template <class... ParamTypes>
            struct parameter_list {
                std::tuple<ParamTypes...> params;

                template <class... Args>
                constexpr explicit parameter_list(Args&&... args)
                    : params(std::forward<Args>(args)...) {}

                static constexpr std::size_t arity = sizeof...(ParamTypes);
            };

            template <class... CapturedTypes>
            struct capture_list {
                std::tuple<CapturedTypes...> captures;

                template <class... Args>
                constexpr explicit capture_list(Args&&... args)
                    : captures(std::forward<Args>(args)...) {}

                static constexpr std::size_t capture_count = sizeof...(CapturedTypes);
            };

            template <class ParamList, class CaptureList, class Body>
            struct lambda_node : interface<lambda_node<ParamList, CaptureList, Body>> {
                using is_lithe_node = void;
                using tag_type = lambda_tag;

                ParamList parameters;
                CaptureList captures;
                Body body;

                std::tuple<ParamList, CaptureList, Body> children;

                constexpr lambda_node(ParamList p, CaptureList c, Body b)
                    : parameters(std::move(p)), captures(std::move(c)), body(std::move(b))
                      , children(parameters, captures, body) {}

                static constexpr std::size_t arity = ParamList::arity;
                static constexpr std::size_t capture_count = CaptureList::capture_count;
            };

            template <class... Params>
            constexpr auto params(Params&&... p) {
                return parameter_list<std::decay_t<Params>...>{std::forward<Params>(p)...};
            }

            template <class... Captures>
            constexpr auto captures(Captures&&... c) {
                return capture_list<std::decay_t<Captures>...>{std::forward<Captures>(c)...};
            }

            template <class ParamList, class CaptureList, class Body>
            constexpr auto make_lambda(ParamList p, CaptureList c, Body body) {
                return lambda_node<ParamList, CaptureList, Body>{
                    std::move(p), std::move(c), std::move(body)
                };
            }

            template <class ParamList, class Body>
            constexpr auto make_lambda(ParamList p, Body body) {
                return make_lambda(std::move(p), capture_list<>{}, std::move(body));
            }

            template <class Function, class... Args>
            struct application_node : interface<application_node<Function, Args...>> {
                using is_lithe_node = void;
                using tag_type = call_tag;

                Function function;
                std::tuple<Args...> arguments;
                std::tuple<Function, Args...> children;

                template <class F, class... A>
                constexpr explicit application_node(F&& f, A&&... args)
                    : function(std::forward<F>(f))
                      , arguments(std::forward<A>(args)...)
                      , children(function, std::forward<A>(args)...) {}

                static constexpr std::size_t arg_count = sizeof...(Args);
            };

            template <class Function, class... Args>
            constexpr auto apply(Function&& func, Args&&... args) {
                return application_node<std::decay_t<Function>, std::decay_t<Args>...>{
                    std::forward<Function>(func), std::forward<Args>(args)...
                };
            }
        } // namespace functional
    } // namespace dsl_extension

    // =========================================================================
    // builder — unchanged; extension_tag-based nodes work via make_node<Tag>.
    // =========================================================================
    namespace builder {
        class IRBuilder {
        public:
            template <class L, class R>
            static constexpr auto add(L&& left, R&& right) {
                return dsl_extension::operator_hooks::dispatch_binary_op(
                    std::forward<L>(left), std::forward<R>(right), add_tag{});
            }

            template <class L, class R>
            static constexpr auto sub(L&& left, R&& right) {
                return dsl_extension::operator_hooks::dispatch_binary_op(
                    std::forward<L>(left), std::forward<R>(right), sub_tag{});
            }

            template <class L, class R>
            static constexpr auto mul(L&& left, R&& right) {
                return dsl_extension::operator_hooks::dispatch_binary_op(
                    std::forward<L>(left), std::forward<R>(right), mul_tag{});
            }

            template <class L, class R>
            static constexpr auto div(L&& left, R&& right) {
                return make_node<div_tag>(std::forward<L>(left), std::forward<R>(right));
            }

            template <class L, class R>
            static constexpr auto mod(L&& left, R&& right) {
                return make_node<mod_tag>(std::forward<L>(left), std::forward<R>(right));
            }

            template <class Operand>
            static constexpr auto neg(Operand&& operand) {
                return make_node<neg_tag>(std::forward<Operand>(operand));
            }

            template <class Operand>
            static constexpr auto logical_not(Operand&& operand) {
                return make_node<not_tag>(std::forward<Operand>(operand));
            }

            template <class Operand>
            static constexpr auto bitwise_not(Operand&& operand) {
                return make_node<bit_not_tag>(std::forward<Operand>(operand));
            }

            template <class L, class R>
            static constexpr auto eq(L&& left, R&& right) {
                return make_node<eq_tag>(std::forward<L>(left), std::forward<R>(right));
            }

            template <class L, class R>
            static constexpr auto ne(L&& left, R&& right) {
                return make_node<ne_tag>(std::forward<L>(left), std::forward<R>(right));
            }

            template <class L, class R>
            static constexpr auto lt(L&& left, R&& right) {
                return make_node<lt_tag>(std::forward<L>(left), std::forward<R>(right));
            }

            template <class L, class R>
            static constexpr auto le(L&& left, R&& right) {
                return make_node<le_tag>(std::forward<L>(left), std::forward<R>(right));
            }

            template <class L, class R>
            static constexpr auto gt(L&& left, R&& right) {
                return make_node<gt_tag>(std::forward<L>(left), std::forward<R>(right));
            }

            template <class L, class R>
            static constexpr auto ge(L&& left, R&& right) {
                return make_node<ge_tag>(std::forward<L>(left), std::forward<R>(right));
            }

            template <class L, class R>
            static constexpr auto logical_and(L&& left, R&& right) {
                return make_node<and_tag>(std::forward<L>(left), std::forward<R>(right));
            }

            template <class L, class R>
            static constexpr auto logical_or(L&& left, R&& right) {
                return make_node<or_tag>(std::forward<L>(left), std::forward<R>(right));
            }

            template <class L, class R>
            static constexpr auto bitwise_and(L&& left, R&& right) {
                return make_node<bit_and_tag>(std::forward<L>(left), std::forward<R>(right));
            }

            template <class L, class R>
            static constexpr auto bitwise_or(L&& left, R&& right) {
                return make_node<bit_or_tag>(std::forward<L>(left), std::forward<R>(right));
            }

            template <class L, class R>
            static constexpr auto bitwise_xor(L&& left, R&& right) {
                return make_node<bit_xor_tag>(std::forward<L>(left), std::forward<R>(right));
            }

            template <class L, class R>
            static constexpr auto shift_left(L&& left, R&& right) {
                return make_node<shl_tag>(std::forward<L>(left), std::forward<R>(right));
            }

            template <class L, class R>
            static constexpr auto shift_right(L&& left, R&& right) {
                return make_node<shr_tag>(std::forward<L>(left), std::forward<R>(right));
            }

            template <class Base, class... Indices>
            static constexpr auto subscript(Base&& base, Indices&&... indices) {
                return make_node<subscript_tag>(std::forward<Base>(base),
                                                std::forward<Indices>(indices)...);
            }

            template <class Condition, class ThenBranch, class ElseBranch>
            static constexpr auto if_then_else(Condition&& cond, ThenBranch&& then_stmt,
                                               ElseBranch&& else_stmt) {
                return make_node<if_tag>(std::forward<Condition>(cond),
                                         std::forward<ThenBranch>(then_stmt),
                                         std::forward<ElseBranch>(else_stmt));
            }

            template <class Condition, class ThenBranch>
            static constexpr auto if_then(Condition&& cond, ThenBranch&& then_stmt) {
                return make_node<if_tag>(std::forward<Condition>(cond),
                                         std::forward<ThenBranch>(then_stmt));
            }

            template <class Condition, class Body>
            static constexpr auto while_loop(Condition&& cond, Body&& body) {
                return make_node<while_tag>(std::forward<Condition>(cond),
                                            std::forward<Body>(body));
            }

            template <class Init, class Condition, class Update, class Body>
            static constexpr auto for_loop(Init&& init, Condition&& cond,
                                           Update&& update, Body&& body) {
                return make_node<for_tag>(std::forward<Init>(init),
                                          std::forward<Condition>(cond),
                                          std::forward<Update>(update),
                                          std::forward<Body>(body));
            }

            template <class Function, class... Args>
            static constexpr auto call(Function&& func, Args&&... args) {
                return dsl_extension::functional::apply(std::forward<Function>(func),
                                                        std::forward<Args>(args)...);
            }

            template <class... Params, class Body>
            static constexpr auto lambda(Body&& body, Params&&... ps) {
                auto param_list =
                    dsl_extension::functional::params(std::forward<Params>(ps)...);
                return dsl_extension::functional::make_lambda(param_list,
                                                              std::forward<Body>(body));
            }

            template <class... Params, class... Captures, class Body>
            static constexpr auto lambda_with_captures(
                Body&& body,
                dsl_extension::functional::parameter_list<Params...> ps,
                dsl_extension::functional::capture_list<Captures...> caps) {
                return dsl_extension::functional::make_lambda(std::move(ps), std::move(caps),
                                                              std::forward<Body>(body));
            }

            template <class... Statements>
            static constexpr auto sequence(Statements&&... stmts) {
                return make_node<seq_tag>(std::forward<Statements>(stmts)...);
            }

            template <class Name, class Value, class Body>
            static constexpr auto let(Name&& name, Value&& value, Body&& body) {
                return make_node<let_tag>(std::forward<Name>(name),
                                          std::forward<Value>(value),
                                          std::forward<Body>(body));
            }

            template <class T>
            static constexpr auto constant(T&& value) {
                return as_expr(std::forward<T>(value));
            }

            template <class T>
            static constexpr auto variable(T&& var) {
                return as_expr(std::forward<T>(var));
            }

            template <class T>
            static constexpr auto symbol(std::string name) {
                return dsl_extension::symbolic::symbol.create<T>(std::move(name));
            }

            template <class T>
            static constexpr auto named_value(const char*, T&& value) {
                return as_expr(std::forward<T>(value));
            }

            template <class CustomTag, class... Args>
            static constexpr auto custom_node(CustomTag, Args&&... args) {
                return make_node<CustomTag>(std::forward<Args>(args)...);
            }

            template <class TargetType, class Expression>
            static constexpr auto cast_to(Expression&& expr) {
                return make_node<cast_tag>(std::forward<Expression>(expr));
            }

            template <class Expression>
            static constexpr auto deref(Expression&& expr) {
                return make_node<deref_tag>(std::forward<Expression>(expr));
            }

            template <class Expression>
            static constexpr auto address_of(Expression&& expr) {
                return make_node<addr_tag>(std::forward<Expression>(expr));
            }

            template <class Expression>
            static constexpr auto size_of(Expression&& expr) {
                return make_node<sizeof_tag>(std::forward<Expression>(expr));
            }

            template <class Expression>
            static constexpr auto return_stmt(Expression&& expr) {
                return make_node<return_tag>(std::forward<Expression>(expr));
            }

            template <class... Statements>
            static constexpr auto block(Statements&&... stmts) {
                return sequence(std::forward<Statements>(stmts)...);
            }

            template <class Pattern, class Value, class... Cases>
            static constexpr auto match(Pattern&&, Value&&, Cases&&... cases) {
                return sequence(std::forward<Cases>(cases)...);
            }
        };

        inline constexpr IRBuilder IR{};

        template <class Domain>
        class domain_builder : public IRBuilder {
        public:
            template <class... Args>
            constexpr auto domain_op(Args&&... args) {
                return static_cast<Domain*>(this)->custom_domain_op(
                    std::forward<Args>(args)...);
            }
        };
    } // namespace builder

    // =========================================================================
    // Free DSL helpers — unchanged.
    // =========================================================================

    template <class Condition, class ThenBranch, class ElseBranch>
    constexpr auto if_else(Condition&& cond, ThenBranch&& then_branch,
                           ElseBranch&& else_branch) {
        return make_node<if_tag>(std::forward<Condition>(cond),
                                 std::forward<ThenBranch>(then_branch),
                                 std::forward<ElseBranch>(else_branch));
    }

    template <class Condition, class Body>
    constexpr auto while_loop(Condition&& cond, Body&& body) {
        return make_node<while_tag>(std::forward<Condition>(cond),
                                    std::forward<Body>(body));
    }

    template <class Init, class Condition, class Update, class Body>
    constexpr auto for_loop(Init&& init, Condition&& cond, Update&& update, Body&& body) {
        return make_node<for_tag>(std::forward<Init>(init),
                                  std::forward<Condition>(cond),
                                  std::forward<Update>(update),
                                  std::forward<Body>(body));
    }

    template <class Name, class Value, class Body>
    constexpr auto let_binding(Name&& name, Value&& value, Body&& body) {
        return make_node<let_tag>(std::forward<Name>(name),
                                  std::forward<Value>(value),
                                  std::forward<Body>(body));
    }

    template <class... Statements>
    constexpr auto sequence(Statements&&... stmts) {
        return make_node<seq_tag>(std::forward<Statements>(stmts)...);
    }

    template <class Function, class... Args>
    constexpr auto call(Function&& func, Args&&... args) {
        return make_node<call_tag>(std::forward<Function>(func),
                                   std::forward<Args>(args)...);
    }

    template <class TargetType, class Expression>
    constexpr auto cast_to(Expression&& expr) {
        return make_node<cast_tag>(std::forward<Expression>(expr));
    }

    template <class Expression>
    constexpr auto size_of(Expression&& expr) {
        return make_node<sizeof_tag>(std::forward<Expression>(expr));
    }

    template <class Expression>
    constexpr auto dereference(Expression&& expr) {
        return make_node<deref_tag>(std::forward<Expression>(expr));
    }

    template <class Expression>
    constexpr auto address_of(Expression&& expr) {
        return make_node<addr_tag>(std::forward<Expression>(expr));
    }

    template <class... Params, class Body>
    constexpr auto lambda(Body&& body, Params&&... ps) {
        return make_node<lambda_tag>(std::forward<Body>(body),
                                     std::forward<Params>(ps)...);
    }

    template <class Expression>
    constexpr auto return_stmt(Expression&& expr) {
        return make_node<return_tag>(std::forward<Expression>(expr));
    }

    // =========================================================================
    // patterns — unchanged.
    // =========================================================================
    namespace patterns {
        template <class Pattern, class Value>
        struct pattern_match {
            Pattern pattern;
            Value value;

            template <class Visitor>
            constexpr auto apply(Visitor&& visitor) const {
                return visitor(pattern, value);
            }
        };

        template <class Pattern, class Value>
        constexpr auto match(Pattern&& pattern, Value&& value) {
            return pattern_match<std::decay_t<Pattern>, std::decay_t<Value>>{
                std::forward<Pattern>(pattern),
                std::forward<Value>(value)
            };
        }

        struct wildcard_pattern {};

        constexpr inline wildcard_pattern _{};

        template <class T>
        struct capture_pattern {
            T* target;

            constexpr explicit capture_pattern(T* t) : target(t) {}
        };

        template <class T>
        constexpr auto capture(T& var) {
            return capture_pattern<T>{&var};
        }
    } // namespace patterns

    // =========================================================================
    // meta — unchanged.
    // =========================================================================
    namespace meta {
        template <class... Ts>
        struct type_list {
            static constexpr std::size_t size = sizeof...(Ts);
        };

        template <std::size_t N, class... Ts>
        using nth_type = std::tuple_element_t<N, std::tuple<Ts...>>;

        template <class List1, class List2>
        struct concat;

        template <class... T1s, class... T2s>
        struct concat<type_list<T1s...>, type_list<T2s...>> {
            using type = type_list<T1s..., T2s...>;
        };

        template <class List1, class List2>
        using concat_t = concat<List1, List2>::type;

        template <template<class> class Predicate, class... Ts>
        struct filter;

        template <template<class> class Predicate>
        struct filter<Predicate> {
            using type = type_list<>;
        };

        template <template<class> class Predicate, class T, class... Rest>
        struct filter<Predicate, T, Rest...> {
            using rest_type = filter<Predicate, Rest...>::type;
            using type = std::conditional_t<
                Predicate<T>::value,
                concat_t<type_list<T>, rest_type>,
                rest_type
            >;
        };

        template <template<class> class Predicate, class... Ts>
        using filter_t = filter<Predicate, Ts...>::type;
    } // namespace meta

    // =========================================================================
    // dsl — unchanged.
    // =========================================================================
    namespace dsl {
        template <class Left, class Right>
        struct compose_expr {
            Left left;
            Right right;

            template <class Input>
            constexpr auto operator()(Input&& input) const {
                return right(left(std::forward<Input>(input)));
            }
        };

        template <class Left, class Right>
        constexpr auto compose(Left&& left, Right&& right) {
            return compose_expr<std::decay_t<Left>, std::decay_t<Right>>{
                std::forward<Left>(left), std::forward<Right>(right)
            };
        }

        template <class Func, class... BoundArgs>
        struct partial_application {
            Func func;
            std::tuple<BoundArgs...> bound_args;

            template <class... RemainingArgs>
            constexpr auto operator()(RemainingArgs&&... remaining) const {
                return std::apply([&](auto&&... bound) {
                    return func(std::forward<decltype(bound)>(bound)...,
                                std::forward<RemainingArgs>(remaining)...);
                }, bound_args);
            }
        };

        template <class Func, class... BoundArgs>
        constexpr auto partial(Func&& func, BoundArgs&&... bound_args) {
            return partial_application<std::decay_t<Func>, std::decay_t<BoundArgs>...>{
                std::forward<Func>(func),
                std::tuple < std::decay_t<BoundArgs>...>{std::forward<BoundArgs>(bound_args)...}
            };
        }

        template <class Func>
        struct curry_helper {
            Func func;

            template <class... Args>
            constexpr auto operator()(Args&&... args) const {
                if constexpr (std::is_invocable_v<Func, Args...>) {
                    return func(std::forward<Args>(args)...);
                }
                else {
                    return partial(func, std::forward<Args>(args)...);
                }
            }
        };

        template <class Func>
        constexpr auto curry(Func&& func) {
            return curry_helper<std::decay_t<Func>>{std::forward<Func>(func)};
        }
    } // namespace dsl
} // namespace lithe

// =============================================================================
// is_terminal specialization for symbolic_var — unchanged.
// =============================================================================
namespace vakya {
    template <class T>
    struct is_terminal<lithe::dsl_extension::symbolic::symbolic_var<T>> : std::true_type {};
} // namespace vakya

namespace std {
    template <class T>
    struct hash<lithe::dsl_extension::symbolic::symbolic_var<T>> {
        std::size_t operator()(
            const lithe::dsl_extension::symbolic::symbolic_var<T>& var) const {
            return std::hash<std::string>{}(var.name) ^ std::hash<std::size_t>{}(var.id);
        }
    };
} // namespace std
