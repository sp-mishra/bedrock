#pragma once

// crank/parser_stats.hpp — Zero-overhead parse statistics.
// All fields populated by frontend::parse() when parse_options::collect_stats == true.
// C++23, header-only, no virtual, no macros.
// Namespace: crank

#include "languages/generic/core/parse_stats.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace crank::lex {
    enum class token_kind : std::uint16_t;
}

namespace crank {
    // Re-export generic timing type.
    using phase_timings = lang::phase_timings;

    // Per-token-kind frequency table (crank-specific: indexed by token_kind value).
    constexpr std::size_t token_kind_count = 96; // token_kind::unknown + 1
    using token_freq_array = std::array<std::uint32_t, token_kind_count>;

    // Production frequency table: production name → count.
    using production_freq_map = std::unordered_map<std::string, std::uint32_t>;

    struct parse_stats : lang::parse_tree_stats {
        // Crank-specific additions
        std::uint32_t  asi_injections{0};          // synthetic stmt_term tokens
        token_freq_array token_by_kind{};           // per-kind breakdown
        std::uint32_t  deepest_fn_name_len{0};      // longest ident token length
    };
} // namespace crank
