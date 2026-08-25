#pragma once
// Internal fragment — include via lithe_codegen.hpp (umbrella).
// Depends on: lithe::codegen physical MIR types defined above in lithe_codegen.hpp.

// ============================================================================
// High-Level (Structured/Regional) MIR — lithe::codegen::hl
// ============================================================================
// Type-enforced progressive-lowering layer.  All HL types live in namespace
// lithe::codegen::hl and are arena-owned.  They are distinct types from the
// flat physical_mir_function so the compiler prevents feeding unlowered ops
// to flat backends.  No virtuals, no macros, no reinterpret_cast.
// ============================================================================

#include "mem/arena.hpp"

namespace lithe::codegen::hl {
    // -------------------------------------------------------------------------
    // 1. hl_opcode — high-level / structured dialect
    // -------------------------------------------------------------------------

    enum class hl_opcode : std::uint8_t {
        // ── Structured control flow ──────────────────────────────────────────
        structured_for, // affine counted loop (carries structured_for_attr)
        structured_reduce, // parallel reduction over a loop nest
        region_yield, // region terminator (transfers control to parent)
        loop_index, // IV value inside a structured_for body

        // ── Multi-dimensional memory ops ─────────────────────────────────────
        memref_load, // load from a memref view (index SSA values as operands)
        memref_store, // store to a memref view

        // ── Scalar arithmetic (mirrors flat opcode, high-level wrapper) ───────
        fadd, fsub, fmul, fdiv, fneg,
        add, sub, mul, div,
        // ── Math builtins ─────────────────────────────────────────────────────
        exp, log, sqrt, abs,
        // ── Control / misc ────────────────────────────────────────────────────
        call, // function call (region-level)
        constant, // immediate constant
        argument, // function argument reference

        // ── CFG (schema 1.1.0) ───────────────────────────────────────────────
        branch, // unconditional branch (target in branch_attr)
        branch_cond, // conditional branch (condition operand; targets in branch_cond_attr)
        ret, // function return (wire name "return"); 0..N value operands

        // ── Compare / select (schema 1.1.0) ──────────────────────────────────
        icmp, // integer compare → i1 result (predicate in compare_attr)
        fcmp, // float compare   → i1 result (predicate in compare_attr)
        select, // ternary select: (i1 cond, T true_val, T false_val) → T

        // ── Integer (schema 1.2.0) ────────────────────────────────────────────
        sdiv, udiv, // signed / unsigned integer division
        srem, urem, // signed / unsigned integer remainder
        bit_and, bit_or, bit_xor, // bitwise binary ops
        bit_not, // bitwise complement (unary)
        shl, lshr, ashr, // shift left / logical right / arithmetic right

        // ── Safety (schema 1.3.0) ─────────────────────────────────────────────
        guard, // runtime assertion; falls through on success (carries guard_attr)
        trap, // unconditional abort / unreachable (terminator; carries trap_attr)

        // ── Cleanup / defer (schema 1.4.0) ───────────────────────────────────
        cleanup_region, // structured defer scope (carries cleanup_attr)
        cleanup_yield, // terminator for cleanup_region body

        // ── Transactions (schema 1.5.0) ───────────────────────────────────────
        tx_region, // transactional scope (carries tx_attr)
        tx_read, // transactional resource read → value result
        tx_write, // transactional resource write
        tx_abort, // explicit transaction abort (terminator)
        tx_yield, // transaction commit yield (terminator)
    };

    // -------------------------------------------------------------------------
    // 2. hl_effect_flags — memory/control-flow side-effect bitmask
    //    Used by DCE and legality checks in hl passes.
    // -------------------------------------------------------------------------

    enum class hl_effect_flags : std::uint8_t {
        none = 0,
        read = 1 << 0, // reads memory
        write = 1 << 1, // writes memory
        terminal = 1 << 2, // region control-flow terminator
        may_trap = 1 << 3, // may trap / abort (guards, traps)
    };

    [[nodiscard]] constexpr hl_effect_flags operator|(hl_effect_flags a, hl_effect_flags b) noexcept {
        return static_cast<hl_effect_flags>(
            static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
    }

    [[nodiscard]] constexpr bool has_effect(hl_effect_flags flags, hl_effect_flags probe) noexcept {
        return (static_cast<std::uint8_t>(flags) & static_cast<std::uint8_t>(probe)) != 0;
    }

    [[nodiscard]] constexpr hl_effect_flags effects_of(hl_opcode op) noexcept {
        switch (op) {
        case hl_opcode::memref_load: return hl_effect_flags::read;
        case hl_opcode::memref_store: return hl_effect_flags::write;
        case hl_opcode::region_yield: return hl_effect_flags::terminal;
        case hl_opcode::structured_for:
        case hl_opcode::structured_reduce: return hl_effect_flags::read | hl_effect_flags::write;
        // CFG terminators
        case hl_opcode::branch:
        case hl_opcode::branch_cond:
        case hl_opcode::ret:
            return hl_effect_flags::terminal;
        // Compare / select / pure integer — no effects
        case hl_opcode::icmp:
        case hl_opcode::fcmp:
        case hl_opcode::select:
        case hl_opcode::sdiv:
        case hl_opcode::udiv:
        case hl_opcode::srem:
        case hl_opcode::urem:
        case hl_opcode::bit_and:
        case hl_opcode::bit_or:
        case hl_opcode::bit_xor:
        case hl_opcode::bit_not:
        case hl_opcode::shl:
        case hl_opcode::lshr:
        case hl_opcode::ashr:
            return hl_effect_flags::none;
        // Safety
        case hl_opcode::guard: return hl_effect_flags::may_trap;
        case hl_opcode::trap: return hl_effect_flags::terminal | hl_effect_flags::may_trap;
        // Cleanup
        case hl_opcode::cleanup_yield: return hl_effect_flags::terminal;
        case hl_opcode::cleanup_region: return hl_effect_flags::none;
        // Transactions
        case hl_opcode::tx_abort:
        case hl_opcode::tx_yield: return hl_effect_flags::terminal;
        case hl_opcode::tx_read: return hl_effect_flags::read;
        case hl_opcode::tx_write: return hl_effect_flags::write;
        case hl_opcode::tx_region: return hl_effect_flags::read | hl_effect_flags::write;
        default: return hl_effect_flags::none;
        }
    }

    [[nodiscard]] constexpr bool is_pure(hl_opcode op) noexcept {
        return effects_of(op) == hl_effect_flags::none;
    }

    [[nodiscard]] constexpr bool is_terminator(hl_opcode op) noexcept {
        return has_effect(effects_of(op), hl_effect_flags::terminal);
    }

    // -------------------------------------------------------------------------
    // 3. memref_type — multi-dimensional strided tensor view descriptor
    //    Inline, trivially copyable, rank ≤ 8 (matches poly::affine_matrix::MaxIVs).
    // -------------------------------------------------------------------------

    struct memref_type {
        static constexpr std::uint8_t max_rank = 8;

        abstract_value_kind elem_kind = abstract_value_kind::floating;
        std::uint32_t elem_bits = 64;
        std::uint8_t rank = 1;
        std::uint16_t alignment_bytes = 64;

        // shape[i] == 0  →  dynamic dimension
        std::array<std::int64_t, max_rank> shape{};
        // strides[i]: element stride (not byte stride) along dimension i
        std::array<std::int64_t, max_rank> strides{};
        bool contiguous = true;

        [[nodiscard]] constexpr bool fully_static() const noexcept {
            for (std::uint8_t i = 0; i < rank; ++i)
                if (shape[i] == 0) return false;
            return true;
        }

        [[nodiscard]] constexpr std::int64_t linear_size() const noexcept {
            std::int64_t n = 1;
            for (std::uint8_t i = 0; i < rank; ++i)
                n *= shape[i];
            return n;
        }

        [[nodiscard]] static constexpr memref_type row_major(
            abstract_value_kind kind, std::uint32_t bits, std::uint8_t r,
            std::array<std::int64_t, max_rank> sh) noexcept {
            memref_type m;
            m.elem_kind = kind;
            m.elem_bits = bits;
            m.rank = r;
            m.shape = sh;
            m.strides[r - 1] = 1;
            for (int i = static_cast<int>(r) - 2; i >= 0; --i)
                m.strides[static_cast<std::uint8_t>(i)] =
                    m.strides[static_cast<std::uint8_t>(i + 1)] * sh[static_cast<std::uint8_t>(i + 1)];
            m.contiguous = true;
            return m;
        }
    };

    // -------------------------------------------------------------------------
    // 4. structured_for_attr — loop nest bounds + tiling hint
    //    Separates bounds from tile sizes (avoids the common conflation bug).
    //    Mirrors poly::affine_matrix::MaxIVs = 8.
    // -------------------------------------------------------------------------

    struct structured_for_attr {
        static constexpr std::uint8_t max_ivs = 8;

        std::uint8_t rank = 1;
        bool is_parallel = false;

        struct iv_bounds {
            int lower = 0;
            int upper = 0; // exclusive
            int step = 1;
            bool lower_known = false;
            bool upper_known = false;
            bool step_known = true;
        };

        std::array<iv_bounds, max_ivs> bounds{};
        // tile[i] == 0 → untiled
        std::array<std::uint32_t, max_ivs> tile{};

        // Optional optimization hints. These fields are non-semantic and may be
        // ignored by consumers that do not use them.
        bool bounds_known = false;
        bool stride_regular = false;
        std::uint64_t trip_count_hint = 0;
    };

    // -------------------------------------------------------------------------
    // 5. memref_attr — annotation for memref_load / memref_store ops
    // -------------------------------------------------------------------------

    struct memref_attr {
        memref_type view{};
        std::int32_t base_operand_index = 0;
    };

    // Constant payloads belong to HL-MIR, rather than to a lowering pass.  This
    // keeps a frozen/thawed program and every backend's legality analysis
    // semantically faithful.
    enum class constant_kind : std::uint8_t { integer, floating_point, boolean };

    struct constant_attr {
        constant_kind kind = constant_kind::integer;
        std::int64_t integer = 0;
        double floating_point = 0.0;
        bool boolean = false;

        [[nodiscard]] static constexpr constant_attr integer_value(
            const std::int64_t value) noexcept {
            return {.kind = constant_kind::integer, .integer = value};
        }

        [[nodiscard]] static constexpr constant_attr floating_value(
            const double value) noexcept {
            return {.kind = constant_kind::floating_point, .floating_point = value};
        }

        [[nodiscard]] static constexpr constant_attr boolean_value(
            const bool value) noexcept {
            return {.kind = constant_kind::boolean, .boolean = value};
        }
    };

    // -------------------------------------------------------------------------
    // 6. hl_op_attr — typed variant attribute payload
    // -------------------------------------------------------------------------

    // Attr structs for CFG + compare (schema 1.1.0)
    struct branch_attr {
        std::uint32_t target_block = 0;
    };

    struct branch_cond_attr {
        std::uint32_t true_block = 0;
        std::uint32_t false_block = 0;
    };

    enum class compare_predicate : std::uint8_t {
        eq, ne, slt, sle, sgt, sge, ult, ule, ugt, uge, // integer
        oeq, one, olt, ole, ogt, oge // float (ordered)
    };

    struct compare_attr {
        compare_predicate pred = compare_predicate::eq;
        bool ordered = true;
    };

    // Attr structs for safety (schema 1.3.0)
    enum class guard_kind : std::uint8_t {
        bounds, div_by_zero, range_cast, assertion, overflow, transaction, parallel_safety
    };

    enum class failure_policy : std::uint8_t {
        return_result, trap, terminate, host_handler
    };

    struct guard_attr {
        guard_kind kind = guard_kind::assertion;
        failure_policy policy = failure_policy::trap;
        std::uint32_t diag_code_idx = 0; // index into string table
        std::uint32_t source_span_idx = 0;
    };

    enum class trap_kind : std::uint8_t {
        bounds_violation, div_by_zero, range_conversion, assert_failed,
        overflow_checked, tx_failed, unreachable, host_trap
    };

    struct trap_attr {
        trap_kind kind = trap_kind::unreachable;
        std::uint32_t diag_code_idx = 0;
    };

    // Attr struct for cleanup (schema 1.4.0)
    struct cleanup_attr {
        std::vector<std::uint32_t> cleanup_ids;
    };

    // Attr struct for transactions (schema 1.5.0)
    enum class tx_isolation : std::uint8_t { read_committed, repeatable_read, serializable };

    enum class tx_replay : std::uint8_t { none, on_conflict };

    enum class tx_conflict : std::uint8_t { abort, retry };

    enum class tx_partial : std::uint8_t { disallow, allow };

    enum class tx_durability : std::uint8_t { volatile_, durable, best_effort };

    struct tx_attr {
        tx_isolation iso = tx_isolation::serializable;
        std::uint16_t retry = 0;
        tx_replay replay = tx_replay::none;
        tx_conflict conflict = tx_conflict::abort;
        tx_partial partial = tx_partial::disallow;
        tx_durability durability = tx_durability::durable;
        std::uint32_t distribution_idx = 0; // string table index
        std::uint32_t coordinator_idx = 0;
    };

    using hl_op_attr = std::variant<
        std::monostate,
        structured_for_attr,
        memref_attr,
        constant_attr,
        branch_attr,
        branch_cond_attr,
        compare_attr,
        guard_attr,
        trap_attr,
        cleanup_attr,
        tx_attr
    >;

    // -------------------------------------------------------------------------
    // 7. Intrusive doubly-linked list — O(1) splice for region fusion
    // -------------------------------------------------------------------------

    template <class T>
    struct intrusive_list_node {
        T* prev = nullptr;
        T* next = nullptr;
    };

    template <class T>
    struct intrusive_list {
        T* head = nullptr;
        T* tail = nullptr;
        std::size_t size_ = 0;

        void push_back(T* node) noexcept {
            node->list_node.prev = tail;
            node->list_node.next = nullptr;
            if (tail) tail->list_node.next = node;
            else head = node;
            tail = node;
            ++size_;
        }

        void erase(T* node) noexcept {
            if (node->list_node.prev) node->list_node.prev->list_node.next = node->list_node.next;
            else head = node->list_node.next;
            if (node->list_node.next) node->list_node.next->list_node.prev = node->list_node.prev;
            else tail = node->list_node.prev;
            node->list_node.prev = node->list_node.next = nullptr;
            --size_;
        }

        // Append all nodes from `other`; `other` becomes empty.
        void splice_back(intrusive_list<T>& other) noexcept {
            if (!other.head) return;
            if (tail) {
                tail->list_node.next = other.head;
                other.head->list_node.prev = tail;
            }
            else { head = other.head; }
            tail = other.tail;
            size_ += other.size_;
            other.head = other.tail = nullptr;
            other.size_ = 0;
        }

        [[nodiscard]] bool empty() const noexcept { return head == nullptr; }
        [[nodiscard]] std::size_t size() const noexcept { return size_; }
    };

    // -------------------------------------------------------------------------
    // 8. hl_use — use-def link (arena-allocated singly-linked chain per result)
    // -------------------------------------------------------------------------

    struct hl_operation; // forward

    struct hl_use {
        hl_operation* user_op = nullptr;
        std::uint32_t operand_index = 0;
        hl_use* next_use = nullptr;
    };

    // -------------------------------------------------------------------------
    // 9. hl_operation, hl_block, hl_region
    // -------------------------------------------------------------------------

    struct hl_block;
    struct hl_region;

    struct hl_operation {
        intrusive_list_node<hl_operation> list_node{};

        hl_opcode op = hl_opcode::constant;
        std::span<ssa_value_id> operands{};
        std::span<ssa_value_id> results{};
        std::span<hl_region*> regions{}; // nested region scopes
        hl_op_attr attr{};
        std::uint32_t id = 0;
        hl_use* first_use = nullptr; // head of use chain for result[0]
    };

    struct hl_block {
        intrusive_list_node<hl_block> list_node{};

        std::uint32_t id = 0;
        std::span<ssa_value_id> block_args{}; // MLIR-style (no PHI nodes)
        intrusive_list<hl_operation> ops{};
        hl_region* parent_region = nullptr;
    };

    struct hl_region {
        intrusive_list_node<hl_region> list_node{};

        hl_operation* parent_op = nullptr;
        intrusive_list<hl_block> blocks{};
        std::uint32_t id = 0;
    };

    // -------------------------------------------------------------------------
    // 10. hl_mir_function — structured MIR module
    //     Owns the LinearArena; all hl nodes are arena-allocated.
    //     Distinct type from physical_mir_function — enforces progressive lowering.
    // -------------------------------------------------------------------------

    struct hl_mir_function {
        smriti::pools::LinearArena arena;

        std::string name{};
        hl_region body_region{};
        std::uint32_t next_id = 1;

        explicit hl_mir_function(std::size_t arena_capacity = 1u << 20 /* 1 MiB */)
            : arena{arena_capacity} {}

        hl_mir_function(const hl_mir_function&) = delete;
        hl_mir_function& operator=(const hl_mir_function&) = delete;
        hl_mir_function(hl_mir_function&&) = default;
        hl_mir_function& operator=(hl_mir_function&&) = default;

        template <class T>
        [[nodiscard]] T* alloc_node() {
            void* p = arena.allocate(sizeof(T), alignof(T));
            return p ? new(p) T{} : nullptr;
        }

        [[nodiscard]] hl_operation* make_op(hl_opcode op) {
            auto* o = alloc_node<hl_operation>();
            if (o) {
                o->op = op;
                o->id = next_id++;
            }
            return o;
        }

        [[nodiscard]] hl_block* make_block() {
            auto* b = alloc_node<hl_block>();
            if (b) { b->id = next_id++; }
            return b;
        }

        [[nodiscard]] hl_region* make_region() {
            auto* r = alloc_node<hl_region>();
            if (r) { r->id = next_id++; }
            return r;
        }

        template <class T>
        [[nodiscard]] std::span<T> alloc_span(std::size_t n) {
            if (n == 0) return {};
            void* p = arena.allocate(n * sizeof(T), alignof(T));
            if (!p) return {};
            T* arr = new(p) T[n]{};
            return {arr, n};
        }

        [[nodiscard]] smriti::pools::LinearArena::Checkpoint checkpoint() const noexcept {
            return arena.checkpoint();
        }

        void rollback(smriti::pools::LinearArena::Checkpoint cp) noexcept {
            arena.rollback(cp);
        }
    };

    // -------------------------------------------------------------------------
    // 11. arena_checkpoint_guard — RAII rollback for speculative passes
    // -------------------------------------------------------------------------

    struct arena_checkpoint_guard {
        hl_mir_function& fn_;
        smriti::pools::LinearArena::Checkpoint cp_;
        bool committed_ = false;

        explicit arena_checkpoint_guard(hl_mir_function& fn) noexcept
            : fn_{fn}, cp_{fn.checkpoint()} {}

        void commit() noexcept { committed_ = true; }

        ~arena_checkpoint_guard() {
            if (!committed_) fn_.rollback(cp_);
        }
    };

    // -------------------------------------------------------------------------
    // 12. task_decomposition_plan — engine-agnostic parallel-task descriptor
    //     Trivially copyable C-ABI POD.  Zero dependency on any task runtime.
    // -------------------------------------------------------------------------

    struct loop_range {
        std::int64_t start = 0;
        std::int64_t end = 0;
        std::int64_t step = 1;
    };

    struct task_decomposition_plan {
        static constexpr std::uint8_t max_rank = 8;

        std::array<loop_range, max_rank> bounds{};
        std::uint8_t rank = 0;
        std::size_t chunk = 1;
        void (*kernel)(void*, std::size_t, std::size_t) = nullptr;
        void* user_data = nullptr;
    };

    static_assert(std::is_trivially_copyable_v<loop_range>);
    static_assert(std::is_trivially_copyable_v<task_decomposition_plan>);
    static_assert(std::is_standard_layout_v<task_decomposition_plan>);
} // namespace lithe::codegen::hl
