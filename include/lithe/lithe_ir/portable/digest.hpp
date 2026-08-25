#pragma once

// =============================================================================
// lithe_ir/portable/digest.hpp — canonical encoding + semantic digest
//
// Namespace: lithe::ir::portable
//
// canonical_encode(module) → deterministic byte sequence (digest preimage):
//   • Sections emitted in fixed id order.
//   • Values by canonical id order.
//   • Ops by (block order, in-block order).
//   • String table content-sorted (lexicographic) — insertion order removed.
//   • Fixed little-endian widths throughout.
//   • No unordered-container iteration reaches output.
//
// semantic_digest(module, alg) → sha256 / null hash of canonical_encode.
//   Distinct from the payload integrity digest in binary_ir_envelope.
//   The semantic digest identifies the *program*; the payload digest identifies
//   the *serialized artifact*.
//
// Built on generic containers::canonical_writer + containers::content_digest.
//
// Pure wire-form: no codegen include.
//
// No virtual, no macros.  Header-only C++23.
// =============================================================================

#include <array>
#include <bit>
#include <cstdint>
#include <span>
#include <vector>

#include "containers/canonical_codec.hpp"  // canonical_writer, content_digest
#include "../security_envelope.hpp"  // digest_algorithm
#include "module.hpp"                // portable_module

namespace lithe::ir::portable {
    // =============================================================================
    // canonical_encode — deterministic preimage bytes for a portable_module
    // =============================================================================

    [[nodiscard]] inline std::vector<std::uint8_t>
    canonical_encode(const portable_module& mod) {
        containers::canonical_writer w;

        // -------------------------------------------------------------------------
        // Section 0: module header
        // -------------------------------------------------------------------------
        w.write_u16(mod.schema.major);
        w.write_u16(mod.schema.minor);
        w.write_u16(mod.schema.patch);
        w.write_u32(static_cast<std::uint32_t>(mod.functions.size()));
        w.write_u32(static_cast<std::uint32_t>(mod.imports.size()));
        w.write_u32(static_cast<std::uint32_t>(mod.exports.size()));
        w.write_u32(static_cast<std::uint32_t>(mod.globals.size()));
        w.write_u32(static_cast<std::uint32_t>(mod.constants.size()));
        w.write_u32(mod.declared_capabilities.bits);

        // -------------------------------------------------------------------------
        // Section 1: manifest (producer, source_language interned)
        // -------------------------------------------------------------------------
        const auto producer_idx = w.intern_string(mod.manifest.producer);
        const auto sourcelang_idx = w.intern_string(mod.manifest.source_language);

        // -------------------------------------------------------------------------
        // Section 2: imports
        // -------------------------------------------------------------------------
        std::vector<std::uint32_t> imp_mod_idx, imp_sym_idx, imp_sig_idx;
        imp_mod_idx.reserve(mod.imports.size());
        imp_sym_idx.reserve(mod.imports.size());
        imp_sig_idx.reserve(mod.imports.size());
        for (const auto& imp : mod.imports) {
            imp_mod_idx.push_back(w.intern_string(imp.module));
            imp_sym_idx.push_back(w.intern_string(imp.symbol));
            imp_sig_idx.push_back(w.intern_string(imp.signature_str));
        }

        // -------------------------------------------------------------------------
        // Section 3: exports
        // -------------------------------------------------------------------------
        std::vector<std::uint32_t> exp_sym_idx, exp_sig_idx;
        exp_sym_idx.reserve(mod.exports.size());
        exp_sig_idx.reserve(mod.exports.size());
        for (const auto& ex : mod.exports) {
            exp_sym_idx.push_back(w.intern_string(ex.symbol));
            exp_sig_idx.push_back(w.intern_string(ex.signature_str));
        }

        // -------------------------------------------------------------------------
        // Section 4: globals
        // -------------------------------------------------------------------------
        std::vector<std::uint32_t> g_name_idx, g_type_idx;
        g_name_idx.reserve(mod.globals.size());
        g_type_idx.reserve(mod.globals.size());
        for (const auto& g : mod.globals) {
            g_name_idx.push_back(w.intern_string(g.name));
            g_type_idx.push_back(w.intern_string(g.type_str));
        }

        // -------------------------------------------------------------------------
        // Section 5: per-function wire ops (all strings interned)
        // For each function, for each op (in canonical id order), for each value.
        // -------------------------------------------------------------------------
        struct fn_interned {
            std::vector<std::uint32_t> fn_name_idx;
            std::vector<std::vector<std::uint32_t>> op_domain_idx;
            std::vector<std::vector<std::uint32_t>> op_name_idx;
            std::vector<std::vector<std::uint32_t>> val_type_idx;
        };

        std::vector<fn_interned> fn_data(mod.functions.size());
        for (std::size_t fi = 0; fi < mod.functions.size(); ++fi) {
            const auto& fn = mod.functions[fi];
            fn_data[fi].fn_name_idx = {w.intern_string(fn.function_name)};
            fn_data[fi].op_domain_idx.resize(fn.ops.size());
            fn_data[fi].op_name_idx.resize(fn.ops.size());
            for (std::size_t oi = 0; oi < fn.ops.size(); ++oi) {
                fn_data[fi].op_domain_idx[oi] = {w.intern_string(fn.ops[oi].domain)};
                fn_data[fi].op_name_idx[oi] = {w.intern_string(fn.ops[oi].name)};
            }
            fn_data[fi].val_type_idx.resize(fn.values.size());
            for (std::size_t vi = 0; vi < fn.values.size(); ++vi)
                fn_data[fi].val_type_idx[vi] = {w.intern_string(fn.values[vi].type_str)};
        }

        // Finalize string table — sorts content, builds remap — BEFORE any write_string_ref
        w.finalize_string_table();

        // -------------------------------------------------------------------------
        // Emit: manifest
        // -------------------------------------------------------------------------
        w.write_string_ref(producer_idx);
        w.write_string_ref(sourcelang_idx);

        // -------------------------------------------------------------------------
        // Emit: imports
        // -------------------------------------------------------------------------
        for (std::size_t i = 0; i < mod.imports.size(); ++i) {
            w.write_string_ref(imp_mod_idx[i]);
            w.write_string_ref(imp_sym_idx[i]);
            w.write_string_ref(imp_sig_idx[i]);
            w.write_u16(mod.imports[i].abi.major);
            w.write_u16(mod.imports[i].abi.minor);
            w.write_u16(mod.imports[i].abi.patch);
            w.write_bool(mod.imports[i].required);
        }

        // -------------------------------------------------------------------------
        // Emit: exports
        // -------------------------------------------------------------------------
        for (std::size_t i = 0; i < mod.exports.size(); ++i) {
            w.write_string_ref(exp_sym_idx[i]);
            w.write_u32(mod.exports[i].function_index);
            w.write_string_ref(exp_sig_idx[i]);
        }

        // -------------------------------------------------------------------------
        // Emit: globals
        // -------------------------------------------------------------------------
        for (std::size_t i = 0; i < mod.globals.size(); ++i) {
            w.write_string_ref(g_name_idx[i]);
            w.write_string_ref(g_type_idx[i]);
            w.write_u32(mod.globals[i].const_index);
            w.write_bool(mod.globals[i].mutable_);
        }

        // -------------------------------------------------------------------------
        // Emit: constants
        // -------------------------------------------------------------------------
        w.write_u32(static_cast<std::uint32_t>(mod.constants.size()));
        for (std::size_t i = 0; i < mod.constants.size(); ++i) {
            // Type already interned — write as string ref is not ideal here since
            // the types vector may have different indices; emit a counted type string.
            const std::string& ts = mod.constants.types[i];
            w.write_u32(static_cast<std::uint32_t>(ts.size()));
            for (unsigned char c : ts) w.write_u8(c);
            const auto& d = mod.constants.data[i];
            w.write_u32(static_cast<std::uint32_t>(d.size()));
            for (std::uint8_t b : d) w.write_u8(b);
        }

        // -------------------------------------------------------------------------
        // Emit: functions (canonical op order = op id order since freeze assigns
        // canonical dense ids in structural walk order)
        // -------------------------------------------------------------------------
        for (std::size_t fi = 0; fi < mod.functions.size(); ++fi) {
            const auto& fn = mod.functions[fi];
            w.write_string_ref(fn_data[fi].fn_name_idx[0]);
            w.write_u32(static_cast<std::uint32_t>(fn.values.size()));
            w.write_u32(static_cast<std::uint32_t>(fn.ops.size()));
            w.write_u32(static_cast<std::uint32_t>(fn.blocks.size()));
            w.write_u32(static_cast<std::uint32_t>(fn.regions.size()));

            // Values (by canonical id)
            for (std::size_t vi = 0; vi < fn.values.size(); ++vi) {
                w.write_u32(fn.values[vi].id);
                w.write_string_ref(fn_data[fi].val_type_idx[vi][0]);
            }

            // Ops (in op table order = canonical order)
            for (std::size_t oi = 0; oi < fn.ops.size(); ++oi) {
                const auto& op = fn.ops[oi];
                w.write_u32(op.id);
                w.write_string_ref(fn_data[fi].op_domain_idx[oi][0]);
                w.write_string_ref(fn_data[fi].op_name_idx[oi][0]);
                w.write_u32(static_cast<std::uint32_t>(op.operand_ids.size()));
                for (std::uint32_t oid : op.operand_ids) w.write_u32(oid);
                w.write_u32(static_cast<std::uint32_t>(op.result_ids.size()));
                for (std::uint32_t rid : op.result_ids) w.write_u32(rid);
                w.write_u32(op.block_id);
                w.write_u32(op.region_id);

                // Attrs
                const std::uint8_t has_for = op.structured_for.has_value() ? 1u : 0u;
                const std::uint8_t has_memref = op.memref.has_value() ? 1u : 0u;
                const std::uint8_t has_constant = op.constant.has_value() ? 1u : 0u;
                const std::uint8_t has_branch = op.branch.has_value() ? 1u : 0u;
                const std::uint8_t has_branch_cond = op.branch_cond.has_value() ? 1u : 0u;
                const std::uint8_t has_compare = op.compare.has_value() ? 1u : 0u;
                const std::uint8_t has_guard = op.guard.has_value() ? 1u : 0u;
                const std::uint8_t has_trap = op.trap.has_value() ? 1u : 0u;
                const std::uint8_t has_cleanup = op.cleanup.has_value() ? 1u : 0u;
                const std::uint8_t has_tx = op.transaction.has_value() ? 1u : 0u;

                w.write_u8(has_for);
                if (has_for) {
                    const auto& fa = *op.structured_for;
                    w.write_u8(fa.rank);
                    w.write_bool(fa.is_parallel);
                    for (std::size_t i = 0; i < fa.lower_bounds.size(); ++i) {
                        w.write_i64(fa.lower_bounds[i]);
                        w.write_i64(fa.upper_bounds[i]);
                        w.write_i64(fa.steps[i]);
                        w.write_u32(i < fa.tile_sizes.size() ? fa.tile_sizes[i] : 0u);
                    }
                }
                w.write_u8(has_memref);
                if (has_memref) {
                    const auto& md = *op.memref;
                    w.write_u8(md.rank);
                    w.write_u8(md.elem_bits);
                    w.write_u32(static_cast<std::uint32_t>(md.element_kind.size()));
                    for (unsigned char c : md.element_kind) w.write_u8(c);
                    for (std::uint64_t sh : md.shape) w.write_u64(sh);
                    for (std::int64_t st : md.strides) w.write_i64(st);
                }
                w.write_u8(has_constant);
                if (has_constant) {
                    const auto& ca = *op.constant;
                    w.write_u8(ca.kind);
                    w.write_i64(ca.integer);
                    w.write_u64(std::bit_cast<std::uint64_t>(ca.floating_point));
                    w.write_bool(ca.boolean);
                }
                w.write_u8(has_branch);
                if (has_branch) {
                    w.write_u32(op.branch->target_block_id);
                }
                w.write_u8(has_branch_cond);
                if (has_branch_cond) {
                    w.write_u32(op.branch_cond->true_block_id);
                    w.write_u32(op.branch_cond->false_block_id);
                }
                w.write_u8(has_compare);
                if (has_compare) {
                    w.write_u32(op.compare->predicate_idx);
                    w.write_bool(op.compare->ordered);
                }
                w.write_u8(has_guard);
                if (has_guard) {
                    const auto& ga = *op.guard;
                    w.write_u32(ga.guard_kind_idx);
                    w.write_u32(ga.policy_idx);
                    w.write_u32(ga.diag_code_idx);
                    w.write_u32(ga.source_span_idx);
                }
                w.write_u8(has_trap);
                if (has_trap) {
                    w.write_u32(op.trap->trap_kind_idx);
                    w.write_u32(op.trap->diag_code_idx);
                }
                w.write_u8(has_cleanup);
                if (has_cleanup) {
                    const auto& cla = *op.cleanup;
                    w.write_u32(static_cast<std::uint32_t>(cla.cleanup_ids.size()));
                    for (std::uint32_t cid : cla.cleanup_ids) w.write_u32(cid);
                }
                w.write_u8(has_tx);
                if (has_tx) {
                    const auto& txa = *op.transaction;
                    w.write_u32(txa.isolation_idx);
                    w.write_u16(txa.retry);
                    w.write_u32(txa.replay_idx);
                    w.write_u32(txa.conflict_idx);
                    w.write_u32(txa.partial_idx);
                    w.write_u32(txa.durability_idx);
                    w.write_u32(txa.distribution_idx);
                    w.write_u32(txa.coordinator_idx);
                }
            }

            // Blocks
            for (const auto& blk : fn.blocks) {
                w.write_u32(blk.id);
                w.write_u32(static_cast<std::uint32_t>(blk.op_ids.size()));
                for (std::uint32_t oid : blk.op_ids) w.write_u32(oid);
                w.write_u32(static_cast<std::uint32_t>(blk.arg_ids.size()));
                for (std::uint32_t aid : blk.arg_ids) w.write_u32(aid);
            }

            // Regions
            for (const auto& reg : fn.regions) {
                w.write_u32(reg.id);
                w.write_u32(static_cast<std::uint32_t>(reg.block_ids.size()));
                for (std::uint32_t bid : reg.block_ids) w.write_u32(bid);
            }

            // Entry blocks
            w.write_u32(static_cast<std::uint32_t>(fn.entry_block_ids.size()));
            for (std::uint32_t eid : fn.entry_block_ids) w.write_u32(eid);
        }

        return w.emit();
    }

    // =============================================================================
    // semantic_digest — content hash of canonical_encode(module)
    //
    // Uses SHA-256 by default.  alg parameter maps digest_algorithm enum to
    // the appropriate policy; only sha256 and none are supported in this milestone.
    //
    // This is the *semantic* digest (program identity).
    // The *payload* digest in binary_ir_envelope is the *artifact integrity* digest.
    // They are intentionally distinct: semantic digest stays stable across re-encodings;
    // payload digest changes whenever the wire bytes change.
    // =============================================================================

    [[nodiscard]] inline std::array<std::uint8_t, 64>
    semantic_digest(const portable_module& mod,
                    digest_algorithm alg = digest_algorithm::sha256) {
        const auto preimage = canonical_encode(mod);
        const std::span<const std::uint8_t> span{preimage.data(), preimage.size()};

        std::array<std::uint8_t, 64> out{};
        const std::size_t n = digest_size_bytes(alg);

        switch (alg) {
        case digest_algorithm::sha256: {
            const auto h = containers::content_digest<containers::sha256_digest_policy>(span);
            for (std::size_t i = 0; i < h.size() && i < out.size(); ++i) out[i] = h[i];
            break;
        }
        default: {
            // none / unsupported — zero-fill
            break;
        }
        }
        (void)n;
        return out;
    }

    // Convenience: compute and store semantic digest into the module manifest.
    inline void stamp_semantic_digest(portable_module& mod,
                                      digest_algorithm alg = digest_algorithm::sha256) {
        mod.manifest.semantic_digest = semantic_digest(mod, alg);
        mod.manifest.digest_len = digest_size_bytes(alg);
    }
} // namespace lithe::ir::portable
