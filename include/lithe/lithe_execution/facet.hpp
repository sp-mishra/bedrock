#pragma once

// =============================================================================
// lithe_execution/facet.hpp — CPO vocabulary, backend_traits, and facet concepts
//
// A backend is a *bag of structurally-detected facets*, not a subclass.  Facets
// are discovered purely by ADL tag_invoke, so:
//   • The static path remains fully typed (compiler_for<B,IR> produces
//     artifact_t<B,IR>, not an erased byte span).
//   • A backend exposing only some facets satisfies exactly those concepts.
//   • A member `compile` on a backend does NOT satisfy compiler_for — only
//     an ADL-visible tag_invoke customisation does.
//
// CPOs defined here (all in namespace lithe::execution::cpo):
//   compile, install, compile_and_install, get_entry, invoke,
//   release, serialize
//
// Associated-type helpers (backend_traits<B>):
//   artifact_t<B,IR>          backend's compiled artifact type for IR
//   resource_t<B,Artifact>    backend's installed resource type for Artifact
//   entry_t<B,Resource,Sig>   backend's callable entry type
//   artifact_value_t<A>       remove_cvref_t — normalises Artifact and Artifact&
//
// Concepts (namespace lithe::execution):
//   compiler_for<B,IR>
//   installer_for<B,Artifact>
//   compile_installer_for<B,IR>
//   entry_provider<B,Resource,Sig>
//   invoker<B,Resource>
//   serializer<B,Artifact>
//
// No virtual, no macros.  Header-only C++23.
// =============================================================================

#include <expected>
#include <functional>
#include <type_traits>
#include <utility>

#include "foundation.hpp"   // compile_error, install_error, execution_error, etc.

namespace lithe::execution {
    // =========================================================================
    // type_tag<T> — explicit type argument for get_entry CPO
    // Avoids the need to pack Sig into a template argument on the backend type.
    // =========================================================================
    template <class T>
    struct type_tag {
        using type = T;
    };

    // =========================================================================
    //  Result traits — normalise artifact/resource references
    // =========================================================================

    // artifact_value_t strips cv-ref so Artifact and const Artifact& map to
    // the same resource kind.
    template <class Artifact>
    using artifact_value_t = std::remove_cvref_t<Artifact>;

    // =========================================================================
    //  backend_traits<B> — associated types per backend
    //
    // Specialise these for each backend to declare its associated types.
    // The CPO machinery reads these; do not add them as member types on backends.
    //
    // Default: all types are void (backend provides no facets by default).
    // =========================================================================

    template <class Backend>
    struct backend_traits {
        // Specialise to provide:
        //   template <class IR> using artifact = <type>;
        //   template <class Artifact> using resource = <type>;
        //   template <class Resource, class Sig> using entry = <type>;
    };

    // Convenience alias helpers — read from backend_traits<B>.
    template <class B, class IR>
    using artifact_t = typename backend_traits<B>::template artifact<IR>;

    template <class B, class Artifact>
    using resource_t = typename backend_traits<B>::template resource<artifact_value_t<Artifact>>;

    template <class B, class Resource, class Sig>
    using entry_t = typename backend_traits<B>::template entry<Resource, Sig>;

    // =========================================================================
    //  CPO tag types — ADL tag_invoke seam
    //
    // Pattern: a CPO tag type + a constexpr inline object.
    // Customisation ONLY via tag_invoke(cpo_tag{}, backend, ...) — ADL-visible.
    // A member function on the backend does NOT satisfy a concept.
    // Detection: if constexpr checks in concept body; no silent default fallback.
    // =========================================================================

    namespace cpo {
        // --------------------------------------------------------------------
        // compile — produce an artifact from an IR object
        //   tag_invoke(compile_t{}, B&, IR&&) -> expected<artifact_t<B,IR>, compile_error>
        // --------------------------------------------------------------------
        struct compile_t {
            template <class B, class IR>
            [[nodiscard]] constexpr auto operator()(B&& b, IR&& ir) const
                noexcept(noexcept(tag_invoke(*this, std::forward<B>(b), std::forward<IR>(ir))))
                -> decltype(tag_invoke(*this, std::forward<B>(b), std::forward<IR>(ir))) {
                return tag_invoke(*this, std::forward<B>(b), std::forward<IR>(ir));
            }
        };

        inline constexpr compile_t compile{};

        // --------------------------------------------------------------------
        // install — take an artifact and install it, returning an owning resource
        //   tag_invoke(install_t{}, B&, Artifact&&) -> expected<resource_t<B,Artifact>, install_error>
        // --------------------------------------------------------------------
        struct install_t {
            template <class B, class Artifact>
            [[nodiscard]] constexpr auto operator()(B&& b, Artifact&& art) const
                noexcept(noexcept(tag_invoke(*this, std::forward<B>(b), std::forward<Artifact>(art))))
                -> decltype(tag_invoke(*this, std::forward<B>(b), std::forward<Artifact>(art))) {
                return tag_invoke(*this, std::forward<B>(b), std::forward<Artifact>(art));
            }
        };

        inline constexpr install_t install{};

        // --------------------------------------------------------------------
        // compile_and_install — fused compile+install for backends that cannot
        // separate the two stages (e.g. AsmJIT emitting directly to exec memory).
        //   tag_invoke(compile_and_install_t{}, B&, IR&&)
        //     -> expected<resource_t<B, artifact_t<B,IR>>, compile_install_error>
        // --------------------------------------------------------------------
        struct compile_and_install_t {
            template <class B, class IR>
            [[nodiscard]] constexpr auto operator()(B&& b, IR&& ir) const
                noexcept(noexcept(tag_invoke(*this, std::forward<B>(b), std::forward<IR>(ir))))
                -> decltype(tag_invoke(*this, std::forward<B>(b), std::forward<IR>(ir))) {
                return tag_invoke(*this, std::forward<B>(b), std::forward<IR>(ir));
            }
        };

        inline constexpr compile_and_install_t compile_and_install{};

        // --------------------------------------------------------------------
        // get_entry — look up a typed callable entry in an installed resource.
        //   tag_invoke(get_entry_t{}, B&, Resource&, type_tag<Sig>{})
        //     -> expected<entry_t<B,Resource,Sig>, execution_error>
        //
        // The type_tag<Sig> argument selects the signature at compile time so
        // the return type is concrete and entry_t<B,Resource,Sig> need not be erased.
        // --------------------------------------------------------------------
        struct get_entry_t {
            template <class B, class Resource, class Sig>
            [[nodiscard]] constexpr auto
            operator()(B&& b, Resource&& res, type_tag<Sig> tag) const
                noexcept(noexcept(tag_invoke(*this, std::forward<B>(b),
                                             std::forward<Resource>(res), tag)))
                -> decltype(tag_invoke(*this, std::forward<B>(b),
                                       std::forward<Resource>(res), tag)) {
                return tag_invoke(*this, std::forward<B>(b),
                                  std::forward<Resource>(res), tag);
            }
        };

        inline constexpr get_entry_t get_entry{};

        // --------------------------------------------------------------------
        // invoke — erased dynamic invocation for dynamic-registry / plugin paths.
        //   tag_invoke(invoke_t{}, B&, Resource&, invocation_request)
        //     -> expected<dynamic_execution_result, execution_error>
        //
        // Note: invocation_request and dynamic_execution_result are declared in
        // resource.hpp (included after this header); the concept below uses a
        // deferred check so this header remains self-contained.
        // --------------------------------------------------------------------
        struct invoke_t {
            template <class B, class Resource, class Request>
            [[nodiscard]] constexpr auto
            operator()(B&& b, Resource&& res, Request&& req) const
                noexcept(noexcept(tag_invoke(*this, std::forward<B>(b),
                                             std::forward<Resource>(res),
                                             std::forward<Request>(req))))
                -> decltype(tag_invoke(*this, std::forward<B>(b),
                                       std::forward<Resource>(res),
                                       std::forward<Request>(req))) {
                return tag_invoke(*this, std::forward<B>(b),
                                  std::forward<Resource>(res),
                                  std::forward<Request>(req));
            }
        };

        inline constexpr invoke_t invoke{};

        // --------------------------------------------------------------------
        // release — release / destroy an installed resource.
        //   tag_invoke(release_t{}, B&, Resource&&) -> void
        // --------------------------------------------------------------------
        struct release_t {
            template <class B, class Resource>
            constexpr auto operator()(B&& b, Resource&& res) const
                noexcept(noexcept(tag_invoke(*this, std::forward<B>(b),
                                             std::forward<Resource>(res))))
                -> decltype(tag_invoke(*this, std::forward<B>(b),
                                       std::forward<Resource>(res))) {
                return tag_invoke(*this, std::forward<B>(b),
                                  std::forward<Resource>(res));
            }
        };

        inline constexpr release_t release{};

        // --------------------------------------------------------------------
        // serialize — serialise an artifact to a buffer for AOT / caching.
        //   tag_invoke(serialize_t{}, B const&, Artifact const&, buffer&) -> bool
        // (returns false on failure; writes bytes into buffer)
        // --------------------------------------------------------------------
        struct serialize_t {
            template <class B, class Artifact, class Buf>
            [[nodiscard]] constexpr auto
            operator()(B&& b, Artifact&& art, Buf&& buf) const
                noexcept(noexcept(tag_invoke(*this, std::forward<B>(b),
                                             std::forward<Artifact>(art),
                                             std::forward<Buf>(buf))))
                -> decltype(tag_invoke(*this, std::forward<B>(b),
                                       std::forward<Artifact>(art),
                                       std::forward<Buf>(buf))) {
                return tag_invoke(*this, std::forward<B>(b),
                                  std::forward<Artifact>(art),
                                  std::forward<Buf>(buf));
            }
        };

        inline constexpr serialize_t serialize{};
    } // namespace cpo

    // =========================================================================
    //  Detection helpers — used by the facet concepts below
    // =========================================================================

    namespace detail {
        template <class B, class IR>
        concept has_compile = requires(B& b, IR ir) {
            cpo::compile(b, std::move(ir));
        };

        template <class B, class Artifact>
        concept has_install = requires(B& b, Artifact art) {
            cpo::install(b, std::move(art));
        };

        template <class B, class IR>
        concept has_compile_and_install = requires(B& b, IR ir) {
            cpo::compile_and_install(b, std::move(ir));
        };

        template <class B, class Resource, class Sig>
        concept has_get_entry = requires(B& b, Resource& res) {
            cpo::get_entry(b, res, type_tag<Sig>{});
        };

        template <class B, class Resource, class Request>
        concept has_invoke = requires(B& b, Resource& res, Request req) {
            cpo::invoke(b, res, std::move(req));
        };

        template <class B, class Resource>
        concept has_release = requires(B& b, Resource res) {
            cpo::release(b, std::move(res));
        };

        template <class B, class Artifact>
        concept has_serialize = requires(B const& b, Artifact const& art, buffer& buf) {
            { cpo::serialize(b, art, buf) } -> std::convertible_to<bool>;
        };
    } // namespace detail

    // =========================================================================
    //  Facet concepts
    // =========================================================================

    // compiler_for<B,IR>: B has a tag_invoke(compile_t{}, B&, IR&&) customisation.
    // A member function compile() on B does NOT satisfy this concept.
    template <class B, class IR>
    concept compiler_for = detail::has_compile<B, IR>;

    // installer_for<B,Artifact>: B can install a compiled artifact.
    template <class B, class Artifact>
    concept installer_for = detail::has_install<B, artifact_value_t<Artifact>>;

    // compile_installer_for<B,IR>: fused compile+install (e.g. AsmJIT).
    template <class B, class IR>
    concept compile_installer_for = detail::has_compile_and_install<B, IR>;

    // entry_provider<B,Resource,Sig>: B can produce a typed entry for Sig from Resource.
    template <class B, class Resource, class Sig>
    concept entry_provider = detail::has_get_entry<B, Resource, Sig>;

    // invoker<B,Resource,Request>: B supports erased dynamic invocation.
    template <class B, class Resource, class Request>
    concept invoker = detail::has_invoke<B, Resource, Request>;

    // serializer<B,Artifact>: B can serialise artifacts (AOT/caching path).
    template <class B, class Artifact>
    concept serializer = detail::has_serialize<B, artifact_value_t<Artifact>>;
} // namespace lithe::execution
