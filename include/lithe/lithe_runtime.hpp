#pragma once

// lithe_runtime.hpp — Runtime object model: MOP protocol + concrete managers
//
// Provides (namespace lithe::runtime::mop):
//   mop_error / mop_error_code  — error taxonomy
//   field_descriptor            — one field slot
//   method_descriptor           — one method slot
//   object_layout               — full object class descriptor
//   object_ptr                  — non-owning typed pointer
//   language_plugin<Tag>        — extension point for language-specific hooks
//   ObjectManager concept       — structural contract for object lifecycle
//   layout_registry             — optional runtime lookup table
//   MOP MIR opcodes + helpers   — mop.alloc / mop.get_field / mop.invoke_method / mop.dealloc
//   default_object_manager<Tag> — new/delete-backed implementation
//   injecting_object_manager    — caller-supplied allocator (testing / arenas)
//   oom_test_object_manager     — exhaustible budget (OOM path testing)
//   mop_context                 — type-erased callback bundle for backend dispatch
//   make_mop_context(M, reg)    — factory: bind a concrete manager to a context

#include "lithe_codegen.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <expected>
#include <functional>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace lithe::runtime::mop {
    // ---------------------------------------------------------------------------
    // Error types
    // ---------------------------------------------------------------------------

    enum class mop_error_code : std::uint8_t {
        ok = 0,
        out_of_memory,
        invalid_layout,
        field_not_found,
        method_not_found,
        invalid_object_ptr,
        type_mismatch,
        oom_zig_zag,
        not_implemented,
    };

    struct mop_error {
        mop_error_code code = mop_error_code::ok;
        std::string message;

        [[nodiscard]] static mop_error oom(const std::string_view ctx = {}) {
            return {
                mop_error_code::out_of_memory,
                ctx.empty() ? "allocation failed" : std::string(ctx)
            };
        }

        [[nodiscard]] static mop_error bad_layout(const std::string_view ctx = {}) {
            return {
                mop_error_code::invalid_layout,
                ctx.empty() ? "invalid object layout" : std::string(ctx)
            };
        }

        [[nodiscard]] static mop_error no_field(const std::string_view name) {
            return {mop_error_code::field_not_found, "field not found: " + std::string(name)};
        }

        [[nodiscard]] static mop_error no_method(const std::uint64_t id) {
            return {
                mop_error_code::method_not_found,
                "method not found: id=" + std::to_string(id)
            };
        }

        [[nodiscard]] static mop_error null_ptr() {
            return {mop_error_code::invalid_object_ptr, "null object pointer"};
        }
    };

    // ---------------------------------------------------------------------------
    // Layout descriptors
    // ---------------------------------------------------------------------------

    struct field_descriptor {
        std::string name;
        std::size_t byte_offset = 0;
        std::size_t size_bytes = 0;
        std::uint32_t type_tag = 0;
    };

    // Typed function pointer for method dispatch: (raw object ptr, arg array, arg count) → i64
    using method_fn_ptr = std::int64_t(*)(void*, std::int64_t const*, std::uint32_t);

    struct method_descriptor {
        std::uint64_t method_id = 0;
        std::string name;
        std::uint8_t arity = 0;
        void* fn_ptr = nullptr;
        void* fn_context = nullptr;
    };

    struct object_layout {
        std::uint64_t layout_id = 0;
        std::size_t size_bytes = 0;
        std::size_t alignment = alignof(std::max_align_t);
        std::string type_name;

        std::unordered_map<std::string, field_descriptor> field_map;
        std::unordered_map<std::uint64_t, method_descriptor> method_table;

        [[nodiscard]] const field_descriptor* find_field(const std::string_view name) const noexcept {
            const auto it = field_map.find(std::string(name));
            return it != field_map.end() ? &it->second : nullptr;
        }

        [[nodiscard]] static object_layout make(
            std::uint64_t id, std::size_t size, std::size_t align, std::string name,
            std::vector<field_descriptor> fields = {},
            std::vector<method_descriptor> methods = {}) {
            object_layout lay;
            lay.layout_id = id;
            lay.size_bytes = size;
            lay.alignment = align;
            lay.type_name = std::move(name);
            for (auto& f : fields) lay.field_map.emplace(f.name, std::move(f));
            for (auto& m : methods) lay.method_table.emplace(m.method_id, std::move(m));
            return lay;
        }

        [[nodiscard]] bool is_zero_sized() const noexcept { return size_bytes == 0; }

        [[nodiscard]] bool is_valid() const noexcept {
            if (alignment == 0 || (alignment & (alignment - 1)) != 0) return false;
            // Every field must fit within size_bytes, no overflow.
            for (const auto& [name, f] : field_map) {
                if (f.byte_offset > size_bytes) return false;
                if (f.size_bytes > size_bytes - f.byte_offset) return false;
            }
            return true;
        }
    };

    // ---------------------------------------------------------------------------
    // object_ptr — non-owning tagged pointer
    // ---------------------------------------------------------------------------

    struct object_ptr {
        void* raw = nullptr;
        std::uint64_t layout_id = 0;

        [[nodiscard]] bool valid() const noexcept { return raw != nullptr; }
        [[nodiscard]] explicit operator bool() const noexcept { return valid(); }
    };

    static_assert(sizeof(object_ptr) == sizeof(void*) + sizeof(std::uint64_t));

    // Returns true iff ptr.layout_id matches layout.layout_id.
    // Call at get_field/set_field/deallocate boundaries to detect ptr/layout mismatches.
    [[nodiscard]] inline bool layout_matches(const object_ptr& ptr,
                                             const object_layout& layout) noexcept {
        return ptr.layout_id == layout.layout_id;
    }

    // ---------------------------------------------------------------------------
    // runtime_value — unified boxed value that bridges JIT registers and the
    // dynamic runtime.  All alternatives are trivially copyable so the variant
    // itself is trivially copyable (and therefore constexpr-constructible).
    //
    //   i64  → std::int64_t       (integer / boolean-as-int from JIT)
    //   f64  → double             (floating-point)
    //   bool → bool               (explicit boolean lane)
    //   ptr  → void*              (raw heap / FFI pointer)
    //   obj  → object_ptr         (tagged MOP object pointer)
    // ---------------------------------------------------------------------------

    using runtime_value = std::variant<std::int64_t, double, bool, void*, object_ptr>;

    // dynamic_value — runtime_value with an opaque type_id header so generic
    // containers can carry heterogeneous values without knowing their C++ type.
    // type_id is user-defined (e.g. an index into a language type table).
    // Trivially copyable when all variant alternatives are (which they are here).
    struct dynamic_value {
        std::uint32_t type_id = 0;
        runtime_value value;

        constexpr dynamic_value() = default;

        constexpr dynamic_value(const std::uint32_t tid, runtime_value v)
            : type_id(tid), value(std::move(v)) {}
    };

    static_assert(std::is_trivially_copyable_v<object_ptr>);
    static_assert(std::is_trivially_copyable_v<runtime_value>);
    static_assert(std::is_trivially_copyable_v<dynamic_value>);

    // ---------------------------------------------------------------------------
    // language_plugin<Tag> — extension point for language-specific hooks
    // ---------------------------------------------------------------------------

    struct default_language_tag {};

    template <class LanguageTag = default_language_tag>
    struct language_plugin {
        using tag_type = LanguageTag;

        [[nodiscard]] static std::optional<std::expected<void*, mop_error>>
        resolve_field(const object_layout&, object_ptr, std::string_view) noexcept {
            return std::nullopt;
        }

        [[nodiscard]] static std::optional<std::expected<std::int64_t, mop_error>>
        resolve_method(const object_layout&, object_ptr,
                       std::uint64_t, std::span<std::int64_t const>) noexcept {
            return std::nullopt;
        }

        static void on_allocate(object_ptr, const object_layout&) noexcept {}

        static void on_deallocate(object_ptr, const object_layout&) noexcept {}
    };

    // ---------------------------------------------------------------------------
    // ObjectManager concept
    // ---------------------------------------------------------------------------

    template <class M>
    concept ObjectManager =
        requires(M mgr, object_layout const& layout, object_ptr ptr,
                 std::string_view field_name, std::uint64_t method_id,
                 std::span<std::int64_t const> args) {
            { mgr.allocate_instance(layout) } -> std::same_as<std::expected<object_ptr, mop_error>>;
            { mgr.deallocate_instance(ptr, layout) } -> std::same_as<std::expected<void, mop_error>>;
            { mgr.get_field(ptr, layout, field_name) } -> std::same_as<std::expected<void*, mop_error>>;
            {
                mgr.invoke_method(ptr, layout, method_id, args)
            } -> std::same_as<std::expected<std::int64_t, mop_error>>;
        };

    // ---------------------------------------------------------------------------
    // layout_registry
    // ---------------------------------------------------------------------------

    class layout_registry {
    public:
        bool register_layout(object_layout lay) {
            const auto id = lay.layout_id;
            return registry_.emplace(id, std::move(lay)).second;
        }

        [[nodiscard]] const object_layout* find(const std::uint64_t id) const noexcept {
            const auto it = registry_.find(id);
            return it == registry_.end() ? nullptr : &it->second;
        }

        [[nodiscard]] std::expected<const object_layout*, mop_error>
        get(const std::uint64_t id) const noexcept {
            if (const auto* p = find(id)) return p;
            return std::unexpected(mop_error::bad_layout("layout_id not registered"));
        }

        [[nodiscard]] std::size_t size() const noexcept { return registry_.size(); }

    private:
        std::unordered_map<std::uint64_t, object_layout> registry_;
    };

    // ---------------------------------------------------------------------------
    // MOP MIR opcodes
    // ---------------------------------------------------------------------------

    namespace opcodes {
        inline constexpr std::string_view mop_alloc = "mop.alloc";
        inline constexpr std::string_view mop_get_field = "mop.get_field";
        inline constexpr std::string_view mop_invoke_method = "mop.invoke_method";
        inline constexpr std::string_view mop_dealloc = "mop.dealloc";

        [[nodiscard]] inline lithe::codegen::operation_id make_mop_op(const std::string_view name) {
            return {
                .domain = "lithe.mop", .name = std::string(name),
                .stable_hash = std::hash<std::string_view>{}(name)
            };
        }
    } // namespace opcodes

    // MIR bridge helpers
    [[nodiscard]] inline lithe::codegen::allocated_instruction
    make_mop_alloc_instr(const std::uint32_t id,
                         lithe::codegen::allocated_operand dest,
                         const std::uint64_t layout_id) {
        using namespace lithe::codegen;
        allocated_instruction i;
        i.id = id;
        i.op = opcode::indirect_call;
        i.defs = {dest};
        i.uses = {allocated_operand::as_i64(static_cast<std::int64_t>(layout_id))};
        i.abstract_operation = opcodes::make_mop_op(opcodes::mop_alloc);
        return i;
    }

    [[nodiscard]] inline lithe::codegen::allocated_instruction
    make_mop_get_field_instr(std::uint32_t id,
                             lithe::codegen::allocated_operand dest,
                             lithe::codegen::allocated_operand src,
                             std::uint64_t field_name_hash) {
        using namespace lithe::codegen;
        allocated_instruction i;
        i.id = id;
        i.op = opcode::indirect_call;
        i.defs = {dest};
        i.uses = {src, allocated_operand::as_i64(static_cast<std::int64_t>(field_name_hash))};
        i.abstract_operation = opcodes::make_mop_op(opcodes::mop_get_field);
        return i;
    }

    [[nodiscard]] inline lithe::codegen::allocated_instruction
    make_mop_invoke_method_instr(std::uint32_t id,
                                 lithe::codegen::allocated_operand ret,
                                 lithe::codegen::allocated_operand obj,
                                 std::uint64_t method_id,
                                 std::vector<lithe::codegen::allocated_operand> args = {}) {
        using namespace lithe::codegen;
        allocated_instruction i;
        i.id = id;
        i.op = opcode::indirect_call;
        i.defs = {ret};
        i.uses.push_back(obj);
        i.uses.push_back(allocated_operand::as_i64(static_cast<std::int64_t>(method_id)));
        for (auto& a : args) i.uses.push_back(a);
        i.abstract_operation = opcodes::make_mop_op(opcodes::mop_invoke_method);
        return i;
    }

    // ---------------------------------------------------------------------------
    // default_object_manager<Tag>
    // ---------------------------------------------------------------------------

    template <class LanguageTag = default_language_tag>
    class default_object_manager {
    public:
        using plugin = language_plugin<LanguageTag>;
        using method_fn = std::function<
            std::expected<std::int64_t, mop_error>(object_ptr, std::span<std::int64_t const>)>;

        [[nodiscard]] std::expected<object_ptr, mop_error>
        allocate_instance(const object_layout& layout) {
            if (!layout.is_valid())
                return std::unexpected(mop_error::bad_layout(layout.type_name));

            void* raw = nullptr;
            if (layout.is_zero_sized()) {
                raw = zst_sentinel();
            }
            else {
#if __cpp_aligned_new >= 201606
                raw = ::operator new(layout.size_bytes,
                                     static_cast<std::align_val_t>(layout.alignment),
                                     std::nothrow);
#else
                const std::size_t sz =
                    ((layout.size_bytes + layout.alignment - 1) / layout.alignment)
                    * layout.alignment;
                raw = std::aligned_alloc(layout.alignment, sz);
#endif
                if (!raw) return std::unexpected(mop_error::oom(layout.type_name));
            }
            object_ptr ptr{raw, layout.layout_id};
            plugin::on_allocate(ptr, layout);
            return ptr;
        }

        // Returns success or a mop_error (e.g. bad_layout) so callers CAN observe a
        // mismatched-layout deallocation; not [[nodiscard]] since fire-and-forget
        // deallocation is a legitimate pattern.
        std::expected<void, mop_error>
        deallocate_instance(object_ptr ptr, const object_layout& layout) noexcept {
            if (!ptr.valid()) return std::unexpected(mop_error::null_ptr());
            if (!layout_matches(ptr, layout))
                return std::unexpected(mop_error::bad_layout("layout_id mismatch on deallocate"));
            plugin::on_deallocate(ptr, layout);
            if (layout.is_zero_sized()) return {};
#if __cpp_aligned_new >= 201606
            ::operator delete(ptr.raw, layout.size_bytes,
                              static_cast<std::align_val_t>(layout.alignment));
#else
            std::free(ptr.raw);
#endif
            return {};
        }

        [[nodiscard]] std::expected<void*, mop_error>
        get_field(object_ptr ptr, const object_layout& layout, std::string_view field_name) {
            if (!ptr.valid()) return std::unexpected(mop_error::null_ptr());
            if (!layout_matches(ptr, layout))
                return std::unexpected(mop_error::bad_layout("layout_id mismatch"));
            if (auto r = plugin::resolve_field(layout, ptr, field_name)) return *r;
            const auto* fd = layout.find_field(field_name);
            if (!fd) return std::unexpected(mop_error::no_field(field_name));
            return static_cast<std::byte*>(ptr.raw) + fd->byte_offset;
        }

        [[nodiscard]] std::expected<std::int64_t, mop_error>
        invoke_method(object_ptr ptr, const object_layout& layout,
                      std::uint64_t method_id, std::span<std::int64_t const> args) {
            if (!ptr.valid()) return std::unexpected(mop_error::null_ptr());
            if (!layout_matches(ptr, layout))
                return std::unexpected(mop_error::bad_layout("layout_id mismatch"));
            if (auto r = plugin::resolve_method(layout, ptr, method_id, args)) return *r;
            // Current Apple libc++ C++26 recursively instantiates
            // std::expected's constrained equality while comparing iterators
            // for containers whose mapped callback returns std::expected.
            // The registry uses direct node traversal to keep this boundary
            // independent of that library defect.
            for (auto* handler = method_handlers_.get(); handler != nullptr;
                 handler = handler->next.get()) {
                if (handler->method_id == method_id)
                    return handler->callback(ptr, args);
            }
            const auto it = layout.method_table.find(method_id);
            if (it == layout.method_table.end())
                return std::unexpected(mop_error::no_method(method_id));
            if (!it->second.fn_ptr)
                return std::unexpected(
                    mop_error{
                        mop_error_code::not_implemented,
                        "method " + it->second.name + " has no fn_ptr"
                    });
            return reinterpret_cast<method_fn_ptr>(it->second.fn_ptr)(
                ptr.raw, args.data(), static_cast<std::uint32_t>(args.size()));
        }

        void register_method_handler(const std::uint64_t method_id, method_fn fn) {
            for (auto* handler = method_handlers_.get(); handler != nullptr;
                 handler = handler->next.get()) {
                if (handler->method_id == method_id) {
                    handler->callback = std::move(fn);
                    return;
                }
            }
            method_handlers_ = std::make_unique<method_handler>(
                method_handler{method_id, std::move(fn), std::move(method_handlers_)});
        }

    private:
        struct method_handler {
            std::uint64_t method_id = 0;
            method_fn callback;
            std::unique_ptr<method_handler> next;
        };

        std::unique_ptr<method_handler> method_handlers_;

        [[nodiscard]] static void* zst_sentinel() noexcept {
            alignas(std::max_align_t) static char s[1]{};
            return s;
        }
    };

    static_assert(ObjectManager<default_object_manager<>>);

    // ---------------------------------------------------------------------------
    // injecting_object_manager<Tag, AllocFn, DeallocFn>
    // ---------------------------------------------------------------------------

    template <
        class LanguageTag = default_language_tag,
        class AllocFn = std::function<void*(std::size_t, std::size_t)>



    ,
    class DeallocFn
    =
    std::function<void(void *, std::size_t, std::size_t)>
    >
    class injecting_object_manager {
    public:
        using inner = default_object_manager<LanguageTag>;
        using plugin = language_plugin<LanguageTag>;
        using method_fn = typename inner::method_fn;

        explicit injecting_object_manager(AllocFn alloc, DeallocFn dealloc)
            : alloc_(std::move(alloc)), dealloc_(std::move(dealloc)) {}

        [[nodiscard]] std::expected<object_ptr, mop_error>
        allocate_instance(const object_layout& layout) {
            if (!layout.is_valid())
                return std::unexpected(mop_error::bad_layout(layout.type_name));
            void* raw = nullptr;
            if (layout.is_zero_sized()) {
                alignas(std::max_align_t) static char zst[1]{};
                raw = zst;
            }
            else {
                raw = alloc_(layout.size_bytes, layout.alignment);
                if (!raw) return std::unexpected(mop_error::oom(layout.type_name));
            }
            object_ptr ptr{raw, layout.layout_id};
            plugin::on_allocate(ptr, layout);
            return ptr;
        }

        std::expected<void, mop_error>
        deallocate_instance(object_ptr ptr, const object_layout& layout) noexcept {
            if (!ptr.valid()) return std::unexpected(mop_error::null_ptr());
            if (!layout_matches(ptr, layout))
                return std::unexpected(mop_error::bad_layout("layout_id mismatch on deallocate"));
            plugin::on_deallocate(ptr, layout);
            if (layout.is_zero_sized()) return {};
            dealloc_(ptr.raw, layout.size_bytes, layout.alignment);
            return {};
        }

        [[nodiscard]] std::expected<void*, mop_error>
        get_field(object_ptr ptr, const object_layout& layout, std::string_view name) {
            return inner_.get_field(ptr, layout, name);
        }

        [[nodiscard]] std::expected<std::int64_t, mop_error>
        invoke_method(object_ptr ptr, const object_layout& layout,
                      std::uint64_t method_id, std::span<std::int64_t const> args) {
            return inner_.invoke_method(ptr, layout, method_id, args);
        }

        void register_method_handler(std::uint64_t method_id, method_fn fn) {
            inner_.register_method_handler(method_id, std::move(fn));
        }

    private:
        AllocFn alloc_;
        DeallocFn dealloc_;
        inner inner_;
    };

    static_assert(ObjectManager<injecting_object_manager<>>);

    // ---------------------------------------------------------------------------
    // oom_test_object_manager
    // ---------------------------------------------------------------------------

    class oom_test_object_manager {
    public:
        explicit oom_test_object_manager(const std::size_t budget) : budget_(budget) {}

        [[nodiscard]] std::expected<object_ptr, mop_error>
        allocate_instance(const object_layout& layout) {
            if (budget_ == 0)
                return std::unexpected(mop_error{
                    mop_error_code::out_of_memory,
                    "OOM budget exhausted"
                });
            --budget_;
            return inner_.allocate_instance(layout);
        }

        std::expected<void, mop_error>
        deallocate_instance(const object_ptr ptr, const object_layout& layout) noexcept {
            return inner_.deallocate_instance(ptr, layout);
        }

        [[nodiscard]] std::expected<void*, mop_error>
        get_field(const object_ptr ptr, const object_layout& layout, const std::string_view name) {
            return inner_.get_field(ptr, layout, name);
        }

        [[nodiscard]] std::expected<std::int64_t, mop_error>
        invoke_method(const object_ptr ptr, const object_layout& layout,
                      const std::uint64_t method_id, const std::span<std::int64_t const> args) {
            return inner_.invoke_method(ptr, layout, method_id, args);
        }

        [[nodiscard]] std::size_t remaining_budget() const noexcept { return budget_; }

    private:
        default_object_manager<> inner_;
        std::size_t budget_;
    };

    static_assert(ObjectManager<oom_test_object_manager>);

    // ---------------------------------------------------------------------------
    // mop_context — type-erased dispatch bundle for backend integration
    // ---------------------------------------------------------------------------

    struct mop_context {
        std::function<std::expected<object_ptr, mop_error>(std::uint64_t)> alloc_fn;
        std::function<void(object_ptr, std::uint64_t)> dealloc_fn;
        std::function<std::expected<void*, mop_error>(object_ptr, std::uint64_t, std::uint64_t)> get_field_fn;
        std::function<std::expected<std::int64_t, mop_error>(object_ptr, std::uint64_t, std::uint64_t,
                                                             std::span<std::int64_t const>)> invoke_fn;
        layout_registry* registry = nullptr;

        [[nodiscard]] bool valid() const noexcept {
            return alloc_fn && dealloc_fn && get_field_fn && invoke_fn;
        }
    };

    template <ObjectManager M>
    [[nodiscard]] mop_context make_mop_context(M& mgr, layout_registry& reg) {
        mop_context ctx;
        ctx.registry = &reg;

        ctx.alloc_fn = [&mgr, &reg](const std::uint64_t layout_id)
            -> std::expected<object_ptr, mop_error> {
                const auto* lay = reg.find(layout_id);
                if (!lay) return std::unexpected(mop_error::bad_layout("unknown layout_id"));
                return mgr.allocate_instance(*lay);
            };

        ctx.dealloc_fn = [&mgr, &reg](object_ptr ptr, const std::uint64_t layout_id) {
            const auto* lay = reg.find(layout_id);
            // The MOP dealloc MIR op has no error return channel; the manager API
            // now reports failure via std::expected, but this ABI callback is void,
            // so an unknown/invalid layout is intentionally a no-op here.
            if (lay && ptr.valid()) (void)mgr.deallocate_instance(ptr, *lay);
        };

        ctx.get_field_fn = [&mgr, &reg](object_ptr ptr, const std::uint64_t layout_id,
                                        const std::uint64_t field_hash)
            -> std::expected<void*, mop_error> {
                const auto* lay = reg.find(layout_id);
                if (!lay) return std::unexpected(mop_error::bad_layout("unknown layout_id"));
                for (const auto& [name, fd] : lay->field_map)
                    if (std::hash<std::string>{}(name) == field_hash)
                        return mgr.get_field(ptr, *lay, name);
                return std::unexpected(mop_error{
                    mop_error_code::field_not_found,
                    "no field with hash " + std::to_string(field_hash)
                });
            };

        ctx.invoke_fn = [&mgr, &reg](object_ptr ptr, const std::uint64_t layout_id,
                                     std::uint64_t method_id,
                                     std::span<std::int64_t const> args)
            -> std::expected<std::int64_t, mop_error> {
                const auto* lay = reg.find(layout_id);
                if (!lay) return std::unexpected(mop_error::bad_layout("unknown layout_id"));
                return mgr.invoke_method(ptr, *lay, method_id, args);
            };

        return ctx;
    }
} // namespace lithe::runtime::mop

// ============================================================================
// namespace lithe::runtime::safepoint
//
// Provides:
//   live_set           — flat vector of preg IDs holding GC roots at one PC
//   safepoint_record   — {instr_id, roots}
//   stack_map          — per-function sorted table of safepoint_records
//   stack_map_table    — multi-function registry; shared_mutex for safe reads
//   make_safepoint_op  — factory for the "lithe.safepoint" / "safepoint_tag" op
// ============================================================================

#include <algorithm>
#include <mutex>
#include <shared_mutex>

namespace lithe::runtime::safepoint {
    // ---------------------------------------------------------------------------
    // live_set — preg IDs of GC roots live at one program point
    // ---------------------------------------------------------------------------

    using live_set = std::vector<std::uint32_t>;

    // ---------------------------------------------------------------------------
    // safepoint_record
    // ---------------------------------------------------------------------------

    struct safepoint_record {
        std::uint32_t instr_id = 0;
        live_set roots;

        [[nodiscard]] bool operator<(const safepoint_record& o) const noexcept {
            return instr_id < o.instr_id;
        }
    };

    // ---------------------------------------------------------------------------
    // stack_map — entries sorted by instr_id for O(log n) lookup
    // ---------------------------------------------------------------------------

    struct stack_map {
        std::string fn_name;
        std::vector<safepoint_record> entries; // maintained sorted

        // Insert and keep sorted; merges roots if instr_id already present.
        void insert(safepoint_record rec) {
            auto it = std::lower_bound(entries.begin(), entries.end(), rec);
            if (it != entries.end() && it->instr_id == rec.instr_id) {
                // Merge roots (union, dedup).
                for (auto r : rec.roots) {
                    if (std::find(it->roots.begin(), it->roots.end(), r) == it->roots.end())
                        it->roots.push_back(r);
                }
            }
            else {
                entries.insert(it, std::move(rec));
            }
        }

        // O(log n) lookup by instr_id.
        [[nodiscard]] const safepoint_record* find(const std::uint32_t id) const noexcept {
            safepoint_record key;
            key.instr_id = id;
            auto it = std::lower_bound(entries.begin(), entries.end(), key);
            if (it == entries.end() || it->instr_id != id) return nullptr;
            return &*it;
        }

        [[nodiscard]] bool empty() const noexcept { return entries.empty(); }
    };

    // ---------------------------------------------------------------------------
    // stack_map_table — registry keyed by fn_name; thread-safe reads
    // ---------------------------------------------------------------------------

    class stack_map_table {
    public:
        // Register (or overwrite) a stack_map for fn_name.
        void register_map(stack_map sm) {
            std::unique_lock lock(mu_);
            table_[sm.fn_name] = std::move(sm);
        }

        // Thread-safe read: returns a copy so callers need no lock.
        [[nodiscard]] std::optional<stack_map> get(const std::string& fn_name) const {
            std::shared_lock lock(mu_);
            auto it = table_.find(fn_name);
            if (it == table_.end()) return std::nullopt;
            return it->second;
        }

        [[nodiscard]] bool contains(const std::string& fn_name) const {
            std::shared_lock lock(mu_);
            return table_.count(fn_name) != 0;
        }

        [[nodiscard]] std::size_t size() const {
            std::shared_lock lock(mu_);
            return table_.size();
        }

    private:
        mutable std::shared_mutex mu_;
        std::unordered_map<std::string, stack_map> table_;
    };

    // ---------------------------------------------------------------------------
    // MIR opcode helpers — "lithe.safepoint" / "safepoint_tag"
    // ---------------------------------------------------------------------------

    inline constexpr std::string_view safepoint_domain = "lithe.safepoint";
    inline constexpr std::string_view safepoint_tag_name = "safepoint_tag";

    [[nodiscard]] inline lithe::codegen::operation_id make_safepoint_op() {
        return {
            .domain = std::string(safepoint_domain),
            .name = std::string(safepoint_tag_name),
            .stable_hash = std::hash<std::string_view>{}(safepoint_tag_name),
        };
    }

    // Build a safepoint_tag allocated_instruction.
    // op      = indirect_call (metadata-only; backend emits no machine code)
    // uses    = one as_preg entry per live GC root
    [[nodiscard]] inline lithe::codegen::allocated_instruction
    make_safepoint_instr(const std::uint32_t id, const live_set& roots) {
        using namespace lithe::codegen;
        allocated_instruction i;
        i.id = id;
        i.op = opcode::indirect_call;
        i.abstract_operation = make_safepoint_op();
        i.uses.reserve(roots.size());
        for (const auto preg_id : roots) {
            preg p;
            p.id = static_cast<std::uint16_t>(preg_id);
            i.uses.push_back(allocated_operand::as_preg(p));
        }
        return i;
    }

    // ---------------------------------------------------------------------------
    // GarbageCollector concept
    //
    // Any GC implementation must provide root_scan(stack_map const&).
    // trigger_safepoint looks up the function's stack_map and calls root_scan.
    //
    // GC integration notes:
    //   • The asmjit_backend::register_stack_map() fires after emit() returns and
    //     populates the stack_map_table automatically — no backend changes needed.
    //   • safepoint_tag instructions in the MIR stream emit zero machine code on
    //     all backends (the domain check in the indirect_call arm skips them) so
    //     the tag is a true zero-overhead NOP for non-GC backends.
    // ---------------------------------------------------------------------------

    template <class GC>
    concept GarbageCollector = requires(GC gc, stack_map const& sm) {
        { gc.root_scan(sm) } -> std::same_as<void>;
    };

    // trigger_safepoint — look up fn_name in the table and invoke gc.root_scan.
    // noexcept: root_scan is required to be noexcept by convention; the lookup
    // is a shared-lock read that cannot throw.
    template <GarbageCollector GC>
    inline void trigger_safepoint(const std::string_view fn_name,
                                  stack_map_table const& table,
                                  GC& gc) noexcept {
        auto sm = table.get(std::string(fn_name));
        if (sm) gc.root_scan(*sm);
    }
} // namespace lithe::runtime::safepoint

// ============================================================================
// namespace lithe::runtime::unwind
//
// Provides:
//   ip_range              — [begin, end) PC interval
//   landing_pad           — target address + cleanup_flags
//   unwind_entry          — ip_range + landing_pad pair
//   unwind_table          — per-function flat sorted table, binary searchable
//   unwind_error          — error type for personality routine
//   unwind_registry       — global fn_name → unwind_table map; thread-safe reads
//   lithe_personality_routine — IP lookup returning landing_pad or unwind_error
//   MIR opcodes           — landing_pad_tag, unwind_region_begin, unwind_region_end
//   make_unwind_op        — operation_id factory for "lithe.unwind" domain
// ============================================================================

namespace lithe::runtime::unwind {
    // ---------------------------------------------------------------------------
    // ip_range — [begin, end) interval of program counter values
    // ---------------------------------------------------------------------------

    struct ip_range {
        uintptr_t begin = 0;
        uintptr_t end = 0;

        [[nodiscard]] bool contains(const uintptr_t ip) const noexcept {
            return ip >= begin && ip < end;
        }

        [[nodiscard]] bool operator<(const ip_range& o) const noexcept {
            return begin < o.begin;
        }
    };

    // ---------------------------------------------------------------------------
    // landing_pad — destination address for cleanup/catch handler
    // ---------------------------------------------------------------------------

    struct landing_pad {
        uintptr_t address = 0;
        uint32_t cleanup_flags = 0;
    };

    // ---------------------------------------------------------------------------
    // unwind_entry — one try-region with its handler
    // ---------------------------------------------------------------------------

    struct unwind_entry {
        ip_range range;
        landing_pad pad;

        [[nodiscard]] bool operator<(const unwind_entry& o) const noexcept {
            return range < o.range;
        }
    };

    // ---------------------------------------------------------------------------
    // unwind_error
    // ---------------------------------------------------------------------------

    enum class unwind_error_code : uint8_t {
        ip_not_found = 0,
        registry_empty,
    };

    struct unwind_error {
        unwind_error_code code = unwind_error_code::ip_not_found;
        std::string message;

        [[nodiscard]] static unwind_error not_found(uintptr_t ip) {
            return {
                unwind_error_code::ip_not_found,
                "no unwind entry for IP 0x" + [ip] {
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "%llx",
                                  static_cast<unsigned long long>(ip));
                    return std::string(buf);
                }()
            };
        }
    };

    // ---------------------------------------------------------------------------
    // unwind_table — flat vector of unwind_entry sorted by ip_range.begin
    // ---------------------------------------------------------------------------

    struct unwind_table {
        std::string fn_name;
        std::vector<unwind_entry> entries; // maintained sorted by range.begin

        // Insert an entry and keep the vector sorted.
        void insert(unwind_entry e) {
            auto it = std::lower_bound(entries.begin(), entries.end(), e);
            entries.insert(it, std::move(e));
        }

        // O(log n) search: find the entry whose ip_range contains ip.
        // Uses binary search on begin values, then validates containment.
        [[nodiscard]] const unwind_entry* find(const uintptr_t ip) const noexcept {
            if (entries.empty()) return nullptr;

            // Find first entry whose begin > ip, then step back.
            unwind_entry key;
            key.range.begin = ip;
            auto it = std::upper_bound(entries.begin(), entries.end(), key,
                                       [](const unwind_entry& k, const unwind_entry& e) {
                                           return k.range.begin < e.range.begin;
                                       });
            // Candidates are at [begin, it); walk backwards to find containment.
            while (it != entries.begin()) {
                --it;
                if (it->range.contains(ip)) return &*it;
                // If this entry's begin is already past ip, no earlier one can match.
                if (it->range.begin > ip) break;
            }
            return nullptr;
        }

        [[nodiscard]] bool empty() const noexcept { return entries.empty(); }
        [[nodiscard]] std::size_t size() const noexcept { return entries.size(); }
    };

    // ---------------------------------------------------------------------------
    // unwind_registry — maps fn_name → unwind_table; thread-safe reads
    // ---------------------------------------------------------------------------

    class unwind_registry {
    public:
        void register_table(unwind_table tbl) {
            std::unique_lock lock(mu_);
            map_[tbl.fn_name] = std::move(tbl);
        }

        // Thread-safe read — returns a copy so callers need no lock.
        [[nodiscard]] std::optional<unwind_table> get(const std::string& fn_name) const {
            std::shared_lock lock(mu_);
            auto it = map_.find(fn_name);
            if (it == map_.end()) return std::nullopt;
            return it->second;
        }

        [[nodiscard]] bool contains(const std::string& fn_name) const {
            std::shared_lock lock(mu_);
            return map_.count(fn_name) != 0;
        }

        [[nodiscard]] std::size_t size() const {
            std::shared_lock lock(mu_);
            return map_.size();
        }

        // Search all registered tables for the given IP.
        [[nodiscard]] const unwind_entry* find_entry(const uintptr_t ip) const noexcept {
            std::shared_lock lock(mu_);
            for (const auto& [_, tbl] : map_)
                if (const auto* e = tbl.find(ip)) return e;
            return nullptr;
        }

    private:
        mutable std::shared_mutex mu_;
        std::unordered_map<std::string, unwind_table> map_;
    };

    // ---------------------------------------------------------------------------
    // lithe_personality_routine
    //
    // Given a faulting instruction pointer, searches the registry for the
    // unwind_entry whose ip_range covers it and returns the landing_pad.
    //
    // Does NOT call setjmp/longjmp. The returned landing_pad.address is used
    // by the runtime dispatcher to redirect control (e.g. via computed goto
    // or platform unwinder integration).
    //
    // Platform notes:
    //   macOS ARM64 — compact unwind entries are registered externally via
    //                 __register_frame/__deregister_frame with DWARF FDEs when
    //                 emitting production code. This routine acts as the Lithe-
    //                 level lookup that the platform personality trampoline calls.
    //   Linux x86-64 — same pattern; integrate with _Unwind_RaiseException by
    //                  calling this from the language-specific personality fn.
    // ---------------------------------------------------------------------------

    [[nodiscard]] inline std::expected<landing_pad, unwind_error>
    lithe_personality_routine(const uintptr_t faulting_ip,
                              const unwind_registry& reg) noexcept {
        const unwind_entry* entry = reg.find_entry(faulting_ip);
        if (!entry)
            return std::unexpected(unwind_error::not_found(faulting_ip));
        return entry->pad;
    }

    // ---------------------------------------------------------------------------
    // MIR opcode constants and helpers — domain "lithe.unwind"
    // ---------------------------------------------------------------------------

    inline constexpr std::string_view unwind_domain = "lithe.unwind";
    inline constexpr std::string_view landing_pad_tag_name = "landing_pad_tag";
    inline constexpr std::string_view unwind_region_begin_name = "unwind_region_begin";
    inline constexpr std::string_view unwind_region_end_name = "unwind_region_end";

    // Factory for operation_id in the "lithe.unwind" domain — parallel to make_mop_op.
    [[nodiscard]] inline lithe::codegen::operation_id
    make_unwind_op(const std::string_view name) {
        return {
            .domain = std::string(unwind_domain),
            .name = std::string(name),
            .stable_hash = std::hash<std::string_view>{}(name),
        };
    }

    // landing_pad_tag instruction:
    //   op    = indirect_call (metadata-only; backend emits no machine code)
    //   uses[0] = immediate cleanup_flags (uint32_t cast to int64_t)
    [[nodiscard]] inline lithe::codegen::allocated_instruction
    make_landing_pad_tag_instr(const std::uint32_t id, const std::uint32_t cleanup_flags) {
        using namespace lithe::codegen;
        allocated_instruction i;
        i.id = id;
        i.op = opcode::indirect_call;
        i.abstract_operation = make_unwind_op(landing_pad_tag_name);
        i.uses.push_back(allocated_operand::as_i64(
            cleanup_flags));
        return i;
    }

    // unwind_region_begin instruction: brackets the start of a try-region.
    [[nodiscard]] inline lithe::codegen::allocated_instruction
    make_unwind_region_begin_instr(const std::uint32_t id) {
        using namespace lithe::codegen;
        allocated_instruction i;
        i.id = id;
        i.op = opcode::indirect_call;
        i.abstract_operation = make_unwind_op(unwind_region_begin_name);
        return i;
    }

    // unwind_region_end instruction: brackets the end of a try-region.
    [[nodiscard]] inline lithe::codegen::allocated_instruction
    make_unwind_region_end_instr(const std::uint32_t id) {
        using namespace lithe::codegen;
        allocated_instruction i;
        i.id = id;
        i.op = opcode::indirect_call;
        i.abstract_operation = make_unwind_op(unwind_region_end_name);
        return i;
    }
} // namespace lithe::runtime::unwind

// =============================================================================
// Dynamic linker & symbol registry
// namespace lithe::runtime::linker
//
// Wraps symtab::SymbolTable and symtab::NamespaceIndex into a linker_context
// that the asmjit_backend can attach to for eager/lazy symbol resolution.
//
// Design
// ------
//   linker_context  — thin adapter; may own or borrow a SymbolTable.
//                     All symbol names are interned by the inner SymbolTable so
//                     every returned string_view is stable for the process lifetime.
//   stub_patch      — records a JIT call site that was compiled against an
//                     unresolved symbol; target_name is an interned pointer.
//   resolve_symbol_stub — C-linkage trampoline invoked on the first call through
//                     a lazy stub; patches the call-site slot and returns the
//                     real address.
//
// MIR opcode convention
// ---------------------
//   domain : "lithe.linker"
//   name   : "external_call_tag"
//   op     : indirect_call
//   uses[0]: allocated_operand::as_symbol(name)   — the symbol name string
//   uses[1..N]: call arguments
//   defs[0]: return value preg
// =============================================================================

#include "containers/symbol/SymbolTable.hpp"

#include <memory>
#include <mutex>
#include <vector>

namespace lithe::runtime::linker {
    // ---------------------------------------------------------------------------
    // Domain / opcode name constants
    // ---------------------------------------------------------------------------

    inline constexpr std::string_view linker_domain = "lithe.linker";
    inline constexpr std::string_view external_call_tag_name = "external_call_tag";

    // ---------------------------------------------------------------------------
    // stub_patch — one per unresolved call site emitted by the backend.
    //   call_site_offset  : byte offset from the JIT function's base address to the
    //                       slot holding the callee pointer (platform-specific).
    //   target_name       : interned string_view — stable, no copy needed on hot path.
    // ---------------------------------------------------------------------------

    struct stub_patch {
        std::uintptr_t call_site_offset{0};
        std::string_view target_name{}; // points into SymbolTable's InternPool
    };

    // ---------------------------------------------------------------------------
    // linker_context
    //   Owns or borrows a symtab::SymbolTable<>.  Always owns a NamespaceIndex.
    //   Thread-safe: all mutation is delegated to SymbolTable (shared_mutex inside).
    // ---------------------------------------------------------------------------

    class linker_context {
    public:
        // Default-construct: owns its own SymbolTable.
        linker_context()
            : owned_table_(std::make_unique<symtab::SymbolTable<>>())
              , table_(owned_table_.get()) {}

        // Borrow an existing SymbolTable (caller must keep it alive).
        explicit linker_context(symtab::SymbolTable<>* borrowed) noexcept
            : table_(borrowed) {}

        linker_context(const linker_context&) = delete;

        linker_context& operator=(const linker_context&) = delete;

        linker_context(linker_context&&) = delete;

        linker_context& operator=(linker_context&&) = delete;

        // ---- symbol registration -----------------------------------------------

        [[nodiscard]] symtab::SymResult<symtab::SymbolId>
        register_symbol(const std::string_view name, void* addr,
                        const std::uint32_t version = 0) {
            auto r = table_->register_symbol(name, addr, version);
            if (r.has_value()) {
                // Mirror into the namespace index so enumerate() works.
                auto entry_r = table_->lookup_entry(name);
                if (entry_r.has_value()) {
                    // NamespaceIndex needs a stable symbol_entry*; use a local
                    // snapshot stored in our side-table.
                    std::unique_lock lk(entries_mtx_);
                    entries_.push_back(entry_r.value());
                    ns_index_.insert(&entries_.back());
                }
            }
            return r;
        }

        // ---- resolution (hot path) ---------------------------------------------

        [[nodiscard]] void* resolve(const std::string_view name) const noexcept {
            return table_->resolve(name);
        }

        [[nodiscard]] void* resolve_versioned(const std::string_view name,
                                              const std::uint32_t version) const noexcept {
            return table_->resolve_versioned(name, version);
        }

        // ---- namespace index access --------------------------------------------

        [[nodiscard]] symtab::NamespaceIndex& index() noexcept { return ns_index_; }
        [[nodiscard]] const symtab::NamespaceIndex& index() const noexcept { return ns_index_; }

        // ---- raw table access (for bulk operations / snapshot-rollback) --------

        [[nodiscard]] symtab::SymbolTable<>& table() noexcept { return *table_; }
        [[nodiscard]] const symtab::SymbolTable<>& table() const noexcept { return *table_; }

        // ---- stub patch list ---------------------------------------------------

        void record_stub_patch(const std::uintptr_t call_site_offset,
                               const std::string_view interned_name) {
            std::unique_lock lk(patches_mtx_);
            patches_.push_back({call_site_offset, interned_name});
        }

        // Apply all recorded stub_patches: for each entry whose symbol is now
        // resolved, write the real address into the call-site slot and remove
        // the entry.  Returns the count of patches applied.
        std::size_t apply_patches(void* jit_base) {
            std::unique_lock lk(patches_mtx_);
            std::size_t applied = 0;
            for (auto it = patches_.begin(); it != patches_.end();) {
                void* addr = table_->resolve(it->target_name);
                if (addr) {
                    auto* slot = reinterpret_cast<void**>(
                        reinterpret_cast<std::uintptr_t>(jit_base) +
                        it->call_site_offset);
                    *slot = addr;
                    it = patches_.erase(it);
                    ++applied;
                }
                else {
                    ++it;
                }
            }
            return applied;
        }

        [[nodiscard]] std::size_t pending_patches() const {
            std::unique_lock lk(patches_mtx_);
            return patches_.size();
        }

    private:
        // Owned table (null when borrowing).
        std::unique_ptr<symtab::SymbolTable<>> owned_table_;
        // Non-owning pointer to the active table (always valid).
        symtab::SymbolTable<>* table_{nullptr};

        // Namespace index.
        symtab::NamespaceIndex ns_index_;

        // Stable symbol_entry copies for the namespace index.
        // std::deque node addresses are stable across growth — required invariant.
        mutable std::mutex entries_mtx_;
        std::deque<symtab::symbol_entry> entries_;

        // Pending stub patches.
        mutable std::mutex patches_mtx_;
        std::vector<stub_patch> patches_;
    };

    // ---------------------------------------------------------------------------
    // resolve_symbol_stub — C-linkage trampoline for lazy binding.
    //
    // The JIT trampoline passes the symbol name (a stable interned char*)
    // and a pointer to the call-site slot (a void** inside the JIT code page).
    // This function:
    //   1. Resolves the symbol via the thread-local linker_context pointer.
    //   2. Writes the resolved address into the slot so future calls bypass stub.
    //   3. Returns the address to the trampoline for a tail call.
    //
    // Global linker_context pointer: set once before JIT code runs; reads are
    // racy but the pointer itself is stable (written before JIT starts).
    // ---------------------------------------------------------------------------

    // Thread-local pointer to the active linker_context. Set by the JIT host
    // before invoking any JIT-compiled function that uses lazy linking.
    inline thread_local linker_context* tl_linker_ctx = nullptr;

    // Set the thread-local linker context for the calling thread.
    inline void set_thread_linker_context(linker_context* ctx) noexcept {
        tl_linker_ctx = ctx;
    }

    extern "C" inline void* resolve_symbol_stub(const char* name,
                                                void** call_site_slot) {
        // Hot path: no allocation, no exception.
        if (!tl_linker_ctx) return nullptr;

        void* addr = tl_linker_ctx->resolve(std::string_view{name});
        if (addr && call_site_slot) {
            // Patch the slot so the next call is direct (no stub overhead).
            *call_site_slot = addr;
        }
        return addr;
    }

    // ---------------------------------------------------------------------------
    // MIR opcode factory
    // ---------------------------------------------------------------------------

    [[nodiscard]] inline lithe::codegen::operation_id
    make_linker_op(const std::string_view name) {
        return {
            .domain = std::string(linker_domain),
            .name = std::string(name),
            .stable_hash = std::hash<std::string_view>{}(name),
        };
    }

    // external_call_tag instruction:
    //   op      = indirect_call
    //   uses[0] = as_symbol(target_name)   — the extern symbol name
    //   uses[1..N] = call arguments
    //   defs[0] = return value preg
    [[nodiscard]] inline lithe::codegen::allocated_instruction
    make_external_call_tag_instr(const std::uint32_t id, std::string target_name,
                                 std::vector<lithe::codegen::allocated_operand> args = {},
                                 std::optional<lithe::codegen::allocated_operand> ret_def = {}) {
        using namespace lithe::codegen;
        allocated_instruction i;
        i.id = id;
        i.op = opcode::indirect_call;
        i.abstract_operation = make_linker_op(external_call_tag_name);
        i.uses.push_back(allocated_operand::as_symbol(std::move(target_name)));
        for (auto& a : args)
            i.uses.push_back(std::move(a));
        if (ret_def.has_value())
            i.defs.push_back(std::move(*ret_def));
        return i;
    }
} // namespace lithe::runtime::linker

// ============================================================================
// namespace lithe::runtime
//
// ExecutionSandbox — budgeted execution context for JIT-compiled functions.
//   fuel_counter  : decremented at every function entry and loop back-edge.
//                   If it reaches zero the JIT-compiled trap fires.
//   max_memory    : byte cap enforced by the allocator (checked in mop_context).
// ============================================================================

namespace lithe::runtime {
    struct ExecutionSandbox {
        std::int64_t fuel_counter = 0;
        std::size_t max_memory = 0;
    };
} // namespace lithe::runtime

// fuel_check_tag MIR opcode helpers (domain "lithe.fuel")
namespace lithe::runtime::fuel {
    inline constexpr std::string_view fuel_domain = "lithe.fuel";
    inline constexpr std::string_view fuel_check_tag_name = "fuel_check_tag";

    [[nodiscard]] inline lithe::codegen::operation_id make_fuel_op() {
        return {
            .domain = std::string(fuel_domain),
            .name = std::string(fuel_check_tag_name),
            .stable_hash = std::hash<std::string_view>{}(fuel_check_tag_name),
        };
    }

    // fuel_check_tag instruction:
    //   op   = indirect_call (no machine code if sandbox is null)
    //   uses = empty (sandbox pointer is a backend member, not an operand)
    [[nodiscard]] inline lithe::codegen::allocated_instruction
    make_fuel_check_instr(const std::uint32_t id) {
        using namespace lithe::codegen;
        allocated_instruction i;
        i.id = id;
        i.op = opcode::indirect_call;
        i.abstract_operation = make_fuel_op();
        return i;
    }
} // namespace lithe::runtime::fuel

// ============================================================================
// namespace lithe::runtime::ffi
//
// Native Binding (FFI) layer — bridges JIT runtime_value to C ABI calls.
//
//   native_proxy            — describes a foreign function: pointer, arity,
//                             and per-argument/return type hints for marshalling.
//   marshal_to_native()     — runtime_value → int64_t for ABI placement.
//   unmarshal_from_native() — int64_t raw result → runtime_value given type hint.
//
// Type hint encoding (uint32_t, user-defined; common convention):
//   0 = i64, 1 = f64, 2 = bool, 3 = ptr, 4 = object_ptr
// ============================================================================

namespace lithe::runtime::ffi {
    // Type hint constants (convention — users may extend beyond 4).
    inline constexpr std::uint32_t type_hint_i64 = 0;
    inline constexpr std::uint32_t type_hint_f64 = 1;
    inline constexpr std::uint32_t type_hint_bool = 2;
    inline constexpr std::uint32_t type_hint_ptr = 3;
    inline constexpr std::uint32_t type_hint_obj = 4;
    inline constexpr std::uint32_t type_hint_func = 5;
    inline constexpr std::uint32_t type_hint_void = 6;

    // marshal_to_native — flatten a runtime_value to a 64-bit integer for
    // placement into an ABI register.  Branchless via variant index dispatch.
    [[nodiscard]] inline std::int64_t
    marshal_to_native(mop::runtime_value const& v) noexcept {
        return std::visit([](auto const& x) -> std::int64_t {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, std::int64_t>)
                return x;
            else if constexpr (std::is_same_v<T, double>) {
                std::int64_t bits;
                static_assert(sizeof(bits) == sizeof(x));
                std::memcpy(&bits, &x, sizeof(bits));
                return bits;
            }
            else if constexpr (std::is_same_v<T, bool>)
                return x ? 1 : 0;
            else if constexpr (std::is_same_v<T, void*>)
                return static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(x));
            else {
                // mop::object_ptr
                return static_cast<std::int64_t>(
                    reinterpret_cast<std::uintptr_t>(x.raw));
            }
        }, v);
    }

    // unmarshal_from_native — reconstruct a runtime_value from a raw int64 result
    // using the caller-supplied type hint.
    [[nodiscard]] inline mop::runtime_value
    unmarshal_from_native(std::int64_t raw, const std::uint32_t type_hint) noexcept {
        switch (type_hint) {
        case type_hint_f64: {
            double d;
            static_assert(sizeof(d) == sizeof(raw));
            std::memcpy(&d, &raw, sizeof(d));
            return d;
        }
        case type_hint_bool:
            return raw != 0;
        case type_hint_ptr:
            return reinterpret_cast<void*>(static_cast<std::uintptr_t>(raw));
        case type_hint_obj:
            return mop::object_ptr{
                reinterpret_cast<void*>(static_cast<std::uintptr_t>(raw)), 0
            };
        default: // type_hint_i64 and unknown → i64
            return raw;
        }
    }

    // native_proxy — pairs a foreign entry address with its arity and type hints
    // so the backend can build the correct ABI call signature. Bound C++
    // callables additionally expose a typed trampoline for the interpreter
    // bridge; unlike fn_ptr it is never recovered through a function-pointer
    // reinterpret_cast.
    struct native_proxy {
        using trampoline_type = std::int64_t(*)(void*, std::int64_t const*, std::uint32_t) noexcept;

        // Raw address for a machine-code backend that emits a direct foreign
        // call. It remains separate from trampoline because a C++ function
        // pointer is not portably representable as void*.
        void* fn_ptr = nullptr;
        trampoline_type trampoline = nullptr;
        std::uint8_t arity = 0;
        std::uint32_t ret_type = type_hint_i64;
        std::array<std::uint32_t, 8> arg_types{};

        [[nodiscard]] bool valid() const noexcept {
            return fn_ptr != nullptr || trampoline != nullptr;
        }
    };
} // namespace lithe::runtime::ffi

// ============================================================================
// namespace lithe::runtime::ffi::binding
//
// Native Binding API — compile-time type-erased wrappers for C++ callables.
//
//   bind_native_function(fn)  — inspects the callable's signature via template
//                               traits, maps C++ types to ffi type_hint_* values,
//                               builds a trampoline that accepts a raw int64_t[]
//                               register array, unmarshals each argument, calls fn,
//                               and marshals the result back to int64_t.
//
// Returns std::expected<native_proxy, mop_error> so the caller can handle:
//   • arity > 8          → type_mismatch error
//   • unsupported type   → type_mismatch error
//
// Constraints:
//   • No RTTI, no heap allocation in the binding machinery itself.
//   • Trampoline is a non-capturing lambda stored as a raw function pointer;
//     the bound callable must be trivially copyable or a plain function pointer.
//   • Supports: int8/16/32/64_t, uint8/16/32/64_t, float, double, bool, T* (any).
// ============================================================================

#include <type_traits>

namespace lithe::runtime::ffi::binding {
    // -------------------------------------------------------------------------
    // cpp_type_to_hint<T> — compile-time mapping from C++ type → type_hint_*
    // -------------------------------------------------------------------------

    namespace detail {
        template <class T>
        struct cpp_type_to_hint;

        // integers → i64
        template <>
        struct cpp_type_to_hint<std::int8_t> {
            static constexpr std::uint32_t value = type_hint_i64;
        };

        template <>
        struct cpp_type_to_hint<std::int16_t> {
            static constexpr std::uint32_t value = type_hint_i64;
        };

        template <>
        struct cpp_type_to_hint<std::int32_t> {
            static constexpr std::uint32_t value = type_hint_i64;
        };

        template <>
        struct cpp_type_to_hint<std::int64_t> {
            static constexpr std::uint32_t value = type_hint_i64;
        };

        template <>
        struct cpp_type_to_hint<std::uint8_t> {
            static constexpr std::uint32_t value = type_hint_i64;
        };

        template <>
        struct cpp_type_to_hint<std::uint16_t> {
            static constexpr std::uint32_t value = type_hint_i64;
        };

        template <>
        struct cpp_type_to_hint<std::uint32_t> {
            static constexpr std::uint32_t value = type_hint_i64;
        };

        template <>
        struct cpp_type_to_hint<std::uint64_t> {
            static constexpr std::uint32_t value = type_hint_i64;
        };

        // floats
        template <>
        struct cpp_type_to_hint<float> {
            static constexpr std::uint32_t value = type_hint_f64;
        };

        template <>
        struct cpp_type_to_hint<double> {
            static constexpr std::uint32_t value = type_hint_f64;
        };

        // bool
        template <>
        struct cpp_type_to_hint<bool> {
            static constexpr std::uint32_t value = type_hint_bool;
        };

        // any pointer
        template <class T>
        struct cpp_type_to_hint<T*> {
            static constexpr std::uint32_t value = type_hint_ptr;
        };

        // void* specifically (also covered by T* above, kept for clarity)
        template <>
        struct cpp_type_to_hint<void*> {
            static constexpr std::uint32_t value = type_hint_ptr;
        };

        // is_supported_type — true if cpp_type_to_hint is defined for T (decay'd)
        template <class T, class = void>
        struct is_supported_type : std::false_type {};

        template <class T>
        struct is_supported_type<T, std::void_t<decltype(cpp_type_to_hint<std::decay_t<T>>::value)>>
            : std::true_type {};

        template <class T>
        inline constexpr bool is_supported_type_v = is_supported_type<T>::value;

        // ---- callable_traits — extract return type + parameter pack -----------

        // is_noexcept_fn: true only for noexcept function/member-function pointers
        // and noexcept callable objects (stateless lambdas).
        // Trampolines are noexcept; allowing throwing callables would invoke
        // std::terminate silently across the noexcept boundary.
        template <class F>
        struct is_noexcept_fn : std::false_type {};

        template <class R, class... Args>
        struct is_noexcept_fn<R(*)(Args...) noexcept> : std::true_type {};

        template <class C, class R, class... Args>
        struct is_noexcept_fn<R(C::*)(Args...) const noexcept> : std::true_type {};

        template <class C, class R, class... Args>
        struct is_noexcept_fn<R(C::*)(Args...) noexcept> : std::true_type {};

        // For callable objects (stateless lambdas): check if &F::operator() is noexcept.
        template <class F>
            requires (!std::is_pointer_v<F> && !std::is_member_function_pointer_v<F>)
        struct is_noexcept_fn<F>
            : is_noexcept_fn<decltype(&std::decay_t<F>::operator())> {};

        template <class F, class = void>
        struct callable_traits : callable_traits<decltype(&std::decay_t<F>::operator())> {};

        // Free / static function pointer (non-noexcept)
        template <class R, class... Args>
        struct callable_traits<R(*)(Args...), void> {
            using return_type = R;
            using param_types = std::tuple<Args...>;
            static constexpr std::size_t arity = sizeof...(Args);
        };

        // Free / static function pointer (noexcept)
        template <class R, class... Args>
        struct callable_traits<R(*)(Args...) noexcept, void> {
            using return_type = R;
            using param_types = std::tuple<Args...>;
            static constexpr std::size_t arity = sizeof...(Args);
        };

        // Const member (lambda operator(), non-noexcept)
        template <class C, class R, class... Args>
        struct callable_traits<R(C::*)(Args...) const, void> {
            using return_type = R;
            using param_types = std::tuple<Args...>;
            static constexpr std::size_t arity = sizeof...(Args);
        };

        // Const member (lambda operator(), noexcept)
        template <class C, class R, class... Args>
        struct callable_traits<R(C::*)(Args...) const noexcept, void> {
            using return_type = R;
            using param_types = std::tuple<Args...>;
            static constexpr std::size_t arity = sizeof...(Args);
        };

        // Non-const member (non-noexcept)
        template <class C, class R, class... Args>
        struct callable_traits<R(C::*)(Args...), void> {
            using return_type = R;
            using param_types = std::tuple<Args...>;
            static constexpr std::size_t arity = sizeof...(Args);
        };

        // Non-const member (noexcept)
        template <class C, class R, class... Args>
        struct callable_traits<R(C::*)(Args...) noexcept, void> {
            using return_type = R;
            using param_types = std::tuple<Args...>;
            static constexpr std::size_t arity = sizeof...(Args);
        };

        // ---- unmarshal_arg<T> — int64_t → T bit-safely -----------------------

        template <class T>
        [[nodiscard]] T unmarshal_arg(std::int64_t raw) noexcept {
            using D = std::decay_t<T>;
            if constexpr (std::is_same_v<D, double>) {
                double v;
                std::memcpy(&v, &raw, sizeof(v));
                return v;
            }
            else if constexpr (std::is_same_v<D, float>) {
                float v;
                std::int32_t bits = static_cast<std::int32_t>(raw);
                std::memcpy(&v, &bits, sizeof(v));
                return v;
            }
            else if constexpr (std::is_same_v<D, bool>) {
                return raw != 0;
            }
            else if constexpr (std::is_pointer_v<D>) {
                return reinterpret_cast<D>(static_cast<std::uintptr_t>(raw));
            }
            else {
                // integral: sign-preserving static_cast
                return static_cast<D>(raw);
            }
        }

        // ---- marshal_result<R> — R → int64_t ---------------------------------

        template <class R>
        [[nodiscard]] std::int64_t marshal_result(R result) noexcept {
            using D = std::decay_t<R>;
            if constexpr (std::is_same_v<D, double>) {
                std::int64_t bits;
                std::memcpy(&bits, &result, sizeof(bits));
                return bits;
            }
            else if constexpr (std::is_same_v<D, float>) {
                std::int32_t bits;
                std::memcpy(&bits, &result, sizeof(bits));
                return static_cast<std::int64_t>(bits);
            }
            else if constexpr (std::is_same_v<D, bool>) {
                return result ? std::int64_t{1} : std::int64_t{0};
            }
            else if constexpr (std::is_pointer_v<D>) {
                return static_cast<std::int64_t>(
                    reinterpret_cast<std::uintptr_t>(result));
            }
            else if constexpr (std::is_void_v<D>) {
                return 0;
            }
            else {
                return static_cast<std::int64_t>(result);
            }
        }

        // ---- all_types_supported<Tuple> — compile-time check -----------------

        template <class Tuple, std::size_t... Is>
        consteval bool check_all_supported(std::index_sequence<Is...>) {
            return (is_supported_type_v<std::tuple_element_t<Is, Tuple>> && ...);
        }

        template <class Tuple>
        inline constexpr bool all_types_supported =
            check_all_supported<Tuple>(
                std::make_index_sequence<std::tuple_size_v<Tuple>>{});

        // ---- build_param_hints<Tuple> — fill arg_types array at compile time --

        template <class Tuple, std::size_t N, std::size_t... Is>
        consteval std::array<std::uint32_t, N> build_hints_impl(std::index_sequence<Is...>) {
            std::array<std::uint32_t, N> a{};
            ((a[Is] = cpp_type_to_hint<std::decay_t<std::tuple_element_t<Is, Tuple>>>::value), ...);
            return a;
        }

        template <class Tuple, std::size_t N>
        consteval std::array<std::uint32_t, N> build_param_hints() {
            return build_hints_impl<Tuple, N>(
                std::make_index_sequence<std::tuple_size_v<Tuple>>{});
        }

        // ---- trampoline_holder<Fn, R, Args...> -----------------------------------
        //
        // Generates a stateless function pointer suitable for storage in
        // native_proxy::trampoline. The callable Fn must be a stateless lambda or
        // function pointer so the trampoline captures nothing on the stack.

        template <auto Fn, class R, class... Args>
        struct trampoline_holder {
            static std::int64_t invoke(void*, std::int64_t const* regs, std::uint32_t) noexcept {
                return invoke_impl(regs, std::index_sequence_for < Args...>{});
            }

        private:
            template <std::size_t... Is>
            static std::int64_t invoke_impl(std::int64_t const* regs, std::index_sequence<Is...>) noexcept {
                return marshal_result<R>(Fn(unmarshal_arg<Args>(regs[Is])...));
            }
        };

        // Specialisation for void return
        template <auto Fn, class... Args>
        struct trampoline_holder<Fn, void, Args...> {
            static std::int64_t invoke(void*, std::int64_t const* regs, std::uint32_t) noexcept {
                invoke_impl(regs, std::index_sequence_for < Args...>{});
                return 0;
            }

        private:
            template <std::size_t... Is>
            static void invoke_impl(std::int64_t const* regs, std::index_sequence<Is...>) noexcept {
                Fn(unmarshal_arg<Args>(regs[Is])...);
            }
        };

        // ---- proxy_builder<Fn, R, Params> — avoids lambda capture of constexpr locals

        template <auto Fn, class R, class Params>
        struct proxy_builder {
            template <std::size_t... Is>
            static constexpr native_proxy build(std::index_sequence<Is...>) noexcept {
                using trampoline = trampoline_holder<
                    Fn, R,
                    std::tuple_element_t < Is, Params>
                ...
                >
                ;
                constexpr std::size_t arity = std::tuple_size_v<Params>;
                constexpr auto param_hints = build_param_hints<Params, 8>();
                constexpr std::uint32_t ret_hint = [] {
                    if constexpr (std::is_void_v<R>) return type_hint_void;
                    else return cpp_type_to_hint<std::decay_t<R>>::value;
                }();

                native_proxy p;
                p.trampoline = &trampoline::invoke;
                p.arity = static_cast<std::uint8_t>(arity);
                p.ret_type = ret_hint;
                p.arg_types = param_hints;
                return p;
            }
        };
    } // namespace detail

    // =========================================================================
    // bind_native_function<auto Fn>()
    //
    // Template parameter Fn must be a constexpr-deducible function pointer or
    // stateless lambda passed as an NTTP.
    //
    // Returns std::expected<native_proxy, mop::mop_error> where the proxy
    // contains a C-linkage-compatible trampoline + complete ABI metadata.
    // =========================================================================

    template <auto Fn>
    [[nodiscard]] constexpr std::expected<native_proxy, mop::mop_error>
    bind_native_function() noexcept {
        using traits = detail::callable_traits<decltype(Fn)>;
        using R = typename traits::return_type;
        using Params = typename traits::param_types;
        constexpr std::size_t arity = traits::arity;

        // Trampolines are noexcept; a throwing callable would hit std::terminate.
        static_assert(detail::is_noexcept_fn<decltype(Fn)>::value,
                      "bind_native_function: callable must be noexcept — "
                      "trampolines are noexcept and cannot safely propagate exceptions.");

        static_assert(arity <= 8,
                      "bind_native_function: arity > 8 detected at compile time — "
                      "reduce parameter count or split the function.");

        if constexpr (arity > 8) {
            return std::unexpected(mop::mop_error{
                mop::mop_error_code::type_mismatch,
                "arity exceeds maximum of 8"
            });
        }
        else if constexpr (!detail::all_types_supported<Params>) {
            return std::unexpected(mop::mop_error{
                mop::mop_error_code::type_mismatch,
                "unsupported parameter type in native binding"
            });
        }
        else if constexpr (!std::is_void_v<R> && !detail::is_supported_type_v<R>) {
            return std::unexpected(mop::mop_error{
                mop::mop_error_code::type_mismatch,
                "unsupported return type in native binding"
            });
        }
        else {
            // All metadata is computed at compile time; use a helper struct to
            // expand the Params tuple into the trampoline's Args... pack.
            // We avoid lambdas that would need to capture constexpr locals.
            return detail::proxy_builder<Fn, R, Params>::build(
                std::make_index_sequence < arity >
            {}
            )
            ;
        }
    }

    // =========================================================================
    // invoke_bound — call a native_proxy using a raw int64_t register array.
    //
    // This is the hot-path dispatch used by the interpreter and JIT shims.
    //   proxy   : previously created by bind_native_function.
    //   regs    : caller-supplied array of at least proxy.arity int64_t values.
    //   ctx     : optional object context (passed through as first void* arg to
    //             the trampoline; usually nullptr for free functions).
    // =========================================================================

    [[nodiscard]] inline std::expected<std::int64_t, mop::mop_error>
    invoke_bound(const native_proxy& proxy,
                 std::span<std::int64_t const> regs,
                 void* ctx = nullptr) noexcept {
        if (!proxy.valid())
            return std::unexpected(mop::mop_error{
                mop::mop_error_code::not_implemented, "null native proxy"
            });
        if (regs.size() < proxy.arity)
            return std::unexpected(mop::mop_error{
                mop::mop_error_code::type_mismatch,
                "insufficient registers: need " + std::to_string(proxy.arity)
                + " got " + std::to_string(regs.size())
            });
        if (proxy.trampoline == nullptr)
            return std::unexpected(mop::mop_error{
                mop::mop_error_code::not_implemented,
                "proxy has a raw foreign address but no typed interpreter trampoline"
            });
        return proxy.trampoline(ctx, regs.data(), proxy.arity);
    }
} // namespace lithe::runtime::ffi::binding

// ============================================================================
// namespace lithe::runtime::values
//
// Abstract Runtime Value Layer — high-performance, JIT-register-aligned value
// abstractions sitting above the raw MOP memory protocol.
//
// Components
// ----------
//   object_ref          — non-owning MOP pointer enriched with layout-plugin tag.
//   native_function_ref — raw fn_ptr + arity + per-param/return type hints.
//   dynamic_value       — std::variant<i64,f64,bool,ptr,object_ref,func_ref>
//                         with constexpr type-check / extraction helpers.
//   boxed_value         — dynamic_value + 32-bit type_hint tag; aligns with
//                         ffi::marshal_to_native / unmarshal_from_native ABI.
//
// Marshalling
// -----------
//   marshal_to_native(dynamic_value)             → int64_t (ABI register word)
//   unmarshal_from_native(int64_t, uint32_t)     → dynamic_value
//
// Terminal conformance
// --------------------
//   is_terminal<dynamic_value>, is_terminal<boxed_value>, is_terminal<object_ref>
//   are specialised to true_type so these types embed directly in lazy AST nodes.
// ============================================================================

namespace lithe::runtime::values {
    // -------------------------------------------------------------------------
    // Forward declarations needed for the variant definition.
    // -------------------------------------------------------------------------

    struct object_ref;
    struct native_function_ref;

    struct void_value {
        [[nodiscard]] constexpr bool operator==(const void_value&) const noexcept = default;
    };

    // -------------------------------------------------------------------------
    // object_ref — non-owning MOP object pointer with explicit layout-tag.
    //
    //   ptr        : raw void* to the allocated instance.
    //   layout_id  : identifies the object_layout in the active layout_registry.
    //   plugin_tag : user-defined 32-bit language/plugin discriminant; zero = default.
    //
    // Trivially copyable so dynamic_value (the variant) remains trivially copyable.
    // -------------------------------------------------------------------------

    struct object_ref {
        void* ptr = nullptr;
        std::uint64_t layout_id = 0;
        std::uint32_t plugin_tag = 0;

        [[nodiscard]] constexpr bool valid() const noexcept { return ptr != nullptr; }
        [[nodiscard]] explicit constexpr operator bool() const noexcept { return valid(); }

        // Losslessly round-trip to/from mop::object_ptr (drops plugin_tag on downcast).
        [[nodiscard]] mop::object_ptr to_mop_ptr() const noexcept {
            return {ptr, layout_id};
        }

        [[nodiscard]] static object_ref from_mop_ptr(
            const mop::object_ptr& p, const std::uint32_t tag = 0) noexcept {
            return {p.raw, p.layout_id, tag};
        }
    };

    static_assert(std::is_trivially_copyable_v<object_ref>);

    // Relocation-safe managed argument. The slot is owned by rooted_ref or the
    // runtime root table and is rewritten by the collector when the object moves.
    struct managed_handle {
        object_ref* slot = nullptr;

        [[nodiscard]] constexpr bool valid() const noexcept { return slot != nullptr; }
        [[nodiscard]] explicit constexpr operator bool() const noexcept { return valid(); }
        [[nodiscard]] constexpr object_ref get() const noexcept {
            return slot ? *slot : object_ref{};
        }
    };

    static_assert(std::is_trivially_copyable_v<managed_handle>);

    // -------------------------------------------------------------------------
    // native_function_ref — binds a raw function pointer with ABI metadata.
    //
    //   fn_ptr     : entry point (void* to avoid cast UB at the call site).
    //   arity      : parameter count; must be ≤ max_arity.
    //   ret_hint   : ffi type_hint for the return register.
    //   param_hints: per-parameter ffi type_hints; unused slots are zero.
    //
    // max_arity = 8 matches ffi::native_proxy::arg_types capacity.
    // Trivially copyable.
    // -------------------------------------------------------------------------

    struct native_function_ref {
        static constexpr std::uint8_t max_arity = 8;

        void* fn_ptr = nullptr;
        std::uint8_t arity = 0;
        std::uint32_t ret_hint = ffi::type_hint_i64;
        std::array<std::uint32_t, max_arity> param_hints{};

        [[nodiscard]] constexpr bool valid() const noexcept { return fn_ptr != nullptr; }
        [[nodiscard]] explicit constexpr operator bool() const noexcept { return valid(); }
    };

    static_assert(std::is_trivially_copyable_v<native_function_ref>);

    // -------------------------------------------------------------------------
    // dynamic_value — the primary variant carrier.
    //
    // Alternative index → type mapping (stable, do not reorder):
    //   0 : std::int64_t        — integer / JIT i64 register
    //   1 : double              — floating-point / JIT f64 register
    //   2 : bool                — boolean lane (separate from i64 for type safety)
    //   3 : void*               — raw FFI / heap pointer
    //   4 : object_ref          — tagged MOP object pointer
    //   5 : native_function_ref — callable entry point + ABI metadata
    //   6 : managed_handle      — relocatable managed root slot
    //   7 : void_value          — explicit successful void result
    //
    // All alternatives are trivially copyable → the variant is trivially copyable.
    // -------------------------------------------------------------------------

    using dynamic_value = std::variant<
        std::int64_t,
        double,
        bool,
        void*,
        object_ref,
        native_function_ref,
        managed_handle,
        void_value
    >;

    static_assert(std::is_trivially_copyable_v<dynamic_value>);

    // ---- constexpr type-check helpers ----------------------------------------

    [[nodiscard]] constexpr bool is_i64(const dynamic_value& v) noexcept {
        return v.index() == 0;
    }

    [[nodiscard]] constexpr bool is_f64(const dynamic_value& v) noexcept {
        return v.index() == 1;
    }

    [[nodiscard]] constexpr bool is_bool(const dynamic_value& v) noexcept {
        return v.index() == 2;
    }

    [[nodiscard]] constexpr bool is_ptr(const dynamic_value& v) noexcept {
        return v.index() == 3;
    }

    [[nodiscard]] constexpr bool is_object(const dynamic_value& v) noexcept {
        return v.index() == 4;
    }

    [[nodiscard]] constexpr bool is_func(const dynamic_value& v) noexcept {
        return v.index() == 5;
    }

    [[nodiscard]] constexpr bool is_managed_handle(const dynamic_value& v) noexcept {
        return v.index() == 6;
    }

    [[nodiscard]] constexpr bool is_void(const dynamic_value& v) noexcept {
        return v.index() == 7;
    }

    // ---- fast unchecked extractors (caller must verify index first) ----------

    [[nodiscard]] constexpr std::int64_t as_i64(const dynamic_value& v) noexcept {
        return *std::get_if<std::int64_t>(&v);
    }

    [[nodiscard]] constexpr double as_f64(const dynamic_value& v) noexcept {
        return *std::get_if<double>(&v);
    }

    [[nodiscard]] constexpr bool as_bool(const dynamic_value& v) noexcept {
        return *std::get_if<bool>(&v);
    }

    [[nodiscard]] constexpr void* as_ptr(const dynamic_value& v) noexcept {
        return *std::get_if<void*>(&v);
    }

    [[nodiscard]] constexpr object_ref as_object(const dynamic_value& v) noexcept {
        return *std::get_if<object_ref>(&v);
    }

    [[nodiscard]] constexpr native_function_ref as_func(const dynamic_value& v) noexcept {
        return *std::get_if<native_function_ref>(&v);
    }

    [[nodiscard]] constexpr managed_handle as_managed_handle(const dynamic_value& v) noexcept {
        return *std::get_if<managed_handle>(&v);
    }

    // ---- factory helpers -------------------------------------------------------

    [[nodiscard]] constexpr dynamic_value make_i64(std::int64_t x) noexcept {
        return dynamic_value{std::in_place_index < 0 >, x};
    }

    [[nodiscard]] constexpr dynamic_value make_f64(double x) noexcept {
        return dynamic_value{std::in_place_index < 1 >, x};
    }

    [[nodiscard]] constexpr dynamic_value make_bool(bool x) noexcept {
        return dynamic_value{std::in_place_index < 2 >, x};
    }

    [[nodiscard]] constexpr dynamic_value make_ptr(void* x) noexcept {
        return dynamic_value{std::in_place_index < 3 >, x};
    }

    [[nodiscard]] constexpr dynamic_value make_object(object_ref x) noexcept {
        return dynamic_value{std::in_place_index < 4 >, x};
    }

    [[nodiscard]] constexpr dynamic_value make_func(native_function_ref x) noexcept {
        return dynamic_value{std::in_place_index < 5 >, x};
    }

    [[nodiscard]] constexpr dynamic_value make_managed_handle(managed_handle x) noexcept {
        return dynamic_value{std::in_place_index < 6 >, x};
    }

    [[nodiscard]] constexpr dynamic_value make_void() noexcept {
        return dynamic_value{std::in_place_index < 7 >, void_value{}};
    }

    // -------------------------------------------------------------------------
    // boxed_value — dynamic_value + host-side type_hint tag.
    //
    //   value     : the payload variant.
    //   type_hint : ffi type hint (type_hint_i64 / f64 / bool / ptr / obj / 5=func).
    //
    // Aligns with ffi marshal_to_native / unmarshal_from_native ABI: the lowering
    // compiler uses type_hint to select the correct ABI register class.
    // Trivially copyable (both members are).
    // -------------------------------------------------------------------------

    inline constexpr std::uint32_t type_hint_func = ffi::type_hint_func;

    struct boxed_value {
        dynamic_value value;
        std::uint32_t type_hint = ffi::type_hint_i64;

        constexpr boxed_value() noexcept = default;

        constexpr boxed_value(dynamic_value v, std::uint32_t hint) noexcept
            : value(v), type_hint(hint) {}
    };

    static_assert(std::is_trivially_copyable_v<boxed_value>);

    // -------------------------------------------------------------------------
    // marshal_to_native — flatten dynamic_value → 64-bit ABI word.
    //
    // All branches are branchless-friendly: the variant index dispatch is a jump
    // table, and float bit-reinterpretation uses std::memcpy to satisfy aliasing
    // rules without narrowing or UB.
    // -------------------------------------------------------------------------

    [[nodiscard]] inline std::int64_t
    marshal_to_native(const dynamic_value& v) noexcept {
        return std::visit([](auto const& x) -> std::int64_t {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, std::int64_t>) {
                return x;
            }
            else if constexpr (std::is_same_v<T, double>) {
                // Bit-preserving reinterpretation: memcpy avoids strict-aliasing UB.
                std::int64_t bits;
                static_assert(sizeof(bits) == sizeof(x));
                std::memcpy(&bits, &x, sizeof(bits));
                return bits;
            }
            else if constexpr (std::is_same_v<T, bool>) {
                return x ? std::int64_t{1} : std::int64_t{0};
            }
            else if constexpr (std::is_same_v<T, void*>) {
                return static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(x));
            }
            else if constexpr (std::is_same_v<T, object_ref>) {
                // Encode the raw pointer; layout_id and plugin_tag are recoverable
                // from the type_hint + a registry lookup at the call site.
                return static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(x.ptr));
            }
            else if constexpr (std::is_same_v<T, native_function_ref>) {
                // native_function_ref — encode the entry point.
                return static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(x.fn_ptr));
            }
            else if constexpr (std::is_same_v<T, managed_handle>) {
                return static_cast<std::int64_t>(
                    reinterpret_cast<std::uintptr_t>(x.get().ptr));
            }
            else {
                return 0; // explicit void_value
            }
        }, v);
    }

    // -------------------------------------------------------------------------
    // unmarshal_from_native — reconstruct a dynamic_value from a raw ABI word.
    //
    // type_hint follows the ffi convention (0-4) extended with type_hint_func=5.
    // Unknown hints fall back to i64.
    // -------------------------------------------------------------------------

    [[nodiscard]] inline dynamic_value
    unmarshal_from_native(std::int64_t raw, std::uint32_t type_hint) noexcept {
        switch (type_hint) {
        case ffi::type_hint_f64: {
            double d;
            static_assert(sizeof(d) == sizeof(raw));
            std::memcpy(&d, &raw, sizeof(d));
            return make_f64(d);
        }
        case ffi::type_hint_bool:
            return make_bool(raw != 0);
        case ffi::type_hint_ptr:
            return make_ptr(reinterpret_cast<void*>(static_cast<std::uintptr_t>(raw)));
        case ffi::type_hint_obj:
            // Reconstruct a minimal object_ref; layout_id / plugin_tag must be
            // filled in by the caller from the active registry if needed.
            return make_object(object_ref{
                reinterpret_cast<void*>(static_cast<std::uintptr_t>(raw)), 0, 0
            });
        case type_hint_func:
            return make_func(native_function_ref{
                reinterpret_cast<void*>(static_cast<std::uintptr_t>(raw)), 0,
                ffi::type_hint_i64, {}
            });
        case ffi::type_hint_void:
            return make_void();
        default: // type_hint_i64 and unknown → i64
            return make_i64(raw);
        }
    }
} // namespace lithe::runtime::values

// ============================================================================
// Terminal trait specialisations for the Abstract Runtime Value Layer.
//
// Specialise lithe::is_terminal so that dynamic_value, boxed_value, and
// object_ref are valid leaf nodes in a lazy Lithe expression AST without
// wrapping them in an explicit lit<> adapter.
// ============================================================================

namespace vakya {
    template <>
    struct is_terminal<lithe::runtime::values::dynamic_value> : std::true_type {};

    template <>
    struct is_terminal<lithe::runtime::values::boxed_value> : std::true_type {};

    template <>
    struct is_terminal<lithe::runtime::values::object_ref> : std::true_type {};
} // namespace vakya
