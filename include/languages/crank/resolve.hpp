#pragma once

// crank/resolve.hpp — symbol table + name resolution for crank semantics (Module 2).
//
// C++23, header-only, no virtual, no macros. Namespace: crank
//
// Produces a module-qualified symbol table from the Vakya AST.
// Tracks let = immutable, var = mutable (mutability drives dependence analysis).
// Enforces:
//   - var zero-value rules: Result/bare enum/fn must initialize
//   - public export rules: uppercase fn/type/const require explicit types; OR explicit `pub` keyword
//   - shadowing: inner let/var shadows outer; double-let in same scope is diagnostic
//
// resolve_result:
//   .symbols    — resolved symbol_table
//   .diagnostics — collecting_sink

#include "languages/crank/std_types.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace crank {
    // ============================================================================
    // mutability_kind — tracks binding mutability for dependence analysis
    // ============================================================================

    enum class mutability_kind : std::uint8_t {
        immutable = 0, // let
        mutable_ = 1, // var
        constant = 2, // const
    };

    // ============================================================================
    // visibility_kind — public vs module-local
    // ============================================================================

    enum class visibility_kind : std::uint8_t {
        module_local = 0, // lowercase name
        exported = 1, // uppercase name (fn/type/const)
    };

    // ============================================================================
    // symbol_kind — what kind of entity the symbol names
    // ============================================================================

    enum class symbol_kind : std::uint8_t {
        value = 0, // let/var binding
        function = 1, // fn
        type_def = 2, // type
        constant = 3, // const
        param = 4, // function parameter
        view = 5, // view declaration (domain views)
    };

    // ============================================================================
    // symbol_entry — one resolved symbol
    // ============================================================================

    struct symbol_entry {
        std::string qualified_name; // "module::name"
        std::string local_name; // unqualified
        symbol_kind kind = symbol_kind::value;
        mutability_kind mutability = mutability_kind::immutable;
        visibility_kind visibility = visibility_kind::module_local;
        std::uint32_t type_id = 0; // resolved type stable_id (0 = unresolved)
        bool initialized = false; // var without initializer when required = diagnostic
        std::uint32_t scope_depth = 0; // lexical depth

        // View-specific metadata (set when kind == symbol_kind::view)
        std::string view_backing_name; // binding name inside view methods (e.g. "base")
        std::uint32_t view_source_type_id = 0; // resolved backing source type id
        std::uint32_t view_registry_id = 0; // id in the view_descriptor registry (0 = unregistered)
    };

    // ============================================================================
    // symbol_table — flat map of qualified_name → symbol_entry, with scope stack
    // ============================================================================

    class symbol_table {
    public:
        symbol_table() { scope_stack_.emplace_back(); }

        // Push a new lexical scope
        void push_scope() { scope_stack_.emplace_back(); }

        // Pop scope, removing all symbols introduced in it
        void pop_scope() {
            if (scope_stack_.size() <= 1) return;
            for (const auto& key : scope_stack_.back())
                entries_.erase(key);
            scope_stack_.pop_back();
        }

        // current depth
        [[nodiscard]] std::uint32_t depth() const noexcept {
            return static_cast<std::uint32_t>(scope_stack_.size() - 1);
        }

        // Define a symbol in current scope. Returns false on duplicate-in-scope (diagnostic).
        [[nodiscard]] bool define(std::string qualified, symbol_entry entry) {
            entry.scope_depth = depth();
            auto [it, inserted] = entries_.insert_or_assign(std::move(qualified), std::move(entry));
            (void)it;
            scope_stack_.back().push_back(it->first);
            return inserted;
        }

        // Lookup symbol by qualified name
        [[nodiscard]] const symbol_entry* lookup(std::string_view name) const {
            auto it = entries_.find(std::string(name));
            if (it == entries_.end()) return nullptr;
            return &it->second;
        }

        // Iterate all symbols
        template <class Fn>
        void for_each(Fn&& fn) const {
            for (const auto& [k, v] : entries_)
                fn(k, v);
        }

        [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

    private:
        std::unordered_map<std::string, symbol_entry> entries_;
        std::vector<std::vector<std::string>> scope_stack_{{}}; // root scope
    };

    // ============================================================================
    // resolve_diagnostic — semantic diagnostic from resolution pass
    // ============================================================================

    struct resolve_diagnostic {
        enum class kind : std::uint8_t {
            uninitialized_var, // var x: Result[...] without init
            missing_return_type, // exported fn missing return type annotation
            missing_param_type, // exported fn param missing type annotation
            duplicate_symbol, // same name in same scope
            undefined_symbol, // use of undefined name
            shadow_warning, // shadowing an outer binding
            extern_unknown_host, // CRANK-EXT-010: @host.link symbol not registered
            extern_arity_mismatch, // CRANK-EXT-011: declared arity ≠ registered arity
        };

        kind k;
        std::string symbol;
        std::string message;

        bool is_error() const noexcept {
            return k != kind::shadow_warning;
        }

        // Stable diagnostic code (never reused for a second meaning). Exported-
        // signature annotation failures carry the CRANK-TYPE-* band the language
        // reference promises; all other resolver diagnostics carry CRANK-RES-*.
        [[nodiscard]] constexpr std::string_view code() const noexcept {
            return to_code(k);
        }

        [[nodiscard]] static constexpr std::string_view
        to_code(kind k) noexcept {
            switch (k) {
            case kind::uninitialized_var: return "CRANK-RES-001";
            case kind::missing_return_type: return "CRANK-TYPE-001";
            case kind::missing_param_type: return "CRANK-TYPE-002";
            case kind::duplicate_symbol: return "CRANK-RES-002";
            case kind::undefined_symbol: return "CRANK-RES-003";
            case kind::shadow_warning: return "CRANK-RES-004";
            case kind::extern_unknown_host: return "CRANK-EXT-010";
            case kind::extern_arity_mismatch: return "CRANK-EXT-011";
            }
            return "CRANK-RES-000";
        }
    };

    // ============================================================================
    // resolve_result
    // ============================================================================

    struct resolve_result {
        symbol_table symbols;
        std::vector<resolve_diagnostic> diagnostics;

        [[nodiscard]] bool ok() const noexcept {
            for (const auto& d : diagnostics)
                if (d.is_error()) return false;
            return true;
        }
    };

    // ============================================================================
    // resolver — walks the symbol table building resolve_result.
    //
    // Designed to operate over a flat representation (e.g. list of symbol specs
    // extracted from the Vakya AST by the sema_types pass). Full AST-walk
    // integration is provided by sema_types.hpp via resolver::declare().
    // ============================================================================

    class resolver {
    public:
        explicit resolver(std::string module_name = "")
            : module_name_(std::move(module_name)) {}

        // --- scope management ---

        void enter_scope() { result_.symbols.push_scope(); }
        void leave_scope() { result_.symbols.pop_scope(); }

        // --- symbol declaration ---

        // Declare a let/var/const value binding.
        //
        // `has_zero_value` states whether the binding's type has a zero-value. When
        // false (Result, bare enum, fn — the no-zero-value set of crank.md §"var
        // zero-value rules"), an uninitialized `var` is a diagnostic. Defaulted so
        // existing callers keep the id-based heuristic (Result id=17 / unresolved
        // opaque id=0 have no zero-value); callers that know the type kind (enum/fn)
        // pass `false` explicitly to enforce the full rule.
        void declare_value(std::string_view name,
                           mutability_kind mut,
                           std::uint32_t type_id,
                           bool initialized,
                           bool has_zero_value) {
            symbol_entry e;
            e.local_name = std::string(name);
            e.qualified_name = qualify(name);
            e.kind = symbol_kind::value;
            e.mutability = mut;
            e.visibility = infer_visibility(name);
            e.type_id = type_id;
            e.initialized = initialized;

            // Var zero-value rule: no-zero-value types (Result / bare enum / fn)
            // must be initialized.
            if (mut == mutability_kind::mutable_ && !initialized && !has_zero_value) {
                emit(resolve_diagnostic::kind::uninitialized_var, std::string(name),
                     "var '" + std::string(name) + "': Result/fn/enum must be initialized");
            }

            if (!result_.symbols.define(e.qualified_name, e)) {
                emit(resolve_diagnostic::kind::duplicate_symbol, std::string(name),
                     "duplicate symbol '" + std::string(name) + "' in same scope");
            }
        }

        // Legacy overload: derives has_zero_value from the type id alone.
        // Result (id=17) and unresolved opaque (id=0) are treated as no-zero-value.
        void declare_value(std::string_view name,
                           mutability_kind mut,
                           std::uint32_t type_id,
                           bool initialized) {
            const bool no_zero =
                type_id == vakya::types::type_descriptor<vakya::types::result_type_tag>::stable_id
                || type_id == 0;
            declare_value(name, mut, type_id, initialized, /*has_zero_value=*/!no_zero);
        }

        // Declare a function.
        // exported: uppercase name OR explicit `pub` keyword.
        // force_exported: set true when the source has `pub fn ...` (lowercase name but pub-forced export).
        void declare_function(std::string_view name,
                              bool return_type_annotated,
                              bool params_typed,
                              std::uint32_t return_type_id = 0,
                              bool force_exported = false) {
            symbol_entry e;
            e.local_name = std::string(name);
            e.qualified_name = qualify(name);
            e.kind = symbol_kind::function;
            e.mutability = mutability_kind::immutable;
            e.visibility = force_exported ? visibility_kind::exported : infer_visibility(name);
            e.type_id = return_type_id;
            e.initialized = true;

            if (e.visibility == visibility_kind::exported) {
                if (!return_type_annotated)
                    emit(resolve_diagnostic::kind::missing_return_type, std::string(name),
                         "exported fn '" + std::string(name) + "' requires explicit return type");
                if (!params_typed)
                    emit(resolve_diagnostic::kind::missing_param_type, std::string(name),
                         "exported fn '" + std::string(name) + "' requires all params annotated");
            }

            if (!result_.symbols.define(e.qualified_name, e)) {
                emit(resolve_diagnostic::kind::duplicate_symbol, std::string(name),
                     "duplicate symbol '" + std::string(name) + "'");
            }
        }

        // Declare a type alias or struct/enum type.
        // force_exported: set true when source has `pub type ...` (lowercase name forced to export).
        void declare_type(std::string_view name, std::uint32_t type_id = 0,
                          bool force_exported = false) {
            symbol_entry e;
            e.local_name = std::string(name);
            e.qualified_name = qualify(name);
            e.kind = symbol_kind::type_def;
            e.mutability = mutability_kind::constant;
            e.visibility = force_exported ? visibility_kind::exported : infer_visibility(name);
            e.type_id = type_id;
            e.initialized = true;

            if (!result_.symbols.define(e.qualified_name, e)) {
                emit(resolve_diagnostic::kind::duplicate_symbol, std::string(name),
                     "duplicate type '" + std::string(name) + "'");
            }
        }

        // Declare a view. backing_name is the binding identifier inside view methods (e.g. "base").
        // source_type_id is the resolved backing storage type id.
        // force_exported: set true when source has `pub view ...`.
        void declare_view(std::string_view name,
                          std::string_view backing_name,
                          std::uint32_t source_type_id = 0,
                          bool force_exported = false) {
            symbol_entry e;
            e.local_name = std::string(name);
            e.qualified_name = qualify(name);
            e.kind = symbol_kind::view;
            e.mutability = mutability_kind::constant;
            e.visibility = force_exported ? visibility_kind::exported : infer_visibility(name);
            e.type_id = 0;
            e.initialized = true;
            e.view_backing_name = std::string(backing_name);
            e.view_source_type_id = source_type_id;

            if (!result_.symbols.define(e.qualified_name, e)) {
                emit(resolve_diagnostic::kind::duplicate_symbol, std::string(name),
                     "duplicate view '" + std::string(name) + "'");
            }
        }

        // Resolve impl target — checks both type and view symbols.
        // Returns the matched symbol_entry or nullptr if not found.
        [[nodiscard]] const symbol_entry* resolve_impl_target(std::string_view name) {
            auto* e = result_.symbols.lookup(qualify(name));
            if (!e) e = result_.symbols.lookup(name);
            // Accept type_def and view as valid impl targets
            if (e && (e->kind == symbol_kind::type_def || e->kind == symbol_kind::view))
                return e;
            return nullptr;
        }

        // Resolve a use-site name. Emits undefined_symbol diagnostic if not found.
        [[nodiscard]] const symbol_entry* resolve(std::string_view name) {
            auto* e = result_.symbols.lookup(qualify(name));
            if (!e) e = result_.symbols.lookup(name); // try unqualified
            if (!e) {
                emit(resolve_diagnostic::kind::undefined_symbol, std::string(name),
                     "undefined symbol '" + std::string(name) + "'");
            }
            return e;
        }

        // Consume and return the result
        [[nodiscard]] resolve_result take() { return std::move(result_); }

        // Read-only diagnostics access
        [[nodiscard]] const std::vector<resolve_diagnostic>& diagnostics() const noexcept {
            return result_.diagnostics;
        }

    private:
        std::string module_name_;
        resolve_result result_;

        [[nodiscard]] std::string qualify(std::string_view name) const {
            if (module_name_.empty()) return std::string(name);
            return module_name_ + "::" + std::string(name);
        }

        [[nodiscard]] static visibility_kind infer_visibility(std::string_view name) noexcept {
            if (!name.empty() && name[0] >= 'A' && name[0] <= 'Z')
                return visibility_kind::exported;
            return visibility_kind::module_local;
        }

        void emit(resolve_diagnostic::kind k, std::string sym, std::string msg) {
            result_.diagnostics.push_back({k, std::move(sym), std::move(msg)});
        }
    };
} // namespace crank
