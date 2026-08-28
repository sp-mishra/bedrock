#pragma once

// crank/std_types.hpp — crank primitive type registry + Result/Option/core.tx mapping.
//
// C++23, header-only, no virtual, no macros. Namespace: crank
//
// Seeds a vakya::types::type_registry (from make_builtin_type_registry()) with
// crank-specific fixed-width primitives:
//   Int8/Int16/Int32/Int64, UInt8/UInt16/UInt32/UInt64, Float32/Float64, Bool, String, Unit
//
// Result[T,E] → std::expected<T,E>  (2-arg constructor type)
// Option[T]   → std::optional<T>    (1-arg constructor type)
//
// core.tx language types (§7c.2, design.md):
//   TxStatus, Isolation, ReplaySafety, ConflictPolicy, PartialCommit, ProofStatus — plain enums
//   TxError, CommitReport — opaque structured records
//   TransactionOptions — aggregate

#include "vakya/types/type_registry.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>

namespace crank {
    // ============================================================================
    // Crank primitive type constructor tags (extension band >= 1000)
    // Allocation: crank type band 2000–2099 (separate from crank AST tag band 1000–1099)
    // ============================================================================

    struct int8_type_tag {};

    struct int16_type_tag {};

    struct int32_type_tag {};

    struct int64_type_tag {};

    struct uint8_type_tag {};

    struct uint16_type_tag {};

    struct uint32_type_tag {};

    struct uint64_type_tag {};

    struct float32_type_tag {};

    struct float64_type_tag {};

    struct bool_type_tag {};

    struct string_type_tag {};

    struct unit_type_tag {};

    // Result[T,E] and Option[T] use vakya built-in result_type_tag / optional_type_tag (id=17/16).
    // These tags just serve as crank's compile-time aliases.
    using result_type_tag = vakya::types::result_type_tag; // arity=2, id=17
    using option_type_tag = vakya::types::optional_type_tag; // arity=1, id=16

    // core.tx opaque type tags — language-level named types, not C++ runtime types
    struct tx_status_type_tag {};

    struct isolation_type_tag {};

    struct replay_safety_type_tag {};

    struct conflict_policy_type_tag {};

    struct partial_commit_type_tag {};

    struct proof_status_type_tag {};

    struct tx_error_type_tag {};

    struct commit_report_type_tag {};

    struct transaction_options_type_tag {};

    // ============================================================================
    // Stable IDs for crank type band (2000–2099)
    // ============================================================================

    inline constexpr std::uint32_t kCrankTypeBase = 2000u;
    inline constexpr std::uint32_t kTypeInt8 = 2000u;
    inline constexpr std::uint32_t kTypeInt16 = 2001u;
    inline constexpr std::uint32_t kTypeInt32 = 2002u;
    inline constexpr std::uint32_t kTypeInt64 = 2003u;
    inline constexpr std::uint32_t kTypeUInt8 = 2004u;
    inline constexpr std::uint32_t kTypeUInt16 = 2005u;
    inline constexpr std::uint32_t kTypeUInt32 = 2006u;
    inline constexpr std::uint32_t kTypeUInt64 = 2007u;
    inline constexpr std::uint32_t kTypeFloat32 = 2008u;
    inline constexpr std::uint32_t kTypeFloat64 = 2009u;
    inline constexpr std::uint32_t kTypeBool = 2010u;
    inline constexpr std::uint32_t kTypeString = 2011u;
    inline constexpr std::uint32_t kTypeUnit = 2012u;
    // core.tx types
    inline constexpr std::uint32_t kTypeTxStatus = 2020u;
    inline constexpr std::uint32_t kTypeIsolation = 2021u;
    inline constexpr std::uint32_t kTypeReplaySafety = 2022u;
    inline constexpr std::uint32_t kTypeConflictPolicy = 2023u;
    inline constexpr std::uint32_t kTypePartialCommit = 2024u;
    inline constexpr std::uint32_t kTypeProofStatus = 2025u;
    inline constexpr std::uint32_t kTypeTxError = 2026u;
    inline constexpr std::uint32_t kTypeCommitReport = 2027u;
    inline constexpr std::uint32_t kTypeTransactionOptions = 2028u;
} // namespace crank

// ============================================================================
// vakya::types::type_descriptor specialisations for crank primitive tags
// ============================================================================

namespace vakya::types {
    template <>
    struct type_descriptor<crank::int8_type_tag> {
        static constexpr std::uint32_t stable_id = crank::kTypeInt8;
        static constexpr std::uint8_t arity = 0;
        static constexpr std::string_view symbol = "Int8";
    };

    template <>
    struct type_descriptor<crank::int16_type_tag> {
        static constexpr std::uint32_t stable_id = crank::kTypeInt16;
        static constexpr std::uint8_t arity = 0;
        static constexpr std::string_view symbol = "Int16";
    };

    template <>
    struct type_descriptor<crank::int32_type_tag> {
        static constexpr std::uint32_t stable_id = crank::kTypeInt32;
        static constexpr std::uint8_t arity = 0;
        static constexpr std::string_view symbol = "Int32";
    };

    template <>
    struct type_descriptor<crank::int64_type_tag> {
        static constexpr std::uint32_t stable_id = crank::kTypeInt64;
        static constexpr std::uint8_t arity = 0;
        static constexpr std::string_view symbol = "Int64";
    };

    template <>
    struct type_descriptor<crank::uint8_type_tag> {
        static constexpr std::uint32_t stable_id = crank::kTypeUInt8;
        static constexpr std::uint8_t arity = 0;
        static constexpr std::string_view symbol = "UInt8";
    };

    template <>
    struct type_descriptor<crank::uint16_type_tag> {
        static constexpr std::uint32_t stable_id = crank::kTypeUInt16;
        static constexpr std::uint8_t arity = 0;
        static constexpr std::string_view symbol = "UInt16";
    };

    template <>
    struct type_descriptor<crank::uint32_type_tag> {
        static constexpr std::uint32_t stable_id = crank::kTypeUInt32;
        static constexpr std::uint8_t arity = 0;
        static constexpr std::string_view symbol = "UInt32";
    };

    template <>
    struct type_descriptor<crank::uint64_type_tag> {
        static constexpr std::uint32_t stable_id = crank::kTypeUInt64;
        static constexpr std::uint8_t arity = 0;
        static constexpr std::string_view symbol = "UInt64";
    };

    template <>
    struct type_descriptor<crank::float32_type_tag> {
        static constexpr std::uint32_t stable_id = crank::kTypeFloat32;
        static constexpr std::uint8_t arity = 0;
        static constexpr std::string_view symbol = "Float32";
    };

    template <>
    struct type_descriptor<crank::float64_type_tag> {
        static constexpr std::uint32_t stable_id = crank::kTypeFloat64;
        static constexpr std::uint8_t arity = 0;
        static constexpr std::string_view symbol = "Float64";
    };

    template <>
    struct type_descriptor<crank::bool_type_tag> {
        static constexpr std::uint32_t stable_id = crank::kTypeBool;
        static constexpr std::uint8_t arity = 0;
        static constexpr std::string_view symbol = "Bool";
    };

    template <>
    struct type_descriptor<crank::string_type_tag> {
        static constexpr std::uint32_t stable_id = crank::kTypeString;
        static constexpr std::uint8_t arity = 0;
        static constexpr std::string_view symbol = "String";
    };

    template <>
    struct type_descriptor<crank::unit_type_tag> {
        static constexpr std::uint32_t stable_id = crank::kTypeUnit;
        static constexpr std::uint8_t arity = 0;
        static constexpr std::string_view symbol = "Unit";
    };

    // core.tx type descriptors
    template <>
    struct type_descriptor<crank::tx_status_type_tag> {
        static constexpr std::uint32_t stable_id = crank::kTypeTxStatus;
        static constexpr std::uint8_t arity = 0;
        static constexpr std::string_view symbol = "TxStatus";
    };

    template <>
    struct type_descriptor<crank::isolation_type_tag> {
        static constexpr std::uint32_t stable_id = crank::kTypeIsolation;
        static constexpr std::uint8_t arity = 0;
        static constexpr std::string_view symbol = "Isolation";
    };

    template <>
    struct type_descriptor<crank::replay_safety_type_tag> {
        static constexpr std::uint32_t stable_id = crank::kTypeReplaySafety;
        static constexpr std::uint8_t arity = 0;
        static constexpr std::string_view symbol = "ReplaySafety";
    };

    template <>
    struct type_descriptor<crank::conflict_policy_type_tag> {
        static constexpr std::uint32_t stable_id = crank::kTypeConflictPolicy;
        static constexpr std::uint8_t arity = 0;
        static constexpr std::string_view symbol = "ConflictPolicy";
    };

    template <>
    struct type_descriptor<crank::partial_commit_type_tag> {
        static constexpr std::uint32_t stable_id = crank::kTypePartialCommit;
        static constexpr std::uint8_t arity = 0;
        static constexpr std::string_view symbol = "PartialCommit";
    };

    template <>
    struct type_descriptor<crank::proof_status_type_tag> {
        static constexpr std::uint32_t stable_id = crank::kTypeProofStatus;
        static constexpr std::uint8_t arity = 0;
        static constexpr std::string_view symbol = "ProofStatus";
    };

    template <>
    struct type_descriptor<crank::tx_error_type_tag> {
        static constexpr std::uint32_t stable_id = crank::kTypeTxError;
        static constexpr std::uint8_t arity = 0;
        static constexpr std::string_view symbol = "TxError";
    };

    template <>
    struct type_descriptor<crank::commit_report_type_tag> {
        static constexpr std::uint32_t stable_id = crank::kTypeCommitReport;
        static constexpr std::uint8_t arity = 0;
        static constexpr std::string_view symbol = "CommitReport";
    };

    template <>
    struct type_descriptor<crank::transaction_options_type_tag> {
        static constexpr std::uint32_t stable_id = crank::kTypeTransactionOptions;
        static constexpr std::uint8_t arity = 0;
        static constexpr std::string_view symbol = "TransactionOptions";
    };
} // namespace vakya::types

namespace crank {
    // ============================================================================
    // C++ boundary type mappings
    // Result[T,E] → std::expected<T,E>
    // Option[T]   → std::optional<T>
    // ============================================================================

    template <class T, class E>
    using Result = std::expected<T, E>;

    template <class T>
    using Option = std::optional<T>;

    // ============================================================================
    // core.tx plain enum representations (C++ side of the boundary).
    // Crank source maps these 1:1; values must match design.md §7c.2.
    // ============================================================================

    enum class TxStatus : std::uint8_t {
        committed = 0,
        aborted = 1,
        pending = 2,
    };

    enum class Isolation : std::uint8_t {
        read_committed = 0,
        snapshot = 1,
        serializable = 2,
    };

    enum class ReplaySafety : std::uint8_t {
        none = 0,
        body_idempotent = 1,
        body_and_effects_idempotent = 2,
    };

    enum class ConflictPolicy : std::uint8_t {
        optimistic = 0,
        pessimistic = 1,
    };

    enum class PartialCommit : std::uint8_t {
        disallow = 0,
        allow = 1,
    };

    enum class ProofStatus : std::uint8_t {
        unknown = 0,
        proven = 1,
        refuted = 2,
        deferred = 3,
    };

    struct TxError {
        TxStatus status{};
        std::uint32_t code = 0;
    };

    struct CommitReport {
        TxStatus status{};
        std::uint32_t retry_count = 0;
    };

    struct TransactionOptions {
        Isolation isolation = Isolation::snapshot;
        ReplaySafety replay = ReplaySafety::none;
        ConflictPolicy conflict = ConflictPolicy::optimistic;
        PartialCommit partial_commit = PartialCommit::disallow;
        std::uint32_t retry = 0;
    };

    // ============================================================================
    // make_crank_type_registry — builtin vakya types + crank primitives + core.tx
    // ============================================================================

    [[nodiscard]] inline vakya::types::type_registry make_crank_type_registry() {
        using namespace vakya::types;

        type_registry reg = make_builtin_type_registry();

        auto add = [&]<class Tag>(type_registry_category cat = type_registry_category::primitive,
                                  type_kind kind = type_kind::primitive) {
            register_type_descriptor<Tag>(reg, cat, kind);
        };

        // Fixed-width numeric types
        add.template operator()<int8_type_tag>();
        add.template operator()<int16_type_tag>();
        add.template operator()<int32_type_tag>();
        add.template operator()<int64_type_tag>();
        add.template operator()<uint8_type_tag>();
        add.template operator()<uint16_type_tag>();
        add.template operator()<uint32_type_tag>();
        add.template operator()<uint64_type_tag>();
        add.template operator()<float32_type_tag>();
        add.template operator()<float64_type_tag>();
        add.template operator()<bool_type_tag>();
        add.template operator()<string_type_tag>();
        add.template operator()<unit_type_tag>();

        // core.tx opaque types — language category
        add.template operator()<tx_status_type_tag>(type_registry_category::language);
        add.template operator()<isolation_type_tag>(type_registry_category::language);
        add.template operator()<replay_safety_type_tag>(type_registry_category::language);
        add.template operator()<conflict_policy_type_tag>(type_registry_category::language);
        add.template operator()<partial_commit_type_tag>(type_registry_category::language);
        add.template operator()<proof_status_type_tag>(type_registry_category::language);
        add.template operator()<tx_error_type_tag>(type_registry_category::language);
        add.template operator()<commit_report_type_tag>(type_registry_category::language);
        add.template operator()<transaction_options_type_tag>(type_registry_category::language);

        return reg;
    }

    // ============================================================================
    // lookup_primitive_id — resolve a type name string to its stable_id.
    // Returns 0 if unknown.
    // ============================================================================

    [[nodiscard]] constexpr std::uint32_t lookup_primitive_id(std::string_view name) noexcept {
        // Simple linear scan — compile-time tables are small.
        struct entry {
            std::string_view sym;
            std::uint32_t id;
        };
        constexpr entry table[] = {
            {"Int8", kTypeInt8}, {"Int16", kTypeInt16},
            {"Int32", kTypeInt32}, {"Int64", kTypeInt64},
            {"UInt8", kTypeUInt8}, {"UInt16", kTypeUInt16},
            {"UInt32", kTypeUInt32}, {"UInt64", kTypeUInt64},
            {"Float32", kTypeFloat32}, {"Float64", kTypeFloat64},
            {"Bool", kTypeBool}, {"String", kTypeString},
            {"Unit", kTypeUnit},
            // Lowercase aliases — lexical aliases for the canonical fixed-width
            // names (crank.md §"Primitives"): identical types, each resolving to
            // the SAME stable_id as its PascalCase spelling.
            {"i8", kTypeInt8}, {"i16", kTypeInt16}, {"i32", kTypeInt32}, {"i64", kTypeInt64},
            {"u8", kTypeUInt8}, {"u16", kTypeUInt16}, {"u32", kTypeUInt32}, {"u64", kTypeUInt64},
            {"f32", kTypeFloat32}, {"f64", kTypeFloat64},
            // core.tx
            {"TxStatus", kTypeTxStatus},
            {"Isolation", kTypeIsolation},
            {"ReplaySafety", kTypeReplaySafety},
            {"ConflictPolicy", kTypeConflictPolicy},
            {"PartialCommit", kTypePartialCommit},
            {"ProofStatus", kTypeProofStatus},
            {"TxError", kTypeTxError},
            {"CommitReport", kTypeCommitReport},
            {"TransactionOptions", kTypeTransactionOptions},
        };
        for (const auto& e : table)
            if (e.sym == name) return e.id;
        return 0u;
    }
} // namespace crank
