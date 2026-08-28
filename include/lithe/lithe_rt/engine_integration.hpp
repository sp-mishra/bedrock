#pragma once

// =============================================================================
// lithe_rt/engine_integration.hpp — managed code version ↔ execution lease (,
// P7)
//
// This adapter depends on BOTH lithe_engine.hpp AND lithe_rt/* (so it must be
// included by callers who need the managed-integration bridge; it is never
// pulled in by lithe_engine.hpp itself, which cannot include
// lithe_rt/engine.hpp).
//
// Responsibilities:
//   • Associates a managed_function (from lithe_rt/engine.hpp) with a
//     resource_store execution lease (from lithe_engine.hpp).
//   • Adapts the chosen typed_entry<Sig> to the ONE stable managed calling
//     convention — closing the managed_function::invoke() gap for the
//     interpreter.
//   • code_version_metadata ownership migrates to rt::code_manager at this
//     boundary ().
//   • Interpreter and typed native entries use this bridge; managed_function
//     deliberately does not guess a raw machine-entry ABI.
//
// No virtual, no macros.  Header-only C++23.
// =============================================================================

#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include "../lithe_engine.hpp" // engine types, resource_store, typed_entry
#include "../lithe_execution/artifact.hpp" // code_version_metadata
#include "../lithe_execution/entry.hpp"    // typed_entry, entry_lease
#include "../lithe_execution/resource.hpp" // resource_store, resource_handle
#include "code_metadata.hpp"               // code_resource, code_manager
#include "engine.hpp" // managed_function, compile(), runtime_instance

namespace lithe::rt {

namespace managed_abi_detail {
template <class T>
using normalized_t = std::remove_cv_t<std::remove_reference_t<T>>;

template <class T>
inline constexpr bool supported_type_v =
    std::is_void_v<normalized_t<T>> ||
    std::is_same_v<normalized_t<T>, std::int64_t> ||
    std::is_same_v<normalized_t<T>, double> ||
    std::is_same_v<normalized_t<T>, bool> ||
    std::is_same_v<normalized_t<T>, void *> ||
    std::is_same_v<normalized_t<T>, object_ref> ||
    std::is_same_v<normalized_t<T>, runtime::values::managed_handle> ||
    std::is_same_v<normalized_t<T>, runtime::values::native_function_ref>;

template <class T>
[[nodiscard]] consteval managed_abi_kind kind_of() {
  using U = normalized_t<T>;
  static_assert(supported_type_v<U>, "unsupported managed ABI type");
  if constexpr (std::is_void_v<U>) return managed_abi_kind::void_result;
  if constexpr (std::is_same_v<U, std::int64_t>) return managed_abi_kind::i64;
  if constexpr (std::is_same_v<U, double>) return managed_abi_kind::f64;
  if constexpr (std::is_same_v<U, bool>) return managed_abi_kind::boolean;
  if constexpr (std::is_same_v<U, void *>) return managed_abi_kind::raw_pointer;
  if constexpr (std::is_same_v<U, object_ref>) return managed_abi_kind::object_reference;
  if constexpr (std::is_same_v<U, runtime::values::managed_handle>)
    return managed_abi_kind::managed_handle;
  return managed_abi_kind::function_reference;
}

template <class Sig> struct signature_builder;

template <class Ret, class... Args>
struct signature_builder<Ret(Args...)> {
  [[nodiscard]] static consteval managed_signature_descriptor make() {
    static_assert(sizeof...(Args) <= managed_signature_descriptor::max_arity,
                  "managed ABI arity exceeds descriptor capacity");
    static_assert((supported_type_v<Args> && ...),
                  "unsupported managed ABI argument type");
    static_assert(supported_type_v<Ret>, "unsupported managed ABI result type");
    managed_signature_descriptor descriptor;
    descriptor.result = kind_of<Ret>();
    descriptor.arity = static_cast<std::uint8_t>(sizeof...(Args));
    std::size_t index = 0;
    ((descriptor.arguments[index++] = kind_of<Args>()), ...);
    return descriptor;
  }
};
} // namespace managed_abi_detail

template <class Sig>
inline constexpr managed_signature_descriptor managed_signature_v =
    managed_abi_detail::signature_builder<Sig>::make();
// =========================================================================
//  managed_entry_adapter<Sig>
//
// Bridges a typed_entry<Sig> (from the execution layer) to the managed
// calling convention used by managed_function::invoke().
//
// The adapter:
//   1. Holds a typed_entry<Sig> that keeps the resource alive via entry_lease.
//   2. Converts dynamic_value args to concrete Sig args (for the interpreter).
//   3. Wraps the result as a runtime_value (dynamic_value).
//   4. Connects code_version_metadata ownership to rt::code_manager.
// =========================================================================

template <class Sig> class managed_entry_adapter;

template <class Ret, class... Args> class managed_entry_adapter<Ret(Args...)> {
public:
  using signature_type = Ret(Args...);
  using entry_type = execution::typed_entry<Ret(Args...)>;
  using runtime_value = lithe::rt::runtime_value;

  managed_entry_adapter() = default;

  explicit managed_entry_adapter(entry_type entry,
                                 std::shared_ptr<code_resource> code,
                                 const code_version_id vid) noexcept
      : entry_(std::move(entry)), code_(std::move(code)), version_id_(vid) {}

  [[nodiscard]] bool valid() const noexcept {
    return entry_.valid() && code_ != nullptr;
  }

  [[nodiscard]] code_version_id version() const noexcept { return version_id_; }

  // invoke — converts dynamic args → typed, calls the entry, wraps result.
  // Only supports int64_t and double args/returns at this stage.
  [[nodiscard]] std::expected<runtime_value, trap>
  invoke(std::span<const runtime_value> args) const {
    if (!valid())
      return std::unexpected(trap::make(trap_code::corrupted_artifact, 0,
                                        version_id_, 0, 0,
                                        "managed_entry_adapter: not bound"));

    struct code_frame_guard {
      std::shared_ptr<code_resource> code;
      bool acquired = false;

      explicit code_frame_guard(std::shared_ptr<code_resource> c)
          : code(std::move(c)) {
        // Take a frame reference first, then re-read the lifecycle state.  If
        // retirement raced with the increment it will observe this frame and
        // defer reclamation; if retirement won the race, refuse entry and
        // immediately release the reference.  Never write state::active here:
        // doing so could resurrect a retiring version.
        code->active_frames.fetch_add(1, std::memory_order_acq_rel);
        const auto state = code->state.load(std::memory_order_acquire);
        if (state == code_state::retiring || state == code_state::retired) {
          code->active_frames.fetch_sub(1, std::memory_order_acq_rel);
          return;
        }
        acquired = true;
      }

      ~code_frame_guard() {
        if (acquired)
          code->active_frames.fetch_sub(1, std::memory_order_acq_rel);
      }

      [[nodiscard]] bool valid() const noexcept { return acquired; }
    } guard{code_};

    if (!guard.valid())
      return std::unexpected(
          trap::make(trap_code::deoptimization_requested, 0, version_id_, 0, 0,
                     "managed_entry_adapter: code is retiring"));

    try {
      return invoke_impl(args, std::index_sequence_for<Args...>{});
    } catch (...) {
      return std::unexpected(
          trap::make(trap_code::uncaught_exception, 0, version_id_, 0, 0,
                     "managed entry threw across the native boundary"));
    }
  }

private:
  template <std::size_t... Is>
  [[nodiscard]] std::expected<runtime_value, trap>
  invoke_impl(std::span<const runtime_value> args,
              std::index_sequence<Is...>) const {
    if (args.size() != sizeof...(Is))
      return std::unexpected(
          trap::make(trap_code::corrupted_artifact, 0, version_id_, 0, 0,
                     "managed_entry_adapter: argument count mismatch"));

    auto converted = std::tuple{extract_arg<Args>(args[Is])...};
    const bool all_valid = std::apply(
        [](const auto &...arg) { return (arg.has_value() && ...); }, converted);
    if (!all_valid)
      return std::unexpected(
          trap::make(trap_code::invalid_indirect_call, 0, version_id_, 0, 0,
                     "managed_entry_adapter: argument type mismatch"));

    // Extract typed arguments from dynamic_value.
    // Supports i64 and f64 for now ( will extend for managed refs).
    if constexpr (std::is_void_v<Ret>) {
      std::apply([this](const auto &...arg) { entry_((*arg)...); }, converted);
      return runtime::values::make_void();
    } else {
      auto result = std::apply(
          [this](const auto &...arg) { return entry_((*arg)...); }, converted);
      return wrap_result(std::move(result));
    }
  }

  template <class T>
  [[nodiscard]] static std::optional<managed_abi_detail::normalized_t<T>>
  extract_arg(const runtime_value &v) noexcept {
    using U = managed_abi_detail::normalized_t<T>;
    if constexpr (std::is_same_v<U, std::int64_t>) {
      if (const auto *p = std::get_if<std::int64_t>(&v))
        return *p;
      return std::nullopt;
    } else if constexpr (std::is_same_v<U, double>) {
      if (const auto *p = std::get_if<double>(&v))
        return *p;
      return std::nullopt;
    } else if constexpr (std::is_same_v<U, bool>) {
      if (const auto *p = std::get_if<bool>(&v))
        return *p;
      return std::nullopt;
    } else if constexpr (std::is_same_v<U, void *>) {
      if (const auto *p = std::get_if<void *>(&v)) return *p;
      return std::nullopt;
    } else if constexpr (std::is_same_v<U, object_ref>) {
      if (const auto *p = std::get_if<object_ref>(&v)) return *p;
      if (const auto *p = std::get_if<runtime::values::managed_handle>(&v))
        return p->get();
      return std::nullopt;
    } else if constexpr (std::is_same_v<U, runtime::values::managed_handle>) {
      if (const auto *p = std::get_if<runtime::values::managed_handle>(&v))
        return *p;
      return std::nullopt;
    } else if constexpr (std::is_same_v<U, runtime::values::native_function_ref>) {
      if (const auto *p = std::get_if<runtime::values::native_function_ref>(&v))
        return *p;
      return std::nullopt;
    } else {
      static_assert(managed_abi_detail::supported_type_v<U>,
                    "unsupported managed ABI argument type");
    }
  }

  template <class R>
  [[nodiscard]] static runtime_value wrap_result(R &&r) noexcept {
    if constexpr (std::is_same_v<std::decay_t<R>, std::int64_t>)
      return runtime_value{r};
    else if constexpr (std::is_same_v<std::decay_t<R>, double>)
      return runtime_value{r};
    else if constexpr (std::is_same_v<std::decay_t<R>, bool>)
      return runtime_value{r};
    else if constexpr (std::is_same_v<std::decay_t<R>, void *>)
      return runtime_value{r};
    else if constexpr (std::is_same_v<std::decay_t<R>, object_ref>)
      return runtime_value{r};
    else if constexpr (std::is_same_v<std::decay_t<R>, runtime::values::managed_handle>)
      return runtime_value{r};
    else if constexpr (std::is_same_v<std::decay_t<R>, runtime::values::native_function_ref>)
      return runtime_value{r};
    else {
      static_assert(managed_abi_detail::supported_type_v<R>,
                    "unsupported managed ABI result type");
    }
  }

  entry_type entry_;
  std::shared_ptr<code_resource> code_;
  code_version_id version_id_ = 0;
};

// =========================================================================
//  /  bind_managed_entry<Sig>
//
// Factory: given a typed_entry<Sig> (from the engine facet path) and an
// existing code_resource (from rt::code_manager), build a
// managed_entry_adapter<Sig> and wire the code_version_metadata ownership.
//
// This is the single function that closes the managed_function::invoke() gap:
//   - The caller builds a typed_entry via the engine's compile_with<>.
//   - They call bind_managed_entry to get an adapter.
//   - They pass the adapter to a managed_function_v2 or use it directly.
//
// code_version_metadata ownership transfer:
//   Before this call: owned by the execution::resource_store (engine side).
//   After this call: owned by rt::code_manager (managed side), the adapter
//   holds a shared_ptr<code_resource> to keep the resource alive.
// =========================================================================

template <class Sig>
[[nodiscard]] std::expected<managed_entry_adapter<Sig>, trap>
bind_managed_entry(execution::typed_entry<Sig> entry,
                   std::shared_ptr<code_resource> code,
                   const code_version_id vid) {
  if (!entry.valid())
    return std::unexpected(
        trap::make(trap_code::corrupted_artifact, 0, vid, 0, 0,
                   "bind_managed_entry: invalid typed_entry"));
  if (!code)
    return std::unexpected(
        trap::make(trap_code::corrupted_artifact, 0, vid, 0, 0,
                   "bind_managed_entry: null code_resource"));

  return managed_entry_adapter<Sig>{std::move(entry), std::move(code), vid};
}

// Bind a typed execution entry directly into managed_function::invoke().
// The adapter is shared because managed_function stores a copyable erased
// callable while typed_entry itself owns a move-only execution lease.
template <class Sig>
[[nodiscard]] std::expected<void, trap>
bind_managed_entry(managed_function &function,
                   execution::typed_entry<Sig> entry) {
  auto adapter = bind_managed_entry<Sig>(
      std::move(entry), function.code_resource_handle(), function.version());
  if (!adapter)
    return std::unexpected(adapter.error());
  auto shared =
      std::make_shared<managed_entry_adapter<Sig>>(std::move(*adapter));
  return function.bind_managed_invoker(
      [shared](std::span<const runtime_value> args) {
        return shared->invoke(args);
      },
      managed_signature_v<Sig>);
}

// =========================================================================
//  managed_integration_context
//
// Holds everything needed to bridge one compiled function from the engine
// side to the managed runtime side.  Passed to invoke() by the caller.
// =========================================================================

template <class Sig> struct managed_integration_context {
  managed_entry_adapter<Sig> adapter;
  std::shared_ptr<runtime_instance> runtime;

  [[nodiscard]] bool valid() const noexcept {
    return adapter.valid() && runtime != nullptr;
  }

  // Call the function through the managed adapter (interpreter path).
  [[nodiscard]] std::expected<runtime_value, trap>
  invoke(thread_attachment &thread, std::span<const runtime_value> args) const {
    if (!valid())
      return std::unexpected(
          trap::make(trap_code::corrupted_artifact, 0, 0, 0, 0,
                     "managed_integration_context: not ready"));
    if (!thread.attached())
      return std::unexpected(trap::make(trap_code::security_violation, 0, 0, 0,
                                        0, "invoke: thread not attached"));
    return adapter.invoke(args);
  }
};

// =========================================================================
//  Verification: managed_function::invoke() gap is closed
//
// For the interpreter vertical, a managed_integration_context<Sig> produces
// a callable that flows through:
//   engine::compile_with<B,Sig,IR> → typed_entry<Sig>
//   bind_managed_entry             → managed_entry_adapter<Sig>
//   managed_integration_context    → invoke() → runtime_value
//
// The gap is closed for any typed_entry whose signature is supported by the
// adapter. managed_function::invoke intentionally rejects raw native entry
// cells until a managed-call ABI bridge exists.
// =========================================================================
} // namespace lithe::rt
