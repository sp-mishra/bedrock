#pragma once

// Lossless portable_module wire codec. canonical_encode() remains the stable
// semantic-digest preimage; this codec is the reversible persistence format.

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "module.hpp"
#include "verify.hpp"

namespace lithe::ir::portable {
struct codec_error {
  std::string detail;
};

struct codec_limits {
  std::size_t max_bytes = 256u * 1024u * 1024u;
  std::uint32_t max_items = 1'000'000;
  std::uint32_t max_string_bytes = 16u * 1024u * 1024u;
};

namespace codec_detail {
struct writer {
  std::vector<std::uint8_t> out;
  void u8(std::uint8_t v) { out.push_back(v); }
  void u16(std::uint16_t v) {
    u8(v & 0xff);
    u8((v >> 8) & 0xff);
  }
  void u32(std::uint32_t v) {
    for (int i = 0; i < 4; ++i)
      u8((v >> (8 * i)) & 0xff);
  }
  void u64(std::uint64_t v) {
    for (int i = 0; i < 8; ++i)
      u8((v >> (8 * i)) & 0xff);
  }
  void i64(std::int64_t v) { u64(static_cast<std::uint64_t>(v)); }
  void boolean(bool v) { u8(v ? 1 : 0); }
  void str(std::string_view s) {
    u32(static_cast<std::uint32_t>(s.size()));
    out.insert(out.end(), s.begin(), s.end());
  }
  void bytes(std::span<const std::uint8_t> s) {
    u32(static_cast<std::uint32_t>(s.size()));
    out.insert(out.end(), s.begin(), s.end());
  }
  template <class T, class F> void vec(const std::vector<T> &xs, F &&f) {
    u32(static_cast<std::uint32_t>(xs.size()));
    for (const auto &x : xs)
      f(x);
  }
  void schema(schema_version v) {
    u16(v.major);
    u16(v.minor);
    u16(v.patch);
  }
};

struct reader {
  std::span<const std::uint8_t> in;
  codec_limits limits;
  std::string error;
  bool take(std::size_t n, std::span<const std::uint8_t> &s) {
    if (n > in.size()) {
      error = "portable codec: truncated input";
      return false;
    }
    s = in.first(n);
    in = in.subspan(n);
    return true;
  }
  bool u8(std::uint8_t &v) {
    std::span<const std::uint8_t> s;
    if (!take(1, s))
      return false;
    v = s[0];
    return true;
  }
  bool u16(std::uint16_t &v) {
    std::span<const std::uint8_t> s;
    if (!take(2, s))
      return false;
    v = s[0] | (std::uint16_t(s[1]) << 8);
    return true;
  }
  bool u32(std::uint32_t &v) {
    std::span<const std::uint8_t> s;
    if (!take(4, s))
      return false;
    v = 0;
    for (int i = 0; i < 4; ++i)
      v |= std::uint32_t(s[i]) << (8 * i);
    return true;
  }
  bool u64(std::uint64_t &v) {
    std::span<const std::uint8_t> s;
    if (!take(8, s))
      return false;
    v = 0;
    for (int i = 0; i < 8; ++i)
      v |= std::uint64_t(s[i]) << (8 * i);
    return true;
  }
  bool i64(std::int64_t &v) {
    std::uint64_t x;
    if (!u64(x))
      return false;
    v = static_cast<std::int64_t>(x);
    return true;
  }
  bool boolean(bool &v) {
    std::uint8_t x;
    if (!u8(x))
      return false;
    if (x > 1) {
      error = "portable codec: invalid bool";
      return false;
    }
    v = x != 0;
    return true;
  }
  bool str(std::string &v) {
    std::uint32_t n;
    if (!u32(n))
      return false;
    if (n > limits.max_string_bytes) {
      error = "portable codec: string limit";
      return false;
    }
    std::span<const std::uint8_t> s;
    if (!take(n, s))
      return false;
    v.assign(reinterpret_cast<const char *>(s.data()), s.size());
    return true;
  }
  bool bytes(std::vector<std::uint8_t> &v) {
    std::uint32_t n;
    if (!u32(n))
      return false;
    if (n > limits.max_bytes) {
      error = "portable codec: byte limit";
      return false;
    }
    std::span<const std::uint8_t> s;
    if (!take(n, s))
      return false;
    v.assign(s.begin(), s.end());
    return true;
  }
  bool count(std::uint32_t &n) {
    if (!u32(n))
      return false;
    if (n > limits.max_items) {
      error = "portable codec: item limit";
      return false;
    }
    return true;
  }
  bool schema(schema_version &v) {
    return u16(v.major) && u16(v.minor) && u16(v.patch);
  }
};

inline void write_u32_vec(writer &w, const std::vector<std::uint32_t> &v) {
  w.vec(v, [&](auto x) { w.u32(x); });
}
inline bool read_u32_vec(reader &r, std::vector<std::uint32_t> &v) {
  std::uint32_t n;
  if (!r.count(n))
    return false;
  v.resize(n);
  for (auto &x : v)
    if (!r.u32(x))
      return false;
  return true;
}
inline void write_i64_vec(writer &w, const std::vector<std::int64_t> &v) {
  w.vec(v, [&](auto x) { w.i64(x); });
}
inline bool read_i64_vec(reader &r, std::vector<std::int64_t> &v) {
  std::uint32_t n;
  if (!r.count(n))
    return false;
  v.resize(n);
  for (auto &x : v)
    if (!r.i64(x))
      return false;
  return true;
}
inline void write_u64_vec(writer &w, const std::vector<std::uint64_t> &v) {
  w.vec(v, [&](auto x) { w.u64(x); });
}
inline bool read_u64_vec(reader &r, std::vector<std::uint64_t> &v) {
  std::uint32_t n;
  if (!r.count(n))
    return false;
  v.resize(n);
  for (auto &x : v)
    if (!r.u64(x))
      return false;
  return true;
}
} // namespace codec_detail

[[nodiscard]] inline std::expected<std::vector<std::uint8_t>, codec_error>
encode_portable(const portable_module &m) {
  using namespace codec_detail;
  if (!m.structurally_complete())
    return std::unexpected(codec_error{"portable codec: incomplete module"});
  writer w;
  w.out.insert(w.out.end(), {'L', 'P', 'M', 'D'});
  w.u16(1);
  w.schema(m.schema);
  w.u32(m.declared_capabilities.bits);
  w.str(m.manifest.producer);
  w.schema(m.manifest.producer_version);
  w.str(m.manifest.source_language);
  w.u8(m.manifest.digest_len);
  w.out.insert(w.out.end(), m.manifest.semantic_digest.begin(),
               m.manifest.semantic_digest.end());
  w.vec(m.imports, [&](const auto &x) {
    w.str(x.module);
    w.str(x.symbol);
    w.str(x.signature_str);
    w.schema(x.abi);
    w.boolean(x.required);
  });
  w.vec(m.exports, [&](const auto &x) {
    w.str(x.symbol);
    w.u32(x.function_index);
    w.str(x.signature_str);
  });
  w.vec(m.globals, [&](const auto &x) {
    w.str(x.name);
    w.str(x.type_str);
    w.u32(x.const_index);
    w.boolean(x.mutable_);
  });
  w.u32(static_cast<std::uint32_t>(m.constants.size()));
  for (std::size_t i = 0; i < m.constants.size(); ++i) {
    w.str(m.constants.types[i]);
    w.bytes(m.constants.data[i]);
  }
  w.vec(m.functions, [&](const auto &fn) {
    w.str(fn.function_name);
    w.u8(static_cast<std::uint8_t>(fn.source_stage));
    w.schema(fn.schema);
    w.vec(fn.strings, [&](const auto &s) { w.str(s); });
    w.vec(fn.values, [&](const auto &v) {
      w.u32(v.id);
      w.str(v.type_str);
    });
    w.vec(fn.ops, [&](const auto &op) {
      w.u32(op.id);
      w.str(op.domain);
      w.str(op.name);
      write_u32_vec(w, op.operand_ids);
      write_u32_vec(w, op.result_ids);
      w.u32(op.block_id);
      w.u32(op.region_id);
      w.schema(op.op_schema);
      w.boolean(op.structured_for.has_value());
      if (op.structured_for) {
        auto &a = *op.structured_for;
        w.u8(a.rank);
        w.boolean(a.is_parallel);
        write_i64_vec(w, a.lower_bounds);
        write_i64_vec(w, a.upper_bounds);
        write_i64_vec(w, a.steps);
        write_u32_vec(w, a.tile_sizes);
      }
      w.boolean(op.memref.has_value());
      if (op.memref) {
        auto &a = *op.memref;
        w.u8(a.rank);
        w.str(a.element_kind);
        w.u8(a.elem_bits);
        write_u64_vec(w, a.shape);
        write_i64_vec(w, a.strides);
      }
      w.boolean(op.constant.has_value());
      if (op.constant) {
        auto &a = *op.constant;
        w.u8(a.kind);
        w.i64(a.integer);
        w.u64(std::bit_cast<std::uint64_t>(a.floating_point));
        w.boolean(a.boolean);
      }
      w.boolean(op.branch.has_value());
      if (op.branch)
        w.u32(op.branch->target_block_id);
      w.boolean(op.branch_cond.has_value());
      if (op.branch_cond) {
        w.u32(op.branch_cond->true_block_id);
        w.u32(op.branch_cond->false_block_id);
      }
      w.boolean(op.compare.has_value());
      if (op.compare) {
        w.u32(op.compare->predicate_idx);
        w.boolean(op.compare->ordered);
      }
      w.boolean(op.guard.has_value());
      if (op.guard) {
        auto &a = *op.guard;
        w.u32(a.guard_kind_idx);
        w.u32(a.policy_idx);
        w.u32(a.diag_code_idx);
        w.u32(a.source_span_idx);
      }
      w.boolean(op.trap.has_value());
      if (op.trap) {
        w.u32(op.trap->trap_kind_idx);
        w.u32(op.trap->diag_code_idx);
      }
      w.boolean(op.cleanup.has_value());
      if (op.cleanup)
        write_u32_vec(w, op.cleanup->cleanup_ids);
      w.boolean(op.transaction.has_value());
      if (op.transaction) {
        auto &a = *op.transaction;
        w.u32(a.isolation_idx);
        w.u16(a.retry);
        w.u32(a.replay_idx);
        w.u32(a.conflict_idx);
        w.u32(a.partial_idx);
        w.u32(a.durability_idx);
        w.u32(a.distribution_idx);
        w.u32(a.coordinator_idx);
      }
    });
    w.vec(fn.blocks, [&](const auto &b) {
      w.u32(b.id);
      write_u32_vec(w, b.op_ids);
      write_u32_vec(w, b.arg_ids);
    });
    w.vec(fn.regions, [&](const auto &g) {
      w.u32(g.id);
      write_u32_vec(w, g.block_ids);
      write_u32_vec(w, g.arg_ids);
    });
    write_u32_vec(w, fn.entry_block_ids);
  });
  return std::move(w.out);
}

[[nodiscard]] inline std::expected<portable_module, codec_error>
decode_portable(std::span<const std::uint8_t> bytes, codec_limits limits = {},
                bool verify = true) {
  using namespace codec_detail;
  if (bytes.size() > limits.max_bytes)
    return std::unexpected(codec_error{"portable codec: payload limit"});
  reader r{bytes, limits, {}};
  std::span<const std::uint8_t> magic;
  if (!r.take(4, magic) ||
      std::string_view(reinterpret_cast<const char *>(magic.data()), 4) !=
          "LPMD")
    return std::unexpected(codec_error{"portable codec: bad magic"});
  std::uint16_t version;
  if (!r.u16(version) || version != 1)
    return std::unexpected(codec_error{"portable codec: unsupported version"});
  portable_module m;
  if (!r.schema(m.schema) || !r.u32(m.declared_capabilities.bits) ||
      !r.str(m.manifest.producer) || !r.schema(m.manifest.producer_version) ||
      !r.str(m.manifest.source_language) || !r.u8(m.manifest.digest_len))
    return std::unexpected(codec_error{r.error});
  if (m.manifest.digest_len > m.manifest.semantic_digest.size())
    return std::unexpected(
        codec_error{"portable codec: invalid digest length"});
  std::span<const std::uint8_t> digest;
  if (!r.take(m.manifest.semantic_digest.size(), digest))
    return std::unexpected(codec_error{r.error});
  std::copy(digest.begin(), digest.end(), m.manifest.semantic_digest.begin());
  auto read_many = [&](auto &xs, auto &&fn) -> bool {
    std::uint32_t n;
    if (!r.count(n))
      return false;
    xs.resize(n);
    for (auto &x : xs)
      if (!fn(x))
        return false;
    return true;
  };
  if (!read_many(m.imports,
                 [&](auto &x) {
                   return r.str(x.module) && r.str(x.symbol) &&
                          r.str(x.signature_str) && r.schema(x.abi) &&
                          r.boolean(x.required);
                 }) ||
      !read_many(m.exports,
                 [&](auto &x) {
                   return r.str(x.symbol) && r.u32(x.function_index) &&
                          r.str(x.signature_str);
                 }) ||
      !read_many(m.globals, [&](auto &x) {
        return r.str(x.name) && r.str(x.type_str) && r.u32(x.const_index) &&
               r.boolean(x.mutable_);
      }))
    return std::unexpected(codec_error{r.error});
  std::uint32_t nc;
  if (!r.count(nc))
    return std::unexpected(codec_error{r.error});
  m.constants.types.resize(nc);
  m.constants.data.resize(nc);
  for (std::uint32_t i = 0; i < nc; ++i)
    if (!r.str(m.constants.types[i]) || !r.bytes(m.constants.data[i]))
      return std::unexpected(codec_error{r.error});
  if (!read_many(m.functions, [&](auto &fn) -> bool {
        std::uint8_t stage_value;
        if (!r.str(fn.function_name) || !r.u8(stage_value) ||
            !r.schema(fn.schema))
          return false;
        fn.source_stage = static_cast<ir::stage>(stage_value);
        if (!read_many(fn.strings, [&](auto &s) { return r.str(s); }) ||
            !read_many(fn.values, [&](auto &v) {
              return r.u32(v.id) && r.str(v.type_str);
            }))
          return false;
        if (!read_many(fn.ops, [&](auto &op) -> bool {
              if (!r.u32(op.id) || !r.str(op.domain) || !r.str(op.name) ||
                  !read_u32_vec(r, op.operand_ids) ||
                  !read_u32_vec(r, op.result_ids) || !r.u32(op.block_id) ||
                  !r.u32(op.region_id) || !r.schema(op.op_schema))
                return false;
              bool has;
              if (!r.boolean(has))
                return false;
              if (has) {
                op.structured_for.emplace();
                auto &a = *op.structured_for;
                if (!r.u8(a.rank) || !r.boolean(a.is_parallel) ||
                    !read_i64_vec(r, a.lower_bounds) ||
                    !read_i64_vec(r, a.upper_bounds) ||
                    !read_i64_vec(r, a.steps) || !read_u32_vec(r, a.tile_sizes))
                  return false;
              }
              if (!r.boolean(has))
                return false;
              if (has) {
                op.memref.emplace();
                auto &a = *op.memref;
                if (!r.u8(a.rank) || !r.str(a.element_kind) ||
                    !r.u8(a.elem_bits) || !read_u64_vec(r, a.shape) ||
                    !read_i64_vec(r, a.strides))
                  return false;
              }
              if (!r.boolean(has))
                return false;
              if (has) {
                op.constant.emplace();
                auto &a = *op.constant;
                std::uint64_t floating_bits = 0;
                if (!r.u8(a.kind) || !r.i64(a.integer) || !r.u64(floating_bits) ||
                    !r.boolean(a.boolean))
                  return false;
                a.floating_point = std::bit_cast<double>(floating_bits);
              }
              if (!r.boolean(has))
                return false;
              if (has) {
                op.branch.emplace();
                if (!r.u32(op.branch->target_block_id))
                  return false;
              }
              if (!r.boolean(has))
                return false;
              if (has) {
                op.branch_cond.emplace();
                if (!r.u32(op.branch_cond->true_block_id) ||
                    !r.u32(op.branch_cond->false_block_id))
                  return false;
              }
              if (!r.boolean(has))
                return false;
              if (has) {
                op.compare.emplace();
                if (!r.u32(op.compare->predicate_idx) ||
                    !r.boolean(op.compare->ordered))
                  return false;
              }
              if (!r.boolean(has))
                return false;
              if (has) {
                op.guard.emplace();
                auto &a = *op.guard;
                if (!r.u32(a.guard_kind_idx) || !r.u32(a.policy_idx) ||
                    !r.u32(a.diag_code_idx) || !r.u32(a.source_span_idx))
                  return false;
              }
              if (!r.boolean(has))
                return false;
              if (has) {
                op.trap.emplace();
                if (!r.u32(op.trap->trap_kind_idx) ||
                    !r.u32(op.trap->diag_code_idx))
                  return false;
              }
              if (!r.boolean(has))
                return false;
              if (has) {
                op.cleanup.emplace();
                if (!read_u32_vec(r, op.cleanup->cleanup_ids))
                  return false;
              }
              if (!r.boolean(has))
                return false;
              if (has) {
                op.transaction.emplace();
                auto &a = *op.transaction;
                if (!r.u32(a.isolation_idx) || !r.u16(a.retry) ||
                    !r.u32(a.replay_idx) || !r.u32(a.conflict_idx) ||
                    !r.u32(a.partial_idx) || !r.u32(a.durability_idx) ||
                    !r.u32(a.distribution_idx) || !r.u32(a.coordinator_idx))
                  return false;
              }
              return true;
            }))
          return false;
        if (!read_many(fn.blocks,
                       [&](auto &b) {
                         return r.u32(b.id) && read_u32_vec(r, b.op_ids) &&
                                read_u32_vec(r, b.arg_ids);
                       }) ||
            !read_many(fn.regions,
                       [&](auto &g) {
                         return r.u32(g.id) && read_u32_vec(r, g.block_ids) &&
                                read_u32_vec(r, g.arg_ids);
                       }) ||
            !read_u32_vec(r, fn.entry_block_ids))
          return false;
        return true;
      }))
    return std::unexpected(codec_error{r.error});
  if (!r.in.empty())
    return std::unexpected(codec_error{"portable codec: trailing bytes"});
  if (!m.structurally_complete())
    return std::unexpected(
        codec_error{"portable codec: incomplete decoded module"});
  if (verify) {
    const auto report = verify_portable(m);
    if (!report.ok)
      return std::unexpected(
          codec_error{"portable codec: decoded module failed verification"});
  }
  return m;
}
} // namespace lithe::ir::portable
