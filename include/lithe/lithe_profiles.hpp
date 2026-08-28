#pragma once

// Internal fragment — include only via lithe_passes.hpp (end of file).
// Provides: lithe::profile — data-driven, composable, introspectable
// optimization profiles backed by the existing preset / compiler::compile
// machinery.  Zero runtime cost on the static path.

#include "lithe_extension.hpp"
#include "lithe_passes.hpp"

#include <any>
#include <functional>
#include <string_view>
#include <vector>

namespace lithe::profile {
// =========================================================================
// profile_descriptor — structural (NTTP-usable) metadata for a profile.
//
// id: fixed char[32] — structural, zero-terminated.
// id convention: "domain.level", e.g. "std.o3", "tensor.o3".
// =========================================================================

struct profile_descriptor {
  char id[32]{};
  version_triple version{1, 0, 0};
  int max_iters = 8;
  passes::ir_stage target_stage = passes::ir_stage::optimized;
  bool trace = false;
  bool deterministic = true;

  // Implicit construction from string literal — fills id, zero-pads remainder.
  template <std::size_t N>
    requires(N <= 32)
  consteval profile_descriptor(

      const char (&src)[N], version_triple ver = {1, 0, 0},

      int iters = 8, passes::ir_stage stage = passes::ir_stage::optimized,

      bool tr = false,

      bool det = true) noexcept
      : version{ver}, max_iters{iters}, target_stage{stage}, trace{tr},
        deterministic{det} {
    for (std::size_t i = 0; i < N; ++i)
      id[i] = src[i];
    for (std::size_t i = N; i < 32; ++i)
      id[i] = '\0';
  }

  [[nodiscard]] constexpr std::string_view id_view() const noexcept {
    std::size_t len = 0;
    while (len < 31 && id[len] != '\0')
      ++len;
    return {id, len};
  }

  constexpr bool operator==(const profile_descriptor &o) const noexcept {
    for (std::size_t i = 0; i < 32; ++i)
      if (id[i] != o.id[i])
        return false;
    return version == o.version && max_iters == o.max_iters &&
           target_stage == o.target_stage && trace == o.trace &&
           deterministic == o.deterministic;
  }
};

// =========================================================================
// detail helpers — bundle concat + dedupe
// =========================================================================

namespace detail {
// append_if_missing: append Desc to Bundle only when not already present.
template <class Bundle, class Desc> struct append_if_missing {
  using type = std::conditional_t<
      passes::contains_pass_v<Bundle, Desc>, Bundle,
      typename passes::detail::bundle_append<Bundle, Desc>::type>;
};

template <class Bundle, class Desc>
using append_if_missing_t = typename append_if_missing<Bundle, Desc>::type;

// bundle_concat_dedup: append all Ds... from RhsBundle into AccBundle,
// deduping.
template <class AccBundle, class RhsBundle> struct bundle_concat_dedup;

template <class AccBundle>
struct bundle_concat_dedup<AccBundle, passes::pass_bundle<>> {
  using type = AccBundle;
};

template <class AccBundle, class Head, class... Tail>
struct bundle_concat_dedup<AccBundle, passes::pass_bundle<Head, Tail...>> {
  using next = append_if_missing_t<AccBundle, Head>;
  using type =
      typename bundle_concat_dedup<next, passes::pass_bundle<Tail...>>::type;
};

template <class LhsBundle, class RhsBundle>
using bundle_concat_dedup_t =
    typename bundle_concat_dedup<LhsBundle, RhsBundle>::type;

// run_bundle_impl: fold ordered bundle descriptors left-to-right.
// Each descriptor's pass_type is default-constructed and wrapped in fixpoint.
template <int MaxIters>
constexpr auto run_bundle_impl(passes::pass_bundle<>, auto &&e) {
  return std::forward<decltype(e)>(e);
}

template <int MaxIters, class Head, class... Tail>
constexpr auto run_bundle_impl(passes::pass_bundle<Head, Tail...>, auto &&e) {
  using Pass = typename Head::pass_type;
  auto step = [&] {
    if constexpr (MaxIters > 1)
      return compiler::compile(std::forward<decltype(e)>(e),
                               passes::fixpoint(Pass{}, MaxIters));
    else
      return compiler::compile(std::forward<decltype(e)>(e), Pass{});
  }();
  return run_bundle_impl<MaxIters>(passes::pass_bundle<Tail...>{},
                                   std::move(step));
}
} // namespace detail

// =========================================================================
// profile<Bundle, Desc> — type-level optimization profile (generic path).
//
// operator() runs each descriptor's pass_type wrapped in fixpoint(p, max_iters)
// through compiler::compile, left-to-right in topo-sorted order.
//
// Built-in std_o0..std_o3/debug/semantic_safe delegate to preset::O* directly
// for exact phase-wrapper and hash parity with existing presets.
// Use profile<> for user-defined pass bundles or profile_inherit extensions.
// =========================================================================

template <class Bundle, profile_descriptor Desc> struct profile {
  static constexpr profile_descriptor descriptor = Desc;
  using bundle = Bundle;
  using ordered = passes::order_pass_bundle_t<Bundle>;

  template <class Expr> constexpr auto operator()(Expr &&e) const {
    return detail::run_bundle_impl<Desc.max_iters>(ordered{},
                                                   std::forward<Expr>(e));
  }
};

// =========================================================================
// profile_inherit<Base, ExtraBundle, NewDesc> — extend a profile.
//
// Result bundle = dedup-concat of Base::bundle and ExtraBundle.
// Descriptor defaults to Base::descriptor; override via NewDesc.
// =========================================================================

template <class BaseProfile, class ExtraBundle,
          profile_descriptor NewDesc = BaseProfile::descriptor>
using profile_inherit = profile<
    detail::bundle_concat_dedup_t<typename BaseProfile::bundle, ExtraBundle>,
    NewDesc>;

// Override descriptor only.
template <class BaseProfile, profile_descriptor NewDesc>
using with_descriptor = profile<typename BaseProfile::bundle, NewDesc>;

// =========================================================================
// profile_compose<BaseProfile, ExtraPasses...> — functor-level composition.
// Useful for extra passes that are not pass_descriptor types.
// =========================================================================

template <class BaseProfile, class... ExtraPasses> struct profile_compose {
  BaseProfile base;
  std::tuple<ExtraPasses...> extras;

  template <class Expr> constexpr auto operator()(Expr &&e) const {
    auto current = base(std::forward<Expr>(e));
    if constexpr (sizeof...(ExtraPasses) > 0) {
      current = std::apply(
          [&](const auto &...pass) {
            return compiler::compile(std::move(current), pass...);
          },
          extras);
    }
    return current;
  }
};

template <class BaseProfile, class... ExtraPasses>
constexpr auto compose_profile(BaseProfile base, ExtraPasses... passes) {
  return profile_compose<BaseProfile, ExtraPasses...>{
      std::move(base), std::tuple<ExtraPasses...>{std::move(passes)...}};
}

// =========================================================================
// profile_valid<P>() — consteval validation.
//
// Checks:
//   1. All descriptor dependencies present in bundle (all_deps_present).
//   2. Topo-sort succeeds (ordered != void).
//   3. No conflicting pass pair in the bundle (no_conflicts).
//
// Note: stages_monotone is intentionally omitted — expression-level
// optimization passes (simplify/constant_fold/cse/strength_reduction)
// share input stages (canonical) while outputting different stages.
// Stage monotonicity is meaningful for IR-lowering pipelines, not for
// expression-level optimization bundles.
// =========================================================================

namespace detail {
// Guard: only instantiate no_conflicts when Bundle has a valid topo-order.
template <class Bundle, bool Valid>
struct profile_conflict_check : std::false_type {};

template <class Bundle>
struct profile_conflict_check<Bundle, true>
    : std::bool_constant<passes::no_conflicts<Bundle>()> {};
} // namespace detail

template <class P> [[nodiscard]] consteval bool profile_valid() noexcept {
  constexpr bool deps_ok =
      passes::detail::all_deps_present<typename P::bundle>::value;
  constexpr bool topo_ok = !std::is_void_v<typename P::ordered>;
  constexpr bool no_conf = detail::profile_conflict_check < typename P::bundle,
                 deps_ok && topo_ok > ::value;
  return deps_ok && topo_ok && no_conf;
}

// =========================================================================
// Built-in pass_descriptor types for standard pass bundles.
//
// Integer NTTP names are local profile IDs (≠ stable_id from pass_type_traits).
// These are used only for bundle topology — not for runtime registry lookup.
// =========================================================================

using desc_simplify_add_zero =
    passes::pass_descriptor<0, passes::simplify_add_zero_pass>;

using desc_simplify_mul_identity =
    passes::pass_descriptor<1, passes::simplify_mul_identity_pass>;

using desc_constant_fold =
    passes::pass_descriptor<2, passes::constant_fold_arith_pass,
                            desc_simplify_add_zero, desc_simplify_mul_identity>;

using desc_strength_reduction =
    passes::pass_descriptor<4, passes::strength_reduction_pass,
                            desc_constant_fold>;

// Standard bundles
using o0_bundle = passes::pass_bundle<>;

using o1_bundle =
    passes::pass_bundle<desc_simplify_add_zero, desc_simplify_mul_identity>;

using o2_bundle =
    passes::pass_bundle<desc_simplify_add_zero, desc_simplify_mul_identity,
                        desc_constant_fold>;

using o3_bundle =
    passes::pass_bundle<desc_simplify_add_zero, desc_simplify_mul_identity,
                        desc_constant_fold, desc_strength_reduction>;

// =========================================================================
// profile_descriptor constants for built-in profiles
// =========================================================================

inline constexpr profile_descriptor k_std_o0_desc{
    "std.o0", {1, 0, 0}, 0, passes::ir_stage::surface, false, true};

inline constexpr profile_descriptor k_std_o1_desc{
    "std.o1", {1, 0, 0}, 4, passes::ir_stage::canonical, false, true};

inline constexpr profile_descriptor k_std_o2_desc{
    "std.o2", {1, 0, 0}, 6, passes::ir_stage::optimized, false, true};

inline constexpr profile_descriptor k_std_o3_desc{
    "std.o3", {1, 0, 0}, 8, passes::ir_stage::optimized, false, true};

inline constexpr profile_descriptor k_std_debug_desc{
    "std.debug", {1, 0, 0}, 6, passes::ir_stage::optimized, true, true};

inline constexpr profile_descriptor k_std_semantic_safe_desc{
    "std.semantic_safe",         {1, 0, 0}, 6,
    passes::ir_stage::optimized, false,     true};

// =========================================================================
// Built-in profiles — std_o0..std_semantic_safe
//
// All built-ins delegate to the corresponding preset::O* functor for
// exact output parity (same phase wrappers, same structural hash).
// The bundle field is declarative metadata only; execution goes through
// the proven preset path (canonicalize + optimize_phase where applicable).
// preset::O0..O3/Debug/SemanticSafe remain unchanged — back-compat.
// =========================================================================

struct std_o0 {
  static constexpr profile_descriptor descriptor = k_std_o0_desc;
  using bundle = o0_bundle;
  using ordered = o0_bundle;

  template <class Expr> constexpr auto operator()(Expr &&e) const {
    return preset::O0{}(std::forward<Expr>(e));
  }
};

struct std_o1 {
  static constexpr profile_descriptor descriptor = k_std_o1_desc;
  using bundle = o1_bundle;
  using ordered = passes::order_pass_bundle_t<o1_bundle>;

  template <class Expr> constexpr auto operator()(Expr &&e) const {
    return preset::O1{descriptor.max_iters}(std::forward<Expr>(e));
  }
};

struct std_o2 {
  static constexpr profile_descriptor descriptor = k_std_o2_desc;
  using bundle = o2_bundle;
  using ordered = passes::order_pass_bundle_t<o2_bundle>;

  template <class Expr> constexpr auto operator()(Expr &&e) const {
    return preset::O2{descriptor.max_iters}(std::forward<Expr>(e));
  }
};

struct std_o3 {
  static constexpr profile_descriptor descriptor = k_std_o3_desc;
  using bundle = o3_bundle;
  using ordered = passes::order_pass_bundle_t<o3_bundle>;

  template <class Expr> constexpr auto operator()(Expr &&e) const {
    return preset::O3{descriptor.max_iters}(std::forward<Expr>(e));
  }
};

struct std_debug {
  static constexpr profile_descriptor descriptor = k_std_debug_desc;
  using bundle = o2_bundle;
  using ordered = passes::order_pass_bundle_t<o2_bundle>;

  template <class Expr> constexpr auto operator()(Expr &&e) const {
    return preset::Debug{}(std::forward<Expr>(e));
  }
};

struct std_semantic_safe {
  static constexpr profile_descriptor descriptor = k_std_semantic_safe_desc;
  using bundle = o2_bundle;
  using ordered = passes::order_pass_bundle_t<o2_bundle>;

  template <class Expr> constexpr auto operator()(Expr &&e) const {
    return preset::SemanticSafe{}(std::forward<Expr>(e));
  }
};

// =========================================================================
// Export / import
//
// profile_record: POD record for serialisation.
// export_profile<P>(): extract record from static profile type.
// dynamic_profile: erased expression-optimizer for import path.
// import_profile(): reconstruct dynamic_profile from record + resolver.
// =========================================================================

struct profile_pass_record {
  std::size_t stable_id = 0;
  std::string id_str;
  int max_iters = 1;
  // Export the real pass effect so tooling does not report a placeholder
  // or analysis-only pass as an optimization transformation.
  passes::pass_effect_kind effect = passes::pass_effect_kind::transforms;
};

struct profile_record {
  std::string profile_id;
  version_triple version{1, 0, 0};
  int max_iters = 8;
  passes::ir_stage target_stage = passes::ir_stage::optimized;
  bool trace = false;
  bool deterministic = true;
  std::vector<profile_pass_record> passes;
};

template <class P> [[nodiscard]] profile_record export_profile() {
  profile_record rec;
  const auto &desc = P::descriptor;
  rec.profile_id = std::string{desc.id_view()};
  rec.version = desc.version;
  rec.max_iters = desc.max_iters;
  rec.target_stage = desc.target_stage;
  rec.trace = desc.trace;
  rec.deterministic = desc.deterministic;

  [&]<class... Ds>(passes::pass_bundle<Ds...>) {
    (rec.passes.push_back(profile_pass_record{
         passes::pass_type_traits<typename Ds::pass_type>::stable_id,
         std::string{
             passes::pass_type_traits<typename Ds::pass_type>::id.view()},
         rec.max_iters,
         passes::pass_type_traits<typename Ds::pass_type>::effect}),
     ...);
  }(typename P::ordered{});

  return rec;
}

// =========================================================================
// dynamic_profile — erased expression optimizer built from a profile_record.
//
// Holds a std::function per pass; for tooling / serialisation only.
// Static path is always preferred for hot-path execution.
// =========================================================================

class dynamic_profile {
public:
  using erased_pass = std::function<void(std::any &)>;

  profile_record record;
  std::vector<erased_pass> pass_fns;

  dynamic_profile() = default;
  explicit dynamic_profile(profile_record r) : record(std::move(r)) {}

  void add_pass(erased_pass fn) { pass_fns.push_back(std::move(fn)); }

  [[nodiscard]] std::string_view id() const noexcept {
    return record.profile_id;
  }
  [[nodiscard]] bool empty() const noexcept { return pass_fns.empty(); }
  [[nodiscard]] std::size_t pass_count() const noexcept {
    return pass_fns.size();
  }

  // Run on an any-wrapped expression; mutates in place.
  void run(std::any &expr) const {
    for (const auto &fn : pass_fns)
      fn(expr);
  }
};

// pass_resolver: maps (stable_id, max_iters) → erased_pass.
using pass_resolver = std::function<dynamic_profile::erased_pass(
    std::size_t stable_id, int max_iters)>;

[[nodiscard]] inline dynamic_profile
import_profile(const profile_record &rec, const pass_resolver &resolver) {
  dynamic_profile dp(rec);
  for (const auto &pr : rec.passes) {
    auto fn = resolver(pr.stable_id, pr.max_iters);
    if (fn)
      dp.add_pass(std::move(fn));
  }
  return dp;
}

[[nodiscard]] inline dynamic_profile import_profile(const profile_record &rec) {
  return dynamic_profile(rec);
}
} // namespace lithe::profile
