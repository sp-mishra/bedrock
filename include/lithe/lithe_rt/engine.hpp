#pragma once

// ============================================================================
// lithe_rt/engine.hpp — the managed engine (M3) + language exceptions (M4)
//
// Everything that sits ABOVE runtime_instance in the dependency graph, folded
// into one header so the overlay stays compact:
//
//   Managed MIR passes : MIR carries no gc_alloc / throw / safepoint /
//     write_barrier opcode — managed semantics ride the operation_id extension
//     mechanism under domain "lithe.rt" and a SIDE TABLE keyed by instruction /
//     vreg id, so the flat-MIR ABI stays byte-stable.  annotate → verify →
//     lower.
//
//   Backend thunks : generated code calls STABLE free functions whose
//     addresses are fixed for the life of the process (it cannot embed C++
//     member-function pointers).  runtime_thunk_table hands the backend those
//     addresses; backend_runtime_context is the one context AsmJit receives.
//
//   Compile + invoke : out-of-line rooted_ref / thread_attachment members
//     (runtime_instance is complete here — the deferred-member pattern), the
//     compile() MIR pipeline, and the managed_function executable handle.
//
//   Language exceptions : rooted in-flight payload, immutable finalized
//     handler tables, two-phase unwind, the explicit landing-pad ABI, and the
//     foreign C++ boundary guard.  exception_state / unwind_phase live in
//     execution.hpp so thread_context is complete; this file adds the metadata
//     and dispatch/unwind logic.
//
// No virtual, no macros.  Header-only C++23.
// ============================================================================

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../lithe_codegen.hpp" // mir::physical_mir_function, opcode, may_trap
#include "../lithe_execution/foundation.hpp" // native_install_unavailable (P0B)
#include "../lithe_runtime.hpp" // runtime::values::dynamic_value, unwind::landing_pad
#include "code_metadata.hpp" // code_resource, code_version_metadata, source_position, function_id
#include "execution.hpp" // rooted_ref, thread_attachment, exception_state, register_save_area
#include "foundation.hpp" // typed_value, classify, managed_op, trap, object_ref
#include "instance.hpp"   // runtime_instance, execution_profile

namespace lithe::rt {
namespace cg = lithe::codegen;

// =========================================================================
// Annotation side table (prompt)
// =========================================================================
struct managed_mir_annotations {
  std::unordered_map<std::uint32_t, typed_value> values;      // by vreg id
  std::unordered_map<std::uint32_t, source_position> sources; // by instr id

  [[nodiscard]] const typed_value *value_of(const std::uint32_t vreg) const {
    const auto it = values.find(vreg);
    return it == values.end() ? nullptr : &it->second;
  }
};

// Extract the vreg id from an SSA operand list slot when present.  MIR's
// allocated_instruction records ssa_defs / ssa_uses as vreg ids.
[[nodiscard]] inline std::optional<std::uint32_t>
first_ssa_def(const cg::mir::allocated_instruction &in) noexcept {
  if (in.ssa_defs.empty())
    return std::nullopt;
  return static_cast<std::uint32_t>(in.ssa_defs.front().id);
}

// Narrow an ssa_value_id to the u32 vreg key used by the annotation table.
[[nodiscard]] inline std::uint32_t vreg_key(const cg::ssa_value_id v) noexcept {
  return static_cast<std::uint32_t>(v.id);
}

// =========================================================================
// managed-type-propagation (prompt)
// =========================================================================
struct annotate_managed_mir {
  // Result carries the annotations plus any hard rejection (ambiguous
  // semantic string) as a trap — the pass never guesses a managed class.
  struct result {
    managed_mir_annotations annotations;
    std::optional<trap> error;
    [[nodiscard]] bool ok() const noexcept { return !error.has_value(); }
  };

  [[nodiscard]] result run(const cg::mir::physical_mir_function &fn) const {
    result out;
    for (const auto &block : fn.function.blocks) {
      for (const auto &in : block.instructions) {
        // Source position (by instruction id).
        source_position sp;
        sp.mir_instruction = in.id;
        out.annotations.sources.emplace(in.id, sp);

        const auto def = first_ssa_def(in);
        if (!def)
          continue;

        typed_value tv;
        // Result type, when the backend attached a semantic string
        // through operation_attributes["type"], drives classify();
        // otherwise fall back to the opcode's abstract kind.
        const auto it = in.operation_attributes.find("type");
        if (it != in.operation_attributes.end()) {
          cg::abstract_value_type at;
          at.semantic_type = it->second;
          at.kind = classify_kind(it->second);
          tv = classify(at);
          // Reject an ambiguous managed/host string : a
          // pointer whose semantic string names neither a known
          // managed nor host form is not silently trusted.
          if (at.kind == cg::abstract_value_kind::pointer &&
              !is_known_pointer_string(it->second)) {
            out.error =
                trap::make(trap_code::corrupted_artifact, 0, 0, in.id, 0,
                           "ambiguous pointer semantic string: " + it->second);
            return out;
          }
        }

        // Mark exception / deopt roles from the extension op name.
        if (in.abstract_operation &&
            in.abstract_operation->domain == managed_op::domain) {
          const std::string_view name = in.abstract_operation->name;
          if (name == managed_op::throw_op || name == managed_op::rethrow_op)
            tv.roles = tv.roles | value_role::exception_value;
        }
        out.annotations.values.emplace(*def, tv);
      }
    }
    return out;
  }

private:
  [[nodiscard]] static cg::abstract_value_kind
  classify_kind(const std::string_view s) noexcept {
    if (s.starts_with("gc_ref") || s.starts_with("managed") ||
        s.starts_with("guest") || s.starts_with("host") || s.ends_with("_ptr"))
      return cg::abstract_value_kind::pointer;
    if (!s.empty() && (s.front() == 'i' || s.front() == 'u'))
      return cg::abstract_value_kind::integer;
    return cg::abstract_value_kind::unknown;
  }

  [[nodiscard]] static bool
  is_known_pointer_string(const std::string_view s) noexcept {
    return s.starts_with("gc_ref") || s.starts_with("managed") ||
           s.starts_with("guest") || s.starts_with("host");
  }
};

// =========================================================================
// managed-MIR verification (prompt)
// =========================================================================
struct verification_result {
  bool ok = true;
  std::vector<std::string> errors;

  void fail(std::string msg) {
    ok = false;
    errors.push_back(std::move(msg));
  }
};

[[nodiscard]] inline verification_result
verify_managed_mir(const cg::mir::physical_mir_function &fn,
                   const managed_mir_annotations &ann,
                   const execution_profile profile) {
  verification_result vr;
  const auto controls = profile_defaults::for_profile(profile);

  // Collect all SSA definitions to check use-after-def (a linear proxy for
  // dominance on a verified-SSA input; the upstream verifier proved SSA).
  std::unordered_set<std::uint32_t> defined;

  for (const auto &block : fn.function.blocks) {
    for (const auto &in : block.instructions) {
      // Every use dominated by a definition (SSA proxy).
      for (const auto u : in.ssa_uses) {
        if (!defined.count(vreg_key(u)))
          vr.fail("use of vreg " + std::to_string(u.id) +
                  " before definition (instr " + std::to_string(in.id) + ")");
      }
      for (const auto d : in.ssa_defs)
        defined.insert(vreg_key(d));

      // Managed references are never treated as integers .
      for (const auto u : in.ssa_uses) {
        const typed_value *tv = ann.value_of(vreg_key(u));
        if (tv && tv->is_managed() &&
            (in.op == cg::opcode::add || in.op == cg::opcode::sub ||
             in.op == cg::opcode::mul || in.op == cg::opcode::div ||
             in.op == cg::opcode::bit_and || in.op == cg::opcode::bit_or))
          vr.fail("managed reference used in integer op (instr " +
                  std::to_string(in.id) + ")");
        // Host pointers forbidden in untrusted code.
        if (tv && controls.forbid_host_pointers &&
            tv->pclass == ptr_class::raw_host)
          vr.fail("raw host pointer forbidden under this profile (instr " +
                  std::to_string(in.id) + ")");
        // A derived pointer must name a live base .
        if (tv && tv->is_derived() &&
            (tv->derived_base == 0 || !defined.count(tv->derived_base)))
          vr.fail("derived pointer without live base (instr " +
                  std::to_string(in.id) + ")");
      }

      // Managed store must use a write barrier : a plain store to
      // a managed slot is illegal — it must be the barrier extension op.
      if (in.op == cg::opcode::store && !in.uses.empty()) {
        const bool has_barrier =
            in.abstract_operation &&
            in.abstract_operation->domain == managed_op::domain &&
            in.abstract_operation->name == managed_op::write_barrier;
        bool stores_managed = false;
        for (const auto u : in.ssa_uses) {
          const typed_value *tv = ann.value_of(vreg_key(u));
          if (tv && tv->is_managed()) {
            stores_managed = true;
            break;
          }
        }
        if (stores_managed && !has_barrier)
          vr.fail("managed store without write barrier (instr " +
                  std::to_string(in.id) + ")");
      }

      // Every indirect call has a signature .
      if (in.op == cg::opcode::indirect_call && !fn.signature.has_value() &&
          in.operation_attributes.find("signature") ==
              in.operation_attributes.end())
        vr.fail("indirect call without signature (instr " +
                std::to_string(in.id) + ")");
    }
  }
  return vr;
}

// =========================================================================
// Lowering passes (prompt) — mandated order
//
// Each rewrites a "lithe.rt" extension op into a fast path plus a call to a
// stable runtime thunk (the actual asmjit emission is the backend boundary,
// D5).  At this layer a pass records, per instruction, the thunk it must call
// so the backend can emit it deterministically.  Passes never allocate MIR
// opcodes; they annotate.
// =========================================================================
struct lowering_plan {
  // instruction id → canonical thunk name to call on the slow path.
  std::unordered_map<std::uint32_t, std::string> thunk_calls;
  // instruction ids where a safepoint poll must be emitted.
  std::vector<std::uint32_t> safepoint_polls;
  // instruction ids that need a write barrier before the store.
  std::vector<std::uint32_t> barrier_sites;
};

struct allocation_lowering {
  void run(const cg::mir::physical_mir_function &fn,
           lowering_plan &plan) const {
    for (const auto &b : fn.function.blocks)
      for (const auto &in : b.instructions)
        if (in.abstract_operation &&
            in.abstract_operation->domain == managed_op::domain &&
            (in.abstract_operation->name == managed_op::gc_alloc ||
             in.abstract_operation->name == managed_op::gc_alloc_pinned))
          plan.thunk_calls.emplace(in.id, "rt_allocate");
  }
};

struct write_barrier_insertion {
  void run(const cg::mir::physical_mir_function &fn,
           const managed_mir_annotations &ann, lowering_plan &plan) const {
    for (const auto &b : fn.function.blocks)
      for (const auto &in : b.instructions) {
        if (in.op != cg::opcode::store)
          continue;
        for (const auto u : in.ssa_uses) {
          const typed_value *tv = ann.value_of(vreg_key(u));
          if (tv && tv->is_managed()) {
            plan.barrier_sites.push_back(in.id);
            plan.thunk_calls.emplace(in.id, "rt_write_barrier");
            break;
          }
        }
      }
  }
};

struct safepoint_placement {
  void run(const cg::mir::physical_mir_function &fn, lowering_plan &plan,
           const bool poll_on_entry) const {
    bool first = true;
    for (const auto &b : fn.function.blocks) {
      for (const auto &in : b.instructions) {
        // Entry poll.
        if (first && poll_on_entry) {
          plan.safepoint_polls.push_back(in.id);
          first = false;
        }
        // Calls that may allocate + allocation slow paths.
        if (in.op == cg::opcode::call || in.op == cg::opcode::indirect_call)
          plan.safepoint_polls.push_back(in.id);
      }
      // Loop backedge: a successor with a smaller id than this block.
      for (const auto succ : b.successors)
        if (succ <= b.id && !b.instructions.empty())
          plan.safepoint_polls.push_back(b.instructions.back().id);
    }
  }
};

// Convenience driver: run the managed lowering sub-passes that this
// header-only layer can perform (the register-allocation + physical-root-map
// + final-verify steps are the existing/backend pipeline).
[[nodiscard]] inline lowering_plan
lower_managed_mir(const cg::mir::physical_mir_function &fn,
                  const managed_mir_annotations &ann,
                  const bool poll_on_entry) {
  lowering_plan plan;
  allocation_lowering{}.run(fn, plan);
  write_barrier_insertion{}.run(fn, ann, plan);
  safepoint_placement{}.run(fn, plan, poll_on_entry);
  return plan;
}

// =========================================================================
// Stable runtime thunks for generated code (prompt)
//
// Generated (JIT/AOT) code cannot embed C++ member-function pointers; it
// calls STABLE free functions whose addresses are fixed for the life of the
// process.  No allocation occurs while binding the table.  rt_throw /
// rt_raise_trap are [[noreturn]]: in this header-only build they throw a
// managed_trap_exception carrying the structured trap, which the
// foreign-boundary guard converts back to std::expected.  A full backend
// replaces this with a real stack unwind to the landing pad.
// =========================================================================

// Carrier for a [[noreturn]] managed throw / trap in the header-only build.
struct managed_trap_exception {
  trap value;
};

// exception_object — a thrown value.  The type is the payload's layout_id
// : no separate, possibly-divergent type field.  Defined here (ahead of
// the thunks) so rt_throw can route a throw through the rooted lifecycle.
struct exception_object {
  object_ref payload{};
  [[nodiscard]] bool valid() const noexcept { return payload.valid(); }
  [[nodiscard]] std::uint64_t type_id() const noexcept {
    return payload.layout_id;
  }
};

// Exception-lifecycle helpers defined below .  Forward-declared so the
// rt_throw thunk can route through the rooted lifecycle (P0-8) rather than
// writing in_flight directly.
std::expected<void, trap>
begin_throw(runtime_instance &rt, thread_context &thread, exception_object ex);
void consume_exception(runtime_instance &rt, thread_context &thread) noexcept;

[[nodiscard]] inline object_ref
rt_allocate(runtime_instance *rt, thread_context * /*thread*/,
            const std::uint64_t layout_id) noexcept {
  if (rt == nullptr)
    return object_ref{};
  auto r = rt->heap().allocate(layout_id);
  return r ? *r : object_ref{};
}

inline void rt_write_barrier(runtime_instance *rt, const object_ref container,
                             const std::uint32_t field_offset,
                             const object_ref value) noexcept {
  if (rt == nullptr || !container.valid())
    return;
  auto *payload = static_cast<std::byte *>(payload_of(header_of(container)));
  auto *slot = reinterpret_cast<object_ref *>(payload + field_offset);
  rt->heap().write_barrier(container, slot, value);
}

inline void rt_safepoint(runtime_instance *rt, thread_context *thread,
                         const std::uint32_t safepoint_id,
                         register_save_area *registers) noexcept {
  if (rt == nullptr || thread == nullptr)
    return;
  safepoint_context ctx;
  ctx.thread = thread;
  ctx.registers = registers;
  ctx.safepoint_id = safepoint_id;
  rt->safepoints().poll(*thread, ctx);
}

// Route a managed throw through the rooted exception lifecycle (P0-8): the
// payload MUST be GC-rooted for the whole unwind window, and an already-active
// exception is consumed (replace policy) before the new one is installed so
// its root is never leaked.  begin_throw acquires the root; the carrier then
// drives the header-only unwind.
[[noreturn]] inline void rt_throw(runtime_instance *rt, thread_context *thread,
                                  const object_ref payload) {
  if (rt != nullptr && thread != nullptr) {
    if (thread->exception.active())
      consume_exception(*rt, *thread);
    (void)begin_throw(*rt, *thread, exception_object{payload});
  } else if (thread != nullptr) {
    thread->exception.in_flight = payload;
    thread->exception.phase = unwind_phase::searching;
  }
  throw managed_trap_exception{trap::from_exception(payload)};
}

[[noreturn]] inline void rt_raise_trap(runtime_instance * /*rt*/,
                                       thread_context * /*thread*/,
                                       const trap_code code,
                                       const std::uint32_t instruction_id) {
  throw managed_trap_exception{trap::make(code, 0, 0, instruction_id)};
}

struct runtime_thunk_table {
  object_ref (*allocate)(runtime_instance *, thread_context *,
                         std::uint64_t) noexcept = &rt_allocate;
  void (*write_barrier)(runtime_instance *, object_ref, std::uint32_t,
                        object_ref) noexcept = &rt_write_barrier;
  void (*safepoint)(runtime_instance *, thread_context *, std::uint32_t,
                    register_save_area *) noexcept = &rt_safepoint;
  void (*throw_)(runtime_instance *, thread_context *, object_ref) = &rt_throw;
  void (*raise_trap)(runtime_instance *, thread_context *, trap_code,
                     std::uint32_t) = &rt_raise_trap;
};

struct backend_runtime_context {
  std::shared_ptr<runtime_instance> runtime;
  runtime_thunk_table thunks{};
  security_policy policy{};
};

// =========================================================================
// Compile + invoke (prompt)
// =========================================================================

using runtime_value = runtime::values::dynamic_value;

enum class managed_abi_kind : std::uint8_t {
  void_result = 0,
  i64,
  f64,
  boolean,
  raw_pointer,
  object_reference,
  managed_handle,
  function_reference,
};

struct managed_signature_descriptor {
  static constexpr std::uint16_t current_version = 1;
  static constexpr std::size_t max_arity = 8;

  std::uint16_t version = current_version;
  managed_abi_kind result = managed_abi_kind::void_result;
  std::array<managed_abi_kind, max_arity> arguments{};
  std::uint8_t arity = 0;

  [[nodiscard]] constexpr bool valid() const noexcept {
    return version == current_version && arity <= max_arity;
  }
  [[nodiscard]] constexpr bool operator==(
      const managed_signature_descriptor &) const noexcept = default;
};

class managed_function; // defined below
[[nodiscard]] std::expected<managed_function, trap>
compile(runtime_instance &rt, const codegen::mir::physical_mir_function &fn);

// ---- rooted_ref — out-of-line members (runtime_instance now complete) ----
// The registered slot is authoritative (P0-2): resolve through the runtime.
inline rooted_ref &rooted_ref::operator=(rooted_ref &&o) noexcept {
  if (this != &o) {
    if (runtime_ && token_

                        != null_root_token)
      runtime_->release_root(token_);
    runtime_ = std::exchange(o.runtime_, nullptr);
    token_ = std::exchange(o.token_, null_root_token);
  }
  return *this;
}

inline rooted_ref::~rooted_ref() {
  if (runtime_ && token_

                      != null_root_token)
    runtime_->release_root(token_);
}

inline object_ref rooted_ref::get() const noexcept {
  return runtime_ ? runtime_->root_value(token_) : object_ref{};
}

inline object_ref *rooted_ref::slot() noexcept {
  return runtime_ ? runtime_->root_slot(token_) : nullptr;
}

// ---- exception_state — live payload from the authoritative root slot -----
inline object_ref
exception_state::live_payload(runtime_instance &rt) const noexcept {
  if (root != null_root_token) {
    const object_ref v = rt.root_value(root);
    if (v.valid())
      return v;
  }
  return in_flight;
}

// ---- thread_attachment — out-of-line members ----------------------------
inline thread_attachment &
thread_attachment::operator=(thread_attachment &&o) noexcept {
  if (this != &o) {
    if (runtime_ && context_)
      runtime_->detach_thread(context_);
    runtime_ = std::exchange(o.runtime_, nullptr);
    context_ = std::exchange(o.context_, nullptr);
  }
  return *this;
}

inline thread_attachment::~thread_attachment() {
  if (runtime_ && context_)
    runtime_->detach_thread(context_);
}

// managed_function — executable handle (prompt)
class managed_function {
public:
  using managed_invoker = std::function<std::expected<runtime_value, trap>(
      std::span<const runtime_value>)>;

  managed_function() = default;

  [[nodiscard]] std::expected<runtime_value, trap>
  invoke(thread_attachment &thread, std::span<const runtime_value> args) {
    if (!runtime_ || !code_)
      return std::unexpected(trap::make(trap_code::corrupted_artifact, 0, 0, 0,
                                        0, "managed_function: not bound"));
    if (!thread.attached())
      return std::unexpected(trap::make(trap_code::security_violation, 0, 0, 0,
                                        0, "invoke: thread not attached"));
    if (thread.context().runtime != runtime_.get())
      return std::unexpected(trap::make(
          trap_code::security_violation, function_id_, version_id_, 0, 0,
          "invoke: thread belongs to a different runtime"));
    if (code_->state.load(std::memory_order_acquire) == code_state::retiring ||
        code_->state.load(std::memory_order_acquire) == code_state::retired)
      return std::unexpected(trap::make(
          trap_code::deoptimization_requested, function_id_, version_id_, 0, 0,
          "managed_function: code version is retiring"));

    // The bound invoker is the managed-call ABI. It owns typed argument
    // conversion and result wrapping; this guard publishes the thread's
    // managed phase for safepoint/GC coordination.
    if (managed_invoker_) {
      if (signature_ && args.size() != signature_->arity)
        return std::unexpected(trap::make(
            trap_code::invalid_indirect_call, function_id_, version_id_, 0, 0,
            "managed_function: signature arity mismatch"));

      // Root object arguments and replace them with relocation-safe handles.
      // Adapters accepting object_ref read the handle immediately; signatures
      // that may retain an object across safepoints should accept managed_handle.
      std::vector<runtime_value> rooted_args(args.begin(), args.end());
      std::vector<rooted_ref> roots;
      roots.reserve(rooted_args.size());
      for (auto &arg : rooted_args) {
        if (const auto *object = std::get_if<object_ref>(&arg);
            object != nullptr && object->valid()) {
          roots.push_back(runtime_->root(*object));
          arg = runtime::values::make_managed_handle(roots.back().handle());
        }
      }

      struct frame_link_guard {
        thread_context &context;
        machine_frame frame;
        machine_frame *previous;

        frame_link_guard(thread_context &ctx, const function_id fid,
                         const code_version_id vid) noexcept
            : context(ctx), frame{ctx.current_frame, fid, vid, 0, nullptr},
              previous(ctx.current_frame) {
          context.current_frame = &frame;
        }
        ~frame_link_guard() noexcept { context.current_frame = previous; }
      } linked_frame{thread.context(), function_id_, version_id_};
      managed_frame_guard frame{thread.context()};
      return managed_invoker_(rooted_args);
    }
    // P0B: native executable-code installation is quarantined.
    // A machine entry target is bound by the backend .
    // Without one there is nothing to execute — return a structured
    // native_install_unavailable diagnostic, never fabricate a result.
    if (code_->entry.target == nullptr)
      return std::unexpected(trap::make(trap_code::unresolved_symbol,
                                        function_id_, version_id_, 0, 0,
                                        "native_install_unavailable: "
                                        "JIT backend not wired "));
    // A raw machine entry is not yet the managed-call ABI: this method
    // receives dynamic values, must establish a landing pad, and must
    // publish GC roots before native code can be entered. Do not call a
    // non-null target and fabricate an empty runtime_value. Native
    // callers use a typed_entry via engine_integration.hpp until that
    // ABI bridge is implemented.
    return std::unexpected(trap::make(
        trap_code::unresolved_symbol, function_id_, version_id_, 0, 0,
        "native_call_abi_unavailable: use a typed_entry "
        "managed integration bridge"));
  }

  [[nodiscard]] function_id id() const noexcept { return function_id_; }
  [[nodiscard]] code_version_id version() const noexcept { return version_id_; }
  [[nodiscard]] bool bound() const noexcept { return runtime_ && code_; }
  [[nodiscard]] bool invocable() const noexcept {
    return bound() && static_cast<bool>(managed_invoker_);
  }
  [[nodiscard]] const std::optional<managed_signature_descriptor> &
  signature() const noexcept {
    return signature_;
  }
  [[nodiscard]] const std::shared_ptr<code_resource> &
  code_resource_handle() const noexcept {
    return code_;
  }

  [[nodiscard]] std::expected<void, trap>
  bind_managed_invoker(
      managed_invoker invoker,
      std::optional<managed_signature_descriptor> signature = std::nullopt) {
    if (!bound())
      return std::unexpected(
          trap::make(trap_code::corrupted_artifact, function_id_, version_id_,
                     0, 0, "bind_managed_invoker: function is not bound"));
    if (!invoker)
      return std::unexpected(trap::make(trap_code::corrupted_artifact,
                                        function_id_, version_id_, 0, 0,
                                        "bind_managed_invoker: empty invoker"));
    if (signature && !signature->valid())
      return std::unexpected(trap::make(
          trap_code::corrupted_artifact, function_id_, version_id_, 0, 0,
          "bind_managed_invoker: invalid signature descriptor"));
    managed_invoker_ = std::move(invoker);
    signature_ = std::move(signature);
    return {};
  }

private:
  friend class runtime_instance;
  friend std::expected<managed_function, trap>
  compile(runtime_instance &, const codegen::mir::physical_mir_function &);

  managed_function(std::shared_ptr<runtime_instance> rt,
                   std::shared_ptr<code_resource> code, const function_id fid,
                   const code_version_id vid)
      : runtime_(std::move(rt)), code_(std::move(code)), function_id_(fid),
        version_id_(vid) {}

  std::shared_ptr<runtime_instance> runtime_;
  std::shared_ptr<code_resource> code_;
  managed_invoker managed_invoker_;
  std::optional<managed_signature_descriptor> signature_;
  function_id function_id_ = 0;
  code_version_id version_id_ = 0;
};

// runtime_instance::compile — managed MIR pipeline (prompt,).
// Defined out-of-line: needs managed_function + the pass structs complete.
[[nodiscard]] inline std::expected<managed_function, trap>
compile(runtime_instance &rt, const codegen::mir::physical_mir_function &fn) {
  // 1. Annotate (type propagation); a hard ambiguity aborts compilation.
  const annotate_managed_mir::result ann = annotate_managed_mir{}.run(fn);
  if (!ann.ok())
    return std::unexpected(*ann.error);

  // 2. Verify; failure prevents emission (prompt).
  const verification_result vr =
      verify_managed_mir(fn, ann.annotations, rt.profile());
  if (!vr.ok) {
    std::string detail = "managed MIR verification failed";
    if (!vr.errors.empty()) {
      detail += ": ";
      detail += vr.errors.front();
    }
    return std::unexpected(trap::make(trap_code::corrupted_artifact, 0, 0, 0, 0,
                                      std::move(detail)));
  }

  // 3. Lower the managed extension ops to fast-path + thunk-call plans.
  const lowering_plan plan =
      lower_managed_mir(fn, ann.annotations, rt.defaults().enforce_fuel);
  (void)plan; // consumed by the backend emitter (D5)

  // 4. Build metadata + install an owning code_resource.
  //
  // P0B: Native executable-code installation is UNAVAILABLE until a real
  // JIT backend wires an entry point ( / AsmJit).
  // We MUST NOT allocate heap bytes and present them as executable memory
  // — that is a false W^X claim and a security foothold.
  //
  // For the interpreter vertical path () the code_resource is
  // installed with zero executable bytes; the interpreter runs directly
  // from the physical_mir_function, which is the correct path.  Only
  // call-sites that expect a machine entry point (entry.target != nullptr)
  // will receive native_install_unavailable from invoke().
  //
  // When the profile requires W^X (jit_service / persistent_aot /
  // untrusted_sandbox), a zero-byte reserve still satisfies the policy
  // probe in create() and avoids the false-executable foothold.
  auto mem = executable_memory::reserve(0, rt.defaults().enforce_w_xor_x);
  if (!mem)
    return std::unexpected(mem.error());

  code_version_metadata md;
  for (const auto &[id, src] : ann.annotations.sources)
    md.source_positions.push_back(src);

  auto installed = rt.code().install(std::move(*mem), std::move(md));
  if (!installed)
    return std::unexpected(installed.error());

  return managed_function{rt.shared_from_this(), *installed,
                          (*installed)->metadata.function,
                          (*installed)->metadata.version_id};
}

// =========================================================================
// Language exceptions (prompt)
// =========================================================================

namespace uw = runtime::unwind;

// ---- Immutable handler metadata (prompt) ----------------------------
using handler_id = std::uint32_t;
inline constexpr handler_id no_handler = 0;

enum class handler_kind : std::uint8_t { catch_typed = 0, catch_all, cleanup };

struct exception_region {
  std::uintptr_t begin = 0;
  std::uintptr_t end = 0;
  std::uint16_t nesting_depth = 0;
  std::optional<handler_id> parent;

  [[nodiscard]] bool contains(const std::uintptr_t ip) const noexcept {
    return ip >= begin && ip < end;
  }
};

struct exception_handler {
  handler_id id = no_handler;
  exception_region region{};
  handler_kind kind = handler_kind::catch_typed;
  std::uint64_t catch_type = 0; // layout_id for catch_typed
  uw::landing_pad pad{};
};

using subtype_fn = bool (*)(std::uint64_t derived, std::uint64_t base) noexcept;

[[nodiscard]] inline bool exact_subtype(const std::uint64_t d,
                                        const std::uint64_t b) noexcept {
  return d == b;
}

// dispatch_result — stable IDs + a precomputed cleanup sequence .  The
// cleanup_sequence is a view into the immutable table's storage, so unwinding
// performs no allocation.
struct dispatch_result {
  std::optional<handler_id> catch_handler;
  std::span<const handler_id> cleanup_sequence;
  [[nodiscard]] bool caught() const noexcept {
    return catch_handler.has_value();
  }
};

// handler_table — finalized before install .  After finalize(), the
// handler list and the per-handler precomputed cleanup sequences are
// immutable, so dispatch() and unwinding never allocate or mutate.
class handler_table {
public:
  void add(exception_handler h) {
    if (finalized_)
      return; // immutable after finalize
    handlers_.push_back(std::move(h));
  }

  // Precompute, for every handler, the innermost→outermost cleanup chain
  // reached while unwinding to it.  Freezes the table.
  void finalize() {
    if (finalized_)
      return;
    for (const auto &h : handlers_) {
      const std::size_t begin = cleanup_pool_.size();
      // Walk cleanup handlers whose region encloses this handler's
      // region, innermost first (greater nesting_depth first).
      std::vector<const exception_handler *> chain;
      for (const auto &c : handlers_)
        if (c.kind == handler_kind::cleanup &&
            c.region.begin <= h.region.begin && c.region.end >= h.region.end &&
            c.id != h.id)
          chain.push_back(&c);
      std::sort(chain.begin(), chain.end(),
                [](const exception_handler *a, const exception_handler *b) {
                  return a->region.nesting_depth > b->region.nesting_depth;
                });
      for (const auto *c : chain)
        cleanup_pool_.push_back(c->id);
      cleanup_span_.emplace(h.id,
                            std::pair{begin, cleanup_pool_.size() - begin});
    }
    finalized_ = true;
  }

  [[nodiscard]] bool is_finalized() const noexcept { return finalized_; }
  [[nodiscard]] std::size_t size() const noexcept { return handlers_.size(); }
  [[nodiscard]] bool empty() const noexcept { return handlers_.empty(); }

  [[nodiscard]] const exception_handler *
  handler(const handler_id id) const noexcept {
    for (const auto &h : handlers_)
      if (h.id == id)
        return &h;
    return nullptr;
  }

  // Select the deepest matching catch covering ip, plus its precomputed
  // cleanup sequence.  Requires finalize().
  [[nodiscard]] dispatch_result
  dispatch(const std::uintptr_t ip, const std::uint64_t type_id,
           const subtype_fn is_subtype = exact_subtype) const {
    dispatch_result r;
    const exception_handler *best = nullptr;
    for (const auto &h : handlers_) {
      if (h.kind == handler_kind::cleanup)
        continue;
      if (!h.region.contains(ip))
        continue;
      const bool matches = (h.kind == handler_kind::catch_all) ||
                           is_subtype(type_id, h.catch_type);
      if (!matches)
        continue;
      // Deepest (greatest nesting_depth) wins.
      if (best == nullptr ||
          h.region.nesting_depth > best->region.nesting_depth)
        best = &h;
    }
    if (best != nullptr) {
      r.catch_handler = best->id;
      const auto it = cleanup_span_.find(best->id);
      if (it != cleanup_span_.end())
        r.cleanup_sequence = std::span<const handler_id>(
            cleanup_pool_.data() + it->second.first, it->second.second);
    }
    return r;
  }

private:
  std::vector<exception_handler> handlers_;
  std::vector<handler_id> cleanup_pool_; // flat cleanup storage
  std::unordered_map<handler_id, std::pair<std::size_t, std::size_t>>
      cleanup_span_;
  bool finalized_ = false;
};

// ---- Rooted exception lifecycle (prompt) ----------------------------

// Begin propagation on a thread: copy the payload into thread_context,
// register it as a GC root, and enter the search phase.  The payload stays
// rooted until mark_caught() / conversion to a trap consumes it.
inline std::expected<void, trap> begin_throw(runtime_instance &rt,
                                             thread_context &thread,
                                             const exception_object ex) {
  if (!ex.valid())
    return std::unexpected(trap::make(trap_code::null_reference, 0, 0, 0, 0,
                                      "throw of a null exception object"));
  // A prior in-flight exception's root must be released before we overwrite
  // it, or the token leaks (P0-8).  Callers that mean to replace should call
  // consume_exception first; this is the defensive backstop.
  if (thread.exception.root != null_root_token)
    rt.release_root(thread.exception.root);
  // Root the payload for the whole unwind window; the token is owned by
  // the thread's exception state and released on consume_exception().
  thread.exception.in_flight = ex.payload;
  thread.exception.root = rt.acquire_root(ex.payload);
  thread.exception.phase = unwind_phase::searching;
  return {};
}

// Rethrow the current in-flight exception .  Illegal with none active.
inline std::expected<void, trap> rethrow(thread_context &thread) {
  if (!thread.exception.active())
    return std::unexpected(trap::make(trap_code::uncaught_exception, 0, 0, 0, 0,
                                      "rethrow with no active exception"));
  thread.exception.phase = unwind_phase::searching;
  return {};
}

// The handler caught it: mark caught (the catch owns the payload now).
inline void mark_caught(thread_context &thread) noexcept {
  thread.exception.phase = unwind_phase::caught;
}

// Fully consume the exception, releasing its GC root.
inline void consume_exception(runtime_instance &rt,
                              thread_context &thread) noexcept {
  if (thread.exception.root != null_root_token)
    rt.release_root(thread.exception.root);
  thread.exception = exception_state{};
}

// Convert an uncaught exception to a structured trap that RETAINS the payload
//.  The payload stays rooted inside the trap's optional.  The live
// payload is read from the authoritative root slot (P0-2), so a GC that
// relocated it during the search phase does not yield a stale pointer.
[[nodiscard]] inline trap
make_uncaught_trap(thread_context &thread, const function_id fid = 0,
                   const std::uint32_t mir_instruction = 0) {
  const object_ref payload =
      thread.runtime ? thread.exception.live_payload(*thread.runtime)
                     : thread.exception.in_flight;
  trap t = trap::from_exception(payload, fid, mir_instruction);
  thread.exception.phase = unwind_phase::caught;
  return t;
}

// ---- Two-phase unwind (prompt) --------------------------------------

// Landing-pad ABI : the machine handler is entered with exactly these
// arguments.  Codegen and the unwinder agree on this signature.
using landing_pad_fn = void (*)(runtime_instance *, thread_context *,
                                object_ref exception, handler_id);

// Search phase result across a stack of frames.  A "frame" here is one
// function's finalized handler_table plus the faulting ip within it.
struct unwind_plan {
  std::optional<handler_id> target; // catch to transfer to
  std::vector<handler_id> cleanups; // innermost→outermost to run
  bool found = false;
};

// Search phase: walk frames without executing code, recording target catch +
// the cleanup handlers to run en route.
struct unwind_frame {
  const handler_table *table = nullptr;
  std::uintptr_t ip = 0;
};

[[nodiscard]] inline unwind_plan
search_phase(const thread_context &thread, std::span<const unwind_frame> frames,
             const subtype_fn is_subtype = exact_subtype) {
  unwind_plan plan;
  const std::uint64_t type_id = thread.exception.type_id();
  for (const auto &f : frames) {
    if (f.table == nullptr)
      continue;
    const dispatch_result dr = f.table->dispatch(f.ip, type_id, is_subtype);
    for (const handler_id c : dr.cleanup_sequence)
      plan.cleanups.push_back(c);
    if (dr.caught()) {
      plan.target = dr.catch_handler;
      plan.found = true;
      return plan; // stop at the matching catch
    }
  }
  return plan; // uncaught: cleanups collected, no target
}

// Cleanup phase: run cleanups innermost→outermost, then transfer to the
// landing pad.  Cleanup-throws policy (D2): if a cleanup
// raises a new exception, it REPLACES the original and remaining cleanups for
// the original are abandoned.  Returns the trap if the unwind ends uncaught.
inline std::expected<void, trap>
cleanup_phase(runtime_instance &rt, thread_context &thread,
              const unwind_plan &plan, const handler_table &target_table,
              const std::function<std::optional<exception_object>(handler_id)>
                  &run_cleanup,
              const landing_pad_fn enter_pad = nullptr) {
  thread.exception.phase = unwind_phase::cleanup;
  for (const handler_id c : plan.cleanups) {
    if (run_cleanup) {
      if (auto replacement = run_cleanup(c);
          replacement && replacement->valid()) {
        // D2: new exception replaces the original.
        consume_exception(rt, thread);
        auto rethrown = begin_throw(rt, thread, *replacement);
        if (!rethrown)
          return std::unexpected(rethrown.error());
        return std::unexpected(trap::from_exception(replacement->payload));
      }
    }
  }
  if (!plan.found)
    return std::unexpected(make_uncaught_trap(thread));

  // Transfer to the landing pad through the explicit ABI.  The pad receives
  // the LIVE payload from the authoritative root slot (P0-2).
  if (const exception_handler *h = target_table.handler(*plan.target)) {
    mark_caught(thread);
    if (enter_pad != nullptr)
      enter_pad(&rt, &thread, thread.exception.live_payload(rt), h->id);
  }
  return {};
}

// ---- Foreign C++ boundary (prompt) ----------------------------------
template <class Fn>
[[nodiscard]] auto guard_foreign_boundary(Fn &&fn)
    -> std::expected<std::invoke_result_t<Fn>, trap> {
  using result_type = std::invoke_result_t<Fn>;
  static_assert(
      !std::is_reference_v<result_type>,
      "guard_foreign_boundary: reference-returning callbacks are unsupported");
  try {
    if constexpr (std::is_void_v<result_type>) {
      std::invoke(std::forward<Fn>(fn));
      return {};
    } else {
      return std::invoke(std::forward<Fn>(fn));
    }
  } catch (const std::exception &e) {
    return std::unexpected(
        trap::make(trap_code::uncaught_exception, 0, 0, 0, 0, e.what()));
  } catch (...) {
    return std::unexpected(trap::make(trap_code::uncaught_exception, 0, 0, 0, 0,
                                      "unknown foreign exception"));
  }
}
} // namespace lithe::rt
