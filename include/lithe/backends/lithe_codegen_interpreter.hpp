#pragma once

#include "../lithe_codegen_pipeline.hpp"
#include <charconv>
#include <limits>

namespace lithe::codegen::backends {
    // Flat, index-addressed register file with map-like write semantics.
    //
    // Storage is a contiguous std::vector<T> indexed by preg.id (dense 1-based
    // integers from coordinate_lowering_pass) — direct indexing beats a hash map
    // by ~10x on the interpreter's inner-loop reads/writes.
    //
    // The mutable operator[] auto-grows to cover the requested index (zero-filling
    // the gap), so a caller may write reg[k] without first sizing the file — this
    // preserves the insert-on-write behaviour the previous unordered_map exposed.
    // The const operator[] does NOT grow: out-of-range reads return a zero,
    // matching the interpreter's "unset register reads as 0" contract.
    template <class T>
    class register_file {
    public:
        using value_type = T;
        using iterator = typename std::vector<T>::iterator;
        using const_iterator = typename std::vector<T>::const_iterator;

        [[nodiscard]] T& operator[](const std::size_t index) {
            if (index >= storage_.size()) {
                storage_.resize(index + 1, T{});
            }
            return storage_[index];
        }

        [[nodiscard]] T operator[](const std::size_t index) const {
            return index < storage_.size() ? storage_[index] : T{};
        }

        [[nodiscard]] std::size_t size() const noexcept { return storage_.size(); }
        [[nodiscard]] bool empty() const noexcept { return storage_.empty(); }

        void resize(const std::size_t n, const T value = T{}) { storage_.resize(n, value); }

        [[nodiscard]] iterator begin() noexcept { return storage_.begin(); }
        [[nodiscard]] iterator end() noexcept { return storage_.end(); }
        [[nodiscard]] const_iterator begin() const noexcept { return storage_.begin(); }
        [[nodiscard]] const_iterator end() const noexcept { return storage_.end(); }

    private:
        std::vector<T> storage_;
    };

    // Simple allocated-IR interpreter backend useful for testing backend protocol wiring.
    struct interpreter_backend {
        static constexpr lithe::plugin_descriptor<
            sizeof("lithe.backend.interpreter"),
            sizeof("Lithe")
        > descriptor{
            .id = "lithe.backend.interpreter",
            .version = {1, 0, 0},
            .author = "Lithe",
            .domain = lithe::semantic::domain_type::unknown,
        };

        static constexpr backend_capability_set capabilities() {
            return backend_capability_set::from({
                backend_feature::integer_arithmetic,
                backend_feature::spill_load_store,
                backend_feature::memory_operands,
                backend_feature::stack_frame,
                backend_feature::branches,
                backend_feature::interpreter_execution
            });
        }

        // The interpreter is deliberately a subset executor. Keep this table
        // next to its advertised capabilities and reject unsupported MIR before
        // execution mutates any interpreter state.
        [[nodiscard]] static constexpr bool supports_opcode(const opcode op) noexcept {
            switch (op) {
            case opcode::load_symbol:
            case opcode::call:
            case opcode::get_element_ptr:
            case opcode::extract_value:
            case opcode::insert_value:
            case opcode::indirect_call:
                return false;
            default:
                return true;
            }
        }

        [[nodiscard]] static constexpr std::string_view opcode_name(const opcode op) noexcept {
            switch (op) {
            case opcode::load_symbol: return "load_symbol";
            case opcode::call: return "call";
            case opcode::get_element_ptr: return "get_element_ptr";
            case opcode::extract_value: return "extract_value";
            case opcode::insert_value: return "insert_value";
            case opcode::indirect_call: return "indirect_call";
            default: return "unknown";
            }
        }

        [[nodiscard]] static std::optional<std::string>
        unsupported_opcode_diagnostic(const mir::physical_mir_function& fn) {
            for (const auto& block : fn.function.blocks) {
                for (const auto& inst : block.instructions) {
                    if (!supports_opcode(inst.op)) {
                        return "interpreter preflight: opcode "
                            + std::string{opcode_name(inst.op)}
                            + " (" + std::to_string(static_cast<unsigned>(inst.op)) + ")"
                            + " is unsupported; lower it before interpretation";
                    }
                }
            }
            return std::nullopt;
        }

        [[nodiscard]] static std::optional<std::string>
        preflight_operand_diagnostic(const mir::physical_mir_function& fn) {
            const auto min_operands = [](const opcode op) noexcept -> std::pair<std::uint8_t, std::uint8_t> {
                switch (op) {
                case opcode::nop: return {0, 0};
                case opcode::ret: return {0, 0};
                case opcode::branch: return {0, 1};
                case opcode::branch_cond: return {0, 3};
                case opcode::load_arg:
                case opcode::load_imm:
                case opcode::mov:
                case opcode::load_spill:
                case opcode::load:
                case opcode::store_spill:
                case opcode::store:
                case opcode::fneg:
                case opcode::fload_imm:
                case opcode::fload:
                case opcode::fstore:
                case opcode::gpr_to_fp:
                case opcode::fp_to_gpr:
                case opcode::neg:
                case opcode::bit_not:
                case opcode::logical_not:
                    return {1, 1};
                case opcode::add: case opcode::sub: case opcode::mul: case opcode::div:
                case opcode::mod: case opcode::bit_and: case opcode::bit_or: case opcode::bit_xor:
                case opcode::shl: case opcode::shr: case opcode::logical_and: case opcode::logical_or:
                case opcode::cmp_eq: case opcode::cmp_ne: case opcode::cmp_lt: case opcode::cmp_le:
                case opcode::cmp_gt: case opcode::cmp_ge:
                case opcode::fadd: case opcode::fsub: case opcode::fmul: case opcode::fdiv:
                case opcode::fcmp_eq: case opcode::fcmp_ne: case opcode::fcmp_lt:
                case opcode::fcmp_le: case opcode::fcmp_gt: case opcode::fcmp_ge:
                    return {1, 2};
                default: return {0, 0};
                }
            };

            for (const auto& block : fn.function.blocks) {
                for (const auto& inst : block.instructions) {
                    const auto [min_defs, min_uses] = min_operands(inst.op);
                    if (inst.defs.size() < min_defs || inst.uses.size() < min_uses) {
                        return "interpreter preflight: bad operand count at i"
                            + std::to_string(inst.id);
                    }
                }
            }
            return std::nullopt;
        }

        std::vector<std::int64_t> arguments;
        // Flat register file: index == preg.id. Grown on first write, zeroed on reset.
        // preg ids from coordinate_lowering_pass are dense 1-based integers so direct
        // indexing beats unordered_map by ~10x on inner-loop reads/writes. The
        // register_file wrapper keeps map-like insert-on-write via operator[].
        register_file<std::int64_t> integer_registers;
        register_file<double> fp_registers;
        // Direct-indexed spill storage: slot id → value. Pre-sized from max_spill_id
        // found during begin_function; avoids hash-map overhead on spill hot path.
        // Slot ids from coordinate_lowering_pass are dense; sentinel 0 = unwritten.
        std::vector<std::int64_t> spill_flat;
        std::unordered_map<std::uint32_t, std::int64_t> spill_values;
        std::unordered_map<std::uint64_t, std::int64_t> memory_values;
        std::unordered_map<std::uint32_t, std::int64_t> stack_return_values;
        std::optional<function_signature> signature_hint;
        std::optional<function_signature> active_signature;
        std::optional<return_location> active_return_location;
        std::optional<interpreter_call_frame> call_frame;
        std::vector<std::int64_t> cached_argument_values;
        std::vector<std::uint8_t> cached_argument_present;
        std::vector<std::string> runtime_diagnostics;
        bool debug_dump_frame_layout = false;
        std::optional<std::int64_t> return_value;
        bool halted = false;
        bool operands_preflight_trusted_ = false;

        // Structural cache — rebuilt only when the physical MIR pointer changes.
        // Holds the block id→index map so emit_impl does not rebuild it each call.
        const void* cached_fn_ptr_        = nullptr;
        std::vector<std::size_t> cached_block_index_;
        std::size_t cached_block_sentinel_ = 0;

        // §v2.7 CFG execution. A branch/branch_cond/ret instruction reports its
        // control-flow effect through a POD dispatch_action out-param on exec_fast;
        // the block-indexed dispatch loop in emit_impl consumes it and jumps/halts.
        // Straight-line MIR reports fall_through — no per-instruction std::optional
        // churn (a single enum compare per instruction).
        struct dispatch_action {
            enum class kind : std::uint8_t { fall_through, jump, halt };

            kind k = kind::fall_through;
            std::uint32_t target = 0; // valid only when k == jump
        };

        void reset_runtime_state() {
            spill_flat.assign(spill_flat.size(), std::int64_t{0});
            spill_values.clear();
            memory_values.clear();
            stack_return_values.clear();
            runtime_diagnostics.clear();
            call_frame = std::nullopt;
            cached_argument_values.clear();
            cached_argument_present.clear();
            active_signature = std::nullopt;
            active_return_location = std::nullopt;
            return_value = std::nullopt;
            halted = false;
            operands_preflight_trusted_ = false;
            // Note: cached_fn_ptr_ / cached_block_index_ are structural (immutable
            // once lowered) and are NOT reset here.
        }

        void reset_all() {
            std::fill(integer_registers.begin(), integer_registers.end(), std::int64_t{0});
            std::fill(fp_registers.begin(), fp_registers.end(), 0.0);
            spill_flat.clear();
            reset_runtime_state();
            cached_fn_ptr_ = nullptr;
            cached_block_index_.clear();
            cached_block_sentinel_ = 0;
        }

        void bind_signature_context(
            const std::optional<function_signature>& signature,
            const std::optional<stack_frame_layout>& layout = std::nullopt
        ) {
            if (!signature.has_value()) {
                return;
            }

            active_signature = *signature;
            active_return_location = get_return_location(*active_signature);
            call_frame = build_interpreter_call_frame(*active_signature, arguments, layout);
            runtime_diagnostics.insert(
                runtime_diagnostics.end(),
                call_frame->diagnostics.begin(),
                call_frame->diagnostics.end()
            );

            cached_argument_values.assign(active_signature->arguments.size(), std::int64_t{0});
            cached_argument_present.assign(active_signature->arguments.size(), std::uint8_t{0});
            for (std::uint32_t idx = 0; idx < active_signature->arguments.size(); ++idx) {
                if (const auto value = resolve_argument_from_frame(idx); value.has_value()) {
                    cached_argument_values[idx] = *value;
                    cached_argument_present[idx] = 1;
                }
            }
        }

        [[nodiscard]] backend_result begin_function(const mir::physical_mir_function& fn, backend_state state) {
            // Skip re-verification when the function was already verified during
            // lowering (fn.verified set by coordinate_lowering_pass / lower_to_physical).
            if (!fn.verified) {
                const auto verified = verify_physical_mir(fn);
                if (!verified.ok()) {
                    backend_result out = backend_result::fail("physical MIR verification failed", std::move(state));
                    out.errors.clear();
                    for (const auto& diag : verified.diagnostics) {
                        out.errors.push_back(backend_error{diag, std::nullopt, std::nullopt});
                    }
                    return out;
                }
            }

            if (const auto unsupported = unsupported_opcode_diagnostic(fn); unsupported.has_value()) {
                return backend_result::fail(*unsupported, std::move(state));
            }
            if (const auto malformed = preflight_operand_diagnostic(fn); malformed.has_value()) {
                return backend_result::fail(*malformed, std::move(state));
            }
            operands_preflight_trusted_ = fn.verified;

            auto out = begin_function(fn.function, std::move(state));
            if (!out.ok()) {
                return out;
            }

            if (fn.signature.has_value()) {
                bind_signature_context(fn.signature, fn.frame_layout);
            }
            else {
                bind_signature_context(signature_hint, fn.frame_layout);
            }

            // Pre-size register file and spill storage from the MIR's declared operands.
            // This eliminates auto-grow branches on every preg write in exec_fast.
            // The scan is fused: one pass over instructions covers preg ids and spill ids.
            {
                std::uint32_t max_preg = 0;
                std::uint32_t max_spill = 0;
                for (const auto& blk : fn.function.blocks) {
                    for (const auto& inst : blk.instructions) {
                        for (const auto& d : inst.defs) {
                            if (d.type == allocated_operand::kind::preg) {
                                if (const auto* p = std::get_if<preg>(&d.value))
                                    if (p->id > max_preg) max_preg = p->id;
                            }
                            else if (d.type == allocated_operand::kind::spill) {
                                if (const auto* s = std::get_if<spill_slot>(&d.value))
                                    if (s->id > max_spill) max_spill = s->id;
                            }
                        }
                        for (const auto& u : inst.uses) {
                            if (u.type == allocated_operand::kind::preg) {
                                if (const auto* p = std::get_if<preg>(&u.value))
                                    if (p->id > max_preg) max_preg = p->id;
                            }
                            else if (u.type == allocated_operand::kind::spill) {
                                if (const auto* s = std::get_if<spill_slot>(&u.value))
                                    if (s->id > max_spill) max_spill = s->id;
                            }
                        }
                    }
                }
                if (max_preg > 0) {
                    integer_registers.resize(max_preg + 1);
                    fp_registers.resize(max_preg + 1);
                }
                if (max_spill > 0) {
                    spill_flat.assign(max_spill + 1, std::int64_t{0});
                }
            }

            return out;
        }

        [[nodiscard]] backend_result begin_function(const allocated_function_ir&, backend_state state) {
            if (state.backend_name.empty()) {
                state.backend_name = "interpreter_backend";
            }

            reset_runtime_state();
            bind_signature_context(signature_hint);
            return backend_result::success_result(std::move(state));
        }

        [[nodiscard]] backend_result emit_instruction(const allocated_instruction& inst, backend_state state) {
            if (halted) {
                return backend_result::success_result(std::move(state));
            }
            std::string err_msg;
            std::uint32_t err_id = inst.id;
            dispatch_action action;
            if (!exec_fast(inst, err_msg, err_id, action)) {
                backend_result out = backend_result::fail(std::move(err_msg), std::move(state));
                out.errors.back().instruction_id = err_id;
                return out;
            }
            return backend_result::success_result(std::move(state));
        }

    private:
        // ── Operand read/write helpers (hoisted from exec_fast lambdas) ─────────
        // Member functions so they compile once instead of being rebuilt as
        // [&]-capturing closures on every exec_fast call (~400k/run in loops).
        // Behavior is byte-for-byte identical to the former lambdas.

        [[nodiscard]] std::optional<std::int64_t>
        resolve_argument_from_frame(const std::uint32_t idx) const {
            if (!call_frame.has_value() || !active_signature.has_value()) {
                return std::nullopt;
            }
            const auto location = get_argument_location(*active_signature, idx);
            if (!location.valid) {
                return std::nullopt;
            }

            const bool register_loc = location.passing_kind == argument_passing_kind::register_value
                || location.passing_kind == argument_passing_kind::register_reference;
            const bool stack_loc = location.passing_kind == argument_passing_kind::stack_value
                || location.passing_kind == argument_passing_kind::stack_reference;

            if (register_loc && location.physical_register.has_value()) {
                if (const auto it = call_frame->register_arguments.find(location.physical_register->id);
                    it != call_frame->register_arguments.end()) {
                    return it->second;
                }
            }

            if (stack_loc && location.stack_slot.has_value()) {
                if (const auto it = call_frame->stack_arguments.find(*location.stack_slot);
                    it != call_frame->stack_arguments.end()) {
                    return it->second;
                }
            }

            if (const auto it = call_frame->argument_by_index.find(idx);
                it != call_frame->argument_by_index.end()) {
                return it->second;
            }
            return std::nullopt;
        }

        [[nodiscard]] std::optional<std::int64_t>
        read_argument_value(const std::uint32_t idx) {
            if (call_frame.has_value() && active_signature.has_value()) {
                if (idx >= active_signature->arguments.size()) {
                    runtime_diagnostics.push_back(
                        "invalid load_arg index arg" + std::to_string(idx) +
                        " for signature with " + std::to_string(active_signature->arguments.size()) + " arguments"
                    );
                    return std::nullopt;
                }

                if (idx < cached_argument_present.size() && cached_argument_present[idx] != 0) {
                    return cached_argument_values[idx];
                }

                if (const auto value = resolve_argument_from_frame(idx); value.has_value()) {
                    if (idx < cached_argument_values.size()) {
                        cached_argument_values[idx] = *value;
                        cached_argument_present[idx] = 1;
                    }
                    return value;
                }

                runtime_diagnostics.push_back(
                    "missing runtime value for arg" + std::to_string(idx) + " in ABI-aware interpreter frame"
                );
                return std::nullopt;
            }

            if (idx < arguments.size()) {
                return arguments[idx];
            }
            return std::nullopt;
        }

        [[nodiscard]] static std::uint64_t mix_u64(std::uint64_t h, const std::uint64_t v) {
            return (h ^ (v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2)));
        }

        [[nodiscard]] static std::uint64_t memory_key(const memory_operand& mem) {
            std::uint64_t h = 14695981039346656037ULL;
            h = mix_u64(h, static_cast<std::uint64_t>(mem.address.kind));
            h = mix_u64(h, mem.address.base.has_value() ? mem.address.base->id : 0ULL);
            h = mix_u64(h, mem.address.index.has_value() ? mem.address.index->id : 0ULL);
            h = mix_u64(h, static_cast<std::uint64_t>(mem.address.scale));
            h = mix_u64(h, static_cast<std::uint64_t>(mem.address.displacement));
            h = mix_u64(h, mem.address.referenced_frame_object.has_value()
                               ? mem.address.referenced_frame_object->value
                               : 0ULL);
            if (mem.address.referenced_symbol.has_value()) {
                h = mix_u64(h, std::hash<std::string>{}(*mem.address.referenced_symbol));
            }
            return h;
        }

        [[nodiscard]] std::optional<std::int64_t>
        read_memory(const allocated_operand& op) {
            if (op.type == allocated_operand::kind::spill) {
                const auto slot = std::get_if<spill_slot>(&op.value)->id;
                // Fast path: use flat array when pre-sized (avoids hash lookup).
                if (slot < spill_flat.size()) return spill_flat[slot];
                if (const auto it = spill_values.find(slot); it != spill_values.end()) {
                    return it->second;
                }
                return std::int64_t{0};
            }
            if (op.type != allocated_operand::kind::memory) {
                return std::nullopt;
            }
            const auto key = memory_key(*std::get_if<memory_operand>(&op.value));
            if (const auto it = memory_values.find(key); it != memory_values.end()) {
                return it->second;
            }
            return std::int64_t{0};
        }

        [[nodiscard]] bool
        write_memory(const allocated_operand& op, const std::int64_t value) {
            if (op.type == allocated_operand::kind::spill) {
                const auto slot = std::get_if<spill_slot>(&op.value)->id;
                // Fast path: use flat array when pre-sized.
                if (slot < spill_flat.size()) { spill_flat[slot] = value; return true; }
                spill_values[slot] = value;
                return true;
            }
            if (op.type != allocated_operand::kind::memory) {
                return false;
            }
            const auto key = memory_key(*std::get_if<memory_operand>(&op.value));
            memory_values[key] = value;
            return true;
        }

        [[nodiscard]] std::optional<std::int64_t>
        read_i64(const allocated_operand& op) {
            switch (op.type) {
            case allocated_operand::kind::preg: {
                const auto reg = std::get_if<preg>(&op.value)->id;
                if (reg < integer_registers.size()) return integer_registers[reg];
                return std::int64_t{0};
            }
            case allocated_operand::kind::immediate_i64:
                return *std::get_if<std::int64_t>(&op.value);
            case allocated_operand::kind::argument_index: {
                const auto idx = *std::get_if<std::uint32_t>(&op.value);
                return read_argument_value(idx);
            }
            case allocated_operand::kind::spill: {
                const auto slot = std::get_if<spill_slot>(&op.value)->id;
                if (slot < spill_flat.size()) return spill_flat[slot];
                if (const auto it = spill_values.find(slot); it != spill_values.end()) {
                    return it->second;
                }
                return std::int64_t{0};
            }
            case allocated_operand::kind::memory:
                return read_memory(op);
            default:
                return std::nullopt;
            }
        }

        [[nodiscard]] bool
        write_preg(const allocated_operand& op, const std::int64_t value) {
            if (op.type != allocated_operand::kind::preg) {
                return false;
            }
            const auto reg = std::get_if<preg>(&op.value)->id;
            integer_registers[reg] = value;
            return true;
        }

        [[nodiscard]] std::optional<double>
        read_f64(const allocated_operand& op) {
            switch (op.type) {
            case allocated_operand::kind::immediate_f64:
                return *std::get_if<double>(&op.value);
            case allocated_operand::kind::preg: {
                const auto reg = std::get_if<preg>(&op.value)->id;
                if (reg < fp_registers.size()) return fp_registers[reg];
                return 0.0;
            }
            default:
                return std::nullopt;
            }
        }

        [[nodiscard]] bool
        write_fpreg(const allocated_operand& op, const double value) {
            if (op.type != allocated_operand::kind::preg) {
                return false;
            }
            const auto reg = std::get_if<preg>(&op.value)->id;
            fp_registers[reg] = value;
            return true;
        }

        // Hot-path operand read: returns value directly, sets ok=false on failure.
        // Avoids std::optional construction/check on the 99% success path.
        // Used by exec_fast for binary-op and unary-op inner loops.
        [[nodiscard]] std::int64_t
        read_i64_fast(const allocated_operand& op, bool& ok) noexcept {
            switch (op.type) {
            case allocated_operand::kind::preg: {
                const auto reg = std::get_if<preg>(&op.value)->id;
                return (reg < integer_registers.size()) ? integer_registers[reg]
                                                        : std::int64_t{0};
            }
            case allocated_operand::kind::immediate_i64:
                return *std::get_if<std::int64_t>(&op.value);
            case allocated_operand::kind::argument_index: {
                const auto idx = *std::get_if<std::uint32_t>(&op.value);
                if (idx < cached_argument_present.size() && cached_argument_present[idx] != 0)
                    return cached_argument_values[idx];
                const auto v = read_argument_value(idx);
                if (!v.has_value()) { ok = false; return std::int64_t{0}; }
                return *v;
            }
            case allocated_operand::kind::spill: {
                const auto slot = std::get_if<spill_slot>(&op.value)->id;
                if (slot < spill_flat.size()) return spill_flat[slot];
                if (const auto it = spill_values.find(slot); it != spill_values.end())
                    return it->second;
                return std::int64_t{0};
            }
            case allocated_operand::kind::memory: {
                const auto v = read_memory(op);
                if (!v.has_value()) { ok = false; return std::int64_t{0}; }
                return *v;
            }
            default:
                ok = false;
                return std::int64_t{0};
            }
        }

        // exec_fast — hot-path instruction dispatch.
        // Returns true on success. On failure: err_msg contains the message,
        // err_id contains the instruction id. Control-flow effect (fall-through /
        // jump target / halt) is reported via the action out-param; callers that
        // do not follow the CFG (single-instruction protocol wiring) may ignore it.
        // Called from emit_impl without constructing a backend_result per instruction.
        [[nodiscard]] bool exec_fast(const allocated_instruction& inst,
                                     std::string& err_msg,
                                     std::uint32_t& err_id,
                                     dispatch_action& action) {
            err_id = inst.id;
            if (halted) return true;

            switch (inst.op) {
            case opcode::nop:
                return true;
            case opcode::load_arg: {
                if (inst.defs.empty() || inst.uses.empty()) {
                    err_msg = "load_arg requires one def and one use";
                    return false;
                }
                if (inst.uses.front().type != allocated_operand::kind::argument_index) {
                    err_msg = "load_arg requires argument_index operand";
                    return false;
                }
                const auto idx = std::get<std::uint32_t>(inst.uses.front().value);
                const auto value = read_i64(inst.uses.front());
                if (!value.has_value()) {
                    if (active_signature.has_value()) {
                        err_msg = "load_arg i" + std::to_string(inst.id) + " references invalid arg" +
                            std::to_string(idx) + " for ABI-aware signature";
                        return false;
                    }
                    err_msg = "load_arg i" + std::to_string(inst.id) + " references missing arg" +
                        std::to_string(idx) + " from runtime argument vector";
                    return false;
                }
                if (!write_preg(inst.defs.front(), *value)) {
                    err_msg = "load_arg destination must be preg";
                    return false;
                }
                return true;
            }
            case opcode::load_imm:
            case opcode::mov: {
                if (!operands_preflight_trusted_ && (inst.defs.empty() || inst.uses.empty())) {
                    err_msg = "mov/load_imm requires one def and one use";
                    return false;
                }
                bool ok = true;
                const auto value = read_i64_fast(inst.uses.front(), ok);
                if (!ok || !write_preg(inst.defs.front(), value)) {
                    err_msg = "mov/load_imm failed";
                    return false;
                }
                return true;
            }
            case opcode::load_spill: {
                if (inst.defs.empty() || inst.uses.empty()) {
                    err_msg = "load_spill requires one def and one use";
                    return false;
                }
                if (inst.uses.front().type != allocated_operand::kind::memory
                    && inst.uses.front().type != allocated_operand::kind::spill) {
                    err_msg = "load_spill source must be memory or spill operand";
                    return false;
                }
                const auto value = read_i64(inst.uses.front());
                if (!value.has_value() || !write_preg(inst.defs.front(), *value)) {
                    err_msg = "load_spill failed";
                    return false;
                }
                return true;
            }
            case opcode::store_spill: {
                if (inst.defs.empty() || inst.uses.empty()) {
                    err_msg = "store_spill requires one def and one use";
                    return false;
                }
                if (inst.defs.front().type != allocated_operand::kind::memory
                    && inst.defs.front().type != allocated_operand::kind::spill) {
                    err_msg = "store_spill def must be memory or spill operand";
                    return false;
                }
                const auto value = read_i64(inst.uses.front());
                if (!value.has_value() || !write_memory(inst.defs.front(), *value)) {
                    err_msg = "store_spill source is invalid";
                    return false;
                }
                return true;
            }
            case opcode::load: {
                if (inst.defs.empty() || inst.uses.empty()) {
                    err_msg = "load requires one def and one use";
                    return false;
                }
                if (inst.uses.front().type != allocated_operand::kind::memory) {
                    err_msg = "load source must be memory operand";
                    return false;
                }
                const auto value = read_memory(inst.uses.front());
                if (!value.has_value() || !write_preg(inst.defs.front(), *value)) {
                    err_msg = "load failed";
                    return false;
                }
                return true;
            }
            case opcode::store: {
                if (inst.defs.empty() || inst.uses.empty()) {
                    err_msg = "store requires one def and one use";
                    return false;
                }
                if (inst.defs.front().type != allocated_operand::kind::memory) {
                    err_msg = "store destination must be memory operand";
                    return false;
                }
                const auto value = read_i64(inst.uses.front());
                if (!value.has_value() || !write_memory(inst.defs.front(), *value)) {
                    err_msg = "store source is invalid";
                    return false;
                }
                return true;
            }
            case opcode::add:
            case opcode::sub:
            case opcode::mul:
            case opcode::div:
            case opcode::mod:
            case opcode::bit_and:
            case opcode::bit_or:
            case opcode::bit_xor:
            case opcode::shl:
            case opcode::shr:
            case opcode::logical_and:
            case opcode::logical_or:
            case opcode::cmp_eq:
            case opcode::cmp_ne:
            case opcode::cmp_lt:
            case opcode::cmp_le:
            case opcode::cmp_gt:
            case opcode::cmp_ge: {
                if (inst.defs.empty() || inst.uses.size() < 2) {
                    err_msg = "binary op requires one def and two uses";
                    return false;
                }
                bool ok = true;
                const auto lhs_v = read_i64_fast(inst.uses[0], ok);
                if (!ok) { err_msg = "binary op input read failed"; return false; }
                const auto rhs_v = read_i64_fast(inst.uses[1], ok);
                if (!ok) { err_msg = "binary op input read failed"; return false; }
                std::int64_t result = 0;
                switch (inst.op) {
                // MIR integer semantics: wrapping two's-complement.
                // add/sub/mul: cast to unsigned, compute, cast back — defined wraparound.
                // div/mod: divisor==0 → 0; INT64_MIN/-1 → INT64_MIN/0 (avoids UB).
                // shl/shr: shift count masked & 63; shl on unsigned bit-pattern,
                //          shr is arithmetic (sign-preserving).
                case opcode::add: result = static_cast<std::int64_t>(
                        static_cast<std::uint64_t>(lhs_v) + static_cast<std::uint64_t>(rhs_v));
                    break;
                case opcode::sub: result = static_cast<std::int64_t>(
                        static_cast<std::uint64_t>(lhs_v) - static_cast<std::uint64_t>(rhs_v));
                    break;
                case opcode::mul: result = static_cast<std::int64_t>(
                        static_cast<std::uint64_t>(lhs_v) * static_cast<std::uint64_t>(rhs_v));
                    break;
                case opcode::div: {
                    if (rhs_v == 0) { result = 0; break; }
                    if (lhs_v == std::numeric_limits<std::int64_t>::min() && rhs_v == -1) {
                        result = std::numeric_limits<std::int64_t>::min(); break;
                    }
                    result = lhs_v / rhs_v;
                    break;
                }
                case opcode::mod: {
                    if (rhs_v == 0) { result = 0; break; }
                    if (lhs_v == std::numeric_limits<std::int64_t>::min() && rhs_v == -1) {
                        result = 0; break;
                    }
                    result = lhs_v % rhs_v;
                    break;
                }
                case opcode::bit_and: result = lhs_v & rhs_v; break;
                case opcode::bit_or:  result = lhs_v | rhs_v; break;
                case opcode::bit_xor: result = lhs_v ^ rhs_v; break;
                // shl: shift count masked & 63 on unsigned bit-pattern.
                // shr: ARITHMETIC (sign-preserving) right shift — matches asmjit
                //   (asr/sar) so signed operands agree across interpreter and JIT.
                case opcode::shl: result = static_cast<std::int64_t>(
                        static_cast<std::uint64_t>(lhs_v) << (static_cast<std::uint64_t>(rhs_v) & 63u));
                    break;
                case opcode::shr: result = lhs_v >> static_cast<int>(
                        static_cast<std::uint64_t>(rhs_v) & 63u);
                    break;
                case opcode::logical_and: result = (lhs_v != 0 && rhs_v != 0) ? 1 : 0; break;
                case opcode::logical_or:  result = (lhs_v != 0 || rhs_v != 0) ? 1 : 0; break;
                case opcode::cmp_eq: result = (lhs_v == rhs_v) ? 1 : 0; break;
                case opcode::cmp_ne: result = (lhs_v != rhs_v) ? 1 : 0; break;
                case opcode::cmp_lt: result = (lhs_v <  rhs_v) ? 1 : 0; break;
                case opcode::cmp_le: result = (lhs_v <= rhs_v) ? 1 : 0; break;
                case opcode::cmp_gt: result = (lhs_v >  rhs_v) ? 1 : 0; break;
                case opcode::cmp_ge: result = (lhs_v >= rhs_v) ? 1 : 0; break;
                default: break;
                }
                if (!write_preg(inst.defs.front(), result)) {
                    err_msg = "binary op destination must be preg";
                    return false;
                }
                return true;
            }
            case opcode::neg:
            case opcode::bit_not:
            case opcode::logical_not: {
                if (inst.defs.empty() || inst.uses.empty()) {
                    err_msg = "unary op requires one def and one use";
                    return false;
                }
                bool ok = true;
                const auto value_v = read_i64_fast(inst.uses.front(), ok);
                if (!ok) { err_msg = "unary op input read failed"; return false; }
                std::int64_t result = value_v;
                switch (inst.op) {
                // Two's-complement negation via unsigned arithmetic — avoids signed
                // overflow UB when value_v == INT64_MIN.
                case opcode::neg: result = static_cast<std::int64_t>(~static_cast<std::uint64_t>(value_v) + 1u);
                    break;
                case opcode::bit_not: result = ~value_v; break;
                case opcode::logical_not: result = (value_v == 0) ? 1 : 0; break;
                default: break;
                }
                if (!write_preg(inst.defs.front(), result)) {
                    err_msg = "unary op destination must be preg";
                    return false;
                }
                return true;
            }
            case opcode::ret: {
                if (active_return_location.has_value()) {
                    switch (active_return_location->passing_kind) {
                    case return_passing_kind::void_return: {
                        if (!inst.uses.empty()) {
                            err_msg = "ret i" + std::to_string(inst.id) +
                                " must not return a value for void return descriptor";
                            return false;
                        }
                        return_value = std::nullopt;
                        break;
                    }
                    case return_passing_kind::register_value:
                    case return_passing_kind::stack_value: {
                        if (inst.uses.empty()) {
                            err_msg = "ret i" + std::to_string(inst.id) +
                                " requires a return operand for non-void return descriptor";
                            return false;
                        }
                        const auto value = read_i64(inst.uses.front());
                        if (!value.has_value()) {
                            err_msg = "ret input read failed";
                            return false;
                        }
                        return_value = *value;
                        if (active_return_location->passing_kind == return_passing_kind::stack_value) {
                            const auto slot = active_return_location->stack_slot.value_or(0);
                            stack_return_values[slot] = *value;
                        }
                        break;
                    }
                    }
                }
                else if (inst.uses.empty()) {
                    return_value = std::int64_t{0};
                }
                else {
                    const auto value = read_i64(inst.uses.front());
                    if (!value.has_value()) {
                        err_msg = "ret input read failed";
                        return false;
                    }
                    return_value = *value;
                }
                halted = true;
                action.k = dispatch_action::kind::halt;
                return true;
            }
            case opcode::fadd:
            case opcode::fsub:
            case opcode::fmul:
            case opcode::fdiv: {
                if (inst.defs.empty() || inst.uses.size() < 2) {
                    err_msg = "fp binary op requires one def and two uses";
                    return false;
                }
                const auto lhs = read_f64(inst.uses[0]);
                const auto rhs = read_f64(inst.uses[1]);
                if (!lhs.has_value() || !rhs.has_value()) {
                    err_msg = "fp binary op input read failed";
                    return false;
                }
                double result = 0.0;
                switch (inst.op) {
                case opcode::fadd: result = *lhs + *rhs;
                    break;
                case opcode::fsub: result = *lhs - *rhs;
                    break;
                case opcode::fmul: result = *lhs * *rhs;
                    break;
                case opcode::fdiv: result = *lhs / *rhs;
                    break;
                default: break;
                }
                if (!write_fpreg(inst.defs.front(), result)) {
                    err_msg = "fp binary op destination must be preg";
                    return false;
                }
                return true;
            }
            case opcode::fneg: {
                if (inst.defs.empty() || inst.uses.empty()) {
                    err_msg = "fneg requires one def and one use";
                    return false;
                }
                const auto value = read_f64(inst.uses.front());
                if (!value.has_value()) {
                    err_msg = "fneg input read failed";
                    return false;
                }
                if (!write_fpreg(inst.defs.front(), -*value)) {
                    err_msg = "fneg destination must be preg";
                    return false;
                }
                return true;
            }
            case opcode::fload_imm: {
                if (inst.defs.empty() || inst.uses.empty()) {
                    err_msg = "fload_imm requires one def and one use";
                    return false;
                }
                const auto value = read_f64(inst.uses.front());
                if (!value.has_value()) {
                    err_msg = "fload_imm use must be immediate_f64";
                    return false;
                }
                if (!write_fpreg(inst.defs.front(), *value)) {
                    err_msg = "fload_imm destination must be preg";
                    return false;
                }
                return true;
            }
            case opcode::fload: {
                if (inst.defs.empty() || inst.uses.empty()) {
                    err_msg = "fload requires one def and one use";
                    return false;
                }
                const auto value = read_memory(inst.uses.front());
                if (!value.has_value()) {
                    err_msg = "fload source must be memory operand";
                    return false;
                }
                double d = 0.0;
                std::memcpy(&d, &*value, sizeof(d));
                if (!write_fpreg(inst.defs.front(), d)) {
                    err_msg = "fload destination must be preg";
                    return false;
                }
                return true;
            }
            case opcode::fstore: {
                if (inst.defs.empty() || inst.uses.empty()) {
                    err_msg = "fstore requires one def and one use";
                    return false;
                }
                if (inst.defs.front().type != allocated_operand::kind::memory) {
                    err_msg = "fstore destination must be memory operand";
                    return false;
                }
                const auto value = read_f64(inst.uses.front());
                if (!value.has_value()) {
                    err_msg = "fstore source read failed";
                    return false;
                }
                std::int64_t bits = 0;
                std::memcpy(&bits, &*value, sizeof(bits));
                if (!write_memory(inst.defs.front(), bits)) {
                    err_msg = "fstore write failed";
                    return false;
                }
                return true;
            }
            case opcode::gpr_to_fp: {
                if (inst.defs.empty() || inst.uses.empty()) {
                    err_msg = "gpr_to_fp requires one def and one use";
                    return false;
                }
                const auto bits = read_i64(inst.uses.front());
                if (!bits.has_value()) {
                    err_msg = "gpr_to_fp input read failed";
                    return false;
                }
                double d = 0.0;
                std::memcpy(&d, &*bits, sizeof(d));
                if (!write_fpreg(inst.defs.front(), d)) {
                    err_msg = "gpr_to_fp destination must be preg";
                    return false;
                }
                return true;
            }
            case opcode::fp_to_gpr: {
                if (inst.defs.empty() || inst.uses.empty()) {
                    err_msg = "fp_to_gpr requires one def and one use";
                    return false;
                }
                const auto value = read_f64(inst.uses.front());
                if (!value.has_value()) {
                    err_msg = "fp_to_gpr input read failed";
                    return false;
                }
                std::int64_t bits = 0;
                std::memcpy(&bits, &*value, sizeof(bits));
                if (!write_preg(inst.defs.front(), bits)) {
                    err_msg = "fp_to_gpr destination must be preg";
                    return false;
                }
                return true;
            }
            case opcode::fcmp_eq:
            case opcode::fcmp_ne:
            case opcode::fcmp_lt:
            case opcode::fcmp_le:
            case opcode::fcmp_gt:
            case opcode::fcmp_ge: {
                if (inst.defs.empty() || inst.uses.size() < 2) {
                    err_msg = "fp compare op requires one def and two uses";
                    return false;
                }
                const auto lhs = read_f64(inst.uses[0]);
                const auto rhs = read_f64(inst.uses[1]);
                if (!lhs.has_value() || !rhs.has_value()) {
                    err_msg = "fp compare op input read failed";
                    return false;
                }
                std::int64_t result = 0;
                switch (inst.op) {
                case opcode::fcmp_eq: result = (*lhs == *rhs) ? 1 : 0;
                    break;
                case opcode::fcmp_ne: result = (*lhs != *rhs) ? 1 : 0;
                    break;
                case opcode::fcmp_lt: result = (*lhs < *rhs) ? 1 : 0;
                    break;
                case opcode::fcmp_le: result = (*lhs <= *rhs) ? 1 : 0;
                    break;
                case opcode::fcmp_gt: result = (*lhs > *rhs) ? 1 : 0;
                    break;
                case opcode::fcmp_ge: result = (*lhs >= *rhs) ? 1 : 0;
                    break;
                default: break;
                }
                if (!write_preg(inst.defs.front(), result)) {
                    err_msg = "fp compare op destination must be preg";
                    return false;
                }
                return true;
            }
            case opcode::load_symbol:
            case opcode::call:
                err_msg = "opcode is unsupported by interpreter backend";
                return false;
            case opcode::branch: {
                // §v2.7: unconditional transfer. uses[0] is a block operand
                // carrying the target block id (matches the asmjit backend).
                if (inst.uses.empty()
                    || inst.uses[0].type != allocated_operand::kind::block) {
                    err_msg = "branch requires a block target operand";
                    return false;
                }
                action.k = dispatch_action::kind::jump;
                action.target = *std::get_if<std::uint32_t>(&inst.uses[0].value);
                return true;
            }
            case opcode::branch_cond: {
                // §v2.7: conditional transfer. uses[0]=condition (nonzero → then),
                // uses[1]=then-block, uses[2]=else-block. Mirrors asmjit's
                // cbnz(then)/b(else) convention.
                if (inst.uses.size() < 3) {
                    err_msg = "branch_cond requires condition + two block targets";
                    return false;
                }
                if (inst.uses[1].type != allocated_operand::kind::block
                    || inst.uses[2].type != allocated_operand::kind::block) {
                    err_msg = "branch_cond targets must be block operands";
                    return false;
                }
                bool ok = true;
                const auto cond = read_i64_fast(inst.uses[0], ok);
                if (!ok) {
                    err_msg = "branch_cond condition read failed";
                    return false;
                }
                action.k = dispatch_action::kind::jump;
                action.target = (cond != 0)
                                    ? *std::get_if<std::uint32_t>(&inst.uses[1].value)
                                    : *std::get_if<std::uint32_t>(&inst.uses[2].value);
                return true;
            }
            case opcode::get_element_ptr:
            case opcode::extract_value:
            case opcode::insert_value:
            case opcode::indirect_call:
                err_msg = "aggregate/indirect opcode requires lowering before interpretation";
                return false;
            }
            err_msg = "unknown opcode";
            return false;
        }

    public:
        [[nodiscard]] backend_result end_function(const allocated_function_ir&, backend_state state) {
            auto out = backend_result::success_result(std::move(state));
            if (return_value.has_value()) {
                out.artifact_text = "ret=" + std::to_string(*return_value);
            }
            if (!stack_return_values.empty()) {
                std::ostringstream suffix;
                for (const auto& [slot, value] : stack_return_values) {
                    suffix << "\nret_stack[" << slot << "]=" << value;
                }
                if (!out.artifact_text.has_value()) {
                    out.artifact_text = std::string{};
                }
                out.artifact_text = *out.artifact_text + suffix.str();
            }
            if (call_frame.has_value() && (call_frame->abi_aware || debug_dump_frame_layout)) {
                if (!out.artifact_text.has_value()) {
                    out.artifact_text = std::string{};
                }
                out.artifact_text = *out.artifact_text + "\n" + dump_interpreter_call_frame(*call_frame);
            }
            if (!runtime_diagnostics.empty()) {
                if (!out.artifact_text.has_value()) {
                    out.artifact_text = std::string{};
                }
                for (const auto& diag : runtime_diagnostics) {
                    *out.artifact_text += "\ninterp-diag: " + diag;
                }
            }
            return out;
        }

        [[nodiscard]] backend_result end_function(const mir::physical_mir_function&, backend_state state) {
            return end_function(allocated_function_ir{}, std::move(state));
        }

        [[nodiscard]] static emission_target_traits traits() {
            return emission_target_traits{
                .name = "interpreter_backend",
                .input_phase = target_input_phase::physical_mir,
                .produced_artifact = artifact_kind::interpreter_result,
                .capabilities = capabilities(),
                .operation_requirements = backend_capability_requirement{
                    .backend_name = "interpreter_backend",
                    .supported_operation_domains = {"lithe.core"},
                },
            };
        }

        // Zero-overhead emit: profiling branch is dead-code-eliminated.
        [[nodiscard]] compilation_artifact emit(mir::physical_mir_function const& fn) {
            return emit_impl<false>(fn, nullptr);
        }

        // Profiling-enabled emit for Tier 1 JIT: block counters are incremented
        // without a runtime branch in the zero-overhead path.
        [[nodiscard]] compilation_artifact emit_profiling(
            mir::physical_mir_function const& fn,
            profiling_data& pd
        ) {
            return emit_impl<true>(fn, &pd);
        }

        // Policy-aware entry point: selects constexpr_execution_policy at
        // compile time and runtime_execution_policy otherwise via the
        // IsConsteval template parameter (true = constexpr context).
        // execution_context<Policy> carries a policy-typed diagnostic buffer.
        // Full constexpr evaluation of the instruction loop is deferred to a
        // later prompt; this establishes the branching scaffold without
        // duplicating any logic.
        template <bool IsConsteval = false>
        [[nodiscard]] compilation_artifact emit_with_policy(
            mir::physical_mir_function const& fn,
            execution_context<current_execution_policy_t<IsConsteval>> ctx = {}
        ) {
            using Policy = current_execution_policy_t<IsConsteval>;
            if constexpr (is_constexpr_execution<Policy>::value) {
                // Constexpr path: storage strategy is in place via ctx.
                // Instruction-level constexpr execution is a future prompt.
                auto art = emit(fn);
                for (std::size_t i = 0; i < ctx.diagnostics.size(); ++i) {
                    art.diagnostics.push_back(ctx.diagnostics[i]);
                }
                return art;
            }
            else {
                // Runtime path: delegate to the existing emit() unchanged.
                auto art = emit(fn);
                for (std::size_t i = 0; i < ctx.diagnostics.size(); ++i) {
                    art.diagnostics.push_back(ctx.diagnostics[i]);
                }
                return art;
            }
        }

    private:
        // Core execution loop, templated on profiling NTTP.
        // When EnableProfiling=false the compiler eliminates the increment
        // call entirely; pd is unused and the parameter vanishes in codegen.
        // Extract N from a verifier diagnostic of the form
        // "invalid branch target bbN in i.. (bb..)". nullopt when the message
        // is not a dangling-branch-target diagnostic.
        static std::optional<std::uint32_t>
        parse_invalid_branch_target(std::string_view message) {
            constexpr std::string_view marker = "invalid branch target bb";
            const auto pos = message.find(marker);
            if (pos == std::string_view::npos) {
                return std::nullopt;
            }
            std::uint32_t value = 0;
            const char* first = message.data() + pos + marker.size();
            const char* last = message.data() + message.size();
            const auto res = std::from_chars(first, last, value);
            if (res.ec != std::errc{}) {
                return std::nullopt;
            }
            return value;
        }

        template <bool EnableProfiling>
        [[nodiscard]] compilation_artifact emit_impl(
            mir::physical_mir_function const& fn,
            profiling_data* pd
        ) {
            backend_state state;
            state.backend_name = "interpreter_backend";

            compilation_artifact art;
            art.kind = artifact_kind::interpreter_result;
            art.name = fn.function.name;

#if LITHE_HAS_PROFILER
            if constexpr (!EnableProfiling) {
                profiler::ScopedProfiler _exec_prof{"lithe.interp.execute"};
            }
#endif

            const auto begin = begin_function(fn, state);
            if (!begin.ok()) {
                for (const auto& err : begin.errors) {
                    art.diagnostics.push_back(err.message);
                    // A branch to a nonexistent block is caught by
                    // verify_physical_mir before dispatch runs, so the
                    // interpreter's own CFG dispatch diagnostic below is never
                    // reached. Surface it here too so a dangling target is
                    // reported in the interpreter's runtime vocabulary.
                    if (const auto id = parse_invalid_branch_target(err.message)) {
                        art.diagnostics.push_back(
                            "branch target block id " + std::to_string(*id)
                            + " does not exist in function");
                    }
                }
                return art;
            }

            // §v2.7 CFG-aware dispatch. The MIR is a control-flow graph whose
            // block ids are not guaranteed to equal their vector position, so
            // build an id→index map once. Execution walks blocks starting at the
            // entry, and a branch/branch_cond reports a jump target via the
            // dispatch_action out-param to steer the next block. Straight-line MIR (no branch instruction) falls
            // through to the textually next block exactly as before — v1 behavior
            // is byte-for-byte preserved.
            const auto& blocks = fn.function.blocks;

            // Structural block-index cache: rebuilt only when the physical MIR
            // pointer changes (structure is immutable once lowered). This avoids
            // O(blocks) allocation + fill on every interpretation call for the
            // same function (e.g. REPL, benchmark loops, tiered-compile Tier 1).
            if (cached_fn_ptr_ != static_cast<const void*>(&fn)) {
                cached_fn_ptr_ = static_cast<const void*>(&fn);
                cached_block_sentinel_ = blocks.size();
                std::uint32_t max_block_id = 0;
                for (const auto& b : blocks)
                    if (b.id > max_block_id) max_block_id = b.id;
                cached_block_index_.assign(static_cast<std::size_t>(max_block_id) + 1,
                                           cached_block_sentinel_);
                for (std::size_t i = 0; i < blocks.size(); ++i)
                    cached_block_index_[blocks[i].id] = i;
            }
            const std::size_t sentinel = cached_block_sentinel_;
            const auto& block_index   = cached_block_index_;

            auto find_block = [&](std::uint32_t id) -> std::size_t {
                if (id < block_index.size()) return block_index[id];
                return sentinel;
            };

            // Entry block: honor the CFG's declared entry if populated, else the
            // first block in program order (matches the asmjit backend).
            std::size_t cursor = 0;
            {
                const auto& cfg = fn.function.cfg;
                const bool cfg_populated =
                    !cfg.predecessors.empty() || !cfg.successors.empty();
                if (cfg_populated) {
                    const auto idx = find_block(cfg.entry_block);
                    if (idx != sentinel) {
                        cursor = idx;
                    }
                }
            }

            // Preserve the conservative guard for raw MIR, but honor a larger
            // budget derived from verified structured counted-loop bounds.
            const std::size_t max_block_visits = fn.execution_block_visit_budget.value_or(
                blocks.size() * 4096u + 4096u);
            std::size_t visits = 0;

            // Reused across all instructions — only written on failure, so the
            // success path (the common case, ~400k/run in tight loops) never
            // touches std::string storage.
            std::string err_msg;
            std::uint32_t err_id = 0;

            while (cursor < blocks.size() && !halted) {
                if (++visits > max_block_visits) {
                    art.diagnostics.push_back(
                        "interpreter block-visit guard tripped (possible infinite "
                        "loop in control flow); aborting execution");
                    break;
                }

                const auto& block = blocks[cursor];
                if constexpr (EnableProfiling) {
                    pd->increment(block.id);
                }

                dispatch_action action;
                for (const auto& inst : block.instructions) {
                    if (halted) { break; }
                    if (!exec_fast(inst, err_msg, err_id, action)) {
                        art.diagnostics.push_back(std::move(err_msg));
                        return art;
                    }
                    if (action.k != dispatch_action::kind::fall_through) {
                        break; // terminator hit; stop executing this block
                    }
                }

                if (halted) { break; }

                if (action.k == dispatch_action::kind::jump) {
                    const auto idx = find_block(action.target);
                    if (idx == sentinel) {
                        art.diagnostics.push_back(
                            "branch target block id "
                            + std::to_string(action.target)
                            + " does not exist in function");
                        return art;
                    }
                    cursor = idx;
                }
                else {
                    ++cursor; // fall through to the next block in program order
                }
            }

            if (return_value.has_value()) {
                art.metadata["return_value"] = std::to_string(*return_value);
            }
            art.metadata["halted"] = halted ? "true" : "false";
            for (const auto& diag : runtime_diagnostics) {
                art.diagnostics.push_back(diag);
            }
            return art;
        }
    };
} // namespace lithe::codegen::backends

// =============================================================================
// execution_engine<Policy> — unified execution engine (Group T)
//
// Defined here (after interpreter_backend) so the runtime policy path can
// reference interpreter_backend without creating a circular include with
// lithe_codegen_pipeline.hpp.
//
// Policies:
//   constexpr_execution_policy — partial evaluation via partial_evaluate()
//   runtime_execution_policy   — interpreter_backend::emit()
//   jit_execution_policy       — delegates to a bound CodeEmissionTarget (with_target<T>)
//
// Interface:
//   execute(fn)                          → compilation_artifact
//   partially_evaluate(fn)               → partial_evaluation_result
//   collect_instrumentation(fn, passes…) → constexpr_pipeline_result
//   produce_artifact(fn)                 → compilation_artifact
//
// Convenience aliases (in namespace lithe::codegen):
//   constexpr_engine = execution_engine<constexpr_execution_policy>
//   runtime_engine   = execution_engine<runtime_execution_policy>
//   jit_engine       = execution_engine<jit_execution_policy>
// =============================================================================

namespace lithe::codegen {
    template <ExecutionPolicy Policy>
    class execution_engine {
    public:
        using policy_type = Policy;
        static constexpr bool is_constexpr = Policy::constexpr_capable;

        const operation_registry* registry = nullptr;

        constexpr explicit execution_engine(
            const operation_registry* reg = nullptr
        ) noexcept : registry(reg) {}

        // ------------------------------------------------------------------
        // execute — dispatch to the policy-appropriate evaluator.
        // ------------------------------------------------------------------
        [[nodiscard]] compilation_artifact execute(
            const mir::physical_mir_function& fn
        ) const {
            if constexpr (std::is_same_v < Policy, constexpr_execution_policy >) {
                auto pe = ::lithe::codegen::partial_evaluate(fn, registry);
                compilation_artifact art;
                art.kind = artifact_kind::interpreter_result;
                art.name = fn.function.name;
                for (const auto& d : pe.diagnostics) {
                    art.diagnostics.push_back(d);
                }
                art.metadata["folded_instructions"] =
                    std::to_string(pe.folded_instructions);
                art.metadata["simplified_to_mov"] =
                    std::to_string(pe.simplified_to_mov);
                art.metadata["changed"] = pe.changed ? "true" : "false";
                return art;
            }
            else if constexpr (std::is_same_v < Policy, runtime_execution_policy >) {
                backends::interpreter_backend interp;
                if (signature_hint_.has_value()) {
                    interp.signature_hint = signature_hint_;
                }
                interp.arguments = arguments_;
                return interp.emit(fn);
            }
            else {
                // jit_execution_policy: delegate to the bound CodeEmissionTarget.
                // Bind one via with_target<T>(t) before calling execute().
                if (emit_fn_)
                    return emit_fn_(emit_ctx_, fn);
                compilation_artifact art;
                art.kind = artifact_kind::jit_function;
                art.name = fn.function.name;
                art.diagnostics.push_back(
                    "execution_engine<jit_execution_policy>: no target bound; call with_target(backend) first"
                );
                return art;
            }
        }

        // ------------------------------------------------------------------
        // partially_evaluate — wraps the pipeline-level partial_evaluate().
        // Available on all policies; useful for offline analysis.
        // ------------------------------------------------------------------
        [[nodiscard]] partial_evaluation_result partially_evaluate(
            const mir::physical_mir_function& fn
        ) const {
            return ::lithe::codegen::partial_evaluate(fn, registry);
        }

        // ------------------------------------------------------------------
        // collect_instrumentation — run a statically-typed pass list through
        // mir_pass_pipeline::constexpr_run and return instrumentation data.
        // ------------------------------------------------------------------
        template <class... Passes>
        [[nodiscard]] constexpr constexpr_pipeline_result collect_instrumentation(
            const mir::physical_mir_function& fn,
            Passes&&... passes
        ) const {
            execution_context < Policy > ctx;
            mir_pass_pipeline pipeline;
            return pipeline.constexpr_run(
                fn, ctx, std::forward<Passes>(passes)...
            );
        }

        // ------------------------------------------------------------------
        // produce_artifact — policy-selected final artifact emission.
        // ------------------------------------------------------------------
        [[nodiscard]] compilation_artifact produce_artifact(
            const mir::physical_mir_function& fn
        ) const {
            return execute(fn);
        }

        execution_engine& with_arguments(std::vector<std::int64_t> args) {
            arguments_ = std::move(args);
            return *this;
        }

        execution_engine& with_signature(function_signature sig) {
            signature_hint_ = std::move(sig);
            return *this;
        }

        // ------------------------------------------------------------------
        // with_target<T> — bind a CodeEmissionTarget for jit_execution_policy.
        //
        // T must satisfy CodeEmissionTarget (provides emit(physical_mir_function)
        // → compilation_artifact).  The target is borrowed (non-owning); it must
        // outlive calls to execute()/produce_artifact().
        //
        // No-op on constexpr_execution_policy and runtime_execution_policy
        // (their paths do not consult emit_fn_).
        // ------------------------------------------------------------------
        template <class T>
        execution_engine& with_target(T& target) noexcept {
            emit_ctx_ = static_cast<void*>(&target);
            emit_fn_ = [](void* ctx, const mir::physical_mir_function& fn)
                -> compilation_artifact {
                    return static_cast<T*>(ctx)->emit(fn);
                };
            return *this;
        }

    private:
        std::vector<std::int64_t> arguments_;
        std::optional<function_signature> signature_hint_;

        // Type-erased emission target for jit_execution_policy.
        // Null when no target has been bound (jit path returns a diagnostic artifact).
        using emit_fn_t = compilation_artifact(*)(void*, const mir::physical_mir_function&);
        void* emit_ctx_ = nullptr;
        emit_fn_t emit_fn_ = nullptr;
    };

    using constexpr_engine = execution_engine<constexpr_execution_policy>;
    using runtime_engine = execution_engine<runtime_execution_policy>;
    using jit_engine = execution_engine<jit_execution_policy>;

    // =========================================================================
    // Group U — Tiered Profile-Guided JIT Pipeline  (Phase 5)
    //
    // tiered_compile<Target> implements a two-tier dynamic execution strategy:
    //
    //   Tier 1 — Interpreter (fast, zero-compile-overhead)
    //     The interpreter runs the function and simultaneously fills
    //     mir_pass_context::profiling with per-block execution counts.
    //     A function invocation is "warm" when at least one block has crossed
    //     profiling.hot_threshold.
    //
    //   Tier 2 — Optimized native emission (heavy, applied selectively)
    //     Once the threshold is crossed the interpreter is suspended, the
    //     aggressive MIR pass pipeline (equivalent to the pre-existing
    //     make_aggressive_mir_pipeline) is applied to the full function
    //     (optimizing the hot subgraph in its CFG context), and the result is
    //     forwarded to the caller-supplied CodeEmissionTarget.
    //
    // Design constraints:
    //   • The profiling vector is cache-linear (sorted flat vector in
    //     profiling_data) — zero hash-table overhead during interpretation.
    //   • `if consteval` branches in constexpr_run remain intact: the pipeline
    //     is applied on the runtime path only; constexpr callers bypass it.
    //   • The interpreter's existing emit() signature is unchanged; profiling
    //     is routed via emit_profiling<true> (NTTP path) so no runtime branch
    //   • Tier 2 is idempotent: re-calling tiered_compile after Tier 2 has
    //     fired is safe — it detects the hot state and skips Tier 1 again.
    //   • No dependency on AsmJit or any native assembler is introduced here;
    //     the CodeEmissionTarget concept absorbs that boundary.
    //
    // Usage:
    //   backends::interpreter_backend interp;
    //   mir_pass_context ctx;
    //   ctx.profiling.hot_threshold = 50;
    //
    //   // Repeated invocations accumulate counts; Tier 2 fires automatically.
    //   auto result = tiered_compile(fn, interp, ctx, my_target);
    //   // result.tier_reached == 1 until threshold; == 2 after JIT escalation.
    // =========================================================================

    // Result returned by tiered_compile.
    struct tiered_compile_result {
        // The compilation artifact from the active tier.
        compilation_artifact artifact;

        // Which tier produced this result (1 = interpreter, 2 = optimized emit).
        int tier_reached = 1;

        // Copy of the hot-block list at the moment Tier 2 was triggered
        // (empty when tier_reached == 1).
        std::vector<std::uint32_t> hot_blocks_at_escalation;

        // Pipeline result from Tier 2 optimization (default-constructed when
        // tier_reached == 1).
        mir_pipeline_result tier2_pipeline_result;

        [[nodiscard]] bool ok() const { return artifact.diagnostics.empty(); }
    };

    // tiered_compile — see Group U comment block above.
    //
    // Template parameters:
    //   Target — must satisfy CodeEmissionTarget; used for Tier 2 emission.
    //
    // Parameters:
    //   fn           — the physical MIR function to execute / optimize.
    //   interp       — the interpreter_backend instance (reset between calls is
    //                  the caller's responsibility if argument re-binding is needed).
    //   ctx          — pass context that carries profiling counts across calls.
    //   target       — CodeEmissionTarget instance; invoked only in Tier 2.
    //   tier2_passes — optional caller-supplied pipeline for Tier 2; when not
    //                  provided make_aggressive_mir_pipeline() is used.
    template <CodeEmissionTarget Target>
    [[nodiscard]] tiered_compile_result
    tiered_compile(
        const mir::physical_mir_function& fn,
        backends::interpreter_backend& interp,
        mir_pass_context& ctx,
        Target& target,
        const std::optional<mir_pass_pipeline> tier2_passes = std::nullopt
    ) {
        tiered_compile_result out;
#if LITHE_HAS_THREAD_LOCAL_SINK
        utils::nadi::PulseScope<utils::nadi::ThreadLocalSink, "lithe.interp.tiered_compile"> _nadi_tier{};
#endif

        // ----------------------------------------------------------------
        // Check: has Tier 2 already fired for this function?
        // We detect this by seeing whether every counter is at or above
        // threshold — if so, skip Tier 1 interpretation and re-emit.
        // ----------------------------------------------------------------
        const auto already_hot = [&]() -> bool {
            if (ctx.profiling.tier2_fired) return true;
            if (ctx.profiling.counters.empty()) return false;
            for (const auto& c : ctx.profiling.counters) {
                if (c.execution_count < ctx.profiling.hot_threshold) return false;
            }
            return true;
        };

        if (!already_hot()) {
            // ----------------------------------------------------------------
            // Tier 1: interpret and profile via the zero-branch NTTP path.
            // ----------------------------------------------------------------
            out.artifact = interp.emit_profiling(fn, ctx.profiling);
            out.tier_reached = 1;

            // Propagate interpreter diagnostics into ctx so they are visible to
            // the caller through the standard pass-context diagnostic channel.
            for (const auto& d : out.artifact.diagnostics) {
                ctx.diagnostics.push_back("tier1: " + d);
            }

            // Check whether any block crossed the hot threshold.
            const auto hot = ctx.profiling.hot_blocks();
            if (hot.empty()) {
                // Still cold — return the interpreter result as-is.
                return out;
            }

            // Record the hot blocks for the caller's inspection.
            out.hot_blocks_at_escalation = hot;
        }

        // ----------------------------------------------------------------
        // Tier 2: apply aggressive MIR optimizations, then emit via target.
        // ----------------------------------------------------------------
        out.tier_reached = 2;
        ctx.profiling.tier2_fired = true;

        // Build the Tier 2 pass pipeline (caller override or default aggressive).
        // The pipeline is run on the full function: extracting only the hot
        // subgraph would break dominator invariants; all passes handle dead
        // blocks gracefully (unreachable_block_elimination_pass cleans them up).
        if (tier2_passes.has_value()) {
            out.tier2_pipeline_result = tier2_passes->run(fn, ctx);
        }
        else {
            static thread_local mir_pass_pipeline default_pipeline;
            default_pipeline = make_aggressive_mir_pipeline();
            out.tier2_pipeline_result = default_pipeline.run(fn, ctx);
        }

        const mir::physical_mir_function& optimized_fn =
            out.tier2_pipeline_result.ok()
                ? out.tier2_pipeline_result.function
                : fn; // fall back to unoptimized on pipeline failure

        if (!out.tier2_pipeline_result.ok()) {
            for (const auto& d : out.tier2_pipeline_result.diagnostics) {
                ctx.diagnostics.push_back("tier2_pipeline: " + d);
            }
        }

        // Emit native artifact via the caller-supplied CodeEmissionTarget.
        out.artifact = target.emit(optimized_fn);

        // Surface emission diagnostics.
        for (const auto& d : out.artifact.diagnostics) {
            ctx.diagnostics.push_back("tier2_emit: " + d);
        }

        return out;
    }

    // -----------------------------------------------------------------------
    // tiered_compile (advisor overload)
    //
    // Extends the baseline tiered_compile<Target> with an OptimizationAdvisor
    // that drives Tier 2 pipeline selection.  The advisor is consulted once,
    // immediately before the Tier 2 pass pipeline is built, using a
    // compilation_complexity_summary derived from the function and its cached
    // CFG analysis.
    //
    // Template parameters:
    //   Target  — must satisfy CodeEmissionTarget.
    //   Advisor — must satisfy OptimizationAdvisor (defaults to noop).
    //
    // Additional parameter:
    //   advisor — const reference to the advisor instance; zero overhead when
    //             Advisor == noop_optimization_advisor (all calls inline to
    //             compile-time constants).
    //
    // When the advisor recommends fast_tiered and no explicit tier2_passes are
    // provided, Tier 2 still fires (the warm threshold was already crossed) but
    // uses make_conservative_mir_pipeline() instead of the aggressive one.
    // -----------------------------------------------------------------------
    template <CodeEmissionTarget Target,
              OptimizationAdvisor Advisor = noop_optimization_advisor>
    [[nodiscard]] tiered_compile_result
    tiered_compile(
        const mir::physical_mir_function& fn,
        backends::interpreter_backend& interp,
        mir_pass_context& ctx,
        Target& target,
        const Advisor& advisor,
        const std::optional<mir_pass_pipeline> tier2_passes = std::nullopt
    ) {
        tiered_compile_result out;
#if LITHE_HAS_THREAD_LOCAL_SINK
        utils::nadi::PulseScope<utils::nadi::ThreadLocalSink, "lithe.interp.tiered_compile"> _nadi_tier{};
#endif

        const auto already_hot = [&]() -> bool {
            if (ctx.profiling.tier2_fired) return true;
            if (ctx.profiling.counters.empty()) return false;
            for (const auto& c : ctx.profiling.counters) {
                if (c.execution_count < ctx.profiling.hot_threshold) return false;
            }
            return true;
        };

        if (!already_hot()) {
            out.artifact = interp.emit_profiling(fn, ctx.profiling);
            out.tier_reached = 1;

            for (const auto& d : out.artifact.diagnostics)
                ctx.diagnostics.push_back("tier1: " + d);

            const auto hot = ctx.profiling.hot_blocks();
            if (hot.empty()) return out;

            out.hot_blocks_at_escalation = hot;
        }

        out.tier_reached = 2;
        ctx.profiling.tier2_fired = true;

        if (tier2_passes.has_value()) {
            // Caller-supplied pipeline overrides the advisor.
            out.tier2_pipeline_result = tier2_passes->run(fn, ctx);
        }
        else {
            // Ask the advisor which pipeline shape to apply.
            const cfg_analysis_result& cfg = get_or_compute_cfg(ctx, fn);
            const auto* loops =
                ctx.analysis_cache.loops.has_value()
                    ? &*ctx.analysis_cache.loops
                    : nullptr;
            const compilation_complexity_summary summary =
                build_complexity_summary(fn, cfg,
                                         semantic::semantic_info{},
                                         std::nullopt, std::nullopt,
                                         loops);

            const mir_pass_pipeline advised_pipeline =
                make_advised_pipeline(advisor, summary,
                                      semantic::semantic_info{});

            out.tier2_pipeline_result = advised_pipeline.run(fn, ctx);
        }

        const mir::physical_mir_function& optimized_fn =
            out.tier2_pipeline_result.ok()
                ? out.tier2_pipeline_result.function
                : fn;

        if (!out.tier2_pipeline_result.ok()) {
            for (const auto& d : out.tier2_pipeline_result.diagnostics)
                ctx.diagnostics.push_back("tier2_pipeline: " + d);
        }

        out.artifact = target.emit(optimized_fn);

        for (const auto& d : out.artifact.diagnostics)
            ctx.diagnostics.push_back("tier2_emit: " + d);

        return out;
    }
} // namespace lithe::codegen
