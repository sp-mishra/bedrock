#pragma once

#include "../lithe_codegen_pipeline.hpp"
#include "../lithe_runtime.hpp"
#include <cassert>
#include <stdexcept>

// AsmJit architecture selection.
// On AArch64 (Apple Silicon, ARM servers) use a64::Compiler.
// Elsewhere default to x86::Compiler.
#if defined(__aarch64__) || defined(_M_ARM64)
#  define LITHE_ASMJIT_ARCH_A64 1
#  include <asmjit/a64.h>
#else
#  define LITHE_ASMJIT_ARCH_X86 1
#  include <asmjit/x86.h>
#endif
#include <asmjit/core.h>

#include <cstdint>
#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace lithe::codegen::backends {
    // ---------------------------------------------------------------------------
    // jit_function_handle
    //
    // Owns a heap-allocated JitRuntime and the function pointer it produced.
    // Move-only: copying would allow double-release of the executable page.
    // ---------------------------------------------------------------------------
    struct jit_function_handle {
        using fn_i64_t = std::int64_t (*)(std::int64_t, std::int64_t);
        using fn_f64_t = double (*)(std::int64_t, std::int64_t);

        // JitRuntime is non-copyable, so we heap-allocate it.
        std::unique_ptr<asmjit::JitRuntime> runtime;
        fn_i64_t fn_ptr = nullptr; // set when the JIT function returns i64
        fn_f64_t fn_ptr_f64 = nullptr; // set when the JIT function returns f64

        jit_function_handle() : runtime(std::make_unique<asmjit::JitRuntime>()) {}

        ~jit_function_handle() {
            if (fn_ptr && runtime) {
                runtime->release(fn_ptr);
            }
            else if (fn_ptr_f64 && runtime) {
                runtime->release(fn_ptr_f64);
            }
        }

        jit_function_handle(const jit_function_handle&) = delete;

        jit_function_handle& operator=(const jit_function_handle&) = delete;

        jit_function_handle(jit_function_handle&& o) noexcept
            : runtime(std::move(o.runtime)),
              fn_ptr(std::exchange(o.fn_ptr, nullptr)),
              fn_ptr_f64(std::exchange(o.fn_ptr_f64, nullptr)) {}

        jit_function_handle& operator=(jit_function_handle&& o) noexcept {
            if (this != &o) {
                if (fn_ptr && runtime) runtime->release(fn_ptr);
                else if (fn_ptr_f64 && runtime) runtime->release(fn_ptr_f64);
                runtime = std::move(o.runtime);
                fn_ptr = std::exchange(o.fn_ptr, nullptr);
                fn_ptr_f64 = std::exchange(o.fn_ptr_f64, nullptr);
            }
            return *this;
        }

        [[nodiscard]] std::int64_t call(const std::int64_t a, const std::int64_t b) const {
            // Unconditional contract check: an assert would vanish under NDEBUG and
            // let a wrong-lane call dereference a null pointer in release builds.
            if (fn_ptr == nullptr) {
                throw std::logic_error("jit_function_handle: call() on non-i64 or invalid handle");
            }
            return fn_ptr(a, b);
        }

        [[nodiscard]] double call_f64(const std::int64_t a, const std::int64_t b) const {
            if (fn_ptr_f64 == nullptr) {
                throw std::logic_error("jit_function_handle: call_f64() on non-f64 or invalid handle");
            }
            return fn_ptr_f64(a, b);
        }

        [[nodiscard]] bool valid() const noexcept { return fn_ptr != nullptr || fn_ptr_f64 != nullptr; }
        [[nodiscard]] bool returns_f64() const noexcept { return fn_ptr_f64 != nullptr; }
    };

    // ---------------------------------------------------------------------------
    // asmjit_backend
    //
    // A CodeEmissionTarget that lowers Lithe physical MIR to native machine code
    // using AsmJit's Compiler API.  Supports:
    //   • AArch64 (primary — macOS M1 / ARM servers)
    //   • x86-64  (secondary — Linux/Windows/macOS Intel)
    //
    // emit() returns a compilation_artifact with kind == jit_function.
    // The JIT-compiled function handle is stored in art.handle (shared ownership).
    // Cast art.handle->get<jit_function_handle>() to access the function pointer.
    //
    // Instruction coverage:
    //   nop, mov, load_imm, load_arg,
    //   add, sub, mul, neg,
    //   bit_and, bit_or, bit_xor, bit_not, shl, shr,
    //   logical_and, logical_or, logical_not,
    //   cmp_eq, cmp_ne, cmp_lt, cmp_le, cmp_gt, cmp_ge,
    //   load_spill, store_spill,
    //   branch, branch_cond, ret,
    //   fadd, fsub, fmul, fdiv, fneg,
    //   fload, fload_imm, gpr_to_fp, fp_to_gpr,
    //   fcmp_eq, fcmp_ne, fcmp_lt, fcmp_le, fcmp_gt, fcmp_ge
    // ---------------------------------------------------------------------------
    struct asmjit_backend {
        // ----------------------------------------------------------------
        // LitheExtension protocol
        // ----------------------------------------------------------------
        static constexpr lithe::plugin_descriptor<
            sizeof("lithe.backend.asmjit"),
            sizeof("Lithe")
        > descriptor{
            .id = "lithe.backend.asmjit",
            .version = {1, 0, 0},
            .author = "Lithe",
            .domain = lithe::semantic::domain_type::unknown,
        };

        // ----------------------------------------------------------------
        // CodeEmissionTarget protocol
        // ----------------------------------------------------------------
        [[nodiscard]] static constexpr backend_capability_set capabilities() {
            return backend_capability_set::from({
                backend_feature::integer_arithmetic,
                backend_feature::floating_arithmetic,
                backend_feature::spill_load_store,
                backend_feature::branches,
                backend_feature::calls,
                backend_feature::memory_operands,
                backend_feature::stack_frame,
            });
        }

        [[nodiscard]] static emission_target_traits traits() {
            return emission_target_traits{
                .name = "asmjit_backend",
                .input_phase = target_input_phase::physical_mir,
                .produced_artifact = artifact_kind::jit_function,
                .capabilities = capabilities(),
                .operation_requirements = backend_capability_requirement{
                    .backend_name = "asmjit_backend",
                    .supported_operation_domains = {"lithe.core", "lithe.linker"},
                },
            };
        }

        [[nodiscard]] compilation_artifact emit(mir::physical_mir_function const& fn) {
#if LITHE_HAS_THREAD_LOCAL_SINK
            utils::nadi::PulseScope<utils::nadi::ThreadLocalSink, "lithe.asmjit.emit"> _nadi_emit{};
#endif
#if defined(LITHE_ASMJIT_ARCH_A64)
            return emit_a64(fn);
#else
            return emit_x86(fn);
#endif
        }

        // Retrieve the jit_function_handle from an artifact (non-owning).
        // Returns nullptr if the artifact carries no JIT handle.
        [[nodiscard]] static jit_function_handle*
        get_handle(const compilation_artifact& art) {
            if (!art.handle || art.handle->kind != artifact_handle_kind::jit_function)
                return nullptr;
            return art.handle->get<jit_function_handle>();
        }

        // ----------------------------------------------------------------
        // MOP integration — optional; set before calling emit() to enable
        // MOP opcode lowering (mop.alloc, mop.get_field, mop.invoke_method).
        // The context is borrowed (non-owning) and must outlive emit().
        // ----------------------------------------------------------------
        void set_mop_context(lithe::runtime::mop::mop_context* ctx) noexcept {
            mop_ctx_ = ctx;
        }

        [[nodiscard]] lithe::runtime::mop::mop_context* mop_context_ptr() const noexcept {
            return mop_ctx_;
        }

        // ----------------------------------------------------------------
        // Stack-map table integration — optional; set before calling emit()
        // to collect GC root metadata for every compiled function.
        // ----------------------------------------------------------------
        void set_stack_map_table(lithe::runtime::safepoint::stack_map_table* tbl) noexcept {
            smt_ = tbl;
        }

        [[nodiscard]] lithe::runtime::safepoint::stack_map_table*
        stack_map_table_ptr() const noexcept {
            return smt_;
        }

        // ----------------------------------------------------------------
        // Unwind registry integration — optional; set before calling emit()
        // to collect unwind metadata for every compiled function.
        // ----------------------------------------------------------------
        void set_unwind_registry(lithe::runtime::unwind::unwind_registry* reg) noexcept {
            unwind_reg_ = reg;
        }

        [[nodiscard]] lithe::runtime::unwind::unwind_registry*
        unwind_registry_ptr() const noexcept {
            return unwind_reg_;
        }

        // ----------------------------------------------------------------
        // Sandbox integration — optional; set before calling emit() to enable
        // fuel_check_tag lowering (decrement fuel, trap on exhaustion).
        // The context is borrowed (non-owning) and must outlive emit().
        // If null, fuel_check_tag instructions are skipped (zero-overhead NOP).
        // ----------------------------------------------------------------
        void set_sandbox(lithe::runtime::ExecutionSandbox* sb) noexcept {
            sandbox_ = sb;
        }

        [[nodiscard]] lithe::runtime::ExecutionSandbox* sandbox_ptr() const noexcept {
            return sandbox_;
        }

        // ----------------------------------------------------------------
        // FFI registry — optional; maps interned symbol names to native_proxy
        // descriptors so the linker emit path can use the correct ABI arity.
        // If a symbol is not in the registry the backend falls back to the
        // legacy two-i64 signature (no regression).
        // ----------------------------------------------------------------
        void register_native_proxy(std::string name,
                                   lithe::runtime::ffi::native_proxy proxy) {
            ffi_registry_[std::move(name)] = proxy;
        }

        [[nodiscard]] const lithe::runtime::ffi::native_proxy*
        find_native_proxy(const std::string_view name) const noexcept {
            auto it = ffi_registry_.find(std::string(name));
            if (it == ffi_registry_.end()) return nullptr;
            return &it->second;
        }

        // ----------------------------------------------------------------
        // Linker integration — optional; set before calling emit() to enable
        // external symbol resolution (eager or lazy via resolve_symbol_stub).
        // The context is borrowed (non-owning) and must outlive emit().
        // ----------------------------------------------------------------
        void set_linker_context(lithe::runtime::linker::linker_context* ctx) noexcept {
            linker_ctx_ = ctx;
        }

        [[nodiscard]] lithe::runtime::linker::linker_context*
        linker_context_ptr() const noexcept {
            return linker_ctx_;
        }

    private:
        lithe::runtime::mop::mop_context* mop_ctx_ = nullptr;
        lithe::runtime::safepoint::stack_map_table* smt_ = nullptr;
        lithe::runtime::unwind::unwind_registry* unwind_reg_ = nullptr;
        lithe::runtime::linker::linker_context* linker_ctx_ = nullptr;
        lithe::runtime::ExecutionSandbox* sandbox_ = nullptr;
        std::unordered_map<std::string,
                           lithe::runtime::ffi::native_proxy> ffi_registry_;

        // ------------------------------------------------------------------
        // Collect safepoint instructions from a compiled function and register
        // the resulting stack_map in smt_ (if set).
        // ------------------------------------------------------------------
        void register_stack_map(const mir::physical_mir_function& phys) const {
            if (!smt_) return;
            using namespace lithe::runtime::safepoint;

            stack_map sm;
            sm.fn_name = phys.function.name;

            for (const auto& block : phys.function.blocks) {
                for (const auto& inst : block.instructions) {
                    if (inst.op != opcode::indirect_call) continue;
                    if (!inst.abstract_operation) continue;
                    if (inst.abstract_operation->domain != std::string(safepoint_domain)) continue;

                    live_set roots;
                    roots.reserve(inst.uses.size());
                    for (const auto& use : inst.uses) {
                        if (use.type == allocated_operand::kind::preg)
                            roots.push_back(std::get<preg>(use.value).id);
                    }

                    safepoint_record rec;
                    rec.instr_id = inst.id;
                    rec.roots = std::move(roots);
                    sm.insert(std::move(rec));
                }
            }

            smt_->register_map(std::move(sm));
        }

        // ------------------------------------------------------------------
        // Unwind tracking state — populated during emit, consumed after
        // finalization to build and register an unwind_table.
        // ------------------------------------------------------------------
        struct unwind_label_pair {
            asmjit::Label begin_label;
            asmjit::Label end_label;
            bool end_set = false;
        };

        struct unwind_lp_record {
            asmjit::Label label;
            std::uint32_t cleanup_flags = 0;
        };

        // Build and register the unwind_table for fn after JIT finalization.
        // begin_pairs: open region bracket pairs (begin/end labels).
        // lp_records:  landing_pad_tag labels.
        // code:        finalized CodeHolder (for offset query).
        // base_addr:   virtual base address of the emitted code.
        void register_unwind_table(
            const std::string& fn_name,
            const std::vector<unwind_label_pair>& begin_pairs,
            const std::vector<unwind_lp_record>& lp_records,
            const asmjit::CodeHolder& code,
            const uintptr_t base_addr) const {
            if (!unwind_reg_) return;
            if (begin_pairs.empty() && lp_records.empty()) return;

            auto label_valid = [&](const asmjit::Label& lbl) -> bool {
                return code.is_label_valid(lbl.id()) &&
                    code.label_entry_of(lbl).is_bound();
            };

            using namespace lithe::runtime::unwind;
            unwind_table tbl;
            tbl.fn_name = fn_name;

            // Match each region pair with the next landing_pad_tag by index (1:1 pairing).
            // Regions without a paired landing_pad are recorded with pad.address==0.
            const std::size_t n = begin_pairs.size();
            for (std::size_t i = 0; i < n; ++i) {
                const auto& pair = begin_pairs[i];
                if (!pair.end_set) continue;
                if (!label_valid(pair.begin_label) || !label_valid(pair.end_label)) continue;

                const auto begin_off = code.label_offset_from_base(pair.begin_label);
                const auto end_off = code.label_offset_from_base(pair.end_label);
                if (end_off <= begin_off) continue;

                landing_pad pad{};
                if (i < lp_records.size() && label_valid(lp_records[i].label)) {
                    pad.address = base_addr + static_cast<uintptr_t>(
                        code.label_offset_from_base(lp_records[i].label));
                    pad.cleanup_flags = lp_records[i].cleanup_flags;
                }

                unwind_entry entry;
                entry.range = {
                    base_addr + static_cast<uintptr_t>(begin_off),
                    base_addr + static_cast<uintptr_t>(end_off)
                };
                entry.pad = pad;
                tbl.insert(std::move(entry));
            }

            if (!tbl.empty())
                unwind_reg_->register_table(std::move(tbl));
        }

        static std::string asmjit_err(const asmjit::Error e) {
            return asmjit::DebugUtils::error_as_string(e);
        }

        // ----------------------------------------------------------------
        // MOP opcode helper — architecture-independent.
        //
        // When the backend sees opcode::indirect_call with an abstract_operation
        // in the "lithe.mop" domain, it calls this helper instead of emitting
        // native code for a generic indirect call.
        //
        // Strategy (zero-overhead at runtime):
        //   mop_alloc        — layout_id is a compile-time immediate;
        //                      resolves the allocation at JIT-compile time and
        //                      embeds the resulting pointer as a 64-bit immediate
        //                      so the JIT-compiled function always returns the
        //                      same pre-allocated instance (suitable for singletons
        //                      or prototype-based languages).
        //                      For per-call allocation, callers should emit a
        //                      native call to the allocator thunk directly.
        //   mop_get_field    — resolves the field byte_offset at JIT-compile time
        //                      and emits an ADD of the object pointer + offset,
        //                      equivalent to a struct member access.
        //   mop_invoke_method — if the method has a fn_ptr, emits a native indirect
        //                       call to it; otherwise records a diagnostic.
        //
        // Returns true if the opcode was handled, false if it should fall through
        // to the generic "unsupported opcode" diagnostic.
        //
        // out_imm_value is set to the resolved 64-bit value for alloc/get_field so
        // the arch-specific path can load it into a register.
        struct mop_dispatch_result {
            enum class kind { unhandled, immediate, field_offset, method_fn_ptr, error };

            kind k = kind::unhandled;
            std::int64_t value = 0; // layout pointer, field offset, or fn_ptr
            std::string error_msg;
        };

        [[nodiscard]] mop_dispatch_result try_mop_dispatch(
            const allocated_instruction& inst) const {
            if (!mop_ctx_ || !mop_ctx_->valid()) return {};
            if (!inst.abstract_operation) return {};

            const auto& op = *inst.abstract_operation;
            if (op.domain != "lithe.mop") return {};

            using namespace lithe::runtime::mop::opcodes;

            // ---- mop.alloc -------------------------------------------------------
            if (op.name == mop_alloc) {
                if (inst.uses.empty() ||
                    inst.uses[0].type != allocated_operand::kind::immediate_i64)
                    return {
                        mop_dispatch_result::kind::error, 0,
                        "mop.alloc: uses[0] must be layout_id immediate"
                    };

                const auto layout_id = static_cast<std::uint64_t>(
                    std::get<std::int64_t>(inst.uses[0].value));
                auto result = mop_ctx_->alloc_fn(layout_id);
                if (!result)
                    return {mop_dispatch_result::kind::error, 0, result.error().message};

                return {
                    mop_dispatch_result::kind::immediate,
                    reinterpret_cast<std::int64_t>(result->raw), {}
                };
            }

            // ---- mop.get_field ---------------------------------------------------
            if (op.name == mop_get_field) {
                if (inst.uses.size() < 2)
                    return {
                        mop_dispatch_result::kind::error, 0,
                        "mop.get_field: need obj_ptr and field_hash uses"
                    };
                if (inst.uses[1].type != allocated_operand::kind::immediate_i64)
                    return {
                        mop_dispatch_result::kind::error, 0,
                        "mop.get_field: uses[1] must be field_hash immediate"
                    };

                const auto field_hash = static_cast<std::uint64_t>(
                    std::get<std::int64_t>(inst.uses[1].value));

                // Resolve field offset from the registry using field_hash.
                // We need a sentinel object_ptr to call get_field_fn; we pass
                // a null-valued one and let the registry do the lookup only.
                // get_field_fn is responsible for the null-ptr check; we only
                // need the offset, which equals (field_ptr - obj_ptr).
                // Instead, look up the field descriptor directly from registry.
                if (!mop_ctx_->registry)
                    return {
                        mop_dispatch_result::kind::error, 0,
                        "mop.get_field: no layout_registry attached to mop_context"
                    };

                // The object pointer is in uses[0] (a preg); the layout_id is not
                // directly encoded here — we must find it from the object's layout_id
                // at runtime.  Since we resolve at JIT-compile time, we need it as
                // an immediate.  Callers should include layout_id as uses[2] for
                // static resolution; if absent, we defer to a runtime call.
                if (inst.uses.size() >= 3 &&
                    inst.uses[2].type == allocated_operand::kind::immediate_i64) {
                    const auto layout_id = static_cast<std::uint64_t>(
                        std::get<std::int64_t>(inst.uses[2].value));
                    const auto* lay = mop_ctx_->registry->find(layout_id);
                    if (!lay)
                        return {
                            mop_dispatch_result::kind::error, 0,
                            "mop.get_field: layout_id not found in registry"
                        };

                    for (const auto& [name, fd] : lay->field_map) {
                        if (std::hash<std::string>{}(name) == field_hash)
                            return {
                                mop_dispatch_result::kind::field_offset,
                                static_cast<std::int64_t>(fd.byte_offset), {}
                            };
                    }
                    return {
                        mop_dispatch_result::kind::error, 0,
                        "mop.get_field: no field matches hash"
                    };
                }
                // No static layout_id — cannot resolve at JIT-compile time.
                return {
                    mop_dispatch_result::kind::error, 0,
                    "mop.get_field: add layout_id as uses[2] for static resolution"
                };
            }

            // ---- mop.invoke_method -----------------------------------------------
            if (op.name == mop_invoke_method) {
                if (inst.uses.size() < 2)
                    return {
                        mop_dispatch_result::kind::error, 0,
                        "mop.invoke_method: need obj_ptr and method_id uses"
                    };
                if (inst.uses[1].type != allocated_operand::kind::immediate_i64)
                    return {
                        mop_dispatch_result::kind::error, 0,
                        "mop.invoke_method: uses[1] must be method_id immediate"
                    };

                const auto method_id = static_cast<std::uint64_t>(
                    std::get<std::int64_t>(inst.uses[1].value));

                // Uses[2] = optional layout_id immediate for static fn_ptr resolution.
                if (inst.uses.size() >= 3 &&
                    inst.uses[2].type == allocated_operand::kind::immediate_i64 &&
                    mop_ctx_->registry) {
                    const auto layout_id = static_cast<std::uint64_t>(
                        std::get<std::int64_t>(inst.uses[2].value));
                    const auto* lay = mop_ctx_->registry->find(layout_id);
                    if (lay) {
                        const auto mit = lay->method_table.find(method_id);
                        if (mit != lay->method_table.end() && mit->second.fn_ptr)
                            return {
                                mop_dispatch_result::kind::method_fn_ptr,
                                reinterpret_cast<std::int64_t>(mit->second.fn_ptr),
                                {}
                            };
                    }
                }
                // No fn_ptr resolvable at JIT-compile time.
                return {
                    mop_dispatch_result::kind::error, 0,
                    "mop.invoke_method: no static fn_ptr; supply method_table fn_ptr"
                };
            }

            // ---- mop.dealloc (no code emitted at JIT time — handled via RAII) ----
            if (op.name == lithe::runtime::mop::opcodes::mop_dealloc)
                return {mop_dispatch_result::kind::immediate, 0, {}}; // nop

            return {}; // unknown MOP sub-opcode
        }

        // ----------------------------------------------------------------
        // Linker dispatch — architecture-independent.
        //
        // Called from the indirect_call handler when abstract_operation is
        // in the "lithe.linker" domain.
        //
        // Strategy:
        //   1. Extract the symbol name from uses[0] (must be kind::symbol).
        //   2. Attempt eager resolution via linker_context::resolve().
        //   3. If resolved → return the fn_ptr so the arch path emits a
        //      direct call.
        //   4. If unresolved → record a stub_patch and return the address
        //      of resolve_symbol_stub so the arch path emits a lazy
        //      trampoline call.
        //
        // Returns: { found=true, addr=<fn>, name=<interned> }
        //          { found=false, addr=stub, name=<interned> }  (lazy path)
        //          { found=false, addr=nullptr, name="" }       (error)
        // ----------------------------------------------------------------
        struct linker_dispatch_result {
            bool found{false}; // true = eager, false = lazy or error
            void* addr{nullptr};
            std::string_view sym_name{};
            bool is_lazy{false};
        };

        [[nodiscard]] linker_dispatch_result try_linker_dispatch(
            const allocated_instruction& inst,
            const std::uintptr_t call_site_hint = 0) const {
            if (!linker_ctx_) return {};
            if (!inst.abstract_operation) return {};
            if (inst.abstract_operation->domain !=
                std::string(lithe::runtime::linker::linker_domain))
                return {};
            if (inst.abstract_operation->name !=
                std::string(lithe::runtime::linker::external_call_tag_name))
                return {};

            // uses[0] must be kind::symbol carrying the extern name.
            if (inst.uses.empty() ||
                inst.uses[0].type != allocated_operand::kind::symbol)
                return {};

            const auto& sym_str = std::get<std::string>(inst.uses[0].value);
            std::string_view name{sym_str};

            // Eager path: symbol already registered.
            void* addr = linker_ctx_->resolve(name);
            if (addr)
                return {true, addr, name, false};

            // Lazy path: emit call through resolve_symbol_stub trampoline.
            // Record a stub_patch so the caller can bulk-patch after link time.
            linker_ctx_->record_stub_patch(call_site_hint, name);
            void* stub = reinterpret_cast<void*>(&lithe::runtime::linker::resolve_symbol_stub);
            return {false, stub, name, true};
        }

#if defined(LITHE_ASMJIT_ARCH_A64)

        using A64Compiler = asmjit::a64::Compiler;
        using A64Gp = asmjit::a64::Gp;
        using A64Vec = asmjit::a64::Vec;
        using RegMap = std::unordered_map<std::uint16_t, A64Gp>;
        using FpRegMap = std::unordered_map<std::uint16_t, A64Vec>;
        using SpillMap = std::unordered_map<std::uint32_t, asmjit::a64::Mem>;

        static A64Gp get_vreg(A64Compiler& cc, RegMap& reg_map, std::uint16_t id) {
            auto it = reg_map.find(id);
            if (it != reg_map.end()) return it->second;
            auto vr = cc.new_gpx();
            reg_map.emplace(id, vr);
            return vr;
        }

        static bool read_to_reg(A64Compiler& cc,
                                RegMap& reg_map,
                                SpillMap& spill_map,
                                asmjit::FuncNode* func_node,
                                const allocated_operand& op,
                                A64Gp& out) {
            switch (op.type) {
            case allocated_operand::kind::preg:
                out = get_vreg(cc, reg_map, std::get<preg>(op.value).id);
                return true;
            case allocated_operand::kind::immediate_i64:
                out = cc.new_gpx();
                cc.mov(out, asmjit::Imm(std::get<std::int64_t>(op.value)));
                return true;
            case allocated_operand::kind::argument_index: {
                const auto idx = std::get<std::uint32_t>(op.value);
                out = cc.new_gpx();
                func_node->set_arg(idx, out);
                return true;
            }
            case allocated_operand::kind::spill: {
                const auto slot = std::get<spill_slot>(op.value).id;
                auto& mem = ensure_spill(cc, spill_map, slot);
                out = cc.new_gpx();
                cc.ldr(out, mem);
                return true;
            }
            default:
                return false;
            }
        }

        static void write_def(A64Compiler& cc,
                              RegMap& reg_map,
                              SpillMap& spill_map,
                              const allocated_operand& def,
                              A64Gp src) {
            if (def.type == allocated_operand::kind::preg) {
                A64Gp dst = get_vreg(cc, reg_map, std::get<preg>(def.value).id);
                cc.mov(dst, src);
            }
            else if (def.type == allocated_operand::kind::spill) {
                const auto slot = std::get<spill_slot>(def.value).id;
                auto& mem = ensure_spill(cc, spill_map, slot);
                cc.str(src, mem);
            }
        }

        // A physical register already denotes the final allocation chosen by
        // physical MIR.  Emit directly into it instead of manufacturing a
        // short-lived virtual register followed by a move.  Spills still need
        // a temporary and are committed through write_def().
        static A64Gp def_or_temp(A64Compiler& cc,
                                 RegMap& reg_map,
                                 const allocated_operand& def) {
            if (def.type == allocated_operand::kind::preg)
                return get_vreg(cc, reg_map, std::get<preg>(def.value).id);
            return cc.new_gpx();
        }

        static void commit_def(A64Compiler& cc,
                               RegMap& reg_map,
                               SpillMap& spill_map,
                               const allocated_operand& def,
                               A64Gp value) {
            if (def.type != allocated_operand::kind::preg)
                write_def(cc, reg_map, spill_map, def, value);
        }

        // ── FP register helpers (A64 double-precision SIMD registers) ──────────

        static A64Vec get_fp_vreg(A64Compiler& cc, FpRegMap& fp_map, std::uint16_t id) {
            auto it = fp_map.find(id);
            if (it != fp_map.end()) return it->second;
            auto vr = cc.new_vec_d();
            fp_map.emplace(id, vr);
            return vr;
        }

        static bool read_fp_to_reg(A64Compiler& cc,
                                   FpRegMap& fp_map,
                                   const allocated_operand& op,
                                   A64Vec& out) {
            if (op.type == allocated_operand::kind::preg) {
                out = get_fp_vreg(cc, fp_map, std::get<preg>(op.value).id);
                return true;
            }
            return false;
        }

        static void write_fp_def(A64Compiler& cc,
                                 FpRegMap& fp_map,
                                 const allocated_operand& def,
                                 A64Vec src) {
            if (def.type == allocated_operand::kind::preg) {
                A64Vec dst = get_fp_vreg(cc, fp_map, std::get<preg>(def.value).id);
                cc.fmov(dst.d(), src.d());
            }
        }

        // Returns a stack slot memory ref for spill id, creating it on first use.
        // Prefers a concrete offset from frame_layout when available.
        static asmjit::a64::Mem& ensure_spill(A64Compiler& cc,
                                              SpillMap& spill_map,
                                              std::uint32_t slot,
                                              const mir::physical_mir_function& phys) {
            auto it = spill_map.find(slot);
            if (it != spill_map.end()) return it->second;

            // If frame_layout carries a concrete offset for this spill, use it.
            if (phys.frame_layout) {
                for (const auto& obj : phys.frame_layout->objects) {
                    if (obj.source_spill && obj.source_spill->id == slot && obj.has_assigned_offset) {
                        auto mem = asmjit::a64::ptr(asmjit::a64::sp, static_cast<int32_t>(obj.offset));
                        return spill_map.emplace(slot, mem).first->second;
                    }
                }
            }
            auto mem = cc.new_stack(8, 8);
            return spill_map.emplace(slot, mem).first->second;
        }

        // Helper kept for call-sites inside read_to_reg that don't have phys context.
        static asmjit::a64::Mem& ensure_spill(A64Compiler& cc,
                                              SpillMap& spill_map,
                                              std::uint32_t slot) {
            auto it = spill_map.find(slot);
            if (it != spill_map.end()) return it->second;
            auto mem = cc.new_stack(8, 8);
            return spill_map.emplace(slot, mem).first->second;
        }

        // Translate arg/return descriptors to AsmJit TypeId (integer-only for now).
        static asmjit::TypeId mir_type_to_asmjit(const argument_descriptor& arg) {
            (void)arg;
            return asmjit::TypeId::kInt64;
        }

        static asmjit::TypeId mir_ret_type_to_asmjit(const return_descriptor& ret) {
            if (ret.passing_kind == return_passing_kind::void_return)
                return asmjit::TypeId::kVoid;
            if (ret.reg_class == register_class::floating)
                return asmjit::TypeId::kFloat64;
            return asmjit::TypeId::kInt64;
        }

        // Scan all instructions in fn to determine if it returns a float.
        // Used as a fallback when phys.signature is not populated.
        static bool function_has_fp_return(const mir::physical_mir_function& phys) {
            for (const auto& block : phys.function.blocks) {
                for (const auto& inst : block.instructions) {
                    if (inst.op == opcode::fadd || inst.op == opcode::fsub ||
                        inst.op == opcode::fmul || inst.op == opcode::fdiv ||
                        inst.op == opcode::fneg || inst.op == opcode::fload ||
                        inst.op == opcode::fload_imm || inst.op == opcode::gpr_to_fp)
                        return true;
                }
            }
            return false;
        }

        // Build a FuncSignature from a function_signature.
        // If signature specifies floating return type, uses kFloat64; otherwise kInt64.
        static asmjit::FuncSignature build_func_signature_a64(const mir::physical_mir_function& phys) {
            if (phys.signature) {
                const auto& msig = *phys.signature;
                asmjit::FuncSignature sig(asmjit::CallConvId::kCDecl,
                                          asmjit::FuncSignature::kNoVarArgs,
                                          mir_ret_type_to_asmjit(msig.return_value));
                for (const auto& arg : msig.arguments)
                    sig.add_arg(mir_type_to_asmjit(arg));
                return sig;
            }
            // Fallback: inspect instructions to decide return type.
            // Two i64 args always (ptr + padding), return type depends on FP presence.
            const auto ret_type = function_has_fp_return(phys)
                                      ? asmjit::TypeId::kFloat64
                                      : asmjit::TypeId::kInt64;
            return asmjit::FuncSignature(asmjit::CallConvId::kCDecl,
                                         asmjit::FuncSignature::kNoVarArgs,
                                         ret_type,
                                         asmjit::TypeId::kInt64,
                                         asmjit::TypeId::kInt64);
        }

        [[nodiscard]] compilation_artifact emit_a64(
            mir::physical_mir_function const& phys
        ) {



#if LITHE_HAS_THREAD_LOCAL_SINK
        utils::nadi::PulseScope<utils::nadi::ThreadLocalSink, "lithe.asmjit.a64"> _nadi_a64{};
#endif
#if LITHE_HAS_PROFILER
        profiler::ScopedProfiler _prof{"lithe.asmjit.emit_a64"};
#endif
        compilation_artifact art;
        art.kind= artifact_kind::jit_function;
        art.name= phys.function.name;

        auto handle = std::make_unique<jit_function_handle>();

        asmjit::CodeHolder code; {
            const asmjit::Error ie = code.init(
                handle->runtime->environment(),
                handle->runtime->cpu_features()
            );
            if (ie != asmjit::kErrorOk) {
                art.diagnostics.push_back("asmjit(a64): CodeHolder::init failed: " + asmjit_err(ie));
                return art;
            }
        }

        A64Compiler cc(&code);

        const asmjit::FuncSignature sig = build_func_signature_a64(phys);
        asmjit::FuncNode* func_node = cc.add_func(sig);
            if (!func_node) {
                art.diagnostics.push_back("asmjit(a64): add_func failed");
                return art;
            }

        const auto& fn = phys.function;
        RegMap reg_map;
        FpRegMap fp_reg_map;
        SpillMap spill_map;

        // Unwind tracking: labels bound at unwind metadata instructions.
        std::vector<unwind_label_pair> unwind_pairs;
        std::vector<unwind_lp_record> unwind_lps;
        // Block IDs in physical MIR are densely packed (0..N-1 after dead block
        // elimination), so a vector gives O(1) lookup without hash overhead.
        std::uint32_t max_block_id = 0;
            for (const auto& block: fn.blocks)
        max_block_id= std::max(max_block_id, block.id);
        std::vector<asmjit::Label> block_labels(max_block_id + 1);
            for (const auto& block: fn.blocks)
        block_labels [block.id] = cc.new_label();

        // Skip dead blocks that survived dead-code elimination.
        // If the CFG tables are completely empty the MIR was built without full
        // CFG metadata — treat every block as reachable in that case.
        const auto& cfg = fn.cfg;
        const bool cfg_populated = !cfg.predecessors.empty() || !cfg.successors.empty();
        auto is_reachable = [&](const std::uint32_t id) -> bool {
            if (!cfg_populated) return true;
            if (id == cfg.entry_block) return true;
            return cfg.predecessors.count(id) != 0 || cfg.successors.count(id) != 0;
        };

        bool error = false;

        auto fail = [&](const char* msg) {
            art.diagnostics.push_back(
                std::string("asmjit(a64): ") + msg + " in " + fn.name);
            error = true;
        };

        auto read = [&](const allocated_operand& op, A64Gp& out) -> bool {
            if (!read_to_reg(cc, reg_map, spill_map, func_node, op, out)) {
                fail("failed to read operand");
                return false;
            }
            return true;
        };

        auto write = [&](const allocated_operand& def, A64Gp src) {
            write_def(cc, reg_map, spill_map, def, src);
        };

        auto result = [&](const allocated_operand& def) {
            return def_or_temp(cc, reg_map, def);
        };

        auto commit = [&](const allocated_operand& def, A64Gp value) {
            commit_def(cc, reg_map, spill_map, def, value);
        };

            for (const auto& block: fn.blocks) {
                if (error) break;
                // Skip dead blocks that survived dead-code elimination.
                if (!is_reachable(block.id)) continue;
                cc.bind(block_labels[block.id]);

                for (const auto &inst: block.instructions) {
                    if (error) break;

                    switch (inst.op) {
                        case opcode::nop:
                            cc.nop();
                            break;

                        case opcode::load_imm:
                        case opcode::mov: {
                            if (inst.defs.empty() || inst.uses.empty()) {
                                fail("mov/load_imm: bad operands");
                                break;
                            }
                            A64Gp dst = result(inst.defs[0]);
                            if (inst.op == opcode::load_imm &&
                                inst.uses[0].type == allocated_operand::kind::immediate_i64) {
                                cc.mov(dst, asmjit::Imm(std::get<std::int64_t>(inst.uses[0].value)));
                            } else {
                                A64Gp src;
                                if (!read(inst.uses[0], src)) break;
                                if (src.id() != dst.id()) cc.mov(dst, src);
                            }
                            commit(inst.defs[0], dst);
                            break;
                        }

                        case opcode::load_arg: {
                            if (inst.defs.empty() || inst.uses.empty()) {
                                fail("load_arg: bad operands");
                                break;
                            }
                            if (inst.uses[0].type != allocated_operand::kind::argument_index) {
                                fail("load_arg: expected argument_index use");
                                break;
                            }
                            const auto idx = std::get<std::uint32_t>(inst.uses[0].value);
                            A64Gp dst = result(inst.defs[0]);
                            func_node->set_arg(idx, dst);
                            commit(inst.defs[0], dst);
                            break;
                        }

                        case opcode::add: {
                            if (inst.defs.empty() || inst.uses.size() < 2) {
                                fail("add: bad operands");
                                break;
                            }
                            A64Gp lhs, rhs, res = result(inst.defs[0]);
                            if (!read(inst.uses[0], lhs)) break;
                            if (!read(inst.uses[1], rhs)) break;
                            cc.add(res, lhs, rhs);
                            commit(inst.defs[0], res);
                            break;
                        }

                        case opcode::sub: {
                            if (inst.defs.empty() || inst.uses.size() < 2) {
                                fail("sub: bad operands");
                                break;
                            }
                            A64Gp lhs, rhs, res = result(inst.defs[0]);
                            if (!read(inst.uses[0], lhs)) break;
                            if (!read(inst.uses[1], rhs)) break;
                            cc.sub(res, lhs, rhs);
                            commit(inst.defs[0], res);
                            break;
                        }

                        case opcode::mul: {
                            if (inst.defs.empty() || inst.uses.size() < 2) {
                                fail("mul: bad operands");
                                break;
                            }
                            A64Gp lhs, rhs, res = result(inst.defs[0]);
                            if (!read(inst.uses[0], lhs)) break;
                            if (!read(inst.uses[1], rhs)) break;
                            cc.mul(res, lhs, rhs);
                            commit(inst.defs[0], res);
                            break;
                        }

                        case opcode::div: {
                            // sdiv res, lhs, rhs; guarded: if rhs==0 result is 0.
                            if (inst.defs.empty() || inst.uses.size() < 2) {
                                fail("div: bad operands");
                                break;
                            }
                            A64Gp lhs, rhs, res = result(inst.defs[0]);
                            if (!read(inst.uses[0], lhs)) break;
                            if (!read(inst.uses[1], rhs)) break;
                            asmjit::Label ok = cc.new_label(), done = cc.new_label();
                            cc.cbnz(rhs, ok);
                            cc.mov(res, asmjit::Imm(0));
                            cc.b(done);
                            cc.bind(ok);
                            cc.sdiv(res, lhs, rhs);
                            cc.bind(done);
                            commit(inst.defs[0], res);
                            break;
                        }

                        case opcode::mod: {
                            // res = lhs - sdiv(lhs,rhs)*rhs  (via msub); guarded for rhs==0.
                            if (inst.defs.empty() || inst.uses.size() < 2) {
                                fail("mod: bad operands");
                                break;
                            }
                            A64Gp lhs, rhs, tmp = cc.new_gpx(), res = result(inst.defs[0]);
                            if (!read(inst.uses[0], lhs)) break;
                            if (!read(inst.uses[1], rhs)) break;
                            asmjit::Label ok = cc.new_label(), done = cc.new_label();
                            cc.cbnz(rhs, ok);
                            cc.mov(res, asmjit::Imm(0));
                            cc.b(done);
                            cc.bind(ok);
                            cc.sdiv(tmp, lhs, rhs);
                            cc.msub(res, tmp, rhs, lhs);
                            cc.bind(done);
                            commit(inst.defs[0], res);
                            break;
                        }

                        case opcode::neg: {
                            if (inst.defs.empty() || inst.uses.empty()) {
                                fail("neg: bad operands");
                                break;
                            }
                            A64Gp src, res = result(inst.defs[0]);
                            if (!read(inst.uses[0], src)) break;
                            cc.neg(res, src);
                            commit(inst.defs[0], res);
                            break;
                        }

                        case opcode::bit_and: {
                            if (inst.defs.empty() || inst.uses.size() < 2) {
                                fail("bit_and: bad operands");
                                break;
                            }
                            A64Gp lhs, rhs, res = result(inst.defs[0]);
                            if (!read(inst.uses[0], lhs)) break;
                            if (!read(inst.uses[1], rhs)) break;
                            cc.and_(res, lhs, rhs);
                            commit(inst.defs[0], res);
                            break;
                        }

                        case opcode::bit_or: {
                            if (inst.defs.empty() || inst.uses.size() < 2) {
                                fail("bit_or: bad operands");
                                break;
                            }
                            A64Gp lhs, rhs, res = result(inst.defs[0]);
                            if (!read(inst.uses[0], lhs)) break;
                            if (!read(inst.uses[1], rhs)) break;
                            cc.orr(res, lhs, rhs);
                            commit(inst.defs[0], res);
                            break;
                        }

                        case opcode::bit_xor: {
                            if (inst.defs.empty() || inst.uses.size() < 2) {
                                fail("bit_xor: bad operands");
                                break;
                            }
                            A64Gp lhs, rhs, res = result(inst.defs[0]);
                            if (!read(inst.uses[0], lhs)) break;
                            if (!read(inst.uses[1], rhs)) break;
                            cc.eor(res, lhs, rhs);
                            commit(inst.defs[0], res);
                            break;
                        }

                        case opcode::bit_not: {
                            if (inst.defs.empty() || inst.uses.empty()) {
                                fail("bit_not: bad operands");
                                break;
                            }
                            A64Gp src, res = result(inst.defs[0]);
                            if (!read(inst.uses[0], src)) break;
                            cc.mvn(res, src);
                            commit(inst.defs[0], res);
                            break;
                        }

                        case opcode::shl: {
                            if (inst.defs.empty() || inst.uses.size() < 2) {
                                fail("shl: bad operands");
                                break;
                            }
                            A64Gp lhs, rhs, res = result(inst.defs[0]);
                            if (!read(inst.uses[0], lhs)) break;
                            if (!read(inst.uses[1], rhs)) break;
                            cc.lsl(res, lhs, rhs);
                            commit(inst.defs[0], res);
                            break;
                        }

                        case opcode::shr: {
                            if (inst.defs.empty() || inst.uses.size() < 2) {
                                fail("shr: bad operands");
                                break;
                            }
                            A64Gp lhs, rhs, res = result(inst.defs[0]);
                            if (!read(inst.uses[0], lhs)) break;
                            if (!read(inst.uses[1], rhs)) break;
                            // MIR shr is ARITHMETIC (sign-preserving) — matches the
                            // interpreter and partial-evaluator reference semantics.
                            cc.asr(res, lhs, rhs);
                            commit(inst.defs[0], res);
                            break;
                        }

                        case opcode::logical_and:
                        case opcode::logical_or: {
                            if (inst.defs.empty() || inst.uses.size() < 2) {
                                fail("logical_and/or: bad operands");
                                break;
                            }
                            A64Gp lhs, rhs;
                            if (!read(inst.uses[0], lhs)) break;
                            if (!read(inst.uses[1], rhs)) break;
                            A64Gp l1 = cc.new_gpx(), r1 = cc.new_gpx(), res = result(inst.defs[0]);
                            cc.cmp(lhs, asmjit::Imm(0));
                            cc.cset(l1, asmjit::a64::CondCode::kNE);
                            cc.cmp(rhs, asmjit::Imm(0));
                            cc.cset(r1, asmjit::a64::CondCode::kNE);
                            if (inst.op == opcode::logical_and)
                                cc.and_(res, l1, r1);
                            else
                                cc.orr(res, l1, r1);
                            commit(inst.defs[0], res);
                            break;
                        }

                        case opcode::logical_not: {
                            if (inst.defs.empty() || inst.uses.empty()) {
                                fail("logical_not: bad operands");
                                break;
                            }
                            A64Gp src, res = result(inst.defs[0]);
                            if (!read(inst.uses[0], src)) break;
                            cc.cmp(src, asmjit::Imm(0));
                            cc.cset(res, asmjit::a64::CondCode::kEQ);
                            commit(inst.defs[0], res);
                            break;
                        }

                        case opcode::cmp_eq:
                        case opcode::cmp_ne:
                        case opcode::cmp_lt:
                        case opcode::cmp_le:
                        case opcode::cmp_gt:
                        case opcode::cmp_ge: {
                            if (inst.defs.empty() || inst.uses.size() < 2) {
                                fail("cmp: bad operands");
                                break;
                            }
                            A64Gp lhs, rhs, res = result(inst.defs[0]);
                            if (!read(inst.uses[0], lhs)) break;
                            if (!read(inst.uses[1], rhs)) break;
                            cc.cmp(lhs, rhs);
                            asmjit::a64::CondCode cc_code = asmjit::a64::CondCode::kEQ;
                            switch (inst.op) {
                                case opcode::cmp_eq: cc_code = asmjit::a64::CondCode::kEQ;
                                    break;
                                case opcode::cmp_ne: cc_code = asmjit::a64::CondCode::kNE;
                                    break;
                                case opcode::cmp_lt: cc_code = asmjit::a64::CondCode::kLT;
                                    break;
                                case opcode::cmp_le: cc_code = asmjit::a64::CondCode::kLE;
                                    break;
                                case opcode::cmp_gt: cc_code = asmjit::a64::CondCode::kGT;
                                    break;
                                case opcode::cmp_ge: cc_code = asmjit::a64::CondCode::kGE;
                                    break;
                                default: break;
                            }
                            cc.cset(res, cc_code);
                            commit(inst.defs[0], res);
                            break;
                        }

                        // load_spill: dst_preg = MEM[spill_slot]
                        case opcode::load_spill: {
                            if (inst.defs.empty() || inst.uses.empty()) {
                                fail("load_spill: bad operands");
                                break;
                            }
                            if (inst.uses[0].type != allocated_operand::kind::spill) {
                                // Already in a register (trivial copy after RA).
                                A64Gp reg;
                                if (!read(inst.uses[0], reg)) break;
                                write(inst.defs[0], reg);
                            } else {
                                const auto slot_id = std::get<spill_slot>(inst.uses[0].value).id;
                                auto &mem = ensure_spill(cc, spill_map, slot_id, phys);
                                A64Gp dst = result(inst.defs[0]);
                                cc.ldr(dst, mem);
                                commit(inst.defs[0], dst);
                            }
                            break;
                        }

                        // store_spill: MEM[spill_slot] = src_preg
                        case opcode::store_spill: {
                            if (inst.defs.empty() || inst.uses.empty()) {
                                fail("store_spill: bad operands");
                                break;
                            }
                            if (inst.defs[0].type != allocated_operand::kind::spill) {
                                // Def is a preg: treat as register move.
                                A64Gp src;
                                if (!read(inst.uses[0], src)) break;
                                write(inst.defs[0], src);
                            } else {
                                const auto slot_id = std::get<spill_slot>(inst.defs[0].value).id;
                                auto &mem = ensure_spill(cc, spill_map, slot_id, phys);
                                A64Gp src;
                                if (!read(inst.uses[0], src)) break;
                                cc.str(src, mem);
                            }
                            break;
                        }

                        // load: dst = [base + offset]  (generic memory load)
                        case opcode::load: {
                            if (inst.defs.empty() || inst.uses.empty()) {
                                fail("load: bad operands");
                                break;
                            }
                            A64Gp base;
                            if (!read(inst.uses[0], base)) break;
                            std::int64_t offset = 0;
                            if (inst.uses.size() > 1 &&
                                inst.uses[1].type == allocated_operand::kind::immediate_i64)
                                offset = std::get<std::int64_t>(inst.uses[1].value);
                            A64Gp dst = result(inst.defs[0]);
                            cc.ldr(dst, asmjit::a64::ptr(base, static_cast<int32_t>(offset)));
                            commit(inst.defs[0], dst);
                            break;
                        }

                        // store: [base + offset] = src
                        case opcode::store: {
                            if (inst.uses.size() < 2) {
                                fail("store: need base and src");
                                break;
                            }
                            A64Gp base, src;
                            if (!read(inst.uses[0], base)) break;
                            if (!read(inst.uses[1], src)) break;
                            std::int64_t offset = 0;
                            if (inst.uses.size() > 2 &&
                                inst.uses[2].type == allocated_operand::kind::immediate_i64)
                                offset = std::get<std::int64_t>(inst.uses[2].value);
                            cc.str(src, asmjit::a64::ptr(base, static_cast<int32_t>(offset)));
                            break;
                        }

                        case opcode::branch: {
                            if (inst.uses.empty() ||
                                inst.uses[0].type != allocated_operand::kind::block) {
                                fail("branch: missing block operand");
                                break;
                            }
                            const auto tid = std::get<std::uint32_t>(inst.uses[0].value);
                            if (tid >= block_labels.size()) {
                                fail("branch: target block id out of range");
                                break;
                            }
                            cc.b(block_labels[tid]);
                            break;
                        }

                        case opcode::branch_cond: {
                            if (inst.uses.size() < 3) {
                                fail("branch_cond: need 3 uses");
                                break;
                            }
                            A64Gp cond;
                            if (!read(inst.uses[0], cond)) break;
                            if (inst.uses[1].type != allocated_operand::kind::block ||
                                inst.uses[2].type != allocated_operand::kind::block) {
                                fail("branch_cond: targets must be blocks");
                                break;
                            }
                            const auto tid_t = std::get<std::uint32_t>(inst.uses[1].value);
                            const auto tid_f = std::get<std::uint32_t>(inst.uses[2].value);
                            if (tid_t >= block_labels.size() || tid_f >= block_labels.size()) {
                                fail("branch_cond: target block id out of range");
                                break;
                            }
                            cc.cbnz(cond, block_labels[tid_t]);
                            cc.b(block_labels[tid_f]);
                            break;
                        }

                        // ── Floating-point arithmetic ──────────────────────────
                        case opcode::fadd: {
                            if (inst.defs.empty() || inst.uses.size() < 2) { fail("fadd: bad operands"); break; }
                            A64Vec lhs, rhs, res = cc.new_vec_d();
                            if (!read_fp_to_reg(cc, fp_reg_map, inst.uses[0], lhs)) { fail("fadd: bad lhs"); break; }
                            if (!read_fp_to_reg(cc, fp_reg_map, inst.uses[1], rhs)) { fail("fadd: bad rhs"); break; }
                            cc.fadd(res.d(), lhs.d(), rhs.d());
                            write_fp_def(cc, fp_reg_map, inst.defs[0], res);
                            break;
                        }
                        case opcode::fsub: {
                            if (inst.defs.empty() || inst.uses.size() < 2) { fail("fsub: bad operands"); break; }
                            A64Vec lhs, rhs, res = cc.new_vec_d();
                            if (!read_fp_to_reg(cc, fp_reg_map, inst.uses[0], lhs)) { fail("fsub: bad lhs"); break; }
                            if (!read_fp_to_reg(cc, fp_reg_map, inst.uses[1], rhs)) { fail("fsub: bad rhs"); break; }
                            cc.fsub(res.d(), lhs.d(), rhs.d());
                            write_fp_def(cc, fp_reg_map, inst.defs[0], res);
                            break;
                        }
                        case opcode::fmul: {
                            if (inst.defs.empty() || inst.uses.size() < 2) { fail("fmul: bad operands"); break; }
                            A64Vec lhs, rhs, res = cc.new_vec_d();
                            if (!read_fp_to_reg(cc, fp_reg_map, inst.uses[0], lhs)) { fail("fmul: bad lhs"); break; }
                            if (!read_fp_to_reg(cc, fp_reg_map, inst.uses[1], rhs)) { fail("fmul: bad rhs"); break; }
                            cc.fmul(res.d(), lhs.d(), rhs.d());
                            write_fp_def(cc, fp_reg_map, inst.defs[0], res);
                            break;
                        }
                        case opcode::fdiv: {
                            if (inst.defs.empty() || inst.uses.size() < 2) { fail("fdiv: bad operands"); break; }
                            A64Vec lhs, rhs, res = cc.new_vec_d();
                            if (!read_fp_to_reg(cc, fp_reg_map, inst.uses[0], lhs)) { fail("fdiv: bad lhs"); break; }
                            if (!read_fp_to_reg(cc, fp_reg_map, inst.uses[1], rhs)) { fail("fdiv: bad rhs"); break; }
                            cc.fdiv(res.d(), lhs.d(), rhs.d());
                            write_fp_def(cc, fp_reg_map, inst.defs[0], res);
                            break;
                        }
                        case opcode::fneg: {
                            if (inst.defs.empty() || inst.uses.empty()) { fail("fneg: bad operands"); break; }
                            A64Vec src, res = cc.new_vec_d();
                            if (!read_fp_to_reg(cc, fp_reg_map, inst.uses[0], src)) { fail("fneg: bad src"); break; }
                            cc.fneg(res.d(), src.d());
                            write_fp_def(cc, fp_reg_map, inst.defs[0], res);
                            break;
                        }
                        // ── Floating-point memory ──────────────────────────────
                        case opcode::fload: {
                            // fload dst = [base_gpr + imm_offset]
                            if (inst.defs.empty() || inst.uses.empty()) { fail("fload: bad operands"); break; }
                            A64Gp base;
                            if (!read(inst.uses[0], base)) break;
                            std::int64_t offset = 0;
                            if (inst.uses.size() > 1 && inst.uses[1].type == allocated_operand::kind::immediate_i64)
                                offset = std::get<std::int64_t>(inst.uses[1].value);
                            A64Vec dst = cc.new_vec_d();
                            cc.ldr(dst.d(), asmjit::a64::ptr(base, static_cast<std::int32_t>(offset)));
                            write_fp_def(cc, fp_reg_map, inst.defs[0], dst);
                            break;
                        }
                        case opcode::fload_imm: {
                            // fload_imm dst = immediate_f64  (via GPR+fmov bridge)
                            if (inst.defs.empty() || inst.uses.empty()) { fail("fload_imm: bad operands"); break; }
                            double imm = 0.0;
                            if (inst.uses[0].type == allocated_operand::kind::immediate_f64)
                                imm = std::get<double>(inst.uses[0].value);
                            else if (inst.uses[0].type == allocated_operand::kind::immediate_i64)
                                imm = std::bit_cast<double>(std::get<std::int64_t>(inst.uses[0].value));
                            A64Gp tmp = cc.new_gpx();
                            cc.mov(tmp, asmjit::Imm(std::bit_cast<std::int64_t>(imm)));
                            A64Vec dst = cc.new_vec_d();
                            cc.fmov(dst.d(), tmp);
                            write_fp_def(cc, fp_reg_map, inst.defs[0], dst);
                            break;
                        }
                        case opcode::fstore: {
                            // fstore [base_gpr + imm_offset] = src_fp
                            if (inst.uses.size() < 2) { fail("fstore: need base and src"); break; }
                            A64Gp base;
                            if (!read(inst.uses[0], base)) break;
                            A64Vec src;
                            if (!read_fp_to_reg(cc, fp_reg_map, inst.uses[1], src)) { fail("fstore: bad src"); break; }
                            std::int64_t offset = 0;
                            if (inst.uses.size() > 2 && inst.uses[2].type == allocated_operand::kind::immediate_i64)
                                offset = std::get<std::int64_t>(inst.uses[2].value);
                            cc.str(src.d(), asmjit::a64::ptr(base, static_cast<std::int32_t>(offset)));
                            break;
                        }
                        // ── FP↔GPR bitcast bridges ────────────────────────────
                        case opcode::gpr_to_fp: {
                            // gpr_to_fp fp_dst = gpr_src  (FMOV Dd, Xn)
                            if (inst.defs.empty() || inst.uses.empty()) { fail("gpr_to_fp: bad operands"); break; }
                            A64Gp src;
                            if (!read(inst.uses[0], src)) break;
                            A64Vec dst = cc.new_vec_d();
                            cc.fmov(dst.d(), src);
                            write_fp_def(cc, fp_reg_map, inst.defs[0], dst);
                            break;
                        }
                        case opcode::fp_to_gpr: {
                            // fp_to_gpr gpr_dst = fp_src  (FMOV Xd, Dn)
                            if (inst.defs.empty() || inst.uses.empty()) { fail("fp_to_gpr: bad operands"); break; }
                            A64Vec src;
                            if (!read_fp_to_reg(cc, fp_reg_map, inst.uses[0], src)) { fail("fp_to_gpr: bad src"); break; }
                            A64Gp dst = cc.new_gpx();
                            cc.fmov(dst, src.d());
                            write(inst.defs[0], dst);
                            break;
                        }
                        // ── Floating-point comparisons ────────────────────────
                        case opcode::fcmp_eq:
                        case opcode::fcmp_ne:
                        case opcode::fcmp_lt:
                        case opcode::fcmp_le:
                        case opcode::fcmp_gt:
                        case opcode::fcmp_ge: {
                            if (inst.defs.empty() || inst.uses.size() < 2) { fail("fcmp: bad operands"); break; }
                            A64Vec lhs, rhs;
                            if (!read_fp_to_reg(cc, fp_reg_map, inst.uses[0], lhs)) { fail("fcmp: bad lhs"); break; }
                            if (!read_fp_to_reg(cc, fp_reg_map, inst.uses[1], rhs)) { fail("fcmp: bad rhs"); break; }
                            cc.fcmp(lhs.d(), rhs.d());
                            A64Gp res = cc.new_gpx();
                            asmjit::Label tl = cc.new_label(), dl = cc.new_label();
                            switch (inst.op) {
                                case opcode::fcmp_eq: cc.b_eq(tl); break;
                                case opcode::fcmp_ne: cc.b_ne(tl); break;
                                case opcode::fcmp_lt: cc.b_mi(tl); break;
                                case opcode::fcmp_le: cc.b_ls(tl); break;
                                case opcode::fcmp_gt: cc.b_gt(tl); break;
                                case opcode::fcmp_ge: cc.b_ge(tl); break;
                                default: break;
                            }
                            cc.mov(res, asmjit::Imm(0));
                            cc.b(dl);
                            cc.bind(tl);
                            cc.mov(res, asmjit::Imm(1));
                            cc.bind(dl);
                            write(inst.defs[0], res);
                            break;
                        }

                        case opcode::ret: {
                            const bool void_ret =
                                    phys.signature &&
                                    phys.signature->return_value.passing_kind == return_passing_kind::void_return;
                            if (inst.uses.empty() || void_ret) {
                                cc.ret();
                            } else if (inst.uses[0].type == allocated_operand::kind::preg) {
                                const auto id = std::get<preg>(inst.uses[0].value).id;
                                if (fp_reg_map.count(id)) {
                                    // FP return: return via D0 register.
                                    A64Vec retval = get_fp_vreg(cc, fp_reg_map, id);
                                    cc.ret(retval.d());
                                } else {
                                    A64Gp retval;
                                    if (!read(inst.uses[0], retval)) break;
                                    cc.ret(retval);
                                }
                            } else {
                                A64Gp retval;
                                if (!read(inst.uses[0], retval)) break;
                                cc.ret(retval);
                            }
                            break;
                        }

                        case opcode::indirect_call: {
                            // Fuel check: decrement sandbox fuel, trap (brk #1) if exhausted.
                            if (inst.abstract_operation &&
                                inst.abstract_operation->domain ==
                                std::string(lithe::runtime::fuel::fuel_domain)) {
                                if (sandbox_) {
                                    A64Gp fuel_ptr = cc.new_gpx();
                                    cc.mov(fuel_ptr, asmjit::Imm(
                                               reinterpret_cast<std::int64_t>(&sandbox_->fuel_counter)));
                                    A64Gp fuel_val = cc.new_gpx();
                                    cc.ldr(fuel_val, asmjit::a64::ptr(fuel_ptr));
                                    cc.sub(fuel_val, fuel_val, asmjit::Imm(1));
                                    cc.str(fuel_val, asmjit::a64::ptr(fuel_ptr));
                                    asmjit::Label ok_fuel = cc.new_label();
                                    cc.cbnz(fuel_val, ok_fuel);
                                    cc.brk(1);
                                    cc.bind(ok_fuel);
                                }
                                break;
                            }

                            // Safepoint tags are metadata-only — no machine code emitted.
                            if (inst.abstract_operation &&
                                inst.abstract_operation->domain ==
                                std::string(lithe::runtime::safepoint::safepoint_domain))
                                break;

                            // Unwind opcodes: bind labels, record for post-emit table build.
                            if (inst.abstract_operation &&
                                inst.abstract_operation->domain ==
                                std::string(lithe::runtime::unwind::unwind_domain)) {
                                const auto &op_name = inst.abstract_operation->name;
                                if (op_name == lithe::runtime::unwind::unwind_region_begin_name) {
                                    unwind_label_pair pair;
                                    pair.begin_label = cc.new_label();
                                    cc.bind(pair.begin_label);
                                    unwind_pairs.push_back(std::move(pair));
                                } else if (op_name == lithe::runtime::unwind::unwind_region_end_name) {
                                    if (!unwind_pairs.empty() && !unwind_pairs.back().end_set) {
                                        unwind_pairs.back().end_label = cc.new_label();
                                        cc.bind(unwind_pairs.back().end_label);
                                        unwind_pairs.back().end_set = true;
                                    }
                                } else if (op_name == lithe::runtime::unwind::landing_pad_tag_name) {
                                    unwind_lp_record lpr;
                                    lpr.label = cc.new_label();
                                    cc.bind(lpr.label);
                                    if (!inst.uses.empty() &&
                                        inst.uses[0].type == allocated_operand::kind::immediate_i64)
                                        lpr.cleanup_flags = static_cast<std::uint32_t>(
                                            std::get<std::int64_t>(inst.uses[0].value));
                                    unwind_lps.push_back(std::move(lpr));
                                }
                                break;
                            }

                            // Dispatch linker opcodes ("lithe.linker" domain).
                            // If a native_proxy is registered for the symbol, use its
                            // arity to build the correct call signature.
                            {
                                const auto lnk = try_linker_dispatch(inst);
                                if (lnk.addr) {
                                    // Look up native_proxy for arity-aware signature.
                                    const auto *proxy = find_native_proxy(lnk.sym_name);
                                    const std::uint8_t nargs = proxy ? proxy->arity : 2u;

                                    asmjit::FuncSignature call_sig(
                                        asmjit::CallConvId::kCDecl,
                                        asmjit::FuncSignature::kNoVarArgs,
                                        asmjit::TypeId::kInt64);
                                    for (std::uint8_t ai = 0; ai < nargs; ++ai)
                                        call_sig.add_arg(asmjit::TypeId::kInt64);

                                    A64Gp fn_reg = cc.new_gpx();
                                    cc.mov(fn_reg, asmjit::Imm(
                                               reinterpret_cast<std::int64_t>(lnk.addr)));
                                    asmjit::InvokeNode *invoke = nullptr;
                                    cc.invoke(asmjit::Out<asmjit::InvokeNode *>(invoke),
                                              fn_reg, call_sig);
                                    if (!invoke) {
                                        fail("linker: invoke failed");
                                        break;
                                    }
                                    // uses[1..N] are call arguments.
                                    for (std::size_t ai = 1; ai < inst.uses.size(); ++ai) {
                                        A64Gp arg;
                                        if (read(inst.uses[ai], arg))
                                            invoke->set_arg(static_cast<uint32_t>(ai - 1), arg);
                                    }
                                    if (!inst.defs.empty()) {
                                        A64Gp ret = cc.new_gpx();
                                        invoke->set_ret(0, ret);
                                        write(inst.defs[0], ret);
                                    }
                                    break;
                                }
                                // If lnk.addr is null and abstract_operation is linker
                                // domain, it was an error; fall through to diagnostics.
                                if (inst.abstract_operation &&
                                    inst.abstract_operation->domain ==
                                    std::string(lithe::runtime::linker::linker_domain)) {
                                    art.diagnostics.push_back(
                                        "asmjit(a64): linker dispatch failed for " + fn.name);
                                    break;
                                }
                            }

                            // Dispatch MOP opcodes when abstract_operation is set.
                            const auto mop = try_mop_dispatch(inst);
                            using K = mop_dispatch_result::kind;
                            if (mop.k == K::error) {
                                fail(("mop dispatch failed: " + mop.error_msg).c_str());
                                break;
                            }
                            if (mop.k == K::immediate) {
                                // Embed pre-resolved pointer/value as a load_imm.
                                if (!inst.defs.empty()) {
                                    A64Gp res = cc.new_gpx();
                                    cc.mov(res, asmjit::Imm(mop.value));
                                    write(inst.defs[0], res);
                                }
                                break;
                            }
                            if (mop.k == K::field_offset) {
                                // Emit: result = obj_ptr + field_offset
                                if (inst.defs.empty() || inst.uses.empty()) {
                                    fail("mop.get_field: need def and obj_ptr use");
                                    break;
                                }
                                A64Gp obj;
                                if (!read(inst.uses[0], obj)) break;
                                A64Gp res = cc.new_gpx();
                                cc.add(res, obj, asmjit::Imm(mop.value));
                                write(inst.defs[0], res);
                                break;
                            }
                            if (mop.k == K::method_fn_ptr) {
                                // Emit: indirect call to fn_ptr with obj_ptr as first arg.
                                if (inst.defs.empty() || inst.uses.empty()) {
                                    fail("mop.invoke_method: need def and obj_ptr use");
                                    break;
                                }
                                // Build call signature: int64_t(void*, int64_t*, uint32_t)
                                asmjit::FuncSignature call_sig(
                                    asmjit::CallConvId::kCDecl,
                                    asmjit::FuncSignature::kNoVarArgs,
                                    asmjit::TypeId::kInt64,
                                    asmjit::TypeId::kUIntPtr, // obj
                                    asmjit::TypeId::kUIntPtr, // args array ptr
                                    asmjit::TypeId::kUInt32 // argc
                                );
                                A64Gp fn_reg = cc.new_gpx();
                                cc.mov(fn_reg, asmjit::Imm(mop.value));
                                asmjit::InvokeNode *invoke = nullptr;
                                cc.invoke(asmjit::Out<asmjit::InvokeNode *>(invoke), fn_reg, call_sig);
                                if (!invoke) {
                                    fail("mop.invoke_method: invoke failed");
                                    break;
                                }
                                A64Gp obj;
                                if (!read(inst.uses[0], obj)) break;
                                invoke->set_arg(0, obj);
                                A64Gp null_args = cc.new_gpx();
                                cc.mov(null_args, asmjit::Imm(0));
                                invoke->set_arg(1, null_args);
                                A64Gp argc_zero = cc.new_gpx();
                                cc.mov(argc_zero, asmjit::Imm(0));
                                invoke->set_arg(2, argc_zero);
                                A64Gp ret = cc.new_gpx();
                                invoke->set_ret(0, ret);
                                write(inst.defs[0], ret);
                                break;
                            }
                            // Unhandled indirect_call (no MOP context or unknown sub-opcode)
                            art.diagnostics.push_back(
                                "asmjit(a64): unresolved indirect_call in " + fn.name
                            );
                            break;
                        }
                        case opcode::load_symbol:
                        case opcode::call:
                        case opcode::get_element_ptr:
                        case opcode::extract_value:
                        case opcode::insert_value:
                        default:
                            art.diagnostics.push_back(
                                "asmjit(a64): unsupported opcode '" +
                                std::string(to_string(inst.op)) + "' in " + fn.name
                            );
                            break;
                    }
                }
            }

        cc.end_func();

            if (error)return art;

            {
                const asmjit::Error fe = cc.finalize();
                if (fe != asmjit::kErrorOk) {
                    art.diagnostics.push_back("asmjit(a64): finalize: " + asmjit_err(fe));
                    return art;
                }
            }

        const bool is_fp_fn = function_has_fp_return(phys);
        if (is_fp_fn) {
            jit_function_handle::fn_f64_t fn_ptr_f64 = nullptr;
            {
                const asmjit::Error ae = handle->runtime->add(&fn_ptr_f64, &code);
                if (ae != asmjit::kErrorOk) {
                    art.diagnostics.push_back("asmjit(a64): runtime.add (f64): " + asmjit_err(ae));
                    return art;
                }
            }
            handle->fn_ptr_f64 = fn_ptr_f64;
            auto h = std::make_shared<artifact_handle>();
            h->kind = artifact_handle_kind::jit_function;
            h->payload = std::move(handle);
            art.handle = std::move(h);
            art.metadata["jit_fn_name"] = art.name;
            art.metadata["returns_f64"] = "true";
            register_stack_map(phys);
            register_unwind_table(fn.name, unwind_pairs, unwind_lps, code,
                                  reinterpret_cast<uintptr_t>(fn_ptr_f64));
        }else {
            jit_function_handle::fn_i64_t fn_ptr = nullptr; {
                const asmjit::Error ae = handle->runtime->add(&fn_ptr, &code);
                if (ae != asmjit::kErrorOk) {
                    art.diagnostics.push_back("asmjit(a64): runtime.add: " + asmjit_err(ae));
                    return art;
                }
            }
            handle->fn_ptr = fn_ptr;
            auto h = std::make_shared<artifact_handle>();
            h->kind = artifact_handle_kind::jit_function;
            h->payload = std::move(handle);
            art.handle = std::move(h);
            art.metadata["jit_fn_name"] = art.name;
            register_stack_map(phys);
            register_unwind_table(fn.name, unwind_pairs, unwind_lps, code,
                                  reinterpret_cast<uintptr_t>(fn_ptr));
        }
            return art;
        }

#else // ================================================================
        // x86-64 path
        // ================================================================

        using X86Compiler = asmjit::x86::Compiler;
        using X86Gp = asmjit::x86::Gp;
        using RegMap = std::unordered_map<std::uint16_t, X86Gp>;
        using SpillMap = std::unordered_map<std::uint32_t, asmjit::x86::Mem>;

        static X86Gp get_vreg(X86Compiler& cc, RegMap& reg_map, std::uint16_t id) {
            auto it = reg_map.find(id);
            if (it != reg_map.end()) return it->second;
            auto vr = cc.new_gp64();
            reg_map.emplace(id, vr);
            return vr;
        }

        static asmjit::x86::Mem& ensure_spill(X86Compiler& cc,
                                              SpillMap& spill_map,
                                              std::uint32_t slot) {
            auto it = spill_map.find(slot);
            if (it != spill_map.end()) return it->second;
            return spill_map.emplace(slot, cc.new_stack(8, 8)).first->second;
        }

        // Overload that honours concrete frame offsets from frame_layout when present.
        static asmjit::x86::Mem& ensure_spill(X86Compiler& cc,
                                              SpillMap& spill_map,
                                              std::uint32_t slot,
                                              const mir::physical_mir_function& phys) {
            auto it = spill_map.find(slot);
            if (it != spill_map.end()) return it->second;

            if (phys.frame_layout) {
                for (const auto& obj : phys.frame_layout->objects) {
                    if (obj.source_spill && obj.source_spill->id == slot && obj.has_assigned_offset) {
                        auto mem = asmjit::x86::qword_ptr(asmjit::x86::rsp, static_cast<int32_t>(obj.offset));
                        return spill_map.emplace(slot, mem).first->second;
                    }
                }
            }
            return spill_map.emplace(slot, cc.new_stack(8, 8)).first->second;
        }

        static bool read_to_reg(X86Compiler& cc,
                                RegMap& reg_map,
                                SpillMap& spill_map,
                                asmjit::FuncNode* func_node,
                                const allocated_operand& op,
                                X86Gp& out) {
            switch (op.type) {
            case allocated_operand::kind::preg:
                out = get_vreg(cc, reg_map, std::get<preg>(op.value).id);
                return true;
            case allocated_operand::kind::immediate_i64:
                out = cc.new_gp64();
                cc.mov(out, asmjit::Imm(std::get<std::int64_t>(op.value)));
                return true;
            case allocated_operand::kind::argument_index: {
                const auto idx = std::get<std::uint32_t>(op.value);
                out = cc.new_gp64();
                func_node->set_arg(idx, out);
                return true;
            }
            case allocated_operand::kind::spill: {
                const auto slot = std::get<spill_slot>(op.value).id;
                auto& mem = ensure_spill(cc, spill_map, slot);
                out = cc.new_gp64();
                cc.mov(out, mem);
                return true;
            }
            default:
                return false;
            }
        }

        static void write_def(X86Compiler& cc,
                              RegMap& reg_map,
                              SpillMap& spill_map,
                              const allocated_operand& def,
                              X86Gp src) {
            if (def.type == allocated_operand::kind::preg) {
                X86Gp dst = get_vreg(cc, reg_map, std::get<preg>(def.value).id);
                cc.mov(dst, src);
            }
            else if (def.type == allocated_operand::kind::spill) {
                auto& mem = ensure_spill(cc, spill_map, std::get<spill_slot>(def.value).id);
                cc.mov(mem, src);
            }
        }

        static X86Gp def_or_temp(X86Compiler& cc,
                                 RegMap& reg_map,
                                 const allocated_operand& def) {
            if (def.type == allocated_operand::kind::preg)
                return get_vreg(cc, reg_map, std::get<preg>(def.value).id);
            return cc.new_gp64();
        }

        static void commit_def(X86Compiler& cc,
                               RegMap& reg_map,
                               SpillMap& spill_map,
                               const allocated_operand& def,
                               X86Gp value) {
            if (def.type != allocated_operand::kind::preg)
                write_def(cc, reg_map, spill_map, def, value);
        }

        static asmjit::TypeId x86_mir_arg_type(const argument_descriptor& arg) {
            (void)arg;
            return asmjit::TypeId::kInt64;
        }

        static asmjit::TypeId x86_mir_ret_type(const return_descriptor& ret) {
            if (ret.passing_kind == return_passing_kind::void_return)
                return asmjit::TypeId::kVoid;
            return asmjit::TypeId::kInt64;
        }

        static asmjit::FuncSignature build_func_signature_x86(const mir::physical_mir_function& phys) {
            if (phys.signature) {
                const auto& msig = *phys.signature;
                asmjit::FuncSignature sig(asmjit::CallConvId::kCDecl,
                                          asmjit::FuncSignature::kNoVarArgs,
                                          x86_mir_ret_type(msig.return_value));
                for (const auto& arg : msig.arguments)
                    sig.add_arg(x86_mir_arg_type(arg));
                return sig;
            }
            // Fallback: two i64 args, i64 return (matches legacy fn_i64_t).
            return asmjit::FuncSignature(asmjit::CallConvId::kCDecl,
                                         asmjit::FuncSignature::kNoVarArgs,
                                         asmjit::TypeId::kInt64,
                                         asmjit::TypeId::kInt64,
                                         asmjit::TypeId::kInt64);
        }

        [[nodiscard]] compilation_artifact emit_x86(
            mir::physical_mir_function const& phys
        ) {
#if LITHE_HAS_THREAD_LOCAL_SINK
            utils::nadi::PulseScope<utils::nadi::ThreadLocalSink, "lithe.asmjit.x86"> _nadi_x86{};
#endif
#if LITHE_HAS_PROFILER
            profiler::ScopedProfiler _prof{"lithe.asmjit.emit_x86"};
#endif
            compilation_artifact art;
            art.kind = artifact_kind::jit_function;
            art.name = phys.function.name;

            auto handle = std::make_unique<jit_function_handle>();

            asmjit::CodeHolder code;
            {
                const asmjit::Error ie = code.init(
                    handle->runtime->environment(),
                    handle->runtime->cpu_features()
                );
                if (ie != asmjit::kErrorOk) {
                    art.diagnostics.push_back("asmjit(x86): CodeHolder::init failed: " + asmjit_err(ie));
                    return art;
                }
            }

            X86Compiler cc(&code);

            const asmjit::FuncSignature sig = build_func_signature_x86(phys);
            asmjit::FuncNode* func_node = cc.add_func(sig);
            if (!func_node) {
                art.diagnostics.push_back("asmjit(x86): add_func failed");
                return art;
            }

            const auto& fn = phys.function;
            RegMap reg_map;
            SpillMap spill_map;

            // Unwind tracking: labels bound at unwind metadata instructions.
            std::vector<unwind_label_pair> unwind_pairs;
            std::vector<unwind_lp_record> unwind_lps;

            // Flat label vector indexed by block id — O(1) access, no hash overhead.
            std::uint32_t max_block_id = 0;
            for (const auto& block : fn.blocks)
                max_block_id = std::max(max_block_id, block.id);
            std::vector<asmjit::Label> block_labels(max_block_id + 1);
            for (const auto& block : fn.blocks)
                block_labels[block.id] = cc.new_label();

            // Skip dead blocks that survived dead-code elimination.
            // If the CFG tables are completely empty the MIR was built without full
            // CFG metadata — treat every block as reachable in that case.
            const auto& cfg = fn.cfg;
            const bool cfg_populated = !cfg.predecessors.empty() || !cfg.successors.empty();
            auto is_reachable = [&](std::uint32_t id) -> bool {
                if (!cfg_populated) return true;
                if (id == cfg.entry_block) return true;
                return cfg.predecessors.count(id) != 0 || cfg.successors.count(id) != 0;
            };

            bool error = false;

            auto fail = [&](const char* msg) {
                art.diagnostics.push_back(
                    std::string("asmjit(x86): ") + msg + " in " + fn.name);
                error = true;
            };

            auto read = [&](const allocated_operand& op, X86Gp& out) -> bool {
                if (!read_to_reg(cc, reg_map, spill_map, func_node, op, out)) {
                    fail("failed to read operand");
                    return false;
                }
                return true;
            };

            auto write = [&](const allocated_operand& def, X86Gp src) {
                write_def(cc, reg_map, spill_map, def, src);
            };

            auto result = [&](const allocated_operand& def) {
                return def_or_temp(cc, reg_map, def);
            };

            auto commit = [&](const allocated_operand& def, X86Gp value) {
                commit_def(cc, reg_map, spill_map, def, value);
            };

            for (const auto& block : fn.blocks) {
                if (error) break;
                // Skip dead blocks that survived dead-code elimination.
                if (!is_reachable(block.id)) continue;
                cc.bind(block_labels[block.id]);

                for (const auto& inst : block.instructions) {
                    if (error) break;

                    switch (inst.op) {
                    case opcode::nop:
                        cc.nop();
                        break;

                    case opcode::load_imm:
                    case opcode::mov: {
                        if (inst.defs.empty() || inst.uses.empty()) {
                            fail("mov/load_imm: bad operands");
                            break;
                        }
                        X86Gp dst = result(inst.defs[0]);
                        if (inst.op == opcode::load_imm &&
                            inst.uses[0].type == allocated_operand::kind::immediate_i64) {
                            cc.mov(dst, asmjit::Imm(std::get<std::int64_t>(inst.uses[0].value)));
                        } else {
                            X86Gp src;
                            if (!read(inst.uses[0], src)) break;
                            if (src.id() != dst.id()) cc.mov(dst, src);
                        }
                        commit(inst.defs[0], dst);
                        break;
                    }

                    case opcode::load_arg: {
                        if (inst.defs.empty() || inst.uses.empty()) {
                            fail("load_arg: bad operands");
                            break;
                        }
                        if (inst.uses[0].type != allocated_operand::kind::argument_index) {
                            fail("load_arg: expected argument_index use");
                            break;
                        }
                        const auto idx = std::get<std::uint32_t>(inst.uses[0].value);
                        X86Gp dst = result(inst.defs[0]);
                        func_node->set_arg(idx, dst);
                        commit(inst.defs[0], dst);
                        break;
                    }

                    case opcode::add: {
                        if (inst.defs.empty() || inst.uses.size() < 2) {
                            fail("add: bad operands");
                            break;
                        }
                        X86Gp lhs, rhs, res = result(inst.defs[0]);
                        if (!read(inst.uses[0], lhs)) break;
                        if (!read(inst.uses[1], rhs)) break;
                        if (res.id() == rhs.id() && res.id() != lhs.id())
                            cc.add(res, lhs);
                        else {
                            if (res.id() != lhs.id()) cc.mov(res, lhs);
                            cc.add(res, rhs);
                        }
                        commit(inst.defs[0], res);
                        break;
                    }

                    case opcode::sub: {
                        if (inst.defs.empty() || inst.uses.size() < 2) {
                            fail("sub: bad operands");
                            break;
                        }
                        X86Gp lhs, rhs, res = cc.new_gp64();
                        if (!read(inst.uses[0], lhs)) break;
                        if (!read(inst.uses[1], rhs)) break;
                        cc.mov(res, lhs);
                        cc.sub(res, rhs);
                        write(inst.defs[0], res);
                        break;
                    }

                    case opcode::mul: {
                        if (inst.defs.empty() || inst.uses.size() < 2) {
                            fail("mul: bad operands");
                            break;
                        }
                        X86Gp lhs, rhs, res = result(inst.defs[0]);
                        if (!read(inst.uses[0], lhs)) break;
                        if (!read(inst.uses[1], rhs)) break;
                        if (res.id() == rhs.id() && res.id() != lhs.id())
                            cc.imul(res, lhs);
                        else {
                            if (res.id() != lhs.id()) cc.mov(res, lhs);
                            cc.imul(res, rhs);
                        }
                        commit(inst.defs[0], res);
                        break;
                    }

                    case opcode::div:
                    case opcode::mod: {
                        // x86-64: idiv requires dividend in rdx:rax (via cqo sign extension).
                        // Guarded to match interpreter/partial-evaluator semantics:
                        //   divisor == 0            -> result 0 (no trap)
                        //   INT64_MIN / -1 (overflow)-> div: INT64_MIN, mod: 0 (idiv would #DE)
                        if (inst.defs.empty() || inst.uses.size() < 2) {
                            fail("div/mod: bad operands");
                            break;
                        }
                        X86Gp lhs, rhs;
                        if (!read(inst.uses[0], lhs)) break;
                        if (!read(inst.uses[1], rhs)) break;
                        // Allocate pinned rax/rdx virtual registers for the idiv protocol.
                        X86Gp rax_vr = cc.new_gp64("rax_div");
                        X86Gp rdx_vr = cc.new_gp64("rdx_div");
                        X86Gp res = cc.new_gp64();
                        asmjit::Label ok = cc.new_label(), do_div = cc.new_label(), done = cc.new_label();

                        // divisor == 0 -> 0
                        cc.test(rhs, rhs);
                        cc.jnz(ok);
                        cc.mov(res, asmjit::Imm(0));
                        cc.jmp(done);

                        cc.bind(ok);
                        // Overflow guard: rhs == -1 && lhs == INT64_MIN.
                        X86Gp min_v = cc.new_gp64();
                        cc.mov(min_v, asmjit::Imm(std::numeric_limits<std::int64_t>::min()));
                        cc.cmp(rhs, asmjit::Imm(-1));
                        cc.jne(do_div);
                        cc.cmp(lhs, min_v);
                        cc.jne(do_div);
                        // INT64_MIN / -1: div -> INT64_MIN, mod -> 0.
                        if (inst.op == opcode::div)
                            cc.mov(res, min_v);
                        else
                            cc.mov(res, asmjit::Imm(0));
                        cc.jmp(done);

                        cc.bind(do_div);
                        cc.mov(rax_vr, lhs);
                        cc.cqo(rdx_vr, rax_vr);
                        cc.idiv(rdx_vr, rax_vr, rhs);
                        if (inst.op == opcode::div)
                            cc.mov(res, rax_vr); // quotient
                        else
                            cc.mov(res, rdx_vr); // remainder
                        cc.bind(done);
                        write(inst.defs[0], res);
                        break;
                    }

                    case opcode::neg: {
                        if (inst.defs.empty() || inst.uses.empty()) {
                            fail("neg: bad operands");
                            break;
                        }
                        X86Gp src, res = cc.new_gp64();
                        if (!read(inst.uses[0], src)) break;
                        cc.mov(res, src);
                        cc.neg(res);
                        write(inst.defs[0], res);
                        break;
                    }

                    case opcode::bit_and: {
                        if (inst.defs.empty() || inst.uses.size() < 2) {
                            fail("bit_and: bad operands");
                            break;
                        }
                        X86Gp lhs, rhs, res = cc.new_gp64();
                        if (!read(inst.uses[0], lhs)) break;
                        if (!read(inst.uses[1], rhs)) break;
                        cc.mov(res, lhs);
                        cc.and_(res, rhs);
                        write(inst.defs[0], res);
                        break;
                    }

                    case opcode::bit_or: {
                        if (inst.defs.empty() || inst.uses.size() < 2) {
                            fail("bit_or: bad operands");
                            break;
                        }
                        X86Gp lhs, rhs, res = cc.new_gp64();
                        if (!read(inst.uses[0], lhs)) break;
                        if (!read(inst.uses[1], rhs)) break;
                        cc.mov(res, lhs);
                        cc.or_(res, rhs);
                        write(inst.defs[0], res);
                        break;
                    }

                    case opcode::bit_xor: {
                        if (inst.defs.empty() || inst.uses.size() < 2) {
                            fail("bit_xor: bad operands");
                            break;
                        }
                        X86Gp lhs, rhs, res = cc.new_gp64();
                        if (!read(inst.uses[0], lhs)) break;
                        if (!read(inst.uses[1], rhs)) break;
                        cc.mov(res, lhs);
                        cc.xor_(res, rhs);
                        write(inst.defs[0], res);
                        break;
                    }

                    case opcode::bit_not: {
                        if (inst.defs.empty() || inst.uses.empty()) {
                            fail("bit_not: bad operands");
                            break;
                        }
                        X86Gp src, res = cc.new_gp64();
                        if (!read(inst.uses[0], src)) break;
                        cc.mov(res, src);
                        cc.not_(res);
                        write(inst.defs[0], res);
                        break;
                    }

                    case opcode::shl: {
                        if (inst.defs.empty() || inst.uses.size() < 2) {
                            fail("shl: bad operands");
                            break;
                        }
                        X86Gp lhs, rhs, res = cc.new_gp64(), cl = cc.new_gp32();
                        if (!read(inst.uses[0], lhs)) break;
                        if (!read(inst.uses[1], rhs)) break;
                        cc.mov(res, lhs);
                        cc.mov(cl, rhs.r32());
                        cc.shl(res, cl.r8());
                        write(inst.defs[0], res);
                        break;
                    }

                    case opcode::shr: {
                        if (inst.defs.empty() || inst.uses.size() < 2) {
                            fail("shr: bad operands");
                            break;
                        }
                        X86Gp lhs, rhs, res = cc.new_gp64(), cl = cc.new_gp32();
                        if (!read(inst.uses[0], lhs)) break;
                        if (!read(inst.uses[1], rhs)) break;
                        cc.mov(res, lhs);
                        cc.mov(cl, rhs.r32());
                        // MIR shr is ARITHMETIC (sign-preserving) — matches the
                        // interpreter and partial-evaluator reference semantics. Use sar.
                        cc.sar(res, cl.r8());
                        write(inst.defs[0], res);
                        break;
                    }

                    case opcode::logical_and:
                    case opcode::logical_or: {
                        if (inst.defs.empty() || inst.uses.size() < 2) {
                            fail("logical_and/or: bad operands");
                            break;
                        }
                        X86Gp lhs, rhs;
                        if (!read(inst.uses[0], lhs)) break;
                        if (!read(inst.uses[1], rhs)) break;
                        X86Gp l1 = cc.new_gp8(), r1 = cc.new_gp8(), res = cc.new_gp64();
                        cc.test(lhs, lhs);
                        cc.setne(l1);
                        cc.test(rhs, rhs);
                        cc.setne(r1);
                        if (inst.op == opcode::logical_and) cc.and_(l1, r1);
                        else cc.or_(l1, r1);
                        cc.movzx(res, l1);
                        write(inst.defs[0], res);
                        break;
                    }

                    case opcode::logical_not: {
                        if (inst.defs.empty() || inst.uses.empty()) {
                            fail("logical_not: bad operands");
                            break;
                        }
                        X86Gp src, res = cc.new_gp64(), tmp = cc.new_gp8();
                        if (!read(inst.uses[0], src)) break;
                        cc.test(src, src);
                        cc.sete(tmp);
                        cc.movzx(res, tmp);
                        write(inst.defs[0], res);
                        break;
                    }

                    case opcode::cmp_eq:
                    case opcode::cmp_ne:
                    case opcode::cmp_lt:
                    case opcode::cmp_le:
                    case opcode::cmp_gt:
                    case opcode::cmp_ge: {
                        if (inst.defs.empty() || inst.uses.size() < 2) {
                            fail("cmp: bad operands");
                            break;
                        }
                        X86Gp lhs, rhs, res = cc.new_gp64(), tmp = cc.new_gp8();
                        if (!read(inst.uses[0], lhs)) break;
                        if (!read(inst.uses[1], rhs)) break;
                        cc.cmp(lhs, rhs);
                        switch (inst.op) {
                        case opcode::cmp_eq: cc.sete(tmp);
                            break;
                        case opcode::cmp_ne: cc.setne(tmp);
                            break;
                        case opcode::cmp_lt: cc.setl(tmp);
                            break;
                        case opcode::cmp_le: cc.setle(tmp);
                            break;
                        case opcode::cmp_gt: cc.setg(tmp);
                            break;
                        case opcode::cmp_ge: cc.setge(tmp);
                            break;
                        default: break;
                        }
                        cc.movzx(res, tmp);
                        write(inst.defs[0], res);
                        break;
                    }

                    // load_spill: dst_preg = MEM[spill_slot]
                    case opcode::load_spill: {
                        if (inst.defs.empty() || inst.uses.empty()) {
                            fail("load_spill: bad operands");
                            break;
                        }
                        if (inst.uses[0].type != allocated_operand::kind::spill) {
                            // Already in a register (trivial copy after RA).
                            X86Gp reg;
                            if (!read(inst.uses[0], reg)) break;
                            write(inst.defs[0], reg);
                        }
                        else {
                            const auto slot_id = std::get<spill_slot>(inst.uses[0].value).id;
                            auto& mem = ensure_spill(cc, spill_map, slot_id, phys);
                            X86Gp dst = cc.new_gp64();
                            cc.mov(dst, mem);
                            write(inst.defs[0], dst);
                        }
                        break;
                    }

                    // store_spill: MEM[spill_slot] = src_preg
                    case opcode::store_spill: {
                        if (inst.defs.empty() || inst.uses.empty()) {
                            fail("store_spill: bad operands");
                            break;
                        }
                        if (inst.defs[0].type != allocated_operand::kind::spill) {
                            // Def is a preg: treat as register move.
                            X86Gp src;
                            if (!read(inst.uses[0], src)) break;
                            write(inst.defs[0], src);
                        }
                        else {
                            const auto slot_id = std::get<spill_slot>(inst.defs[0].value).id;
                            auto& mem = ensure_spill(cc, spill_map, slot_id, phys);
                            X86Gp src;
                            if (!read(inst.uses[0], src)) break;
                            cc.mov(mem, src);
                        }
                        break;
                    }

                    // load: dst = [base + offset]  (generic memory load)
                    case opcode::load: {
                        if (inst.defs.empty() || inst.uses.empty()) {
                            fail("load: bad operands");
                            break;
                        }
                        X86Gp base;
                        if (!read(inst.uses[0], base)) break;
                        std::int64_t offset = 0;
                        if (inst.uses.size() > 1 &&
                            inst.uses[1].type == allocated_operand::kind::immediate_i64)
                            offset = std::get<std::int64_t>(inst.uses[1].value);
                        X86Gp dst = cc.new_gp64();
                        cc.mov(dst, asmjit::x86::qword_ptr(base, static_cast<int32_t>(offset)));
                        write(inst.defs[0], dst);
                        break;
                    }

                    // store: [base + offset] = src
                    case opcode::store: {
                        if (inst.uses.size() < 2) {
                            fail("store: need base and src");
                            break;
                        }
                        X86Gp base, src;
                        if (!read(inst.uses[0], base)) break;
                        if (!read(inst.uses[1], src)) break;
                        std::int64_t offset = 0;
                        if (inst.uses.size() > 2 &&
                            inst.uses[2].type == allocated_operand::kind::immediate_i64)
                            offset = std::get<std::int64_t>(inst.uses[2].value);
                        cc.mov(asmjit::x86::qword_ptr(base, static_cast<int32_t>(offset)), src);
                        break;
                    }

                    case opcode::branch: {
                        if (inst.uses.empty() ||
                            inst.uses[0].type != allocated_operand::kind::block) {
                            fail("branch: missing block operand");
                            break;
                        }
                        const auto tid = std::get<std::uint32_t>(inst.uses[0].value);
                        if (tid >= block_labels.size()) {
                            fail("branch: target block id out of range");
                            break;
                        }
                        cc.jmp(block_labels[tid]);
                        break;
                    }

                    case opcode::branch_cond: {
                        if (inst.uses.size() < 3) {
                            fail("branch_cond: need 3 uses");
                            break;
                        }
                        X86Gp cond;
                        if (!read(inst.uses[0], cond)) break;
                        if (inst.uses[1].type != allocated_operand::kind::block ||
                            inst.uses[2].type != allocated_operand::kind::block) {
                            fail("branch_cond: targets must be blocks");
                            break;
                        }
                        const auto tid_t = std::get<std::uint32_t>(inst.uses[1].value);
                        const auto tid_f = std::get<std::uint32_t>(inst.uses[2].value);
                        if (tid_t >= block_labels.size() || tid_f >= block_labels.size()) {
                            fail("branch_cond: target block id out of range");
                            break;
                        }
                        cc.test(cond, cond);
                        cc.jnz(block_labels[tid_t]);
                        cc.jmp(block_labels[tid_f]);
                        break;
                    }

                    case opcode::ret: {
                        const bool void_ret =
                            phys.signature &&
                            phys.signature->return_value.passing_kind == return_passing_kind::void_return;
                        if (inst.uses.empty() || void_ret) {
                            cc.ret();
                        }
                        else {
                            X86Gp retval;
                            if (!read(inst.uses[0], retval)) break;
                            cc.ret(retval);
                        }
                        break;
                    }

                    case opcode::indirect_call: {
                        // Fuel check: decrement sandbox fuel, trap (ud2) if exhausted.
                        if (inst.abstract_operation &&
                            inst.abstract_operation->domain ==
                            std::string(lithe::runtime::fuel::fuel_domain)) {
                            if (sandbox_) {
                                X86Gp fuel_ptr = cc.new_gp64();
                                cc.mov(fuel_ptr, asmjit::Imm(
                                           reinterpret_cast<std::int64_t>(&sandbox_->fuel_counter)));
                                X86Gp fuel_val = cc.new_gp64();
                                cc.mov(fuel_val, asmjit::x86::qword_ptr(fuel_ptr));
                                cc.sub(fuel_val, asmjit::Imm(1));
                                cc.mov(asmjit::x86::qword_ptr(fuel_ptr), fuel_val);
                                asmjit::Label ok_fuel = cc.new_label();
                                cc.test(fuel_val, fuel_val);
                                cc.jnz(ok_fuel);
                                cc.ud2();
                                cc.bind(ok_fuel);
                            }
                            break;
                        }

                        // Safepoint tags are metadata-only — no machine code emitted.
                        if (inst.abstract_operation &&
                            inst.abstract_operation->domain ==
                            std::string(lithe::runtime::safepoint::safepoint_domain))
                            break;

                        // Unwind opcodes: bind labels, record for post-emit table build.
                        if (inst.abstract_operation &&
                            inst.abstract_operation->domain ==
                            std::string(lithe::runtime::unwind::unwind_domain)) {
                            const auto& op_name = inst.abstract_operation->name;
                            if (op_name == lithe::runtime::unwind::unwind_region_begin_name) {
                                unwind_label_pair pair;
                                pair.begin_label = cc.new_label();
                                cc.bind(pair.begin_label);
                                unwind_pairs.push_back(std::move(pair));
                            }
                            else if (op_name == lithe::runtime::unwind::unwind_region_end_name) {
                                if (!unwind_pairs.empty() && !unwind_pairs.back().end_set) {
                                    unwind_pairs.back().end_label = cc.new_label();
                                    cc.bind(unwind_pairs.back().end_label);
                                    unwind_pairs.back().end_set = true;
                                }
                            }
                            else if (op_name == lithe::runtime::unwind::landing_pad_tag_name) {
                                unwind_lp_record lpr;
                                lpr.label = cc.new_label();
                                cc.bind(lpr.label);
                                if (!inst.uses.empty() &&
                                    inst.uses[0].type == allocated_operand::kind::immediate_i64)
                                    lpr.cleanup_flags = static_cast<std::uint32_t>(
                                        std::get<std::int64_t>(inst.uses[0].value));
                                unwind_lps.push_back(std::move(lpr));
                            }
                            break;
                        }

                        // Dispatch linker opcodes ("lithe.linker" domain).
                        // If a native_proxy is registered for the symbol, use its
                        // arity to build the correct call signature.
                        {
                            const auto lnk = try_linker_dispatch(inst);
                            if (lnk.addr) {
                                const auto* proxy = find_native_proxy(lnk.sym_name);
                                const std::uint8_t nargs = proxy ? proxy->arity : 2u;

                                asmjit::FuncSignature call_sig(
                                    asmjit::CallConvId::kCDecl,
                                    asmjit::FuncSignature::kNoVarArgs,
                                    asmjit::TypeId::kInt64);
                                for (std::uint8_t ai = 0; ai < nargs; ++ai)
                                    call_sig.add_arg(asmjit::TypeId::kInt64);

                                X86Gp fn_reg = cc.new_gp64();
                                cc.mov(fn_reg, asmjit::Imm(
                                           reinterpret_cast<std::int64_t>(lnk.addr)));
                                asmjit::InvokeNode* invoke = nullptr;
                                cc.invoke(asmjit::Out<asmjit::InvokeNode*>(invoke),
                                          fn_reg, call_sig);
                                if (!invoke) {
                                    fail("linker: invoke failed");
                                    break;
                                }
                                for (std::size_t ai = 1; ai < inst.uses.size(); ++ai) {
                                    X86Gp arg;
                                    if (read(inst.uses[ai], arg))
                                        invoke->set_arg(static_cast<uint32_t>(ai - 1), arg);
                                }
                                if (!inst.defs.empty()) {
                                    X86Gp ret = cc.new_gp64();
                                    invoke->set_ret(0, ret);
                                    write(inst.defs[0], ret);
                                }
                                break;
                            }
                            if (inst.abstract_operation &&
                                inst.abstract_operation->domain ==
                                std::string(lithe::runtime::linker::linker_domain)) {
                                art.diagnostics.push_back(
                                    "asmjit(x86): linker dispatch failed for " + fn.name);
                                break;
                            }
                        }

                        const auto mop = try_mop_dispatch(inst);
                        using K = mop_dispatch_result::kind;
                        if (mop.k == K::error) {
                            fail(("mop dispatch failed: " + mop.error_msg).c_str());
                            break;
                        }
                        if (mop.k == K::immediate) {
                            if (!inst.defs.empty()) {
                                X86Gp res = cc.new_gp64();
                                cc.mov(res, asmjit::Imm(mop.value));
                                write(inst.defs[0], res);
                            }
                            break;
                        }
                        if (mop.k == K::field_offset) {
                            if (inst.defs.empty() || inst.uses.empty()) {
                                fail("mop.get_field: need def and obj_ptr use");
                                break;
                            }
                            X86Gp obj;
                            if (!read(inst.uses[0], obj)) break;
                            X86Gp res = cc.new_gp64();
                            cc.lea(res, asmjit::x86::qword_ptr(obj, static_cast<int32_t>(mop.value)));
                            write(inst.defs[0], res);
                            break;
                        }
                        if (mop.k == K::method_fn_ptr) {
                            if (inst.defs.empty() || inst.uses.empty()) {
                                fail("mop.invoke_method: need def and obj_ptr use");
                                break;
                            }
                            asmjit::FuncSignature call_sig(
                                asmjit::CallConvId::kCDecl,
                                asmjit::FuncSignature::kNoVarArgs,
                                asmjit::TypeId::kInt64,
                                asmjit::TypeId::kUIntPtr,
                                asmjit::TypeId::kUIntPtr,
                                asmjit::TypeId::kUInt32
                            );
                            X86Gp fn_reg = cc.new_gp64();
                            cc.mov(fn_reg, asmjit::Imm(mop.value));
                            asmjit::InvokeNode* invoke = nullptr;
                            cc.invoke(asmjit::Out<asmjit::InvokeNode*>(invoke), fn_reg, call_sig);
                            if (!invoke) {
                                fail("mop.invoke_method: invoke failed");
                                break;
                            }
                            X86Gp obj;
                            if (!read(inst.uses[0], obj)) break;
                            invoke->set_arg(0, obj);
                            X86Gp null_args = cc.new_gp64();
                            cc.mov(null_args, asmjit::Imm(0));
                            invoke->set_arg(1, null_args);
                            X86Gp argc_zero = cc.new_gp32();
                            cc.mov(argc_zero, asmjit::Imm(0));
                            invoke->set_arg(2, argc_zero);
                            X86Gp ret = cc.new_gp64();
                            invoke->set_ret(0, ret);
                            write(inst.defs[0], ret);
                            break;
                        }
                        art.diagnostics.push_back(
                            "asmjit(x86): unresolved indirect_call in " + fn.name
                        );
                        break;
                    }

                    default:
                        art.diagnostics.push_back(
                            "asmjit(x86): unsupported opcode '" +
                            std::string(to_string(inst.op)) + "' in " + fn.name
                        );
                        break;
                    }
                }
            }

            cc.end_func();

            if (error) return art;

            {
                const asmjit::Error fe = cc.finalize();
                if (fe != asmjit::kErrorOk) {
                    art.diagnostics.push_back("asmjit(x86): finalize: " + asmjit_err(fe));
                    return art;
                }
            }

            jit_function_handle::fn_i64_t fn_ptr = nullptr;
            {
                const asmjit::Error ae = handle->runtime->add(&fn_ptr, &code);
                if (ae != asmjit::kErrorOk) {
                    art.diagnostics.push_back("asmjit(x86): runtime.add: " + asmjit_err(ae));
                    return art;
                }
            }

            handle->fn_ptr = fn_ptr;
            auto h = std::make_shared<artifact_handle>();
            h->kind = artifact_handle_kind::jit_function;
            h->payload = std::move(handle);
            art.handle = std::move(h);
            art.metadata["jit_fn_name"] = art.name;
            register_stack_map(phys);
            register_unwind_table(fn.name, unwind_pairs, unwind_lps, code,
                                  reinterpret_cast<uintptr_t>(fn_ptr));
            return art;
        }
#endif // arch
    };

    static_assert(CodeEmissionTarget<asmjit_backend>,
                  "asmjit_backend must satisfy CodeEmissionTarget");
} // namespace lithe::codegen::backends
