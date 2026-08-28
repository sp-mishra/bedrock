#pragma once

// Local, single-process Lithe orchestration. This layer composes the static
// execution engine, managed runtime, portable optimizer, and durable store.
// Distributed placement/retries/transport remain outside Lithe.

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "lithe_engine.hpp"
#include "lithe_codegen_hl_passes.hpp"
#include "lithe_ir/portable/bridge.hpp"
#include "lithe_ir/portable/codec.hpp"
#include "lithe_ir/portable/opt/levels.hpp"
#include "lithe_execution/store/store.hpp"
#include "lithe_rt/engine_integration.hpp"

namespace lithe {

enum class local_engine_stage : std::uint8_t {
    validation,
    optimization,
    persistence,
    compatibility,
    thaw,
    managed_compile,
    backend_compile,
    binding,
    invocation,
};

struct local_engine_error {
    local_engine_stage stage = local_engine_stage::validation;
    std::string detail;
};

struct portable_cache_config {
    ir::portable::opt::portable_level level =
        ir::portable::opt::portable_level::balanced;
    ir::portable::opt::semantic_policy policy{};
    execution::store::pipeline_id_record pipeline{"portable.balanced"};
    execution::store::pipeline_version_record pipeline_version{1, 0};
    execution::store::abi_fingerprint abi{};
    execution::store::security_policy_id security{};
    execution::store::host_profile host{};
    std::string producer = "lithe.local_engine";
};

struct portable_cache_result {
    ir::portable::portable_module module;
    execution::store::artifact_key key;
    bool cache_hit = false;
    std::optional<ir::portable::opt::pass_record> optimization_record;
};

struct target_cache_config {
    execution::store::backend_id backend{};
    execution::store::backend_version backend_version{};
    execution::store::backend_pipeline_version pipeline_version{};
    execution::store::capability_fingerprint target_capabilities{};
    execution::store::specialization_fingerprint specialization{};
    execution::store::symbol_resolution_fingerprint external_symbols{};
    execution::store::compatibility_manifest compatibility{};
    std::string producer = "lithe.local_engine.target";
};

template <class Artifact>
struct target_cache_result {
    Artifact artifact;
    execution::store::artifact_key key;
    bool cache_hit = false;
};

namespace local_engine_detail {
using managed_cache_key = std::array<std::uint8_t, 32>;

struct managed_cache_key_hash {
    [[nodiscard]] std::size_t
    operator()(const managed_cache_key &key) const noexcept {
        std::size_t result = 0;
        constexpr std::size_t bytes =
            std::min(sizeof(result), managed_cache_key{}.size());
        for (std::size_t i = 0; i < bytes; ++i)
            result |= static_cast<std::size_t>(key[i]) << (i * 8);
        return result;
    }
};

inline void write_string(containers::canonical_writer &writer,
                         const std::string_view value) {
    writer.write_u32(static_cast<std::uint32_t>(value.size()));
    for (const unsigned char byte : value) writer.write_u8(byte);
}

inline void write_operand(containers::canonical_writer &writer,
                          const codegen::allocated_operand &operand) {
    writer.write_u8(static_cast<std::uint8_t>(operand.type));
    std::visit(
        [&](const auto &value) {
            using T = std::remove_cvref_t<decltype(value)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
                return;
            } else if constexpr (std::is_same_v<T, codegen::preg>) {
                writer.write_u16(value.id);
                write_string(writer, value.name);
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                writer.write_i64(value);
            } else if constexpr (std::is_same_v<T, double>) {
                writer.write_u64(std::bit_cast<std::uint64_t>(value));
            } else if constexpr (std::is_same_v<T, std::string>) {
                write_string(writer, value);
            } else if constexpr (std::is_same_v<T, codegen::spill_slot>) {
                writer.write_u32(value.id);
                writer.write_u32(value.size);
                writer.write_u32(value.alignment);
                writer.write_i64(value.frame_offset);
            } else if constexpr (std::is_same_v<T, codegen::memory_operand>) {
                const auto &address = value.address;
                writer.write_u8(static_cast<std::uint8_t>(address.kind));
                writer.write_bool(address.base.has_value());
                if (address.base) {
                    writer.write_u16(address.base->id);
                    write_string(writer, address.base->name);
                }
                writer.write_bool(address.index.has_value());
                if (address.index) {
                    writer.write_u16(address.index->id);
                    write_string(writer, address.index->name);
                }
                writer.write_i32(address.scale);
                writer.write_i64(address.displacement);
                writer.write_bool(address.referenced_frame_object.has_value());
                if (address.referenced_frame_object)
                    writer.write_u32(address.referenced_frame_object->value);
                writer.write_bool(address.referenced_symbol.has_value());
                if (address.referenced_symbol)
                    write_string(writer, *address.referenced_symbol);
            } else {
                writer.write_u32(value);
            }
        },
        operand.value);
}

[[nodiscard]] inline managed_cache_key physical_fingerprint(
    const codegen::mir::physical_mir_function &ir,
    const rt::managed_signature_descriptor &signature) {
    containers::canonical_writer writer;
    write_string(writer, ir.function.name);
    writer.write_u32(ir.function.cfg.entry_block);
    writer.write_u16(signature.version);
    writer.write_u8(static_cast<std::uint8_t>(signature.result));
    writer.write_u8(signature.arity);
    for (std::size_t i = 0; i < signature.arity; ++i)
        writer.write_u8(static_cast<std::uint8_t>(signature.arguments[i]));
    writer.write_u32(static_cast<std::uint32_t>(ir.function.blocks.size()));
    for (const auto &block : ir.function.blocks) {
        writer.write_u32(block.id);
        write_string(writer, block.name);
        writer.write_u32(static_cast<std::uint32_t>(block.predecessors.size()));
        for (const auto id : block.predecessors) writer.write_u32(id);
        writer.write_u32(static_cast<std::uint32_t>(block.successors.size()));
        for (const auto id : block.successors) writer.write_u32(id);
        writer.write_u32(static_cast<std::uint32_t>(block.instructions.size()));
        for (const auto &instruction : block.instructions) {
            writer.write_u32(instruction.id);
            writer.write_u16(static_cast<std::uint16_t>(instruction.op));
            writer.write_u32(static_cast<std::uint32_t>(instruction.defs.size()));
            for (const auto &operand : instruction.defs)
                write_operand(writer, operand);
            writer.write_u32(static_cast<std::uint32_t>(instruction.uses.size()));
            for (const auto &operand : instruction.uses)
                write_operand(writer, operand);
            writer.write_u32(static_cast<std::uint32_t>(instruction.ssa_defs.size()));
            for (const auto value : instruction.ssa_defs)
                writer.write_u64(value.id);
            writer.write_u32(static_cast<std::uint32_t>(instruction.ssa_uses.size()));
            for (const auto value : instruction.ssa_uses)
                writer.write_u64(value.id);
            writer.write_bool(instruction.abstract_operation.has_value());
            if (instruction.abstract_operation) {
                write_string(writer, instruction.abstract_operation->domain);
                write_string(writer, instruction.abstract_operation->name);
            }
            std::vector<std::pair<std::string, std::string>> attributes(
                instruction.operation_attributes.begin(),
                instruction.operation_attributes.end());
            std::sort(attributes.begin(), attributes.end());
            writer.write_u32(static_cast<std::uint32_t>(attributes.size()));
            for (const auto &[name, value] : attributes) {
                write_string(writer, name);
                write_string(writer, value);
            }
            writer.write_bool(instruction.result_type_id.has_value());
            if (instruction.result_type_id)
                writer.write_u64(*instruction.result_type_id);
        }
    }
    const auto bytes = writer.emit();
    return containers::content_digest<containers::sha256_digest_policy>(
        std::span<const std::uint8_t>{bytes.data(), bytes.size()});
}

[[nodiscard]] inline execution::store::policy_fingerprint
fingerprint_policy(const ir::portable::opt::semantic_policy &policy) {
    containers::canonical_writer writer;
    writer.write_u8(static_cast<std::uint8_t>(policy.int_overflow));
    writer.write_u8(static_cast<std::uint8_t>(policy.fp));
    writer.write_bool(policy.preserve_defer);
    writer.write_bool(policy.preserve_exceptions);
    writer.write_bool(policy.preserve_transactions);
    writer.write_bool(policy.preserve_traps);
    writer.write_u8(static_cast<std::uint8_t>(policy.determinism));
    writer.write_bool(policy.paranoid);
    const auto bytes = writer.emit();
    execution::store::policy_fingerprint result;
    result.digest = containers::content_digest<containers::sha256_digest_policy>(
        std::span<const std::uint8_t>{bytes.data(), bytes.size()});
    return result;
}

[[nodiscard]] inline execution::store::optimized_key
make_portable_key(const ir::portable::portable_module &module,
                  const portable_cache_config &config) {
    execution::store::optimized_key key;
    key.semantic_digest = ir::portable::semantic_digest(module);
    key.semantic_digest_len = 32;
    key.ir_schema = module.schema;
    key.abi = config.abi;
    key.pipe_id = config.pipeline;
    key.pipe_ver = config.pipeline_version;
    key.policy = fingerprint_policy(config.policy);
    return key;
}

[[nodiscard]] inline execution::store::compatibility_manifest
make_compatibility(const ir::portable::portable_module &module,
                   const portable_cache_config &config) {
    execution::store::compatibility_manifest compatibility;
    compatibility.ir_schema = module.schema;
    compatibility.abi = config.abi;
    compatibility.required_caps.bits = module.declared_capabilities.bits;
    compatibility.security = config.security;
    for (const auto &import : module.imports) {
        if (!import.required) continue;
        compatibility.ext_syms.push_back({import.symbol, import.signature_str});
    }
    return compatibility;
}

[[nodiscard]] inline std::string first_verification_error(
    const ir::portable::verify_report &report) {
    for (const auto &diagnostic : report.diagnostics) {
        if (diagnostic.level == diag::severity::error)
            return diagnostic.code + ": " + diagnostic.message;
    }
    return "portable verification failed";
}
} // namespace local_engine_detail

template <execution::store::catalog Catalog,
          execution::store::artifact_store BlobStore>
class portable_artifact_cache {
public:
    portable_artifact_cache(Catalog &catalog, BlobStore &blobs,
                            portable_cache_config config = {})
        : catalog_(&catalog), blobs_(&blobs), config_(std::move(config)) {}

    [[nodiscard]] std::expected<portable_cache_result, local_engine_error>
    load_or_optimize(ir::portable::portable_module input) {
        const auto verification = ir::portable::verify_portable(input);
        if (!verification.ok)
            return std::unexpected(local_engine_error{
                local_engine_stage::validation,
                local_engine_detail::first_verification_error(verification)});

        const execution::store::artifact_key key =
            local_engine_detail::make_portable_key(input, config_);
        bool built_here = false;
        std::optional<ir::portable::opt::pass_record> pass_record;

        auto build_record = [&]()
            -> std::expected<execution::store::artifact_record, std::string> {
            built_here = true;
            auto optimized = input;
            auto pipeline = ir::portable::opt::make_pipeline(
                config_.level, config_.policy,
                {config_.pipeline.name},
                {config_.pipeline_version.major, config_.pipeline_version.minor});
            ir::portable::opt::all_providers providers;
            auto run = pipeline.run(optimized, config_.policy, providers);
            if (!run.ok) return std::unexpected("portable optimization failed");

            const auto post_verify = ir::portable::verify_portable(optimized);
            if (!post_verify.ok)
                return std::unexpected(
                    local_engine_detail::first_verification_error(post_verify));

            ir::portable::stamp_semantic_digest(optimized);
            auto encoded = ir::portable::encode_portable(optimized);
            if (!encoded) return std::unexpected(encoded.error().detail);

            execution::store::artifact_record record;
            record.key = key;
            record.kind = execution::store::artifact_kind::optimized_portable;
            record.manifest.produced_from = execution::ir_kind::hl_mir;
            record.manifest.role = execution::artifact_class::metadata_only;
            record.semantic_digest = std::get<execution::store::optimized_key>(key)
                                         .semantic_digest;
            record.semantic_digest_len = 32;
            record.payload = execution::store::inline_payload{std::move(*encoded)};
            record.prov.pipe = config_.pipeline;
            record.prov.pipe_ver = config_.pipeline_version;
            record.prov.producer = config_.producer;
            record.compat = local_engine_detail::make_compatibility(input, config_);
            pass_record = std::move(run.record);
            return record;
        };

        for (std::uint8_t attempt = 0; attempt < 2; ++attempt) {
            auto entry = execution::store::get_or_compile(
                *catalog_, *blobs_, key, build_record);
            if (!entry)
                return std::unexpected(local_engine_error{
                    local_engine_stage::persistence, entry.error().detail});

            const auto compatible = execution::store::check_compatible(
                entry->compat, config_.host, config_.host.active_policy);
            if (!compatible.passed) {
                const auto failed = std::find_if(
                    compatible.clauses.begin(), compatible.clauses.end(),
                    [](const auto &clause) { return !clause.passed; });
                return std::unexpected(local_engine_error{
                    local_engine_stage::compatibility,
                    failed == compatible.clauses.end()
                        ? "artifact compatibility check failed"
                        : failed->detail});
            }

            auto blob = blobs_->get(entry->blob_addr);
            std::optional<ir::portable::portable_module> decoded_module;
            std::string corruption;
            if (!blob) {
                corruption = blob.error().detail;
            } else if (auto decoded = ir::portable::decode_portable(blob->data);
                       !decoded) {
                corruption = decoded.error().detail;
            } else {
                const auto payload_digest =
                    ir::portable::semantic_digest(*decoded);
                if (decoded->manifest.digest_len != 32 ||
                    !std::equal(payload_digest.begin(),
                                payload_digest.begin() + 32,
                                decoded->manifest.semantic_digest.begin())) {
                    corruption = "portable artifact semantic digest mismatch";
                } else {
                    decoded_module.emplace(std::move(*decoded));
                }
            }

            if (decoded_module)
                return portable_cache_result{
                    std::move(*decoded_module), key, !built_here,
                    std::move(pass_record)};

            // Broken metadata/blob pairs are quarantined by removing catalog
            // visibility, then rebuilt once from the verified source module.
            (void)blobs_->erase(entry->blob_addr);
            auto evicted = catalog_->evict(key);
            if (!evicted)
                return std::unexpected(local_engine_error{
                    local_engine_stage::persistence, evicted.error().detail});
            if (attempt != 0)
                return std::unexpected(local_engine_error{
                    local_engine_stage::persistence, std::move(corruption)});
            built_here = false;
            pass_record.reset();
        }

        return std::unexpected(local_engine_error{
            local_engine_stage::persistence,
            "portable artifact recovery exhausted"});
    }

    [[nodiscard]] std::expected<
        std::vector<codegen::hl::hl_mir_function>, local_engine_error>
    freeze_optimize_thaw(
        std::span<const codegen::hl::hl_mir_function *const> functions,
        const ir::portable::module_freeze_options &freeze_options = {},
        const ir::portable::thaw_options &thaw_options = {}) {
        auto frozen = ir::portable::freeze_module(functions, freeze_options);
        if (!frozen)
            return std::unexpected(local_engine_error{
                local_engine_stage::validation, frozen.error().detail});
        auto cached = load_or_optimize(std::move(*frozen));
        if (!cached) return std::unexpected(cached.error());
        auto thawed = ir::portable::thaw_module(cached->module, thaw_options);
        if (!thawed)
            return std::unexpected(local_engine_error{
                local_engine_stage::thaw, thawed.error().detail});
        return std::move(*thawed);
    }

    [[nodiscard]] const portable_cache_config &config() const noexcept {
        return config_;
    }

private:
    Catalog *catalog_;
    BlobStore *blobs_;
    portable_cache_config config_;
};

// Durable target-local artifacts are deliberately provider-driven.  Backends
// supply a safe persist form and codec; live JIT handles and process-local
// pointers cannot satisfy this API because the encoder returns owned bytes and
// every result is reconstructed by the decoder before use.
template <execution::store::catalog Catalog,
          execution::store::artifact_store BlobStore>
class target_artifact_cache {
public:
    target_artifact_cache(Catalog &catalog, BlobStore &blobs,
                          target_cache_config config = {})
        : catalog_(&catalog), blobs_(&blobs), config_(std::move(config)) {}

    template <class Artifact, class CompileFn, class EncodeFn, class DecodeFn>
    [[nodiscard]] std::expected<target_cache_result<Artifact>, local_engine_error>
    load_or_compile(const execution::store::optimized_key &portable_key,
                    CompileFn &&compile_fn, EncodeFn &&encode_fn,
                    DecodeFn &&decode_fn) {
        if (config_.backend.empty())
            return std::unexpected(local_engine_error{
                local_engine_stage::backend_compile,
                "target cache requires a stable backend id"});

        execution::store::executable_key executable;
        executable.base = portable_key;
        executable.backend = config_.backend;
        executable.backend_ver = config_.backend_version;
        executable.target_caps = config_.target_capabilities;
        executable.backend_pipe = config_.pipeline_version;
        executable.spec = config_.specialization;
        executable.ext_syms = config_.external_symbols;
        const execution::store::artifact_key key{executable};
        bool built_here = false;

        auto build_record = [&]()
            -> std::expected<execution::store::artifact_record, std::string> {
            built_here = true;
            auto artifact = std::invoke(compile_fn);
            if (!artifact) return std::unexpected(artifact.error());
            auto encoded = std::invoke(encode_fn, *artifact);
            if (!encoded) return std::unexpected(encoded.error());

            execution::store::artifact_record record;
            record.key = key;
            record.kind = execution::store::artifact_kind::executable;
            record.manifest.produced_from = execution::ir_kind::physical_mir;
            record.manifest.role = execution::artifact_class::binary_object;
            record.semantic_digest = portable_key.semantic_digest;
            record.semantic_digest_len = portable_key.semantic_digest_len;
            record.payload = execution::store::inline_payload{std::move(*encoded)};
            record.prov.pipe = portable_key.pipe_id;
            record.prov.pipe_ver = portable_key.pipe_ver;
            record.prov.backend = config_.backend;
            record.prov.backend_ver = config_.backend_version;
            record.prov.producer = config_.producer;
            record.compat = config_.compatibility;
            return record;
        };

        for (std::uint8_t attempt = 0; attempt < 2; ++attempt) {
            auto entry = execution::store::get_or_compile(
                *catalog_, *blobs_, key, build_record);
            if (!entry)
                return std::unexpected(local_engine_error{
                    local_engine_stage::persistence, entry.error().detail});

            const auto compatible = execution::store::check_compatible(
                entry->compat, host_, host_.active_policy);
            if (!compatible.passed) {
                const auto failed = std::find_if(
                    compatible.clauses.begin(), compatible.clauses.end(),
                    [](const auto &clause) { return !clause.passed; });
                return std::unexpected(local_engine_error{
                    local_engine_stage::compatibility,
                    failed == compatible.clauses.end()
                        ? "target artifact compatibility check failed"
                        : failed->detail});
            }

            auto blob = blobs_->get(entry->blob_addr);
            if (blob) {
                auto decoded = std::invoke(
                    decode_fn,
                    std::span<const std::uint8_t>{blob->data.data(),
                                                  blob->data.size()});
                if (decoded)
                    return target_cache_result<Artifact>{
                        std::move(*decoded), key, !built_here};
            }

            (void)blobs_->erase(entry->blob_addr);
            auto evicted = catalog_->evict(key);
            if (!evicted)
                return std::unexpected(local_engine_error{
                    local_engine_stage::persistence, evicted.error().detail});
            if (attempt != 0)
                return std::unexpected(local_engine_error{
                    local_engine_stage::persistence,
                    "target artifact decode failed after rebuild"});
            built_here = false;
        }

        return std::unexpected(local_engine_error{
            local_engine_stage::persistence,
            "target artifact recovery exhausted"});
    }

    void set_host(execution::store::host_profile host) {
        host_ = std::move(host);
    }

    [[nodiscard]] const target_cache_config &config() const noexcept {
        return config_;
    }

private:
    Catalog *catalog_;
    BlobStore *blobs_;
    target_cache_config config_;
    execution::store::host_profile host_{};
};

template <class BackendSet, execution::store::catalog Catalog,
          execution::store::artifact_store BlobStore,
          class Algorithms =
              algorithms::algorithm_pack<algorithms::cost_based_backend_selector>,
          class MemoryPolicies = algorithms::default_lifecycle_policies>
class local_lithe_engine {
public:
    using execution_engine_type =
        basic_lithe_engine<BackendSet, Algorithms, MemoryPolicies>;

    local_lithe_engine(BackendSet backends,
                       std::shared_ptr<rt::runtime_instance> runtime,
                       Catalog &catalog, BlobStore &blobs,
                       portable_cache_config cache_config = {},
                       Algorithms algorithms = {},
                       MemoryPolicies memory = {},
                       const std::size_t managed_cache_capacity = 512,
                       target_cache_config target_config = {})
        : execution_(std::move(backends), std::move(algorithms),
                     std::move(memory)),
          runtime_(std::move(runtime)),
          portable_cache_(catalog, blobs, std::move(cache_config)),
          target_cache_(catalog, blobs, std::move(target_config)),
          managed_cache_capacity_(managed_cache_capacity) {}

    template <class Sig>
    [[nodiscard]] std::expected<rt::managed_function, local_engine_error>
    compile_managed(codegen::mir::physical_mir_function ir) {
        if (!runtime_)
            return std::unexpected(local_engine_error{
                local_engine_stage::managed_compile, "runtime is null"});

        constexpr auto signature = rt::managed_signature_v<Sig>;
        const auto cache_key =
            local_engine_detail::physical_fingerprint(ir, signature);
        std::shared_ptr<managed_compile_flight> flight;
        for (;;) {
            std::unique_lock cache_lock{managed_cache_mutex_};
            if (const auto found = managed_cache_.find(cache_key);
                found != managed_cache_.end()) {
                const auto state = found->second.function.code_resource_handle()
                                       ->state.load(std::memory_order_acquire);
                if (state != rt::code_state::retiring &&
                    state != rt::code_state::retired) {
                    managed_recency_.splice(managed_recency_.begin(),
                                            managed_recency_,
                                            found->second.recency);
                    ++managed_cache_hits_;
                    return found->second.function;
                }
                erase_cached_locked(found);
            }

            if (const auto active = managed_flights_.find(cache_key);
                active != managed_flights_.end()) {
                flight = active->second;
                flight->ready.wait(cache_lock, [&] { return flight->done; });
                continue;
            }

            flight = std::make_shared<managed_compile_flight>();
            managed_flights_.emplace(cache_key, flight);
            ++managed_cache_misses_;
            break;
        }

        auto compiled = compile_uncached<Sig>(std::move(ir));
        {
            std::scoped_lock cache_lock{managed_cache_mutex_};
            if (compiled && managed_cache_capacity_ != 0) {
                managed_recency_.push_front(cache_key);
                managed_cache_.insert_or_assign(
                    cache_key,
                    managed_cache_entry{*compiled, managed_recency_.begin()});
                enforce_capacity_locked();
            }
            flight->done = true;
            managed_flights_.erase(cache_key);
        }
        flight->ready.notify_all();
        return compiled;
    }

    template <class Sig>
    [[nodiscard]] std::expected<rt::managed_function, local_engine_error>
    compile_managed(codegen::hl::hl_mir_function hl_ir,
                    const ir::portable::module_freeze_options &freeze_options = {},
                    const ir::portable::thaw_options &thaw_options = {}) {
        const std::array<const codegen::hl::hl_mir_function *, 1> functions{&hl_ir};
        auto optimized = portable_cache_.freeze_optimize_thaw(
            functions, freeze_options, thaw_options);
        if (!optimized) return std::unexpected(optimized.error());
        if (optimized->size() != 1)
            return std::unexpected(local_engine_error{
                local_engine_stage::thaw,
                "single-function compilation produced an unexpected module size"});

        codegen::hl::coordinate_lowering_pass lowering;
        auto lowered = lowering.run(optimized->front());
        if (!lowered.ok())
            return std::unexpected(local_engine_error{
                local_engine_stage::managed_compile,
                lowered.diagnostics.empty()
                    ? "HL MIR lowering failed"
                    : lowered.diagnostics.front()});
        return compile_managed<Sig>(std::move(lowered.fn));
    }

private:
    template <class Sig>
    [[nodiscard]] std::expected<rt::managed_function, local_engine_error>
    compile_uncached(codegen::mir::physical_mir_function ir) {
        auto managed = rt::compile(*runtime_, ir);
        if (!managed)
            return std::unexpected(local_engine_error{
                local_engine_stage::managed_compile, managed.error().detail});

        auto selected = execution_.template compile_best<Sig>(std::move(ir));
        if (!selected)
            return std::unexpected(local_engine_error{
                local_engine_stage::backend_compile,
                std::string{selected.error().detail}});

        std::optional<rt::trap> binding_error;
        std::visit(
            [&](auto &entry) {
                auto bound = rt::bind_managed_entry<Sig>(
                    *managed, std::move(entry.entry));
                if (!bound) binding_error = bound.error();
            },
            *selected);
        if (binding_error)
            return std::unexpected(local_engine_error{
                local_engine_stage::binding, binding_error->detail});
        return std::move(*managed);
    }

public:

    template <class Sig>
    [[nodiscard]] std::expected<rt::runtime_value, local_engine_error>
    compile_and_invoke(codegen::mir::physical_mir_function ir,
                       rt::thread_attachment &thread,
                       std::span<const rt::runtime_value> args) {
        auto function = compile_managed<Sig>(std::move(ir));
        if (!function) return std::unexpected(function.error());
        auto result = function->invoke(thread, args);
        if (!result)
            return std::unexpected(local_engine_error{
                local_engine_stage::invocation, result.error().detail});
        execution_.profiling().record_invocation();
        return std::move(*result);
    }

    template <class Sig>
    [[nodiscard]] std::expected<rt::runtime_value, local_engine_error>
    compile_and_invoke(codegen::hl::hl_mir_function hl_ir,
                       rt::thread_attachment &thread,
                       std::span<const rt::runtime_value> args,
                       const ir::portable::module_freeze_options &freeze_options = {},
                       const ir::portable::thaw_options &thaw_options = {}) {
        auto function = compile_managed<Sig>(
            std::move(hl_ir), freeze_options, thaw_options);
        if (!function) return std::unexpected(function.error());
        auto result = function->invoke(thread, args);
        if (!result)
            return std::unexpected(local_engine_error{
                local_engine_stage::invocation, result.error().detail});
        execution_.profiling().record_invocation();
        return std::move(*result);
    }

    [[nodiscard]] portable_artifact_cache<Catalog, BlobStore> &
    portable_cache() noexcept {
        return portable_cache_;
    }
    [[nodiscard]] execution_engine_type &execution_engine() noexcept {
        return execution_;
    }
    [[nodiscard]] target_artifact_cache<Catalog, BlobStore> &
    target_cache() noexcept {
        return target_cache_;
    }
    [[nodiscard]] const std::shared_ptr<rt::runtime_instance> &runtime() const noexcept {
        return runtime_;
    }
    [[nodiscard]] std::size_t managed_cache_size() const {
        std::scoped_lock lock{managed_cache_mutex_};
        return managed_cache_.size();
    }
    [[nodiscard]] std::uint64_t managed_cache_hits() const noexcept {
        return managed_cache_hits_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::uint64_t managed_cache_misses() const noexcept {
        return managed_cache_misses_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::uint64_t managed_cache_evictions() const noexcept {
        return managed_cache_evictions_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::size_t managed_cache_capacity() const {
        std::scoped_lock lock{managed_cache_mutex_};
        return managed_cache_capacity_;
    }
    void set_managed_cache_capacity(const std::size_t capacity) {
        std::scoped_lock lock{managed_cache_mutex_};
        managed_cache_capacity_ = capacity;
        enforce_capacity_locked();
    }
    [[nodiscard]] std::size_t retirement_pending() const {
        return retirement_.pending();
    }
    [[nodiscard]] std::size_t drain_retired() { return retirement_.drain(); }

    template <class Sig>
    [[nodiscard]] bool
    evict_managed(const codegen::mir::physical_mir_function &ir) {
        const auto key = local_engine_detail::physical_fingerprint(
            ir, rt::managed_signature_v<Sig>);
        std::scoped_lock lock{managed_cache_mutex_};
        const auto found = managed_cache_.find(key);
        if (found == managed_cache_.end()) return false;
        retire_locked(found->second.function);
        erase_cached_locked(found);
        ++managed_cache_evictions_;
        return true;
    }

private:
    struct managed_cache_entry {
        rt::managed_function function;
        std::list<local_engine_detail::managed_cache_key>::iterator recency;
    };

    struct managed_compile_flight {
        std::condition_variable ready;
        bool done = false;
    };

    using managed_cache_map = std::unordered_map<
        local_engine_detail::managed_cache_key, managed_cache_entry,
        local_engine_detail::managed_cache_key_hash>;

    void retire_locked(rt::managed_function &function) {
        auto code = function.code_resource_handle();
        code->state.store(rt::code_state::retiring, std::memory_order_release);
        if (code->has_active_frames()) {
            retirement_.defer(std::move(code));
            return;
        }
        code->unwind.live = false;
        code->roots.live = false;
        code->state.store(rt::code_state::retired, std::memory_order_release);
    }

    void erase_cached_locked(const managed_cache_map::iterator position) {
        managed_recency_.erase(position->second.recency);
        managed_cache_.erase(position);
    }

    void enforce_capacity_locked() {
        while (managed_cache_.size() > managed_cache_capacity_) {
            const auto key = managed_recency_.back();
            const auto found = managed_cache_.find(key);
            if (found == managed_cache_.end()) {
                managed_recency_.pop_back();
                continue;
            }
            retire_locked(found->second.function);
            erase_cached_locked(found);
            ++managed_cache_evictions_;
        }
    }

    execution_engine_type execution_;
    std::shared_ptr<rt::runtime_instance> runtime_;
    portable_artifact_cache<Catalog, BlobStore> portable_cache_;
    target_artifact_cache<Catalog, BlobStore> target_cache_;
    mutable std::mutex managed_cache_mutex_;
    std::list<local_engine_detail::managed_cache_key> managed_recency_;
    managed_cache_map managed_cache_;
    std::unordered_map<local_engine_detail::managed_cache_key,
                       std::shared_ptr<managed_compile_flight>,
                       local_engine_detail::managed_cache_key_hash>
        managed_flights_;
    execution::store::retirement_queue retirement_;
    std::size_t managed_cache_capacity_ = 512;
    std::atomic<std::uint64_t> managed_cache_hits_{0};
    std::atomic<std::uint64_t> managed_cache_misses_{0};
    std::atomic<std::uint64_t> managed_cache_evictions_{0};
};

} // namespace lithe
