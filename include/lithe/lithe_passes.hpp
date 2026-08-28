#pragma once

#include "lithe_algorithms/pipeline.hpp"
#include "lithe_core.hpp"
#include "lithe_diagnostics.hpp"
#include "lithe_extension.hpp"
#include "lithe_semantic.hpp"

#include <algorithm>
#include <any>
#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifndef LITHE_ENABLE_OBSERVABILITY
#define LITHE_ENABLE_OBSERVABILITY 0
#endif

#if defined(__has_include)
#if __has_include("../observability/nadi.hpp")
#include "observability/nadi.hpp"
#define LITHE_HAS_NADI 1
#endif
#if defined(LITHE_HAS_NADI) &&                                                 \
    __has_include("../observability/sinks/thread_local_sink.hpp")
#include "observability/sinks/thread_local_sink.hpp"
#define LITHE_HAS_THREAD_LOCAL_SINK 1
#endif
#if __has_include("../utils/profiler.hpp")
#include "utils/profiler.hpp"
#define LITHE_HAS_PROFILER 1
#endif
#endif
#ifndef LITHE_HAS_NADI
#define LITHE_HAS_NADI 0
#endif
#ifndef LITHE_HAS_THREAD_LOCAL_SINK
#define LITHE_HAS_THREAD_LOCAL_SINK 0
#endif
#ifndef LITHE_HAS_PROFILER
#define LITHE_HAS_PROFILER 0
#endif

namespace lithe {
// -----------------------------
// Compiler helpers: pipeline runner with optimization levels
// -----------------------------
namespace compiler {
namespace observability {
inline constexpr bool enabled_by_default = false;

struct compilation_event {
  enum class kind : std::uint8_t { started, finished, failed };

  kind type = kind::started;
  std::string phase;
  std::uint64_t timestamp_ns = 0;
};

struct pass_event {
  std::string pass_name;
  std::size_t pass_index = 0;
  std::uint64_t start_ns = 0;
  std::uint64_t end_ns = 0;
  bool changed = false;
  std::string ir_before_dump;
  std::string ir_after_dump;
  std::string ir_diff;
  structural_hash_t input_hash = 0;
  structural_hash_t output_hash = 0;
  structural_hash_t input_structural_hash = 0;
  structural_hash_t output_structural_hash = 0;
  // §3.3 pass/egraph telemetry — zero-default; unused fields cost nothing
  // when no observer reads them.  Populated by pass runner and egraph_optimize.
  std::string rule_fired;       // last rewrite rule name fired this pass
  std::size_t iterations = 0;   // fixpoint loop iterations (>1 only for egraph)
  std::size_t nodes_before = 0; // IR/enode count before pass
  std::size_t nodes_after = 0;  // IR/enode count after pass
  std::uint64_t pass_cost_ns = 0; // alias: end_ns - start_ns (convenience)
  std::size_t egraph_enodes =
      0; // saturation_report.enodes (egraph passes only)
  std::size_t egraph_eclasses =
      0; // saturation_report.eclasses (egraph passes only)
};

struct lowering_event {
  std::string backend;
  structural_hash_t input_hash = 0;
  structural_hash_t output_hash = 0;
  std::uint64_t timestamp_ns = 0;
};

struct codegen_event {
  std::string stage;
  std::uint64_t timestamp_ns = 0;
  structural_hash_t ir_hash = 0;
};

struct rewrite_event {
  std::string pass_name;
  std::size_t rewrites_attempted = 0;
  std::size_t rewrites_applied = 0;
  std::uint64_t timestamp_ns = 0;
};

struct structural_hash_event {
  std::string label;
  structural_hash_t expression_hash = 0;
  structural_hash_t structural_hash = 0;
  std::uint64_t timestamp_ns = 0;
};

struct backend_lowering_event {
  std::string backend;
  bool legal = true;
  std::vector<std::string> reasons;
  std::uint64_t timestamp_ns = 0;
};

struct codegen_diagnostic_event {
  std::uint8_t level = 0;
  std::string stage;
  std::string message;
  std::uint64_t timestamp_ns = 0;
};

// Prompt 8 — semantic canonicalization observability event.
// Emitted once per semantic_canonicalization_pass execution.
struct semantic_canonicalization_event {
  // Name of the canonicalization pass that ran.
  std::string pass_name;
  // Count of nodes visited by the pass.
  std::size_t visited_nodes = 0;
  // Count of nodes whose semantic_info was changed.
  std::size_t rewritten_nodes = 0;
  // Names of rules that fired at least once.
  std::vector<std::string> fired_rules;
  // Wall-clock timestamps (nanoseconds).
  std::uint64_t start_ns = 0;
  std::uint64_t end_ns = 0;
};

struct compile_trace {
  std::vector<compilation_event> compilation_events;
  std::vector<pass_event> pass_events;
  std::vector<lowering_event> lowering_events;
  std::vector<codegen_event> codegen_events;
  std::vector<rewrite_event> rewrite_events;
  std::vector<structural_hash_event> structural_hash_events;
  std::vector<backend_lowering_event> backend_lowering_events;
  std::vector<codegen_diagnostic_event> codegen_diagnostic_events;
  // Prompt 8 — semantic canonicalization events.
  std::vector<semantic_canonicalization_event> semantic_canonicalization_events;
};

struct null_observer {
  template <class Event>
  constexpr void on_event(const Event &) const noexcept {}
};

struct trace_observer {
  compile_trace trace;

  void on_event(const compilation_event &event) {
    trace.compilation_events.push_back(event);
  }
  void on_event(const pass_event &event) { trace.pass_events.push_back(event); }
  void on_event(const lowering_event &event) {
    trace.lowering_events.push_back(event);
  }
  void on_event(const codegen_event &event) {
    trace.codegen_events.push_back(event);
  }
  void on_event(const rewrite_event &event) {
    trace.rewrite_events.push_back(event);
  }
  void on_event(const structural_hash_event &event) {
    trace.structural_hash_events.push_back(event);
  }
  void on_event(const backend_lowering_event &event) {
    trace.backend_lowering_events.push_back(event);
  }

  void on_event(const codegen_diagnostic_event &event) {
    trace.codegen_diagnostic_events.push_back(event);
  }

  void on_event(const semantic_canonicalization_event &event) {
    trace.semantic_canonicalization_events.push_back(event);
  }
};

[[nodiscard]] inline std::uint64_t now_ns() noexcept {
#if LITHE_HAS_NADI
  return utils::nadi::SteadyClockPolicy::now();
#else
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
#endif
}

[[nodiscard]] inline structural_hash_t hash_text(const std::string_view text) {
  return std::hash<std::string_view>{}(text);
}

[[nodiscard]] inline std::string diff_ir(const std::string_view before,
                                         const std::string_view after) {
  if (before == after) {
    return "no-change";
  }
  std::ostringstream os;
  os << "changed bytes=" << before.size() << "->" << after.size();
  return os.str();
}

template <class Expr>
[[nodiscard]] inline std::string dump_ir(const Expr &expr) {
  if constexpr (requires { emit::dump(expr); }) {
    return emit::dump(expr);
  } else {
    return "<ir-dump-unavailable>";
  }
}

template <bool Enabled, class Observer, class Event>
constexpr void emit(Observer &observer, Event event) {
  if constexpr (Enabled) {
    if constexpr (requires(Observer obs, Event e) { obs.on_event(e); }) {
      observer.on_event(std::move(event));
    }
  } else {
    (void)observer;
    (void)event;
  }
}
} // namespace observability

enum class opt_level { O0, O1, OG1, O2 };

// -----------------------------------------------------------------------
// Diagnostic types — unified via lithe::diag (lithe_diagnostics.hpp).
// Aliases kept for backward compatibility; existing passes:: references
// continue to compile unchanged.
// -----------------------------------------------------------------------

using source_span = lithe::diag::source_span;
using diagnostic_level = lithe::diag::severity; // note/info/warning/error/fatal
using diagnostic_code =
    lithe::diag::diagnostic_code; // enum → string via to_code_string
using diagnostic_note = lithe::diag::diagnostic_note; // = diag::diagnostic
using diagnostic = lithe::diag::diagnostic;
using diagnostic_engine =
    lithe::diag::collecting_sink; // .entries / emit() / has_errors()

struct optimization_budget {
  std::size_t max_passes = 128;
  std::size_t max_rewrites = 1u << 20;
  std::size_t max_diagnostics = 4096;
  bool abort_on_error = true;
};

struct pass_statistics {
  std::string name;
  std::uint64_t start_ns = 0;
  std::uint64_t end_ns = 0;
  std::size_t rewrite_count = 0;
  std::size_t cache_hits = 0;
  bool changed = false;
#if LITHE_HAS_NADI
  std::uint64_t nadi_event_id = 0;
  std::uint64_t nadi_trace_id = 0;
  std::uint64_t nadi_parent_id = 0;
#endif
};

struct rewrite_statistics {
  std::size_t total = 0;
  std::unordered_map<std::string, std::size_t> by_pass;
};

using pass_cache_value =
    std::variant<std::monostate, bool, std::int64_t, double, std::string,
                 structural_hash_t, std::any>;

struct pass_local_cache {
  std::unordered_map<std::string,
                     std::unordered_map<structural_hash_t, pass_cache_value>>
      buckets;

  template <class T>
  void put(std::string pass_name, const structural_hash_t key, T value) {
    pass_cache_value v;
    if constexpr (std::is_same_v<std::decay_t<T>, bool>) {
      v = static_cast<bool>(value);
    } else if constexpr (std::is_integral_v<std::decay_t<T>>) {
      v = static_cast<std::int64_t>(value);
    } else if constexpr (std::is_floating_point_v<std::decay_t<T>>) {
      v = static_cast<double>(value);
    } else if constexpr (std::is_same_v<std::decay_t<T>, std::string>) {
      v = std::move(value);
    } else {
      v = std::any{std::move(value)};
    }
    buckets[std::move(pass_name)].insert_or_assign(key, std::move(v));
  }

  template <class T>
  [[nodiscard]] std::optional<T> get(const std::string &pass_name,
                                     const structural_hash_t key) const {
    const auto pass_it = buckets.find(pass_name);
    if (pass_it == buckets.end())
      return std::nullopt;
    const auto value_it = pass_it->second.find(key);
    if (value_it == pass_it->second.end())
      return std::nullopt;
    const auto &v = value_it->second;
    if constexpr (std::is_same_v<std::decay_t<T>, bool>) {
      if (const auto *p = std::get_if<bool>(&v))
        return *p;
    } else if constexpr (std::is_integral_v<std::decay_t<T>>) {
      if (const auto *p = std::get_if<std::int64_t>(&v))
        return static_cast<T>(*p);
    } else if constexpr (std::is_floating_point_v<std::decay_t<T>>) {
      if (const auto *p = std::get_if<double>(&v))
        return static_cast<T>(*p);
    } else if constexpr (std::is_same_v<std::decay_t<T>, std::string>) {
      if (const auto *p = std::get_if<std::string>(&v))
        return *p;
    } else {
      if (const auto *any_p = std::get_if<std::any>(&v)) {
        if (const auto *p = std::any_cast<T>(any_p))
          return *p;
      }
    }
    return std::nullopt;
  }
};

struct compilation_trace {
  std::uint64_t started_at_ns = 0;
  std::uint64_t finished_at_ns = 0;
  std::vector<std::string> pass_order;
  std::vector<pass_statistics> passes;
  std::vector<diagnostic> diagnostics;
};

struct pass_context {
  semantic::semantic_registry local_semantic_registry;
  semantic::semantic_registry *semantic_registry_ref = nullptr;
  semantic::backend_capability_registry backend_capability_registry;
  diagnostic_engine diagnostics;
  optimization_budget budget;
  rewrite_statistics rewrites;
  std::vector<pass_statistics> pass_stats;
  pass_local_cache local_cache;
  compilation_trace trace;

  pass_context()
      : semantic_registry_ref(std::addressof(local_semantic_registry)) {}

  explicit pass_context(semantic::semantic_registry &semantic_registry)
      : semantic_registry_ref(std::addressof(semantic_registry)) {}

  [[nodiscard]] semantic::semantic_registry &semantic_registry() {
    return *semantic_registry_ref;
  }

  [[nodiscard]] const semantic::semantic_registry &semantic_registry() const {
    return *semantic_registry_ref;
  }

  void emit_diagnostic(diagnostic diag) {
    diagnostics.emit(diag);
    trace.diagnostics.push_back(std::move(diag));
  }

  void emit_diagnostic(const diagnostic_level level, const diagnostic_code code,
                       std::string message,
                       std::optional<source_span> span = std::nullopt,
                       std::vector<diagnostic_note> notes = {},
                       std::vector<diagnostic> related = {}) {
    emit_diagnostic(diagnostic{.level = level,
                               .stage = lithe::diag::stage::optimization,
                               .code = lithe::diag::to_code_string(code),
                               .message = std::move(message),
                               .span = span,
                               .notes = std::move(notes),
                               .related = std::move(related)});
  }

  [[nodiscard]] bool has_errors() const { return diagnostics.has_errors(); }

  [[nodiscard]] bool should_abort() const {
    return (budget.abort_on_error && has_errors()) ||
           pass_stats.size() >= budget.max_passes ||
           rewrites.total >= budget.max_rewrites ||
           diagnostics.entries.size() >= budget.max_diagnostics;
  }

  void begin_pass(std::string pass_name) {
    if (trace.started_at_ns == 0) {
      trace.started_at_ns = now_ns();
    }
    active_pass_ =
        pass_statistics{std::move(pass_name), now_ns(), 0, 0, 0, false};
    trace.pass_order.push_back(active_pass_->name);
#if LITHE_HAS_NADI
    active_pass_->nadi_event_id = utils::nadi::generate_event_id().value;
    active_pass_->nadi_trace_id =
        utils::nadi::detail::current_lineage.root_id.value;
    active_pass_->nadi_parent_id =
        utils::nadi::detail::current_lineage.trace_id.value;
#endif
  }

  void end_pass(const bool changed) {
    if (!active_pass_.has_value()) {
      return;
    }
    active_pass_->changed = changed;
    active_pass_->end_ns = now_ns();
    pass_stats.push_back(*active_pass_);
    trace.passes.push_back(*active_pass_);
#if LITHE_HAS_THREAD_LOCAL_SINK
    if (active_pass_->nadi_event_id != 0) {
      using namespace utils::nadi;
      using P = Pulse<FixedString{"lithe.pass"}>;
      P pulse{};
      pulse.id = EventId{active_pass_->nadi_event_id};
      pulse.phase = PulsePhase::Duration;
      pulse.timestamp_ns = active_pass_->end_ns;
      pulse.trace_id = active_pass_->nadi_trace_id;
      pulse.parent_id = active_pass_->nadi_parent_id;
      route_pulse<ThreadLocalSink>(pulse);
    }
#endif
    active_pass_.reset();
    trace.finished_at_ns = now_ns();
  }

  void increment_rewrite_count(const std::string_view pass_name = {},
                               const std::size_t delta = 1) {
    rewrites.total += delta;
    const std::string effective_name = resolve_pass_name(pass_name);
    if (!effective_name.empty()) {
      rewrites.by_pass[effective_name] += delta;
      if (active_pass_.has_value() && active_pass_->name == effective_name) {
        active_pass_->rewrite_count += delta;
      }
    }
  }

  void record_cache_hit(const std::string_view pass_name = {}) {
    const std::string effective_name = resolve_pass_name(pass_name);
    if (active_pass_.has_value() && active_pass_->name == effective_name) {
      ++active_pass_->cache_hits;
    }
  }

private:
  std::optional<pass_statistics> active_pass_;

  [[nodiscard]] static std::uint64_t now_ns() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
  }

  [[nodiscard]] std::string
  resolve_pass_name(const std::string_view explicit_name) const {
    if (!explicit_name.empty()) {
      return std::string{explicit_name};
    }
    if (active_pass_.has_value()) {
      return active_pass_->name;
    }
    return {};
  }
};

struct identity_pass {
  template <class E> constexpr decltype(auto) operator()(E &&e) const {
    return std::forward<E>(e);
  }
};

template <class P, class E>
concept Pass = std::invocable<P, E>;

namespace detail {
template <class P, class E>
constexpr auto invoke_pass(P &&pass, E &&expr, pass_context &ctx) {
  if constexpr (std::invocable<P, E, pass_context &>) {
    return std::forward<P>(pass)(std::forward<E>(expr), ctx);
  } else {
    return std::forward<P>(pass)(std::forward<E>(expr));
  }
}

template <bool ObservabilityEnabled, class Observer, class E>
constexpr auto apply_passes_observed(E &&e, pass_context &, Observer &,
                                     std::size_t &) {
  return std::forward<E>(e);
}

template <bool ObservabilityEnabled, class Observer, class E, class P,
          class... Rest>
constexpr auto
apply_passes_observed(E &&e, pass_context &ctx, Observer &observer,
                      std::size_t &pass_index, P &&p, Rest &&...rest) {
#if LITHE_HAS_PROFILER
  profiler::ScopedProfiler _pass_prof{std::string{"lithe.pass_"} +
                                      std::to_string(pass_index)};
#endif
  const auto start = observability::now_ns();
  const auto rewrites_before = ctx.rewrites.total;

  std::string before_dump;
  structural_hash_t before_hash = 0;
  structural_hash_t before_structural = 0;
  if constexpr (ObservabilityEnabled) {
    before_dump = observability::dump_ir(e);
    before_hash = observability::hash_text(before_dump);
    before_structural = structural_hash(e);
  }

  auto mid = invoke_pass(std::forward<P>(p), std::forward<E>(e), ctx);

  std::string after_dump;
  structural_hash_t after_hash = 0;
  structural_hash_t after_structural = 0;
  if constexpr (ObservabilityEnabled) {
    after_dump = observability::dump_ir(mid);
    after_hash = observability::hash_text(after_dump);
    after_structural = structural_hash(mid);
  }

  const auto end = observability::now_ns();
  const auto rewrites_after = ctx.rewrites.total;
  const auto rewrites_applied =
      rewrites_after >= rewrites_before ? rewrites_after - rewrites_before : 0;
  const auto changed = rewrites_applied > 0 || (before_hash != after_hash);
  const auto pass_name = std::string{"pass_"} + std::to_string(pass_index);
  auto ir_diff = observability::diff_ir(before_dump, after_dump);

  observability::emit<ObservabilityEnabled>(observer, [&] {
    observability::pass_event ev;
    ev.pass_name = pass_name;
    ev.pass_index = pass_index;
    ev.start_ns = start;
    ev.end_ns = end;
    ev.changed = changed;
    ev.ir_before_dump = std::move(before_dump);
    ev.ir_after_dump = std::move(after_dump);
    ev.ir_diff = std::move(ir_diff);
    ev.input_hash = before_hash;
    ev.output_hash = after_hash;
    ev.input_structural_hash = before_structural;
    ev.output_structural_hash = after_structural;
    ev.pass_cost_ns = (end >= start) ? (end - start) : 0u;
    ev.iterations = 1;
    return ev;
  }());
  observability::emit<ObservabilityEnabled>(
      observer, observability::rewrite_event{pass_name, rewrites_applied,
                                             rewrites_applied, end});
  observability::emit<ObservabilityEnabled>(
      observer, observability::structural_hash_event{pass_name, after_hash,
                                                     after_structural, end});

  ++pass_index;
  return apply_passes_observed<ObservabilityEnabled>(
      std::move(mid), ctx, observer, pass_index, std::forward<Rest>(rest)...);
}
} // namespace detail

template <class E> constexpr E apply_passes(E &&e) {
  return std::forward<E>(e);
}

template <class E> constexpr E apply_passes(E &&e, pass_context &) {
  return std::forward<E>(e);
}

template <class E, class P, class... Rest>
constexpr auto apply_passes(E &&e, P &&p, Rest &&...rest) {
  auto mid = std::forward<P>(p)(std::forward<E>(e));
  return apply_passes(std::move(mid), std::forward<Rest>(rest)...);
}

template <class E, class P, class... Rest>
constexpr auto apply_passes(E &&e, pass_context &ctx, P &&p, Rest &&...rest) {
  auto mid = detail::invoke_pass(std::forward<P>(p), std::forward<E>(e), ctx);
  return apply_passes(std::move(mid), ctx, std::forward<Rest>(rest)...);
}

template <class E, class... Passes>
constexpr auto compile(E &&e, Passes &&...ps) {
  if constexpr (sizeof...(ps) == 0) {
    return std::forward<E>(e);
  } else {
    return apply_passes(std::forward<E>(e), std::forward<Passes>(ps)...);
  }
}

template <class E, class... Passes>
constexpr auto compile(E &&e, pass_context &ctx, Passes &&...ps) {
  if constexpr (sizeof...(ps) == 0) {
    return std::forward<E>(e);
  } else {
    return apply_passes(std::forward<E>(e), ctx, std::forward<Passes>(ps)...);
  }
}

template <bool ObservabilityEnabled = observability::enabled_by_default,
          class E, class Observer = observability::null_observer,
          class... Passes>
constexpr auto compile_observed(E &&e, pass_context &ctx, Observer &observer,
                                Passes &&...ps) {
  observability::emit<ObservabilityEnabled>(
      observer, observability::compilation_event{
                    observability::compilation_event::kind::started,
                    "passes::compile", observability::now_ns()});

  auto emit_failure = [&](std::string_view msg) {
    observability::emit<ObservabilityEnabled>(
        observer,
        observability::codegen_diagnostic_event{
            static_cast<std::uint8_t>(diagnostic_level::error),
            "passes::compile", std::string{msg}, observability::now_ns()});
    observability::emit<ObservabilityEnabled>(
        observer, observability::compilation_event{
                      observability::compilation_event::kind::failed,
                      "passes::compile", observability::now_ns()});
    ctx.emit_diagnostic(diagnostic_level::error,
                        diagnostic_code::lowering_failed, std::string{msg});
  };

  try {
    if constexpr (sizeof...(ps) == 0) {
      observability::emit<ObservabilityEnabled>(
          observer, observability::compilation_event{
                        observability::compilation_event::kind::finished,
                        "passes::compile", observability::now_ns()});
      return std::forward<E>(e);
    } else {
      std::size_t pass_index = 0;
      auto out = detail::apply_passes_observed<ObservabilityEnabled>(
          std::forward<E>(e), ctx, observer, pass_index,
          std::forward<Passes>(ps)...);
      observability::emit<ObservabilityEnabled>(
          observer, observability::compilation_event{
                        observability::compilation_event::kind::finished,
                        "passes::compile", observability::now_ns()});
      return out;
    }
  } catch (const std::exception &ex) {
    emit_failure(ex.what());
    throw;
  } catch (...) {
    emit_failure("unknown pass failure");
    throw;
  }
}

template <class E, class PassType>
  requires std::invocable<PassType, E> &&
           std::convertible_to<std::invoke_result_t<PassType, E>, E>
constexpr auto optimize(E e, PassType p, int max_iters) {
  if (max_iters <= 0)
    max_iters = 1;
  for (int i = 0; i < max_iters; ++i) {
    auto next = p(std::move(e));
    if (emit::structural_equal(next, e)) {
      return next;
    }
    e = std::move(next);
  }
  return e;
}

template <class E, class Pass>
  requires std::invocable<Pass, E> &&
           std::invocable<Pass, std::invoke_result_t<Pass, E>>
constexpr auto optimize_any(E e, Pass p, int max_iters) {
  if (max_iters <= 0)
    max_iters = 1;
  auto cur = p(std::forward<E>(e));
  for (int i = 1; i < max_iters; ++i) {
    auto next = p(cur);
    if (emit::structural_equal(next, cur)) {
      return next;
    }
    cur = std::move(next);
  }
  return cur;
}

template <class IR, class E> constexpr IR to_ir(E &&e) {
  return IR{std::forward<E>(e)};
}

struct context {
  bool trace = false;
  int passes_run = 0;
  std::vector<std::string> logs;
};
} // namespace compiler

// -----------------------------
// Passes: conservative, self-documenting pass objects
// -----------------------------
namespace passes {
template <class E> constexpr auto canonicalize(E &&expr);

namespace detail {
template <class E> constexpr decltype(auto) phase_unwrap(E &&expr) {
  return lithe::unwrap_expr(std::forward<E>(expr));
}

template <class InExpr, class OutExpr>
constexpr auto wrap_canonical(OutExpr &&out) {
  if constexpr (lithe::is_surface_expr_v<InExpr> ||
                lithe::is_canonical_expr_v<InExpr>) {
    return lithe::canonical_expr<
        std::decay_t<decltype(phase_unwrap(std::forward<OutExpr>(out)))>>{
        phase_unwrap(std::forward<OutExpr>(out))};
  } else {
    return phase_unwrap(std::forward<OutExpr>(out));
  }
}

template <class InExpr, class OutExpr>
constexpr auto wrap_optimized(OutExpr &&out) {
  if constexpr (lithe::is_canonical_expr_v<InExpr> ||
                lithe::is_optimized_expr_v<InExpr>) {
    return lithe::optimized_expr<
        std::decay_t<decltype(phase_unwrap(std::forward<OutExpr>(out)))>>{
        phase_unwrap(std::forward<OutExpr>(out))};
  } else {
    return phase_unwrap(std::forward<OutExpr>(out));
  }
}
} // namespace detail

struct dag_node {
  structural_hash_t structural_id = 0;
  std::string op;
  std::vector<structural_hash_t> operands;
  std::optional<semantic::semantic_info> semantic;

  [[nodiscard]] bool is_leaf() const { return operands.empty(); }
};

using shared_expr = std::shared_ptr<const dag_node>;

struct dag_region {
  std::string name = "root";
  std::vector<structural_hash_t> roots;
  std::vector<structural_hash_t> ordered_nodes;
};

struct dag_expr {
  shared_expr root;
  dag_region region;
  std::unordered_map<structural_hash_t, shared_expr> nodes;

  [[nodiscard]] bool empty() const { return root == nullptr; }

  [[nodiscard]] std::size_t node_count() const { return nodes.size(); }
};

class structural_intern_table {
public:
  [[nodiscard]] shared_expr intern(dag_node node) {
    const auto id = node.structural_id;
    if (const auto it = pool_.find(id); it != pool_.end()) {
      return it->second;
    }
    auto interned = std::make_shared<const dag_node>(std::move(node));
    pool_.insert_or_assign(id, interned);
    return interned;
  }

  [[nodiscard]] std::optional<shared_expr>
  find(const structural_hash_t id) const {
    if (const auto it = pool_.find(id); it != pool_.end()) {
      return it->second;
    }
    return std::nullopt;
  }

  [[nodiscard]] const std::unordered_map<structural_hash_t, shared_expr> &
  entries() const {
    return pool_;
  }

  [[nodiscard]] std::size_t size() const { return pool_.size(); }

private:
  std::unordered_map<structural_hash_t, shared_expr> pool_;
};

namespace detail {
inline std::optional<semantic::semantic_info>
resolve_semantics(const structural_hash_t id,
                  const semantic::semantic_registry *registry) {
  if (registry != nullptr) {
    return registry->get(id);
  }
  return semantic::get_semantics(semantic::semantic_node::from_key(id));
}

struct canonical_dag_builder {
  structural_intern_table *intern = nullptr;
  const semantic::semantic_registry *semantic_registry = nullptr;

  template <class T> structural_hash_t on_terminal(T &&terminal) const {
    const auto id = lithe::structural_key(terminal);
    dag_node node;
    node.structural_id = id;
    node.op = emit::dump(terminal);
    node.semantic = resolve_semantics(id, semantic_registry);
    (void)intern->intern(std::move(node));
    return id;
  }

  template <class Tag, class... ChildIds>
  structural_hash_t on_node(Tag, ChildIds... child_ids) const {
    std::vector<structural_hash_t> operands{
        static_cast<structural_hash_t>(child_ids)...};
    std::size_t id = emit::tag_id<Tag>::value;
    for (const auto child : operands) {
      id = emit::hash_combine(id, child);
    }

    dag_node node;
    node.structural_id = id;
    node.op = emit::tag_name<Tag>::value;
    node.operands = std::move(operands);
    node.semantic = resolve_semantics(node.structural_id, semantic_registry);
    (void)intern->intern(std::move(node));
    return id;
  }
};
} // namespace detail

template <class CanonicalExpr>
[[nodiscard]] dag_expr
to_dag_expr(const canonical_expr<CanonicalExpr> &canonical,
            const semantic::semantic_registry *semantic_registry = nullptr,
            std::string region_name = "root") {
  structural_intern_table intern;
  detail::canonical_dag_builder builder{std::addressof(intern),
                                        semantic_registry};
  const auto root_id = lithe::visit(detail::phase_unwrap(canonical), builder);

  dag_region region;
  region.name = std::move(region_name);
  region.roots.push_back(root_id);
  region.ordered_nodes.reserve(intern.size());
  for (const auto &[id, node] : intern.entries()) {
    (void)node;
    region.ordered_nodes.push_back(id);
  }
  std::ranges::sort(region.ordered_nodes);

  dag_expr out;
  out.region = std::move(region);
  out.nodes = intern.entries();
  if (const auto root = intern.find(root_id); root.has_value()) {
    out.root = *root;
  }
  return out;
}

template <class Expr>
[[nodiscard]] dag_expr
to_canonical_dag(const Expr &expr,
                 const semantic::semantic_registry *semantic_registry = nullptr,
                 std::string region_name = "root") {
  auto canonical = canonicalize(expr);
  return to_dag_expr(canonical, semantic_registry, std::move(region_name));
}

template <auto Name, class Pass, class... Dependencies> struct pass_descriptor {
  static constexpr auto name = Name;
  using pass_type = Pass;
  using dependencies = std::tuple<Dependencies...>;
};

template <class... Descriptors> struct pass_bundle {};

namespace detail {
template <class Bundle, class Descriptor> struct bundle_contains;

template <class Descriptor, class... Ds>
struct bundle_contains<pass_bundle<Ds...>, Descriptor>
    : std::bool_constant<(std::is_same_v<Descriptor, Ds> || ...)> {};

template <class Bundle, class Descriptor> struct bundle_append;

template <class... Ds, class Descriptor>
struct bundle_append<pass_bundle<Ds...>, Descriptor> {
  using type = pass_bundle<Ds..., Descriptor>;
};

template <class Bundle, class Descriptor> struct bundle_remove;

template <class Descriptor> struct bundle_remove<pass_bundle<>, Descriptor> {
  using type = pass_bundle<>;
};

template <class Descriptor, class Head, class... Tail>
struct bundle_remove<pass_bundle<Head, Tail...>, Descriptor> {
private:
  using tail_removed = bundle_remove<pass_bundle<Tail...>, Descriptor>::type;

public:
  using type =
      std::conditional_t<std::is_same_v<Head, Descriptor>, pass_bundle<Tail...>,
                         typename bundle_append<tail_removed, Head>::type>;
};

template <class Descriptor, class Ordered> struct dependencies_satisfied;

template <auto Name, class Pass, class... Deps, class Ordered>
struct dependencies_satisfied<pass_descriptor<Name, Pass, Deps...>, Ordered>
    : std::bool_constant<(bundle_contains<Ordered, Deps>::value && ...)> {};

template <class Remaining, class Ordered> struct first_ready_descriptor;

template <class Ordered> struct first_ready_descriptor<pass_bundle<>, Ordered> {
  using type = void;
};

template <class Ordered, class Head, class... Tail>
struct first_ready_descriptor<pass_bundle<Head, Tail...>, Ordered> {
  using type = std::conditional_t<
      dependencies_satisfied<Head, Ordered>::value, Head,
      typename first_ready_descriptor<pass_bundle<Tail...>, Ordered>::type>;
};

template <class Ordered, class Remaining> struct topo_sort_impl;

template <bool HasReady, class Ordered, class Remaining, class Next>
struct topo_sort_step;

template <class Ordered, class Remaining, class Next>
struct topo_sort_step<true, Ordered, Remaining, Next> {
  using type =
      topo_sort_impl<typename bundle_append<Ordered, Next>::type,
                     typename bundle_remove<Remaining, Next>::type>::type;
};

template <class Ordered, class Remaining, class Next>
struct topo_sort_step<false, Ordered, Remaining, Next> {
  using type = void;
};

template <class Ordered> struct topo_sort_impl<Ordered, pass_bundle<>> {
  using type = Ordered;
};

template <class Ordered, class... Remaining>
struct topo_sort_impl<Ordered, pass_bundle<Remaining...>> {
private:
  using remaining_bundle = pass_bundle<Remaining...>;
  using next = first_ready_descriptor<remaining_bundle, Ordered>::type;

public:
  using type = topo_sort_step<!std::is_void_v<next>, Ordered, remaining_bundle,
                              next>::type;
};

template <class Bundle> struct dependency_closure_ok;

template <class... Ds>
struct dependency_closure_ok<pass_bundle<Ds...>>
    : std::bool_constant<((bundle_contains<pass_bundle<Ds...>, Ds>::value) &&
                          ...)> {};

template <class Bundle, class Descriptor> struct descriptor_deps_present;

template <class Bundle, auto Name, class Pass, class... Deps>
struct descriptor_deps_present<Bundle, pass_descriptor<Name, Pass, Deps...>>
    : std::bool_constant<(bundle_contains<Bundle, Deps>::value && ...)> {};

template <class Bundle> struct all_deps_present;

template <class... Ds>
struct all_deps_present<pass_bundle<Ds...>>
    : std::bool_constant<(
          descriptor_deps_present<pass_bundle<Ds...>, Ds>::value && ...)> {};
} // namespace detail

template <class Bundle, class Descriptor>
struct contains_pass : detail::bundle_contains<Bundle, Descriptor> {};

template <class Bundle, class Descriptor>
inline constexpr bool contains_pass_v =
    contains_pass<Bundle, Descriptor>::value;

template <class Descriptor, class Dependency> struct depends_on;

template <auto Name, class Pass, class... Deps, class Dependency>
struct depends_on<pass_descriptor<Name, Pass, Deps...>, Dependency>
    : std::bool_constant<(std::is_same_v<Dependency, Deps> || ...)> {};

template <class Descriptor, class Dependency>
inline constexpr bool depends_on_v = depends_on<Descriptor, Dependency>::value;

template <class Bundle> struct order_pass_bundle {
  using type = detail::topo_sort_impl<pass_bundle<>, Bundle>::type;
};

template <class Bundle>
using order_pass_bundle_t = order_pass_bundle<Bundle>::type;

template <class Bundle>
struct validate_pass_bundle
    : std::bool_constant<detail::all_deps_present<Bundle>::value &&
                         !std::is_void_v<order_pass_bundle_t<Bundle>>> {};

template <class Bundle>
inline constexpr bool validate_pass_bundle_v =
    validate_pass_bundle<Bundle>::value;

enum class pass_category {
  analysis,
  normalization,
  canonicalization,
  optimization,
  lowering_prep,
  semantic_validation,
  graph_optimization
};

// Describes the observable role of a pass independently of its pipeline
// category. An optimization-category pass is not necessarily a
// transformation yet: some names are retained for API compatibility
// while their implementation is analysis-only or a placeholder.
enum class pass_effect_kind : std::uint8_t {
  transforms,
  analyzes,
  annotates,
  placeholder
};

[[nodiscard]] constexpr std::string_view
to_string(const pass_effect_kind effect) noexcept {
  switch (effect) {
  case pass_effect_kind::transforms:
    return "transforms";
  case pass_effect_kind::analyzes:
    return "analyzes";
  case pass_effect_kind::annotates:
    return "annotates";
  case pass_effect_kind::placeholder:
    return "placeholder";
  }
  return "unknown";
}

enum class pass_stage {
  pre_analysis,
  analysis,
  normalization,
  canonicalization,
  optimization,
  lowering_prep,
  semantic_validation,
  graph_optimization,
  post
};

// =====================================================================
//  ir_stage — monotone IR transformation stage for pass ordering checks.
//  Values are ordered: surface ≤ canonical ≤ optimized ≤ lowered.
//  Distinct from the runtime pass_stage enum used by the dynamic planner.
// =====================================================================

enum class ir_stage : std::uint8_t {
  surface = 0,
  canonical = 1,
  optimized = 2,
  lowered = 3
};

// =====================================================================
//  pass_type_traits<Pass> — compile-time metadata keyed on the pass type.
//
//  Mirrors the tag_descriptor<Tag> specialization pattern.
//  Users specialize this for their own passes; built-in passes have
//  explicit specializations below their definitions.
//
//  stable_id bands:
//    [0, 1000)  — built-in passes (kExtensionIdBase from lithe_core.hpp)
//    [1000, ∞)  — extension / plugin passes
// =====================================================================

struct pass_type_traits_base {
  static constexpr auto id = lithe::fixed_string{"unknown"};
  static constexpr lithe::version_triple version{0, 0, 0};
  static constexpr pass_category category = pass_category::optimization;
  static constexpr pass_effect_kind effect = pass_effect_kind::transforms;
  static constexpr ir_stage in_stage = ir_stage::surface;
  static constexpr ir_stage out_stage = ir_stage::surface;
  static constexpr std::size_t stable_id = 0;

  [[nodiscard]] static constexpr algorithms::preserved_analysis_set
  preserved() noexcept {
    return algorithms::preserved_analysis_set::none_set();
  }

  static constexpr std::array<std::size_t, 0> conflicts{};
};

template <class Pass> struct pass_type_traits : pass_type_traits_base {};

template <class Pass>
concept StaticPassTraits = requires {
  {
    pass_type_traits<Pass>::id.view()
  } -> std::convertible_to<std::string_view>;
  { pass_type_traits<Pass>::stable_id } -> std::convertible_to<std::size_t>;
  { pass_type_traits<Pass>::in_stage } -> std::convertible_to<ir_stage>;
  { pass_type_traits<Pass>::out_stage } -> std::convertible_to<ir_stage>;
};

// =====================================================================
//  Consteval introspection over pass_bundle<Descriptors...>
//  All helpers operate on the topo-sorted bundle; zero runtime cost.
// =====================================================================

namespace detail {
template <class Bundle> struct bundle_pass_types;

template <class... Ds> struct bundle_pass_types<pass_bundle<Ds...>> {
  using type = std::tuple<typename Ds::pass_type...>;
};

template <class Bundle>
using bundle_pass_types_t = typename bundle_pass_types<Bundle>::type;

// bundle_size_v<Bundle>: struct-based, avoids function template partial spec.
template <class Bundle> struct bundle_size_v;

template <class... Ds>
struct bundle_size_v<pass_bundle<Ds...>>
    : std::integral_constant<std::size_t, sizeof...(Ds)> {};

// tuple_has_category: any pass in Tuple has pass_category C.
template <pass_category C, class Tuple>
struct tuple_has_category_impl : std::false_type {};

template <pass_category C, class P, class... Ps>
struct tuple_has_category_impl<C, std::tuple<P, Ps...>>
    : std::conditional_t<pass_type_traits<P>::category == C, std::true_type,
                         tuple_has_category_impl<C, std::tuple<Ps...>>> {};

// stages_monotone: walk ordered passes; running max(out_stage) must not
// exceed the next pass's in_stage.
template <ir_stage RunningMax, class Tuple>
struct stages_monotone_impl : std::true_type {};

template <ir_stage RunningMax, class P, class... Ps>
struct stages_monotone_impl<RunningMax, std::tuple<P, Ps...>> {
private:
  static constexpr ir_stage in_s = pass_type_traits<P>::in_stage;
  static constexpr ir_stage out_s = pass_type_traits<P>::out_stage;
  // in_stage must be at least the running max of prior out_stages.
  static constexpr bool in_ok =
      static_cast<std::uint8_t>(RunningMax) <= static_cast<std::uint8_t>(in_s);
  // out_stage must not regress below in_stage.
  static constexpr bool out_ok =
      static_cast<std::uint8_t>(in_s) <= static_cast<std::uint8_t>(out_s);
  static constexpr ir_stage
      next_max = static_cast<std::uint8_t>(out_s) >
                         static_cast<std::uint8_t>(RunningMax)
                     ? out_s
                     : RunningMax;

public:
  static constexpr bool value =
      in_ok && out_ok &&
      stages_monotone_impl<next_max, std::tuple<Ps...>>::value;
};

// conflicts_with_any: check if pass P conflicts with any pass in Tuple.
template <class P, class Tuple>
struct conflicts_with_any_impl : std::false_type {};

template <class P, class Q, class... Qs>
struct conflicts_with_any_impl<P, std::tuple<Q, Qs...>> {
private:
  static constexpr auto arr = pass_type_traits<P>::conflicts;
  static constexpr bool hits_Q = []() consteval {
    if constexpr (std::is_same_v<P, Q>)
      return false;
    for (std::size_t i = 0; i < arr.size(); ++i)
      if (arr[i] == pass_type_traits<Q>::stable_id)
        return true;
    return false;
  }();

public:
  static constexpr bool value =
      hits_Q || conflicts_with_any_impl<P, std::tuple<Qs...>>::value;
};

// no_conflicts_impl: for each pass in Remaining, check no conflict with
// AllPasses.
template <class AllPasses, class Remaining>
struct no_conflicts_impl : std::true_type {};

template <class AllPasses, class P, class... Ps>
struct no_conflicts_impl<AllPasses, std::tuple<P, Ps...>> {
  static constexpr bool value =
      !conflicts_with_any_impl<P, AllPasses>::value &&
      no_conflicts_impl<AllPasses, std::tuple<Ps...>>::value;
};
} // namespace detail

// bundle_size<Bundle>() — number of pass descriptors in a bundle.
template <class Bundle>
[[nodiscard]] consteval std::size_t bundle_size() noexcept {
  return detail::bundle_size_v<Bundle>::value;
}

// bundle_has_category<Bundle, C>() — true iff any pass has category C.
template <class Bundle, pass_category C>
[[nodiscard]] consteval bool bundle_has_category() noexcept {
  using sorted = order_pass_bundle_t<Bundle>;
  using passes = detail::bundle_pass_types_t<sorted>;
  return detail::tuple_has_category_impl<C, passes>::value;
}

// stages_monotone<Bundle>() — true iff topo-sorted passes have non-regressing
// ir_stage.
template <class Bundle>
[[nodiscard]] consteval bool stages_monotone() noexcept {
  using sorted = order_pass_bundle_t<Bundle>;
  using passes = detail::bundle_pass_types_t<sorted>;
  return detail::stages_monotone_impl<ir_stage::surface, passes>::value;
}

// no_conflicts<Bundle>() — true iff no pass conflicts with another in the
// bundle.
template <class Bundle> [[nodiscard]] consteval bool no_conflicts() noexcept {
  using sorted = order_pass_bundle_t<Bundle>;
  using passes = detail::bundle_pass_types_t<sorted>;
  return detail::no_conflicts_impl<passes, passes>::value;
}

struct pass_metadata {
  std::size_t id = 0;
  std::string name;
  pass_category category = pass_category::optimization;
  pass_effect_kind effect = pass_effect_kind::transforms;
  pass_stage stage = pass_stage::optimization;
  std::vector<std::size_t> dependencies;
  bool enabled = true;
  bool optional = false;
  int priority = 0;
};

struct pass_dependency_edge {
  std::size_t from = 0;
  std::size_t to = 0;
};

struct pass_dependency_issue {
  std::size_t pass_id = 0;
  std::size_t dependency_id = 0;
};

struct pass_dependency_graph {
  std::unordered_map<std::size_t, pass_metadata> nodes;

  void add(pass_metadata metadata) {
    nodes.insert_or_assign(metadata.id, std::move(metadata));
  }

  void set_enabled(const std::size_t id, const bool enabled) {
    if (auto it = nodes.find(id); it != nodes.end()) {
      it->second.enabled = enabled;
    }
  }

  [[nodiscard]] bool contains(const std::size_t id) const {
    return nodes.find(id) != nodes.end();
  }

  [[nodiscard]] std::vector<pass_dependency_issue>
  missing_dependencies() const {
    std::vector<pass_dependency_issue> missing;
    for (const auto &[id, meta] : nodes) {
      if (!meta.enabled) {
        continue;
      }
      for (const auto dep : meta.dependencies) {
        if (!contains(dep) || !nodes.at(dep).enabled) {
          missing.push_back(pass_dependency_issue{id, dep});
        }
      }
    }
    return missing;
  }

  [[nodiscard]] std::vector<pass_dependency_edge> edges() const {
    std::vector<pass_dependency_edge> out;
    for (const auto &[id, meta] : nodes) {
      if (!meta.enabled) {
        continue;
      }
      for (const auto dep : meta.dependencies) {
        if (!contains(dep) || !nodes.at(dep).enabled) {
          continue;
        }
        out.push_back(pass_dependency_edge{dep, id});
      }
    }
    return out;
  }
};

struct pass_execution_plan {
  bool valid = true;
  bool has_cycle = false;
  std::vector<pass_metadata> ordered_passes;
  std::unordered_map<pass_stage, std::vector<std::size_t>> stage_groups;
  std::vector<std::size_t> disabled_passes;
  std::vector<pass_dependency_issue> missing_dependencies;
  std::vector<std::size_t> cycle_nodes;
};

namespace detail {
[[nodiscard]] constexpr int stage_rank(pass_stage stage) {
  return static_cast<int>(stage);
}

[[nodiscard]] inline bool pass_less(const pass_metadata &a,
                                    const pass_metadata &b) {
  if (a.stage != b.stage) {
    return stage_rank(a.stage) < stage_rank(b.stage);
  }
  if (a.priority != b.priority) {
    return a.priority > b.priority;
  }
  return a.id < b.id;
}
} // namespace detail

class pass_scheduler {
public:
  [[nodiscard]] static pass_execution_plan
  build_plan(const pass_dependency_graph &graph) {
    pass_execution_plan plan;

    for (const auto &[id, meta] : graph.nodes) {
      if (!meta.enabled) {
        plan.disabled_passes.push_back(id);
      }
    }
    std::sort(plan.disabled_passes.begin(), plan.disabled_passes.end());

    plan.missing_dependencies = graph.missing_dependencies();
    if (!plan.missing_dependencies.empty()) {
      plan.valid = false;
      return plan;
    }

    std::unordered_map<std::size_t, std::size_t> indegree;
    std::unordered_map<std::size_t, std::vector<std::size_t>> adjacency;

    for (const auto &[id, meta] : graph.nodes) {
      if (!meta.enabled) {
        continue;
      }
      indegree.try_emplace(id, 0);
    }

    for (const auto &edge : graph.edges()) {
      adjacency[edge.from].push_back(edge.to);
      ++indegree[edge.to];
    }

    std::deque<std::size_t> ready;
    for (const auto &[id, deg] : indegree) {
      if (deg == 0) {
        ready.push_back(id);
      }
    }

    auto sort_ready = [&]() {
      std::sort(ready.begin(), ready.end(),
                [&](const std::size_t lhs, const std::size_t rhs) {
                  return detail::pass_less(graph.nodes.at(lhs),
                                           graph.nodes.at(rhs));
                });
    };

    sort_ready();
    while (!ready.empty()) {
      const auto next_id = ready.front();
      ready.pop_front();

      const auto &meta = graph.nodes.at(next_id);
      plan.ordered_passes.push_back(meta);
      plan.stage_groups[meta.stage].push_back(next_id);

      if (auto it = adjacency.find(next_id); it != adjacency.end()) {
        for (const auto succ : it->second) {
          auto &deg = indegree[succ];
          if (deg > 0) {
            --deg;
            if (deg == 0) {
              ready.push_back(succ);
            }
          }
        }
      }
      sort_ready();
    }

    if (plan.ordered_passes.size() != indegree.size()) {
      plan.valid = false;
      plan.has_cycle = true;
      for (const auto &[id, deg] : indegree) {
        if (deg > 0) {
          plan.cycle_nodes.push_back(id);
        }
      }
      std::sort(plan.cycle_nodes.begin(), plan.cycle_nodes.end());
    }

    return plan;
  }
};

template <class Descriptor> struct pass_traits {
  static constexpr pass_category category = pass_category::optimization;
  static constexpr pass_effect_kind effect = pass_effect_kind::transforms;
  static constexpr pass_stage stage = pass_stage::optimization;
  static constexpr bool enabled_by_default = true;
  static constexpr bool optional = false;
  static constexpr int priority = 0;
};

// Keep existing pass_traits specializations source-compatible: custom
// descriptors written before pass_effect_kind was introduced default to
// a transforming pass unless they opt in with an explicit effect member.
template <class Descriptor>
inline constexpr pass_effect_kind pass_effect_of_v = [] {
  if constexpr (requires { pass_traits<Descriptor>::effect; }) {
    return pass_traits<Descriptor>::effect;
  } else {
    return pass_effect_kind::transforms;
  }
}();

template <class Descriptor>
[[nodiscard]] pass_metadata
make_pass_metadata(const std::size_t id, std::string name,
                   std::vector<std::size_t> dependencies = {}) {
  return pass_metadata{id,
                       std::move(name),
                       pass_traits<Descriptor>::category,
                       pass_effect_of_v<Descriptor>,
                       pass_traits<Descriptor>::stage,
                       std::move(dependencies),
                       pass_traits<Descriptor>::enabled_by_default,
                       pass_traits<Descriptor>::optional,
                       pass_traits<Descriptor>::priority};
}

enum class diff_kind { unchanged, replaced, inserted, removed, reordered };

struct diff_entry {
  diff_kind kind = diff_kind::unchanged;
  std::vector<std::size_t> path;
  structural_hash_t old_hash = 0;
  structural_hash_t new_hash = 0;
};

struct structural_diff_result {
  structural_hash_t old_root_hash = 0;
  structural_hash_t new_root_hash = 0;
  bool equivalent = false;
  std::vector<diff_entry> entries;
};

struct invalidation_hint {
  bool equivalent = false;
  bool has_reorder = false;
  bool has_replacements = false;
  bool has_insert_or_remove = false;
  bool requires_partial_rebuild = false;
  bool requires_full_rebuild = false;
  std::size_t affected_entries = 0;
};

using diff_path = std::vector<std::size_t>;

struct subtree_diff {
  diff_kind kind = diff_kind::unchanged;
  diff_path old_path;
  diff_path new_path;
  structural_hash_t old_hash = 0;
  structural_hash_t new_hash = 0;
  bool reused = false;
  bool moved = false;
  bool reordered = false;
  bool canonical_equivalent = false;
  bool semantic_equivalent = false;
};

struct change_region {
  diff_path path;
  std::size_t depth = 0;
  structural_hash_t old_hash = 0;
  structural_hash_t new_hash = 0;
  diff_kind dominant_kind = diff_kind::unchanged;
  bool semantic_sensitive = false;
};

struct structural_patch {
  structural_hash_t old_root_hash = 0;
  structural_hash_t new_root_hash = 0;
  bool equivalent = false;
  std::vector<subtree_diff> diffs;
  std::vector<change_region> regions;
};

struct invalidation_region {
  diff_path path;
  structural_hash_t old_hash = 0;
  structural_hash_t new_hash = 0;
  bool structural_invalidation = true;
  bool semantic_invalidation = false;
  std::size_t priority = 0;
};

struct structural_diff_options {
  bool detect_reuse = true;
  bool detect_moves = true;
  bool detect_reordered_children = true;
  bool canonical_equality = true;
  bool semantic_aware = false;
};

namespace detail {
template <class Expr, std::size_t... I>
constexpr auto child_hashes_impl(const Expr &expr, std::index_sequence<I...>) {
  return std::array<structural_hash_t, sizeof...(I)>{
      structural_hash(std::get<I>(expr.children))...};
}

template <class A, class B>
constexpr bool children_reordered(const A &old_expr, const B &new_expr) {
  constexpr std::size_t old_n =
      std::tuple_size_v<std::decay_t<decltype(old_expr.children)>>;
  constexpr std::size_t new_n =
      std::tuple_size_v<std::decay_t<decltype(new_expr.children)>>;

  if constexpr (old_n != new_n || old_n <= 1) {
    return false;
  } else {
    auto old_hashes =
        child_hashes_impl(old_expr, std::make_index_sequence<old_n>{});
    auto new_hashes =
        child_hashes_impl(new_expr, std::make_index_sequence<new_n>{});
    if (old_hashes == new_hashes) {
      return false;
    }

    auto old_sorted = old_hashes;
    auto new_sorted = new_hashes;
    std::sort(old_sorted.begin(), old_sorted.end());
    std::sort(new_sorted.begin(), new_sorted.end());
    return old_sorted == new_sorted;
  }
}

template <diff_kind Kind, std::size_t Start, class Expr, std::size_t... I>
constexpr void append_unmatched_children(const Expr &expr,
                                         std::vector<std::size_t> &path,
                                         structural_diff_result &out,
                                         std::index_sequence<I...>) {
  (
      [&] {
        constexpr std::size_t idx = Start + I;
        path.push_back(idx);
        const auto &child = std::get<idx>(expr.children);
        if constexpr (Kind == diff_kind::removed) {
          out.entries.push_back(
              diff_entry{Kind, path, structural_hash(child), 0});
        } else {
          out.entries.push_back(
              diff_entry{Kind, path, 0, structural_hash(child)});
        }
        path.pop_back();
      }(),
      ...);
}

template <class OldExpr, class NewExpr>
constexpr void diff_impl(const OldExpr &old_expr, const NewExpr &new_expr,
                         std::vector<std::size_t> &path,
                         structural_diff_result &out) {
  if constexpr (VariantExpr<OldExpr> && VariantExpr<NewExpr>) {
    std::visit(
        [&](const auto &old_alt, const auto &new_alt) {
          diff_impl(old_alt, new_alt, path, out);
        },
        old_expr, new_expr);
  } else if constexpr (VariantExpr<OldExpr>) {
    std::visit(
        [&](const auto &old_alt) { diff_impl(old_alt, new_expr, path, out); },
        old_expr);
  } else if constexpr (VariantExpr<NewExpr>) {
    std::visit(
        [&](const auto &new_alt) { diff_impl(old_expr, new_alt, path, out); },
        new_expr);
  } else if (structural_equal(old_expr, new_expr)) {
    return;
  } else if constexpr (Expression<OldExpr> && Expression<NewExpr>) {
    using old_tag = std::decay_t<OldExpr>::tag_type;
    using new_tag = std::decay_t<NewExpr>::tag_type;

    if constexpr (!std::is_same_v<old_tag, new_tag>) {
      out.entries.push_back(diff_entry{diff_kind::replaced, path,
                                       structural_hash(old_expr),
                                       structural_hash(new_expr)});
      return;
    }

    constexpr std::size_t old_n =
        std::tuple_size_v<std::decay_t<decltype(old_expr.children)>>;
    constexpr std::size_t new_n =
        std::tuple_size_v<std::decay_t<decltype(new_expr.children)>>;
    constexpr std::size_t common_n = old_n < new_n ? old_n : new_n;

    if (children_reordered(old_expr, new_expr)) {
      out.entries.push_back(diff_entry{diff_kind::reordered, path,
                                       structural_hash(old_expr),
                                       structural_hash(new_expr)});
    }

    [&]<std::size_t... I>(std::index_sequence<I...>) {
      (
          [&] {
            path.push_back(I);
            diff_impl(std::get<I>(old_expr.children),
                      std::get<I>(new_expr.children), path, out);
            path.pop_back();
          }(),
          ...);
    }(std::make_index_sequence<common_n>{});

    if constexpr (old_n > common_n) {
      append_unmatched_children<diff_kind::removed, common_n>(
          old_expr, path, out, std::make_index_sequence<old_n - common_n>{});
    }
    if constexpr (new_n > common_n) {
      append_unmatched_children<diff_kind::inserted, common_n>(
          new_expr, path, out, std::make_index_sequence<new_n - common_n>{});
    }
  } else {
    out.entries.push_back(diff_entry{diff_kind::replaced, path,
                                     structural_hash(old_expr),
                                     structural_hash(new_expr)});
  }
}
} // namespace detail

template <class OldExpr, class NewExpr>
constexpr structural_diff_result diff(const OldExpr &old_expr,
                                      const NewExpr &new_expr) {
  structural_diff_result result;
  result.old_root_hash = structural_hash(old_expr);
  result.new_root_hash = structural_hash(new_expr);
  result.equivalent = structural_equal(old_expr, new_expr);

  if (result.equivalent) {
    result.entries.push_back(diff_entry{
        diff_kind::unchanged, {}, result.old_root_hash, result.new_root_hash});
    return result;
  }

  std::vector<std::size_t> path;
  detail::diff_impl(old_expr, new_expr, path, result);
  if (result.entries.empty()) {
    result.entries.push_back(diff_entry{
        diff_kind::replaced, {}, result.old_root_hash, result.new_root_hash});
  }
  return result;
}

constexpr invalidation_hint
compute_invalidation(const structural_diff_result &diff_result) {
  invalidation_hint hint;
  hint.equivalent = diff_result.equivalent;

  for (const auto &entry : diff_result.entries) {
    switch (entry.kind) {
    case diff_kind::unchanged:
      break;
    case diff_kind::reordered:
      hint.has_reorder = true;
      ++hint.affected_entries;
      break;
    case diff_kind::replaced:
      hint.has_replacements = true;
      ++hint.affected_entries;
      break;
    case diff_kind::inserted:
    case diff_kind::removed:
      hint.has_insert_or_remove = true;
      ++hint.affected_entries;
      break;
    }
  }

  hint.requires_full_rebuild = hint.has_insert_or_remove;
  hint.requires_partial_rebuild = !hint.requires_full_rebuild &&
                                  (hint.has_replacements || hint.has_reorder);
  return hint;
}

namespace detail {
[[nodiscard]] inline bool is_path_prefix(const diff_path &prefix,
                                         const diff_path &full) {
  if (prefix.size() > full.size()) {
    return false;
  }
  for (std::size_t i = 0; i < prefix.size(); ++i) {
    if (prefix[i] != full[i]) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] inline bool same_semantics(const semantic::semantic_info &a,
                                         const semantic::semantic_info &b) {
  return a.effect == b.effect && a.domain == b.domain &&
         a.ownership == b.ownership && a.purity_level == b.purity_level &&
         a.mutability_kind == b.mutability_kind &&
         a.allocation == b.allocation &&
         a.synchronization == b.synchronization &&
         a.evaluation == b.evaluation &&
         a.capabilities.bits == b.capabilities.bits;
}

template <class Tag>
inline constexpr bool is_commutative_tag_v =
    std::is_same_v<Tag, add_tag> || std::is_same_v<Tag, mul_tag> ||
    std::is_same_v<Tag, and_tag> || std::is_same_v<Tag, or_tag> ||
    std::is_same_v<Tag, bit_and_tag> || std::is_same_v<Tag, bit_or_tag> ||
    std::is_same_v<Tag, bit_xor_tag> || std::is_same_v<Tag, eq_tag> ||
    std::is_same_v<Tag, ne_tag>;

template <class A, class B>
constexpr bool canonical_equal(const A &old_expr, const B &new_expr) {
  if (structural_equal(old_expr, new_expr)) {
    return true;
  }

  if constexpr (VariantExpr<A> && VariantExpr<B>) {
    return std::visit(
        [&](const auto &old_alt, const auto &new_alt) {
          return canonical_equal(old_alt, new_alt);
        },
        old_expr, new_expr);
  } else if constexpr (VariantExpr<A>) {
    return std::visit(
        [&](const auto &old_alt) { return canonical_equal(old_alt, new_expr); },
        old_expr);
  } else if constexpr (VariantExpr<B>) {
    return std::visit(
        [&](const auto &new_alt) { return canonical_equal(old_expr, new_alt); },
        new_expr);
  } else if constexpr (Expression<A> && Expression<B>) {
    using old_tag = std::decay_t<A>::tag_type;
    using new_tag = std::decay_t<B>::tag_type;

    if constexpr (!std::is_same_v<old_tag, new_tag>) {
      return false;
    } else if constexpr (!is_commutative_tag_v<old_tag>) {
      return false;
    } else {
      constexpr std::size_t old_n =
          std::tuple_size_v<std::decay_t<decltype(old_expr.children)>>;
      constexpr std::size_t new_n =
          std::tuple_size_v<std::decay_t<decltype(new_expr.children)>>;
      if constexpr (old_n != new_n) {
        return false;
      } else {
        auto old_hashes =
            child_hashes_impl(old_expr, std::make_index_sequence<old_n>{});
        auto new_hashes =
            child_hashes_impl(new_expr, std::make_index_sequence<new_n>{});
        std::sort(old_hashes.begin(), old_hashes.end());
        std::sort(new_hashes.begin(), new_hashes.end());
        return old_hashes == new_hashes;
      }
    }
  } else {
    return false;
  }
}

struct indexed_subtree {
  structural_hash_t hash = 0;
  diff_path path;
};

template <class Expr>
void index_subtrees(const Expr &expr, diff_path &path,
                    std::vector<indexed_subtree> &index) {
  if constexpr (VariantExpr<Expr>) {
    std::visit([&](const auto &alt) { index_subtrees(alt, path, index); },
               expr);
  } else {
    index.push_back(indexed_subtree{structural_hash(expr), path});
    if constexpr (Expression<Expr>) {
      constexpr std::size_t n =
          std::tuple_size_v<std::decay_t<decltype(expr.children)>>;
      [&]<std::size_t... I>(std::index_sequence<I...>) {
        (
            [&] {
              path.push_back(I);
              index_subtrees(std::get<I>(expr.children), path, index);
              path.pop_back();
            }(),
            ...);
      }(std::make_index_sequence<n>{});
    }
  }
}

[[nodiscard]] inline subtree_diff make_subtree_diff(const diff_entry &entry) {
  subtree_diff d;
  d.kind = entry.kind;
  d.old_hash = entry.old_hash;
  d.new_hash = entry.new_hash;
  if (entry.kind == diff_kind::inserted) {
    d.new_path = entry.path;
  } else if (entry.kind == diff_kind::removed) {
    d.old_path = entry.path;
  } else {
    d.old_path = entry.path;
    d.new_path = entry.path;
  }
  d.reordered = entry.kind == diff_kind::reordered;
  return d;
}

[[nodiscard]] inline change_region
make_region(const subtree_diff &d, const bool semantic_sensitive = false) {
  change_region r;
  r.path = d.new_path.empty() ? d.old_path : d.new_path;
  r.depth = r.path.size();
  r.old_hash = d.old_hash;
  r.new_hash = d.new_hash;
  r.dominant_kind = d.kind;
  r.semantic_sensitive = semantic_sensitive;
  return r;
}
} // namespace detail

template <class OldExpr, class NewExpr>
structural_patch compute_patch(const OldExpr &old_expr, const NewExpr &new_expr,
                               structural_diff_options options = {}) {
  structural_patch patch;
  patch.old_root_hash = structural_hash(old_expr);
  patch.new_root_hash = structural_hash(new_expr);

  const auto base = diff(old_expr, new_expr);
  patch.equivalent = base.equivalent;

  for (const auto &entry : base.entries) {
    if (entry.kind == diff_kind::unchanged) {
      continue;
    }
    patch.diffs.push_back(detail::make_subtree_diff(entry));
  }

  if (patch.diffs.empty() && patch.equivalent) {
    patch.diffs.push_back(subtree_diff{diff_kind::unchanged,
                                       {},
                                       {},
                                       patch.old_root_hash,
                                       patch.new_root_hash,
                                       true,
                                       false,
                                       false,
                                       options.canonical_equality,
                                       options.semantic_aware});
  }

  if (options.detect_reuse || options.detect_moves) {
    std::vector<detail::indexed_subtree> old_index;
    std::vector<detail::indexed_subtree> new_index;
    diff_path path;
    detail::index_subtrees(old_expr, path, old_index);
    detail::index_subtrees(new_expr, path, new_index);

    std::unordered_map<structural_hash_t, std::vector<diff_path>>
        old_paths_by_hash;
    old_paths_by_hash.reserve(old_index.size());
    for (const auto &node : old_index) {
      old_paths_by_hash[node.hash].push_back(node.path);
    }

    for (auto &d : patch.diffs) {
      if (d.new_hash == 0) {
        continue;
      }
      const auto it = old_paths_by_hash.find(d.new_hash);
      if (it == old_paths_by_hash.end()) {
        continue;
      }

      d.reused = options.detect_reuse;
      if (!it->second.empty() && d.old_path.empty()) {
        d.old_path = it->second.front();
        d.old_hash = d.new_hash;
      }

      if (options.detect_moves && !d.new_path.empty()) {
        const bool already_same_place =
            std::find(it->second.begin(), it->second.end(), d.new_path) !=
            it->second.end();
        d.moved = !already_same_place;
      }
    }
  }

  if (options.canonical_equality) {
    const bool canonical_eq = detail::canonical_equal(old_expr, new_expr);
    for (auto &d : patch.diffs) {
      d.canonical_equivalent = canonical_eq;
    }
  }

  if (options.semantic_aware) {
    const auto old_sem = semantic::infer_semantics(old_expr);
    const auto new_sem = semantic::infer_semantics(new_expr);
    const bool semantic_eq = detail::same_semantics(old_sem, new_sem);
    for (auto &d : patch.diffs) {
      d.semantic_equivalent = semantic_eq;
    }
  }

  patch.regions.reserve(patch.diffs.size());
  for (const auto &d : patch.diffs) {
    if (d.kind == diff_kind::unchanged) {
      continue;
    }
    patch.regions.push_back(detail::make_region(d, options.semantic_aware &&
                                                       !d.semantic_equivalent));
  }

  return patch;
}

template <class OldExpr, class NewExpr>
structural_patch compute_diff(const OldExpr &old_expr, const NewExpr &new_expr,
                              structural_diff_options options = {}) {
  return compute_patch(old_expr, new_expr, options);
}

template <class OldExpr, class NewExpr>
std::vector<change_region>
compute_reuse_regions(const OldExpr &old_expr, const NewExpr &new_expr,
                      const structural_diff_options options = {}) {
  std::vector<change_region> reuse;
  if (!options.detect_reuse) {
    return reuse;
  }

  std::vector<detail::indexed_subtree> old_index;
  std::vector<detail::indexed_subtree> new_index;
  diff_path path;
  detail::index_subtrees(old_expr, path, old_index);
  detail::index_subtrees(new_expr, path, new_index);

  std::unordered_set<structural_hash_t> old_hashes;
  old_hashes.reserve(old_index.size());
  for (const auto &node : old_index) {
    old_hashes.insert(node.hash);
  }

  for (const auto &node : new_index) {
    if (!old_hashes.contains(node.hash)) {
      continue;
    }
    reuse.push_back(change_region{node.path, node.path.size(), node.hash,
                                  node.hash, diff_kind::unchanged, false});
  }

  return reuse;
}

inline std::vector<invalidation_region>
minimal_invalidation_set(const structural_patch &patch) {
  std::vector<invalidation_region> candidates;
  candidates.reserve(patch.regions.size());
  for (const auto &region : patch.regions) {
    candidates.push_back(
        invalidation_region{region.path, region.old_hash, region.new_hash,
                            region.dominant_kind != diff_kind::unchanged,
                            region.semantic_sensitive, region.depth});
  }

  std::sort(candidates.begin(), candidates.end(),
            [](const invalidation_region &a, const invalidation_region &b) {
              if (a.path.size() != b.path.size()) {
                return a.path.size() < b.path.size();
              }
              return a.path < b.path;
            });

  std::vector<invalidation_region> minimized;
  for (const auto &candidate : candidates) {
    const bool covered = std::any_of(minimized.begin(), minimized.end(),
                                     [&](const invalidation_region &existing) {
                                       return detail::is_path_prefix(
                                           existing.path, candidate.path);
                                     });
    if (!covered) {
      minimized.push_back(candidate);
    }
  }
  return minimized;
}

template <class OldExpr, class NewExpr>
std::vector<invalidation_region>
minimal_invalidation_set(const OldExpr &old_expr, const NewExpr &new_expr,
                         structural_diff_options options = {}) {
  return minimal_invalidation_set(compute_patch(old_expr, new_expr, options));
}

template <class Pass> struct fixpoint_pass {
  Pass p;
  int max_iters;

  template <class E> constexpr auto operator()(E &&e) const {
    return compiler::optimize_any(std::forward<E>(e), p, max_iters);
  }
};

template <class Pass> constexpr auto fixpoint(Pass p, int max_iters) {
  return fixpoint_pass<Pass>{std::move(p), max_iters};
}

struct simplify_add_zero_rule {
  template <class T> constexpr decltype(auto) on_terminal(T &&t) const {
    return std::forward<T>(t);
  }

  template <class A, class B, class TA, class TB>
  constexpr auto on_node(add_tag, A &&origA, B &&origB, TA ta, TB tb) const {
    using node_t = lithe::node<add_tag, std::decay_t<A>, std::decay_t<B>>;
    using altA_t = std::decay_t<TA>;
    using altB_t = std::decay_t<TB>;
    using var_t = std::variant<node_t, altA_t, altB_t>;

    auto is_zero = [](auto const &v) constexpr -> bool {
      using V = std::decay_t<decltype(v)>;
      if constexpr (std::is_arithmetic_v<V>) {
        return v == 0;
      } else if constexpr (requires { v.value; }) {
        using ValT = std::decay_t<decltype(v.value)>;
        if constexpr (std::is_arithmetic_v<ValT>) {
          return v.value == 0;
        } else {
          return false;
        }
      } else if constexpr (requires { v.p; }) {
        using Pointee = std::decay_t<decltype(*v.p)>;
        if constexpr (std::is_arithmetic_v<Pointee>) {
          return (*v.p) == 0;
        } else {
          return false;
        }
      } else {
        return false;
      }
    };

    if (is_zero(origB)) {
      return var_t{std::in_place_index<1>, ta};
    }
    if (is_zero(origA)) {
      return var_t{std::in_place_index<2>, tb};
    }

    return var_t{std::in_place_index<0>,
                 lithe::rebuild<add_tag>(std::decay_t<A>(origA),
                                         std::decay_t<B>(origB))};
  }

  template <class Tag, class... Args>
  constexpr decltype(auto) on_node(Tag, Args &&...args) const {
    constexpr std::size_t N = sizeof...(Args);
    static_assert(N % 2 == 0);
    auto tup = std::forward_as_tuple(std::forward<Args>(args)...);
    return [&]<std::size_t... I>(std::index_sequence<I...>) {
      return lithe::rebuild<Tag>(std::get<I>(tup)...);
    }(std::make_index_sequence<N / 2>{});
  }
};

struct simplify_add_zero_pass {
  template <class E> constexpr auto operator()(E &&e) const {
    auto out = rewrite_once(detail::phase_unwrap(std::forward<E>(e)),
                            simplify_add_zero_rule{});
    return detail::wrap_optimized<E>(std::move(out));
  }
};

struct simplify_mul_identity_rule {
  template <class T> constexpr decltype(auto) on_terminal(T &&t) const {
    return std::forward<T>(t);
  }

  template <class A, class B, class TA, class TB>
  constexpr auto on_node(mul_tag, A &&origA, B &&origB, TA ta, TB tb) const {
    using node_t = lithe::node<mul_tag, std::decay_t<A>, std::decay_t<B>>;
    using altA_t = std::decay_t<TA>;
    using altB_t = std::decay_t<TB>;
    using var_t = std::variant<node_t, altA_t, altB_t>;

    auto is_zero = [](auto const &v) constexpr -> bool {
      using V = std::decay_t<decltype(v)>;
      if constexpr (std::is_arithmetic_v<V>) {
        return v == 0;
      } else if constexpr (requires { v.value; }) {
        using ValT = std::decay_t<decltype(v.value)>;
        if constexpr (std::is_arithmetic_v<ValT>) {
          return v.value == 0;
        } else {
          return false;
        }
      } else if constexpr (requires { v.p; }) {
        using Pointee = std::decay_t<decltype(*v.p)>;
        if constexpr (std::is_arithmetic_v<Pointee>) {
          return (*v.p) == 0;
        } else {
          return false;
        }
      } else {
        return false;
      }
    };

    auto is_one = [](auto const &v) constexpr -> bool {
      using V = std::decay_t<decltype(v)>;
      if constexpr (std::is_arithmetic_v<V>) {
        return v == 1;
      } else if constexpr (requires { v.value; }) {
        using ValT = std::decay_t<decltype(v.value)>;
        if constexpr (std::is_arithmetic_v<ValT>) {
          return v.value == 1;
        } else {
          return false;
        }
      } else if constexpr (requires { v.p; }) {
        using Pointee = std::decay_t<decltype(*v.p)>;
        if constexpr (std::is_arithmetic_v<Pointee>) {
          return (*v.p) == 1;
        } else {
          return false;
        }
      } else {
        return false;
      }
    };

    if (is_one(origB)) {
      return var_t{std::in_place_index<1>, ta};
    }
    if (is_one(origA)) {
      return var_t{std::in_place_index<2>, tb};
    }

    return var_t{std::in_place_index<0>,
                 lithe::rebuild<mul_tag>(std::decay_t<A>(origA),
                                         std::decay_t<B>(origB))};
  }

  template <class Tag, class... Args>
  constexpr decltype(auto) on_node(Tag, Args &&...args) const {
    constexpr std::size_t N = sizeof...(Args);
    static_assert(N % 2 == 0);
    auto tup = std::forward_as_tuple(std::forward<Args>(args)...);
    return [&]<std::size_t... I>(std::index_sequence<I...>) {
      return lithe::rebuild<Tag>(std::get<I>(tup)...);
    }(std::make_index_sequence<N / 2>{});
  }
};

struct simplify_mul_identity_pass {
  template <class E> constexpr auto operator()(E &&e) const {
    auto out = rewrite_once(detail::phase_unwrap(std::forward<E>(e)),
                            simplify_mul_identity_rule{});
    return detail::wrap_optimized<E>(std::move(out));
  }
};

struct constant_fold_arith_rule {
  template <class T> constexpr decltype(auto) on_terminal(T &&t) const {
    return std::forward<T>(t);
  }

  template <class A, class B, class TA, class TB>
  constexpr auto on_node(add_tag, A &&origA, B &&origB, TA a, TB b) const {
    using node_t = lithe::node<add_tag, std::decay_t<A>, std::decay_t<B>>;
    using altA_t = std::decay_t<TA>;
    using altB_t = std::decay_t<TB>;

    if constexpr (std::is_arithmetic_v<altA_t> &&
                  std::is_arithmetic_v<altB_t>) {
      using sum_t = std::decay_t<decltype(a + b)>;
      using var_t = std::variant<node_t, sum_t, altA_t, altB_t>;
      return var_t{std::in_place_index<1>, static_cast<sum_t>(a + b)};
    } else {
      using var_t = std::variant<node_t, altA_t, altB_t>;
      return var_t{std::in_place_index<0>,
                   lithe::rebuild<add_tag>(std::decay_t<A>(origA),
                                           std::decay_t<B>(origB))};
    }
  }

  template <class A, class B, class TA, class TB>
  constexpr auto on_node(mul_tag, A &&origA, B &&origB, TA a, TB b) const {
    using node_t = lithe::node<mul_tag, std::decay_t<A>, std::decay_t<B>>;
    using altA_t = std::decay_t<TA>;
    using altB_t = std::decay_t<TB>;

    if constexpr (std::is_arithmetic_v<altA_t> &&
                  std::is_arithmetic_v<altB_t>) {
      using prod_t = std::decay_t<decltype(a * b)>;
      using var_t = std::variant<node_t, prod_t, altA_t, altB_t>;
      return var_t{std::in_place_index<1>, static_cast<prod_t>(a * b)};
    } else {
      using var_t = std::variant<node_t, altA_t, altB_t>;
      return var_t{std::in_place_index<0>,
                   lithe::rebuild<mul_tag>(std::decay_t<A>(origA),
                                           std::decay_t<B>(origB))};
    }
  }

  template <class A, class TA>
  constexpr auto on_node(neg_tag, A &&origA, TA a) const {
    using node_t = lithe::node<neg_tag, std::decay_t<A>>;
    using altA_t = std::decay_t<TA>;
    using neg_t = std::decay_t<decltype(-a)>;
    using var_t = std::variant<node_t, neg_t, altA_t>;

    if constexpr (std::is_arithmetic_v<altA_t>) {
      return var_t{std::in_place_index<1>, static_cast<neg_t>(-a)};
    }
    return var_t{std::in_place_index<0>,
                 lithe::rebuild<neg_tag>(std::decay_t<A>(origA))};
  }

  template <class Tag, class... Args>
  constexpr decltype(auto) on_node(Tag, Args &&...args) const {
    constexpr std::size_t N = sizeof...(Args);
    static_assert(N % 2 == 0);
    auto tup = std::forward_as_tuple(std::forward<Args>(args)...);
    return [&]<std::size_t... I>(std::index_sequence<I...>) {
      return lithe::rebuild<Tag>(std::get<I>(tup)...);
    }(std::make_index_sequence<N / 2>{});
  }
};

struct constant_fold_arith_pass {
  template <class E> constexpr auto operator()(E &&e) const {
    auto out = rewrite_once(detail::phase_unwrap(std::forward<E>(e)),
                            constant_fold_arith_rule{});
    return detail::wrap_optimized<E>(std::move(out));
  }
};

// -----------------------------------------------------------------------
// domain_folding_rule<Folder>
//
// A rewrite rule that delegates constant folding to a DomainFolder.
// No hardcoded switch on tag types — every node dispatches through
// Folder::try_fold() via a fold_op_key derived from the tag name.
//
// The rule is intentionally iterative (rewrite_once is called from the
// enclosing pass in a bounded loop), so constexpr step limits are safe.
// -----------------------------------------------------------------------
template <lithe::folding::DomainFolder Folder> struct domain_folding_rule {
  Folder folder;

  // Terminals: attempt to extract a fold_operand from arithmetic leaves.
  // Non-arithmetic terminals are forwarded unchanged.
  template <class T> constexpr decltype(auto) on_terminal(T &&t) const {
    return std::forward<T>(t);
  }

  // Generic node handler — replaces all hardcoded tag switches.
  //
  // The transform mechanism provides pairs (origI, evalI): the first
  // N/2 arguments are the original children, the last N/2 are the
  // recursively-evaluated (potentially folded) children.
  template <class Tag, class... Args>
  constexpr decltype(auto) on_node(Tag, Args &&...args) const {
    constexpr std::size_t N = sizeof...(Args);
    static_assert(
        N % 2 == 0,
        "domain_folding_rule: expected paired (orig, evaluated) arguments");
    constexpr std::size_t arity = N / 2;

    auto tup = std::forward_as_tuple(std::forward<Args>(args)...);

    // Collect fold_operands from the evaluated (second) half of args.
    // Uses a bounded compile-time loop — no recursion.
    std::array<lithe::folding::fold_operand, arity> operands{};
    [&]<std::size_t... I>(std::index_sequence<I...>) {
      ((operands[I] =
            lithe::folding::to_fold_operand(std::get<arity + I>(tup))),
       ...);
    }(std::make_index_sequence<arity>{});

    // Build the operation key from the tag's compile-time name.
    // Falls back to "lithe.core" for built-in tags without a domain.
    constexpr std::string_view op_name = emit::tag_name<Tag>::value;
    lithe::folding::fold_op_key key{"lithe.core", op_name};

    const lithe::folding::fold_result result = folder.try_fold(
        key, std::span<const lithe::folding::fold_operand>{operands});

    if (result.has_value()) {
      // Folder succeeded — return the folded scalar in a variant so
      // the rewrite system can propagate it as a terminal.
      const auto &folded = *result;
      if (folded.is_i64()) {
        using rebuild_t =
            decltype([&]<std::size_t... I>(std::index_sequence<I...>) {
              return lithe::rebuild<Tag>(std::get<I>(tup)...);
            }(std::make_index_sequence<arity>{}));
        using var_t = std::variant<rebuild_t, std::int64_t, double>;
        return var_t{std::in_place_index<1>, folded.as_i64()};
      } else {
        using rebuild_t =
            decltype([&]<std::size_t... I>(std::index_sequence<I...>) {
              return lithe::rebuild<Tag>(std::get<I>(tup)...);
            }(std::make_index_sequence<arity>{}));
        using var_t = std::variant<rebuild_t, std::int64_t, double>;
        return var_t{std::in_place_index<2>, folded.as_f64()};
      }
    }

    // No fold — reconstruct with original children (preserves structure).
    return [&]<std::size_t... I>(std::index_sequence<I...>) {
      using rebuild_t = decltype(lithe::rebuild<Tag>(std::get<I>(tup)...));
      using var_t = std::variant<rebuild_t, std::int64_t, double>;
      return var_t{std::in_place_index<0>,
                   lithe::rebuild<Tag>(std::get<I>(tup)...)};
    }(std::make_index_sequence<arity>{});
  }
};

// -----------------------------------------------------------------------
// domain_folding_pass<Folder>
//
// Wraps domain_folding_rule in the standard pass protocol.
// Uses rewrite_once (bounded, non-recursive) to satisfy constexpr limits.
// Call make_domain_folding_pass<domain_type::arithmetic>() to get an
// instance bound to the appropriate built-in folder.
// -----------------------------------------------------------------------
template <lithe::folding::DomainFolder Folder> struct domain_folding_pass {
  Folder folder;

  template <class E> constexpr auto operator()(E &&e) const {
    domain_folding_rule<Folder> rule{folder};
    auto out = rewrite_once(detail::phase_unwrap(std::forward<E>(e)), rule);
    return detail::wrap_optimized<E>(std::move(out));
  }
};

// Factory: selects the correct folder for a domain at compile time.
template <lithe::semantic::domain_type D>
[[nodiscard]] constexpr auto make_domain_folding_pass() {
  return domain_folding_pass<lithe::folding::domain_folder_for_t<D>>{
      lithe::folding::make_folder<D>()};
}

struct canonicalize_commutative_rule {
  template <class T> constexpr decltype(auto) on_terminal(T &&t) const {
    return std::forward<T>(t);
  }

  template <class U>
  static constexpr bool is_arith_val([[maybe_unused]] const U &v) {
    using V = std::decay_t<U>;
    if constexpr (std::is_arithmetic_v<V>) {
      return true;
    } else if constexpr (requires { v.value; }) {
      using ValT = std::decay_t<decltype(v.value)>;
      return std::is_arithmetic_v<ValT>;
    } else if constexpr (requires { v.p; }) {
      using Pointee = std::decay_t<decltype(*v.p)>;
      return std::is_arithmetic_v<Pointee>;
    } else {
      return false;
    }
  }

  template <class CT, class U> static constexpr CT to_common(const U &v) {
    using V = std::decay_t<U>;
    if constexpr (std::is_arithmetic_v<V>) {
      return static_cast<CT>(v);
    } else if constexpr (requires { v.value; }) {
      return static_cast<CT>(v.value);
    } else if constexpr (requires { v.p; }) {
      return static_cast<CT>(*v.p);
    } else {
      return CT{};
    }
  }

  template <class Tag, class A, class B, class TA, class TB>
  constexpr auto handle_comm(Tag, A &&, B &&, TA ta, TB tb) const {
    constexpr bool a_num = is_arith_val(ta);
    constexpr bool b_num = is_arith_val(tb);
    auto lhs = std::decay_t<TA>(ta);
    auto rhs = std::decay_t<TB>(tb);

    if constexpr (a_num && b_num) {
      using CTA = std::decay_t<TA>;
      using CTB = std::decay_t<TB>;
      using CT = std::common_type_t<CTA, CTB>;
      auto aa = to_common<CT>(lhs);
      auto bb = to_common<CT>(rhs);
      if (aa <= bb) {
        return lithe::rebuild<Tag>(std::move(lhs), std::move(rhs));
      } else {
        return lithe::rebuild<Tag>(std::move(rhs), std::move(lhs));
      }
    } else if constexpr (a_num && !b_num) {
      return lithe::rebuild<Tag>(std::move(rhs), std::move(lhs));
    } else if constexpr (!a_num && b_num) {
      return lithe::rebuild<Tag>(std::move(lhs), std::move(rhs));
    } else {
      return lithe::rebuild<Tag>(std::move(lhs), std::move(rhs));
    }
  }

  template <class A, class B, class TA, class TB>
  constexpr auto on_node(add_tag, A &&origA, B &&origB, TA ta, TB tb) const {
    return handle_comm(add_tag{}, std::forward<A>(origA),
                       std::forward<B>(origB), ta, tb);
  }

  template <class A, class B, class TA, class TB>
  constexpr auto on_node(mul_tag, A &&origA, B &&origB, TA ta, TB tb) const {
    return handle_comm(mul_tag{}, std::forward<A>(origA),
                       std::forward<B>(origB), ta, tb);
  }

  template <class Tag, class... Args>
  constexpr decltype(auto) on_node(Tag, Args &&...args) const {
    constexpr std::size_t N = sizeof...(Args);
    static_assert(N % 2 == 0);
    auto tup = std::forward_as_tuple(std::forward<Args>(args)...);
    return [&]<std::size_t... I>(std::index_sequence<I...>) {
      return lithe::rebuild<Tag>(std::get<I>(tup)...);
    }(std::make_index_sequence<N / 2>{});
  }
};

struct canonicalize_commutative_pass {
  template <class E> constexpr auto operator()(E &&e) const {
    auto out = rewrite_once(detail::phase_unwrap(std::forward<E>(e)),
                            canonicalize_commutative_rule{});
    return detail::wrap_canonical<E>(std::move(out));
  }
};

struct explicit_coercion_normalization_pass {
  template <class E> constexpr auto operator()(E &&e) const {
    // Conservative placeholder: typed coercion normalization hooks live here.
    auto unwrapped = detail::phase_unwrap(std::forward<E>(e));
    using owned_t = std::decay_t<decltype(unwrapped)>;
    owned_t owned = std::move(unwrapped);
    return detail::wrap_canonical<E>(std::move(owned));
  }
};

struct syntax_sugar_removal_pass {
  template <class E> constexpr auto operator()(E &&e) const {
    // Conservative placeholder: desugaring hooks live here.
    auto unwrapped = detail::phase_unwrap(std::forward<E>(e));
    using owned_t = std::decay_t<decltype(unwrapped)>;
    owned_t owned = std::move(unwrapped);
    return detail::wrap_canonical<E>(std::move(owned));
  }
};

struct normalized_node_forms_pass {
  template <class E> constexpr auto operator()(E &&e) const {
    // Conservative placeholder: normalized node form hooks live here.
    auto unwrapped = detail::phase_unwrap(std::forward<E>(e));
    using owned_t = std::decay_t<decltype(unwrapped)>;
    owned_t owned = std::move(unwrapped);
    return detail::wrap_canonical<E>(std::move(owned));
  }
};

struct associative_flattening_pass {
  template <class E> constexpr auto operator()(E &&e) const {
    // Conservative placeholder: exact-semantics-safe flattening hooks live
    // here.
    auto unwrapped = detail::phase_unwrap(std::forward<E>(e));
    using owned_t = std::decay_t<decltype(unwrapped)>;
    owned_t owned = std::move(unwrapped);
    return detail::wrap_canonical<E>(std::move(owned));
  }
};

struct normalized_comparison_rule {
  template <class T> constexpr decltype(auto) on_terminal(T &&t) const {
    return std::forward<T>(t);
  }

  template <class A, class B, class TA, class TB>
  constexpr auto on_node(eq_tag, A &&origA, B &&origB, TA ta, TB tb) const {
    (void)origA;
    (void)origB;
    auto lhs = std::decay_t<TA>(ta);
    auto rhs = std::decay_t<TB>(tb);
    const auto ha = emit::structural_hash(lhs);
    const auto hb = emit::structural_hash(rhs);
    if (ha <= hb) {
      return lithe::rebuild<eq_tag>(std::move(lhs), std::move(rhs));
    }
    return lithe::rebuild<eq_tag>(std::move(rhs), std::move(lhs));
  }

  template <class A, class B, class TA, class TB>
  constexpr auto on_node(ne_tag, A &&origA, B &&origB, TA ta, TB tb) const {
    (void)origA;
    (void)origB;
    auto lhs = std::decay_t<TA>(ta);
    auto rhs = std::decay_t<TB>(tb);
    const auto ha = emit::structural_hash(lhs);
    const auto hb = emit::structural_hash(rhs);
    if (ha <= hb) {
      return lithe::rebuild<ne_tag>(std::move(lhs), std::move(rhs));
    }
    return lithe::rebuild<ne_tag>(std::move(rhs), std::move(lhs));
  }

  template <class A, class B, class TA, class TB>
  constexpr auto on_node(gt_tag, A &&, B &&, TA ta, TB tb) const {
    auto lhs = std::decay_t<TA>(ta);
    auto rhs = std::decay_t<TB>(tb);
    return lithe::rebuild<lt_tag>(std::move(rhs), std::move(lhs));
  }

  template <class A, class B, class TA, class TB>
  constexpr auto on_node(ge_tag, A &&, B &&, TA ta, TB tb) const {
    auto lhs = std::decay_t<TA>(ta);
    auto rhs = std::decay_t<TB>(tb);
    return lithe::rebuild<le_tag>(std::move(rhs), std::move(lhs));
  }

  template <class Tag, class... Args>
  constexpr decltype(auto) on_node(Tag, Args &&...args) const {
    constexpr std::size_t N = sizeof...(Args);
    static_assert(N % 2 == 0);
    auto tup = std::forward_as_tuple(std::forward<Args>(args)...);
    return [&]<std::size_t... I>(std::index_sequence<I...>) {
      return lithe::rebuild<Tag>(std::get<I>(tup)...);
    }(std::make_index_sequence<N / 2>{});
  }
};

struct normalized_comparison_pass {
  template <class E> constexpr auto operator()(E &&e) const {
    auto out = rewrite_once(detail::phase_unwrap(std::forward<E>(e)),
                            normalized_comparison_rule{});
    return detail::wrap_canonical<E>(std::move(out));
  }
};

struct enhanced_algebraic_canonicalization_pass;
struct constant_propagation_pass;
struct strength_reduction_pass;
struct dead_subtree_elimination_pass;
struct true_cse_pass;

template <class Pass> struct phase_traits {
  static constexpr pass_category category = pass_category::optimization;
};

template <> struct phase_traits<explicit_coercion_normalization_pass> {
  static constexpr pass_category category = pass_category::canonicalization;
};

template <> struct phase_traits<syntax_sugar_removal_pass> {
  static constexpr pass_category category = pass_category::canonicalization;
};

template <> struct phase_traits<normalized_node_forms_pass> {
  static constexpr pass_category category = pass_category::canonicalization;
};

template <> struct phase_traits<associative_flattening_pass> {
  static constexpr pass_category category = pass_category::canonicalization;
};

template <> struct phase_traits<canonicalize_commutative_pass> {
  static constexpr pass_category category = pass_category::canonicalization;
};

template <> struct phase_traits<enhanced_algebraic_canonicalization_pass> {
  static constexpr pass_category category = pass_category::canonicalization;
};

template <> struct phase_traits<normalized_comparison_pass> {
  static constexpr pass_category category = pass_category::canonicalization;
};

template <> struct phase_traits<constant_fold_arith_pass> {
  static constexpr pass_category category = pass_category::optimization;
};

template <lithe::folding::DomainFolder Folder>
struct phase_traits<domain_folding_pass<Folder>> {
  static constexpr pass_category category = pass_category::optimization;
};

template <> struct phase_traits<constant_propagation_pass> {
  static constexpr pass_category category = pass_category::optimization;
};

template <> struct phase_traits<strength_reduction_pass> {
  static constexpr pass_category category = pass_category::optimization;
};

template <> struct phase_traits<dead_subtree_elimination_pass> {
  static constexpr pass_category category = pass_category::optimization;
};

template <> struct phase_traits<true_cse_pass> {
  static constexpr pass_category category = pass_category::optimization;
};

template <class Pass>
inline constexpr bool is_canonicalization_pass_v =
    phase_traits<std::decay_t<Pass>>::category ==
    pass_category::canonicalization;

template <class Pass>
inline constexpr bool is_optimization_pass_v =
    phase_traits<std::decay_t<Pass>>::category == pass_category::optimization;

template <class... P>
  requires(is_canonicalization_pass_v<P> && ...)
struct canonicalization_pipeline {
  std::tuple<P...> passes;

  template <class E> constexpr auto operator()(E &&expr) const {
    auto out = std::apply(
        [&](const auto &...p) {
          return compiler::compile(detail::phase_unwrap(std::forward<E>(expr)),
                                   p...);
        },
        passes);
    return lithe::as_canonical_expr(detail::phase_unwrap(std::move(out)));
  }
};

template <class... P>
  requires(is_optimization_pass_v<P> && ...)
struct optimization_pipeline {
  std::tuple<P...> passes;

  template <class E> constexpr auto operator()(E &&expr) const {
    auto canonical =
        lithe::as_canonical_expr(detail::phase_unwrap(std::forward<E>(expr)));
    auto out = std::apply(
        [&](const auto &...p) {
          return compiler::compile(std::move(canonical), p...);
        },
        passes);
    return lithe::as_optimized_expr(detail::phase_unwrap(std::move(out)));
  }
};

[[nodiscard]] constexpr auto make_default_canonicalization_pipeline() {
  return canonicalization_pipeline<
      syntax_sugar_removal_pass, explicit_coercion_normalization_pass,
      normalized_node_forms_pass, canonicalize_commutative_pass,
      associative_flattening_pass, normalized_comparison_pass>{std::tuple{
      syntax_sugar_removal_pass{}, explicit_coercion_normalization_pass{},
      normalized_node_forms_pass{}, canonicalize_commutative_pass{},
      associative_flattening_pass{}, normalized_comparison_pass{}}};
}

template <class E, class Pass>
constexpr auto rewrite_fixpoint(E e, Pass p, int max_iters) {
  if (max_iters <= 0)
    max_iters = 1;
  auto cur = p(std::forward<E>(e));
  for (int i = 1; i < max_iters; ++i) {
    cur = p(std::move(cur));
  }
  return cur;
}

template <class... P> struct pipeline_pass {
  std::tuple<P...> ps;

  template <class E> constexpr auto operator()(E &&e) const {
    return std::apply(
        [&](auto const &...p) {
          return compiler::compile(std::forward<E>(e), p...);
        },
        ps);
  }
};

template <class... P> constexpr auto pipeline(P... p) {
  return pipeline_pass<P...>{std::tuple<P...>{std::move(p)...}};
}

template <class E> constexpr auto to_surface(E &&expr) {
  return lithe::as_surface_expr(std::forward<E>(expr));
}

template <class E> constexpr auto canonicalize(E &&expr) {
  auto pipeline = make_default_canonicalization_pipeline();
  return pipeline(std::forward<E>(expr));
}

template <class E, class... OptPasses>
  requires(std::invocable<OptPasses, canonical_expr<E>> && ...)
constexpr auto optimize_phase(canonical_expr<E> expr, OptPasses... passes) {
  auto out = compiler::compile(std::move(expr), std::move(passes)...);
  if constexpr (lithe::is_optimized_expr_v<decltype(out)>) {
    return out;
  } else {
    return lithe::as_optimized_expr(detail::phase_unwrap(std::move(out)));
  }
}

template <class T> struct transform_pass {
  T t;

  template <class E> constexpr auto operator()(E &&e) const {
    return lithe::transform(std::forward<E>(e), t);
  }
};

template <class T> constexpr auto from_transformer(T t) {
  return transform_pass<T>{std::move(t)};
}

template <class R> struct rewrite_pass {
  R r;

  template <class E> constexpr auto operator()(E &&e) const {
    return lithe::rewrite_once(std::forward<E>(e), r);
  }
};

template <class R> constexpr auto from_rewriter(R r) {
  return rewrite_pass<R>{std::move(r)};
}

template <class Guard>
concept semantic_guard =
    requires(const Guard &g, const semantic::semantic_info &info) {
      { g(info) } -> std::convertible_to<bool>;
    };

struct pure_only_guard {
  constexpr bool operator()(const semantic::semantic_info &info) const {
    return semantic::is_pure(info);
  }
};

struct no_throw_guard {
  constexpr bool operator()(const semantic::semantic_info &info) const {
    return semantic::is_no_throw(info);
  }
};

struct reorder_safe_guard {
  constexpr bool operator()(const semantic::semantic_info &info) const {
    return semantic::is_safe_to_reorder(info, info);
  }
};

struct cse_safe_guard {
  constexpr bool operator()(const semantic::semantic_info &info) const {
    return semantic::is_safe_to_cse(info);
  }
};

template <class RewritePass, semantic_guard Guard> struct guarded_rewrite_pass {
  RewritePass rewrite;
  Guard guard;

  template <class E> constexpr auto operator()(E &&e) const {
    using input_t = std::decay_t<E>;
    using result_t = std::decay_t<decltype(rewrite(std::declval<input_t>()))>;
    input_t input = std::forward<E>(e);
    const auto before = semantic::analyze_semantics(input);
    if (!guard(before)) {
      if constexpr (std::constructible_from<result_t, input_t>) {
        return result_t{std::move(input)};
      } else if constexpr (requires(input_t &&v) {
                             typename result_t::value_type;
                             result_t{
                                 typename result_t::value_type{std::move(v)}};
                           }) {
        return result_t{typename result_t::value_type{std::move(input)}};
      } else {
        return result_t{rewrite(input_t{input})};
      }
    }
    auto rewritten = rewrite(std::move(input));
    return result_t{std::move(rewritten)};
  }
};

template <class RewritePass, semantic_guard Guard>
constexpr auto guarded(RewritePass rewrite, Guard guard) {
  return guarded_rewrite_pass<RewritePass, Guard>{std::move(rewrite),
                                                  std::move(guard)};
}

template <class V> struct visit_pass {
  V *v;

  template <class E> constexpr auto operator()(E &&e) const {
    emit::visit(std::forward<E>(e), *v);
    return std::forward<E>(e);
  }
};

template <class V> constexpr auto from_visitor(V &v) {
  return visit_pass<V>{std::addressof(v)};
}

template <class P> struct trace_pass {
  P p;
  compiler::context *ctx;

  template <class E> constexpr auto operator()(E &&e) const {
    if (ctx && ctx->trace) {
      ctx->logs.push_back(emit::dump(std::forward<E>(e)));
      auto res = p(std::forward<E>(e));
      ++ctx->passes_run;
      if constexpr (requires { emit::dump(res); }) {
        ctx->logs.push_back(emit::dump(res));
      }
      return res;
    } else {
      if (ctx)
        ++ctx->passes_run;
      return p(std::forward<E>(e));
    }
  }
};

template <class P> constexpr auto with_trace(P p, compiler::context &ctx) {
  return trace_pass<P>{std::move(p), std::addressof(ctx)};
}

template <class Pass, class IR> struct memoize_pass {
  Pass p;
  std::unordered_map<std::size_t, IR> *cache;

  template <class E> IR operator()(E &&e) const {
    auto h = emit::structural_hash(e);
    if (auto it = cache->find(h); it != cache->end())
      return it->second;
    IR out = p(std::forward<E>(e));
    cache->insert_or_assign(h, std::move(out));
    return cache->find(h)->second;
  }
};

template <class Pass, class IR>
constexpr auto memoize(Pass p, std::unordered_map<std::size_t, IR> &cache) {
  return memoize_pass<Pass, IR>{std::move(p), std::addressof(cache)};
}

// -----------------------------
// Advanced Optimization Passes for Serious Transformations
// -----------------------------
struct constant_propagation_rule {
  mutable std::unordered_map<std::size_t, double> known_values;

  template <class T> constexpr decltype(auto) on_terminal(T &&t) const {
    return std::forward<T>(t);
  }

  template <class A, class B, class TA, class TB>
  constexpr auto on_node(add_tag, A &&origA, B &&origB, TA ta, TB tb) const {
    using node_t = lithe::node<add_tag, std::decay_t<A>, std::decay_t<B>>;
    using altA_t = std::decay_t<TA>;
    using altB_t = std::decay_t<TB>;

    if constexpr (std::is_arithmetic_v<altA_t> &&
                  std::is_arithmetic_v<altB_t>) {
      using result_t = std::decay_t<decltype(ta + tb)>;
      using var_t = std::variant<node_t, result_t>;
      return var_t{std::in_place_index<1>, static_cast<result_t>(ta + tb)};
    }

    if constexpr (std::is_arithmetic_v<altB_t>) {
      if (tb == 0) {
        using var_t = std::variant<node_t, altA_t>;
        return var_t{std::in_place_index<1>, ta};
      }
    }
    if constexpr (std::is_arithmetic_v<altA_t>) {
      if (ta == 0) {
        using var_t = std::variant<node_t, altB_t>;
        return var_t{std::in_place_index<1>, tb};
      }
    }

    using var_t = std::variant<node_t>;
    return var_t{std::in_place_index<0>,
                 lithe::rebuild<add_tag>(std::decay_t<A>(origA),
                                         std::decay_t<B>(origB))};
  }

  template <class A, class B, class TA, class TB>
  constexpr auto on_node(mul_tag, A &&origA, B &&origB, TA ta, TB tb) const {
    using node_t = lithe::node<mul_tag, std::decay_t<A>, std::decay_t<B>>;
    using altA_t = std::decay_t<TA>;
    using altB_t = std::decay_t<TB>;

    if constexpr (std::is_arithmetic_v<altA_t> &&
                  std::is_arithmetic_v<altB_t>) {
      using result_t = std::decay_t<decltype(ta * tb)>;
      using var_t = std::variant<node_t, result_t>;
      return var_t{std::in_place_index<1>, static_cast<result_t>(ta * tb)};
    }

    if constexpr (std::is_arithmetic_v<altB_t>) {
      if (tb == 0) {
        using var_t = std::variant<node_t, altB_t>;
        return var_t{std::in_place_index<1>, tb};
      }
      if (tb == 1) {
        using var_t = std::variant<node_t, altA_t>;
        return var_t{std::in_place_index<1>, ta};
      }
    }
    if constexpr (std::is_arithmetic_v<altA_t>) {
      if (ta == 0) {
        using var_t = std::variant<node_t, altA_t>;
        return var_t{std::in_place_index<1>, ta};
      }
      if (ta == 1) {
        using var_t = std::variant<node_t, altB_t>;
        return var_t{std::in_place_index<1>, tb};
      }
    }

    using var_t = std::variant<node_t>;
    return var_t{std::in_place_index<0>,
                 lithe::rebuild<mul_tag>(std::decay_t<A>(origA),
                                         std::decay_t<B>(origB))};
  }

  template <class Tag, class... Args>
  constexpr decltype(auto) on_node(Tag, Args &&...args) const {
    constexpr std::size_t N = sizeof...(Args);
    static_assert(N % 2 == 0);
    auto tup = std::forward_as_tuple(std::forward<Args>(args)...);
    return [&]<std::size_t... I>(std::index_sequence<I...>) {
      return lithe::rebuild<Tag>(std::get<I>(tup)...);
    }(std::make_index_sequence<N / 2>{});
  }
};

// constant_propagation_pass composes constant_fold_arith_pass (arithmetic
// folding) with constant_propagation_rule (identity folding: x+0=x, x*1=x) to
// avoid duplicating fold logic.
struct constant_propagation_pass {
  template <class E> constexpr auto operator()(E &&e) const {
    auto folded = constant_fold_arith_pass{}(std::forward<E>(e));
    auto out = rewrite_once(detail::phase_unwrap(std::move(folded)),
                            constant_propagation_rule{});
    return detail::wrap_optimized<E>(std::move(out));
  }
};

struct strength_reduction_rule {
  template <class T> constexpr decltype(auto) on_terminal(T &&t) const {
    return std::forward<T>(t);
  }

  static constexpr bool is_power_of_two(auto value) {
    if constexpr (std::is_integral_v<decltype(value)>) {
      return value > 0 && (value & (value - 1)) == 0;
    }
    return false;
  }

  static constexpr int get_power_of_two_exponent(auto value) {
    if constexpr (std::is_integral_v<decltype(value)>) {
      int result = 0;
      auto v = value;
      while (v > 1) {
        v >>= 1;
        ++result;
      }
      return result;
    }
    return 0;
  }

  // Read the scalar value out of a (possibly wrapped) terminal:
  // raw arithmetic, expr<T> (.value), or expr_ref<T> (.p).
  // Non-scalar terminals collapse to int{0} (never
  // power-of-two/unsigned-matched).
  template <class V> static constexpr auto scalar_value(const V &v) {
    using D = std::decay_t<V>;
    if constexpr (std::is_arithmetic_v<D>) {
      return v;
    } else if constexpr (requires { v.value; }) {
      return v.value;
    } else if constexpr (requires { *v.p; }) {
      return std::remove_cvref_t<decltype(*v.p)>(*v.p);
    } else {
      return 0;
    }
  }

  // Underlying scalar type of a (possibly wrapped) terminal.
  template <class V>
  using scalar_of =
      std::remove_cvref_t<decltype(scalar_value(std::declval<const V &>()))>;

  template <class A, class B, class TA, class TB>
  constexpr auto on_node(mul_tag, A &&origA, B &&origB, TA ta, TB tb) const {
    using node_t = lithe::node<mul_tag, std::decay_t<A>, std::decay_t<B>>;
    using sa_t = scalar_of<TA>;
    using sb_t = scalar_of<TB>;

    // shift-node types are value-stable (shift amount is an int value,
    // not a bound reference), so a single variant covers every return.
    using shlA_t = decltype(lithe::rebuild<shl_tag>(
        std::decay_t<A>(std::declval<A &&>()), int{}));
    using shlB_t = decltype(lithe::rebuild<shl_tag>(
        std::decay_t<B>(std::declval<B &&>()), int{}));

    // Strength-reduce mul->shl only when BOTH operands' effective
    // computation types are integral. Otherwise the shift would run on a
    // float bit-pattern (e.g. double(1.5) * uint64_t(2) must stay a mul).
    if constexpr (std::is_integral_v<sa_t> && std::is_integral_v<sb_t>) {
      using var_t = std::variant<node_t, shlA_t, shlB_t>;
      if (is_power_of_two(scalar_value(tb))) {
        int shift_amount = get_power_of_two_exponent(scalar_value(tb));
        return var_t{
            std::in_place_index<1>,
            lithe::rebuild<shl_tag>(std::decay_t<A>(origA), int(shift_amount))};
      }
      if (is_power_of_two(scalar_value(ta))) {
        int shift_amount = get_power_of_two_exponent(scalar_value(ta));
        return var_t{
            std::in_place_index<2>,
            lithe::rebuild<shl_tag>(std::decay_t<B>(origB), int(shift_amount))};
      }
      return var_t{std::in_place_index<0>,
                   lithe::rebuild<mul_tag>(std::decay_t<A>(origA),
                                           std::decay_t<B>(origB))};
    } else {
      using var_t = std::variant<node_t>;
      return var_t{std::in_place_index<0>,
                   lithe::rebuild<mul_tag>(std::decay_t<A>(origA),
                                           std::decay_t<B>(origB))};
    }
  }

  template <class A, class B, class TA, class TB>
  constexpr auto on_node(div_tag, A &&origA, B &&origB, TA ta, TB tb) const {
    using node_t = lithe::node<div_tag, std::decay_t<A>, std::decay_t<B>>;
    using sb_t = scalar_of<TB>;

    if constexpr (std::is_integral_v<sb_t> && std::is_unsigned_v<sb_t>) {
      // shift amount is a value (never a bound reference) so the shr node
      // type is stable regardless of which return statement is taken.
      using shift_node_t =
          decltype(lithe::rebuild<shr_tag>(std::decay_t<A>(origA), int{}));
      using var_t = std::variant<node_t, shift_node_t>;
      if (is_power_of_two(scalar_value(tb))) {
        int shift_amount = get_power_of_two_exponent(scalar_value(tb));
        return var_t{
            std::in_place_index<1>,
            lithe::rebuild<shr_tag>(std::decay_t<A>(origA), int(shift_amount))};
      }
      return var_t{std::in_place_index<0>,
                   lithe::rebuild<div_tag>(std::decay_t<A>(origA),
                                           std::decay_t<B>(origB))};
    } else {
      using var_t = std::variant<node_t>;
      return var_t{std::in_place_index<0>,
                   lithe::rebuild<div_tag>(std::decay_t<A>(origA),
                                           std::decay_t<B>(origB))};
    }
  }

  template <class Tag, class... Args>
  constexpr decltype(auto) on_node(Tag, Args &&...args) const {
    constexpr std::size_t N = sizeof...(Args);
    static_assert(N % 2 == 0);
    auto tup = std::forward_as_tuple(std::forward<Args>(args)...);
    return [&]<std::size_t... I>(std::index_sequence<I...>) {
      return lithe::rebuild<Tag>(std::get<I>(tup)...);
    }(std::make_index_sequence<N / 2>{});
  }
};

struct strength_reduction_pass {
  template <class E> constexpr auto operator()(E &&e) const {
    auto out = rewrite_once(detail::phase_unwrap(std::forward<E>(e)),
                            strength_reduction_rule{});
    return detail::wrap_optimized<E>(std::move(out));
  }
};

struct enhanced_algebraic_canonicalization_rule {
  template <class T> constexpr decltype(auto) on_terminal(T &&t) const {
    return std::forward<T>(t);
  }

  template <class U>
  static constexpr auto extract_numeric_value(const U &v)
      -> std::optional<double> {
    using V = std::decay_t<U>;
    if constexpr (std::is_arithmetic_v<V>) {
      return static_cast<double>(v);
    } else if constexpr (requires { v.value; }) {
      using ValT = std::decay_t<decltype(v.value)>;
      if constexpr (std::is_arithmetic_v<ValT>) {
        return static_cast<double>(v.value);
      }
    } else if constexpr (requires { v.p; }) {
      using Pointee = std::decay_t<decltype(*v.p)>;
      if constexpr (std::is_arithmetic_v<Pointee>) {
        return static_cast<double>(*v.p);
      }
    }
    return std::nullopt;
  }

  template <class A, class B, class TA, class TB>
  constexpr auto on_node(add_tag, A &&origA, B &&origB, TA ta, TB tb) const {
    auto val_a = extract_numeric_value(ta);
    auto val_b = extract_numeric_value(tb);

    if (val_a && val_b) {
      if (*val_a <= *val_b) {
        return lithe::rebuild<add_tag>(std::forward<A>(origA),
                                       std::forward<B>(origB));
      } else {
        return lithe::rebuild<add_tag>(std::forward<B>(origB),
                                       std::forward<A>(origA));
      }
    }

    if (val_a && !val_b) {
      return lithe::rebuild<add_tag>(std::forward<B>(origB),
                                     std::forward<A>(origA));
    }
    if (!val_a && val_b) {
      return lithe::rebuild<add_tag>(std::forward<A>(origA),
                                     std::forward<B>(origB));
    }

    auto hash_a = emit::structural_hash(ta);
    auto hash_b = emit::structural_hash(tb);
    if (hash_a <= hash_b) {
      return lithe::rebuild<add_tag>(std::forward<A>(origA),
                                     std::forward<B>(origB));
    } else {
      return lithe::rebuild<add_tag>(std::forward<B>(origB),
                                     std::forward<A>(origA));
    }
  }

  template <class A, class B, class TA, class TB>
  constexpr auto on_node(mul_tag, A &&origA, B &&origB, TA ta, TB tb) const {
    auto val_a = extract_numeric_value(ta);
    auto val_b = extract_numeric_value(tb);

    if (val_a && val_b) {
      if (*val_a <= *val_b) {
        return lithe::rebuild<mul_tag>(std::forward<A>(origA),
                                       std::forward<B>(origB));
      } else {
        return lithe::rebuild<mul_tag>(std::forward<B>(origB),
                                       std::forward<A>(origA));
      }
    }

    if (val_a && !val_b) {
      return lithe::rebuild<mul_tag>(std::forward<B>(origB),
                                     std::forward<A>(origA));
    }
    if (!val_a && val_b) {
      return lithe::rebuild<mul_tag>(std::forward<A>(origA),
                                     std::forward<B>(origB));
    }

    auto hash_a = emit::structural_hash(ta);
    auto hash_b = emit::structural_hash(tb);
    if (hash_a <= hash_b) {
      return lithe::rebuild<mul_tag>(std::forward<A>(origA),
                                     std::forward<B>(origB));
    } else {
      return lithe::rebuild<mul_tag>(std::forward<B>(origB),
                                     std::forward<A>(origA));
    }
  }

  template <class Tag, class... Args>
  constexpr decltype(auto) on_node(Tag, Args &&...args) const {
    constexpr std::size_t N = sizeof...(Args);
    static_assert(N % 2 == 0);
    auto tup = std::forward_as_tuple(std::forward<Args>(args)...);
    return [&]<std::size_t... I>(std::index_sequence<I...>) {
      return lithe::rebuild<Tag>(std::get<I>(tup)...);
    }(std::make_index_sequence<N / 2>{});
  }
};

struct enhanced_algebraic_canonicalization_pass {
  template <class E> constexpr auto operator()(E &&e) const {
    auto out = rewrite_once(detail::phase_unwrap(std::forward<E>(e)),
                            enhanced_algebraic_canonicalization_rule{});
    return detail::wrap_canonical<E>(std::move(out));
  }
};

// live_subtree_analysis_rule: traverses and rebuilds all nodes, tracking
// structural hashes of live subtrees. At the expression-template level,
// type-level trees cannot prune nodes (it would change the static type). Real
// dead-code elimination requires lowering to a DAG or MIR first. This rule is
// an identity rebuild that populates live_subtrees for analysis purposes — no
// elimination occurs here.
struct live_subtree_analysis_rule {
  mutable std::unordered_set<std::size_t> live_subtrees;

  template <class T> constexpr decltype(auto) on_terminal(T &&t) const {
    return std::forward<T>(t);
  }

  template <class Tag, class... Args>
  constexpr auto on_node(Tag, Args &&...args) const {
    constexpr std::size_t N = sizeof...(Args);
    static_assert(N % 2 == 0);
    auto tup = std::forward_as_tuple(std::forward<Args>(args)...);

    auto rebuilt = [&]<std::size_t... I>(std::index_sequence<I...>) {
      return lithe::rebuild<Tag>(std::get<I>(tup)...);
    }(std::make_index_sequence<N / 2>{});

    auto hash = emit::structural_hash(rebuilt);
    live_subtrees.insert(hash);

    return rebuilt;
  }

  template <class... Args>
  constexpr auto on_node(seq_tag, Args &&...args) const {
    constexpr std::size_t N = sizeof...(Args);
    static_assert(N % 2 == 0);
    auto tup = std::forward_as_tuple(std::forward<Args>(args)...);
    return [&]<std::size_t... I>(std::index_sequence<I...>) {
      return lithe::rebuild<seq_tag>(std::get<I>(tup)...);
    }(std::make_index_sequence<N / 2>{});
  }
};

struct live_subtree_analysis_pass {
  template <class E> constexpr auto operator()(E &&e) const {
    auto out = rewrite_once(detail::phase_unwrap(std::forward<E>(e)),
                            live_subtree_analysis_rule{});
    return detail::wrap_optimized<E>(std::move(out));
  }
};

// dead_subtree_elimination_pass: deprecated name — analysis only, no
// elimination. Real DCE requires lowering to DAG/MIR. Kept as a real type (not
// alias) to satisfy existing forward declarations and phase_traits
// specializations.
struct dead_subtree_elimination_pass : live_subtree_analysis_pass {};

// Aliases for old names.
using dead_subtree_elimination_rule = live_subtree_analysis_rule;
using dead_code_elimination_rule = live_subtree_analysis_rule;
using dead_code_elimination_pass = dead_subtree_elimination_pass;

// true_cse_pass: placeholder until full DAG-based CSE is integrated into the
// pass pipeline. Until then, it forwards the expression unchanged — no
// subexpression deduplication is performed. To get CSE, use emit::build_dag /
// graph::shared_expr directly.
struct true_cse_pass {
  template <class E> constexpr auto operator()(E &&e) const {
    return detail::wrap_optimized<E>(std::forward<E>(e));
  }
};

// =====================================================================
//  pass_type_traits specializations for built-in passes.
//  stable_id: builtin band [1, 1000).
//  in/out ir_stage assignments:
//    simplify_*  : canonical → canonical  (identity transform, no stage change)
//    constant_fold, cse, strength: canonical → optimized
//    dead_subtree: optimized → optimized
//    constant_propagation: canonical → optimized
//    enhanced_algebraic: canonical → optimized
//    canonicalize_commutative, live_subtree_analysis: canonical → canonical
// =====================================================================

template <>
struct pass_type_traits<simplify_add_zero_pass> : pass_type_traits_base {
  static constexpr auto id = lithe::fixed_string{"std.simplify_add_zero"};
  static constexpr pass_category category = pass_category::optimization;
  static constexpr ir_stage in_stage = ir_stage::canonical;
  static constexpr ir_stage out_stage = ir_stage::canonical;
  static constexpr std::size_t stable_id = 10;
};

template <>
struct pass_type_traits<simplify_mul_identity_pass> : pass_type_traits_base {
  static constexpr auto id = lithe::fixed_string{"std.simplify_mul_identity"};
  static constexpr pass_category category = pass_category::optimization;
  static constexpr ir_stage in_stage = ir_stage::canonical;
  static constexpr ir_stage out_stage = ir_stage::canonical;
  static constexpr std::size_t stable_id = 11;
};

template <>
struct pass_type_traits<constant_fold_arith_pass> : pass_type_traits_base {
  static constexpr auto id = lithe::fixed_string{"std.constant_fold"};
  static constexpr pass_category category = pass_category::optimization;
  static constexpr ir_stage in_stage = ir_stage::canonical;
  static constexpr ir_stage out_stage = ir_stage::optimized;
  static constexpr std::size_t stable_id = 12;
};

template <>
struct pass_type_traits<strength_reduction_pass> : pass_type_traits_base {
  static constexpr auto id = lithe::fixed_string{"std.strength_reduction"};
  static constexpr pass_category category = pass_category::optimization;
  static constexpr ir_stage in_stage = ir_stage::canonical;
  static constexpr ir_stage out_stage = ir_stage::optimized;
  static constexpr std::size_t stable_id = 13;
};

template <>
struct pass_type_traits<dead_subtree_elimination_pass> : pass_type_traits_base {
  static constexpr auto id =
      lithe::fixed_string{"std.dead_subtree_elimination"};
  static constexpr pass_category category = pass_category::optimization;
  static constexpr pass_effect_kind effect = pass_effect_kind::analyzes;
  static constexpr ir_stage in_stage = ir_stage::optimized;
  static constexpr ir_stage out_stage = ir_stage::optimized;
  static constexpr std::size_t stable_id = 14;
};

template <> struct pass_type_traits<true_cse_pass> : pass_type_traits_base {
  static constexpr auto id = lithe::fixed_string{"std.true_cse"};
  static constexpr pass_category category = pass_category::optimization;
  static constexpr pass_effect_kind effect = pass_effect_kind::placeholder;
  static constexpr ir_stage in_stage = ir_stage::canonical;
  static constexpr ir_stage out_stage = ir_stage::optimized;
  static constexpr std::size_t stable_id = 15;
};

template <>
struct pass_type_traits<constant_propagation_pass> : pass_type_traits_base {
  static constexpr auto id = lithe::fixed_string{"std.constant_propagation"};
  static constexpr pass_category category = pass_category::optimization;
  static constexpr ir_stage in_stage = ir_stage::canonical;
  static constexpr ir_stage out_stage = ir_stage::optimized;
  static constexpr std::size_t stable_id = 16;
};

template <>
struct pass_type_traits<canonicalize_commutative_pass> : pass_type_traits_base {
  static constexpr auto id =
      lithe::fixed_string{"std.canonicalize_commutative"};
  static constexpr pass_category category = pass_category::canonicalization;
  static constexpr ir_stage in_stage = ir_stage::surface;
  static constexpr ir_stage out_stage = ir_stage::canonical;
  static constexpr std::size_t stable_id = 17;
};

template <>
struct pass_type_traits<live_subtree_analysis_pass> : pass_type_traits_base {
  static constexpr auto id = lithe::fixed_string{"std.live_subtree_analysis"};
  static constexpr pass_category category = pass_category::analysis;
  static constexpr ir_stage in_stage = ir_stage::canonical;
  static constexpr ir_stage out_stage = ir_stage::canonical;
  static constexpr std::size_t stable_id = 18;
};

template <>
struct pass_type_traits<enhanced_algebraic_canonicalization_pass>
    : pass_type_traits_base {
  static constexpr auto id =
      lithe::fixed_string{"std.enhanced_algebraic_canon"};
  static constexpr pass_category category = pass_category::canonicalization;
  static constexpr ir_stage in_stage = ir_stage::surface;
  static constexpr ir_stage out_stage = ir_stage::canonical;
  static constexpr std::size_t stable_id = 19;
};

template <class Pass, class IR> struct enhanced_memoize_pass {
  Pass p;
  std::unordered_map<std::size_t, IR> *cache;
  mutable std::size_t hits = 0;
  mutable std::size_t misses = 0;

  template <class E> IR operator()(E &&e) const {
    auto h = emit::structural_hash(e);
    if (auto it = cache->find(h); it != cache->end()) {
      ++hits;
      return it->second;
    }

    ++misses;
    IR out = p(std::forward<E>(e));
    cache->insert_or_assign(h, out);
    return out;
  }

  double hit_rate() const {
    auto total = hits + misses;
    return total > 0 ? static_cast<double>(hits) / total : 0.0;
  }
};

template <class Pass, class IR>
constexpr auto enhanced_memoize(Pass p,
                                std::unordered_map<std::size_t, IR> &cache) {
  return enhanced_memoize_pass<Pass, IR>{std::move(p), std::addressof(cache), 0,
                                         0};
}

[[nodiscard]] constexpr auto make_default_optimization_pipeline() {
  return optimization_pipeline<constant_fold_arith_pass,
                               strength_reduction_pass>{
      std::tuple{constant_fold_arith_pass{}, strength_reduction_pass{}}};
}

template <class E> constexpr auto optimize_phase(canonical_expr<E> expr) {
  auto pipeline = make_default_optimization_pipeline();
  return pipeline(std::move(expr));
}
} // namespace passes

namespace preset {
template <class BasePreset, class... ExtraPasses> struct composed {
  BasePreset base;
  std::tuple<ExtraPasses...> extras;

  template <class Expr> constexpr auto operator()(Expr &&expr) const {
    auto current = base(std::forward<Expr>(expr));
    if constexpr (sizeof...(ExtraPasses) > 0) {
      current = std::apply(
          [&](const auto &...pass) {
            return compiler::compile(std::move(current), pass...);
          },
          extras);
    }
    return current;
  }

  template <class... NewPasses> constexpr auto with(NewPasses... passes) const {
    return composed<BasePreset, ExtraPasses..., NewPasses...>{
        base,
        std::tuple_cat(extras, std::tuple<NewPasses...>{std::move(passes)...})};
  }
};

template <class BasePreset> constexpr auto compose(BasePreset base) {
  return composed<BasePreset>{std::move(base), {}};
}

struct O0 {
  template <class E> constexpr auto operator()(E &&e) const {
    return std::forward<E>(e);
  }
};

struct O1 {
  int max_iters = 4;

  template <class E> constexpr auto operator()(E &&e) const {
    return compiler::compile(
        std::forward<E>(e),
        passes::fixpoint(passes::simplify_add_zero_pass{}, max_iters),
        passes::fixpoint(passes::simplify_mul_identity_pass{}, max_iters));
  }
};

struct O2 {
  int max_iters = 6;

  template <class E> constexpr auto operator()(E &&e) const {
    auto normalized = O1{max_iters}(std::forward<E>(e));
    auto canonical = passes::canonicalize(std::move(normalized));
    return passes::optimize_phase(
        std::move(canonical),
        passes::fixpoint(passes::constant_fold_arith_pass{}, max_iters));
  }
};

struct O3 {
  int max_iters = 8;

  template <class E> constexpr auto operator()(E &&e) const {
    auto normalized = O1{max_iters}(std::forward<E>(e));
    auto canonical = passes::canonicalize(std::move(normalized));
    return passes::optimize_phase(
        std::move(canonical),
        passes::fixpoint(passes::constant_fold_arith_pass{}, max_iters),
        passes::fixpoint(passes::strength_reduction_pass{}, max_iters));
  }
};

struct Debug {
  compiler::context *ctx = nullptr;
  int max_iters = 6;

  template <class E> constexpr auto operator()(E &&e) const {
    compiler::context local_ctx;
    auto *active = ctx ? ctx : &local_ctx;
    active->trace = true;
    auto traced = passes::with_trace(O2{max_iters}, *active);
    return traced(std::forward<E>(e));
  }
};

struct SemanticSafe {
  int max_iters = 6;

  template <class E> constexpr auto operator()(E &&e) const {
    auto normalized = compiler::compile(
        std::forward<E>(e),
        passes::fixpoint(passes::guarded(passes::simplify_add_zero_pass{},
                                         passes::pure_only_guard{}),
                         max_iters),
        passes::fixpoint(passes::guarded(passes::simplify_mul_identity_pass{},
                                         passes::pure_only_guard{}),
                         max_iters));
    auto canonical = passes::canonicalize(std::move(normalized));
    return compiler::compile(
        std::move(canonical),
        passes::fixpoint(passes::guarded(passes::constant_fold_arith_pass{},
                                         passes::no_throw_guard{}),
                         max_iters),
        passes::guarded(passes::true_cse_pass{}, passes::cse_safe_guard{}));
  }
};
} // namespace preset

// -------------------------------------------------------------------------
// Prompt 8 — Semantic canonicalization integration for the pass pipeline
//
// run_semantic_canonicalization_pass wraps semantic_optimization_pipeline
// execution, emits a semantic_canonicalization_event to any attached observer,
// and returns the combined semantic_optimization_report.  It operates purely
// on the semantic registry — no MIR is touched.
// -------------------------------------------------------------------------
namespace semantic_pass {
// Run `pipeline` over `node_ids` in `reg`, emit an observability event to
// `observer` (any type with on_event(semantic_canonicalization_event)),
// and return the report.
template <class Observer>
[[nodiscard]] semantic::semantic_optimization_report
run_semantic_canonicalization_pass(
    semantic::semantic_registry &reg,
    const std::vector<structural_hash_t> &node_ids,
    const semantic::semantic_optimization_pipeline &pipeline,
    Observer &observer,
    const std::string pass_name = "semantic_canonicalization") {
  using namespace compiler::observability;
  semantic_canonicalization_event ev;
  ev.pass_name = pass_name;
  ev.start_ns = now_ns();

  auto report = pipeline.run(reg, node_ids);

  ev.end_ns = now_ns();
  ev.visited_nodes = report.visited_nodes;
  ev.rewritten_nodes = report.rewritten_nodes;

  for (const auto &trace : report.traces) {
    if (trace.changed && !trace.rule_name.empty()) {
      if (std::ranges::find(ev.fired_rules, trace.rule_name) ==
          ev.fired_rules.end()) {
        ev.fired_rules.push_back(trace.rule_name);
      }
    }
  }

  observer.on_event(ev);
  return report;
}

// Overload for semantic_context.
template <class Observer>
[[nodiscard]] semantic::semantic_optimization_report
run_semantic_canonicalization_pass(
    semantic::semantic_context &ctx,
    const std::vector<structural_hash_t> &node_ids,
    const semantic::semantic_optimization_pipeline &pipeline,
    Observer &observer,
    const std::string pass_name = "semantic_canonicalization") {
  using namespace compiler::observability;
  semantic_canonicalization_event ev;
  ev.pass_name = pass_name;
  ev.start_ns = now_ns();

  auto report = pipeline.run(ctx, node_ids);

  ev.end_ns = now_ns();
  ev.visited_nodes = report.visited_nodes;
  ev.rewritten_nodes = report.rewritten_nodes;

  for (const auto &trace : report.traces) {
    if (trace.changed && !trace.rule_name.empty()) {
      if (std::ranges::find(ev.fired_rules, trace.rule_name) ==
          ev.fired_rules.end()) {
        ev.fired_rules.push_back(trace.rule_name);
      }
    }
  }

  observer.on_event(ev);
  return report;
}

// No-observer convenience overload — uses null_observer.
[[nodiscard]] inline semantic::semantic_optimization_report
run_semantic_canonicalization_pass(
    semantic::semantic_registry &reg,
    const std::vector<structural_hash_t> &node_ids,
    const semantic::semantic_optimization_pipeline &pipeline,
    std::string pass_name = "semantic_canonicalization") {
  compiler::observability::null_observer noop;
  return run_semantic_canonicalization_pass(reg, node_ids, pipeline, noop,
                                            std::move(pass_name));
}
} // namespace semantic_pass

namespace compiler {
namespace detail {
template <opt_level Level> struct preset_for;

template <> struct preset_for<opt_level::O0> {
  using type = preset::O0;
};

template <> struct preset_for<opt_level::O1> {
  using type = preset::O1;
};

template <> struct preset_for<opt_level::OG1> {
  using type = preset::O2;
};

template <> struct preset_for<opt_level::O2> {
  using type = preset::O3;
};
} // namespace detail

template <opt_level Level, class E> constexpr auto optimize_preset(E &&e) {
  return typename detail::preset_for<Level>::type{}(std::forward<E>(e));
}

template <class E>
using preset_result_t =
    std::variant<std::decay_t<decltype(preset::O0{}(std::declval<E>()))>,
                 std::decay_t<decltype(preset::O1{}(std::declval<E>()))>,
                 std::decay_t<decltype(preset::O2{}(std::declval<E>()))>,
                 std::decay_t<decltype(preset::O3{}(std::declval<E>()))>>;

template <class E>
constexpr auto optimize_preset(E &&e, const opt_level level)
    -> preset_result_t<E> {
  switch (level) {
  case opt_level::O0:
    return preset_result_t<E>{std::in_place_index<0>,
                              preset::O0{}(std::forward<E>(e))};
  case opt_level::O1:
    return preset_result_t<E>{std::in_place_index<1>,
                              preset::O1{}(std::forward<E>(e))};
  case opt_level::OG1:
    return preset_result_t<E>{std::in_place_index<2>,
                              preset::O2{}(std::forward<E>(e))};
  case opt_level::O2:
    return preset_result_t<E>{std::in_place_index<3>,
                              preset::O3{}(std::forward<E>(e))};
  default:
    return preset_result_t<E>{std::in_place_index<0>,
                              preset::O0{}(std::forward<E>(e))};
  }
}
} // namespace compiler

template <class E, class... Passes>
constexpr auto compile(E &&e, Passes &&...ps) {
  return compiler::compile(std::forward<E>(e), std::forward<Passes>(ps)...);
}

template <class E, class... Passes>
constexpr auto compile(E &&e, compiler::pass_context &ctx, Passes &&...ps) {
  return compiler::compile(std::forward<E>(e), ctx,
                           std::forward<Passes>(ps)...);
}

using O0 = preset::O0;
using O1 = preset::O1;
using O2 = preset::O2;
} // namespace lithe

#include "lithe_profiles.hpp"
