#pragma once

// crank/std/fs.hpp — std.fs module: filesystem operations for Crank.
//
// C++23, header-only, no virtual, no macros. Namespace: crank::stdlib
//
// Backed by std::filesystem (portable, synchronous). Every entry carries the
// FileSystem effect plus Read or Write capability. Errors are reported through
// return values (empty/false) rather than exceptions — the wrappers use the
// std::error_code overloads so nothing throws across the host boundary.

#include "languages/crank/std/detail/register.hpp"
#include "languages/crank/effects.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

namespace crank::stdlib {
    namespace fs_fns {
        namespace fs = std::filesystem;

        // read_file — whole-file contents, or empty string on error.
        [[nodiscard]] inline std::string read_file(std::string path) {
            std::ifstream in(path, std::ios::binary);
            if (!in) return {};
            return std::string(std::istreambuf_iterator<char>(in),
                               std::istreambuf_iterator<char>());
        }

        // write_file — overwrite path with contents; true on success.
        [[nodiscard]] inline bool write_file(std::string path, std::string contents) {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            if (!out) return false;
            out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
            return static_cast<bool>(out);
        }

        [[nodiscard]] inline bool exists(std::string path) noexcept {
            std::error_code ec;
            return fs::exists(path, ec);
        }
        [[nodiscard]] inline bool is_dir(std::string path) noexcept {
            std::error_code ec;
            return fs::is_directory(path, ec);
        }
        [[nodiscard]] inline std::int64_t file_size(std::string path) noexcept {
            std::error_code ec;
            auto n = fs::file_size(path, ec);
            return ec ? -1 : static_cast<std::int64_t>(n);
        }
        [[nodiscard]] inline bool remove_(std::string path) noexcept {
            std::error_code ec;
            return fs::remove(path, ec);
        }
        [[nodiscard]] inline bool mkdir_(std::string path) noexcept {
            std::error_code ec;
            return fs::create_directories(path, ec) || fs::is_directory(path, ec);
        }
        [[nodiscard]] inline bool copy_(std::string from, std::string to) noexcept {
            std::error_code ec;
            fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
            return !ec;
        }
        [[nodiscard]] inline bool rename_(std::string from, std::string to) noexcept {
            std::error_code ec;
            fs::rename(from, to, ec);
            return !ec;
        }
    } // namespace fs_fns

    inline void install_std_fs(crank::context& ctx) {
        namespace f = fs_fns;
        ffi_module_builder mod{"std.fs"};
        const function_options rd{
            .effects = vakya::types::kEffectMaskFileSystem,
            .capabilities = vakya::types::kCapMaskRead,
            .flags = static_cast<function_flags>(function_flag::blocking),
            .blocking = blocking_class::potentially_blocking,
        };
        const function_options wr{
            .effects = vakya::types::kEffectMaskFileSystem,
            .capabilities = vakya::types::kCapMaskWrite,
            .flags = static_cast<function_flags>(function_flag::blocking),
            .blocking = blocking_class::potentially_blocking,
        };
        const function_options rdwr{
            .effects = vakya::types::kEffectMaskFileSystem,
            .capabilities = vakya::types::kCapMaskRead | vakya::types::kCapMaskWrite,
            .flags = static_cast<function_flags>(function_flag::blocking),
            .blocking = blocking_class::potentially_blocking,
        };

        detail::add_fn<"std.fs.read_file", &f::read_file>(mod, ctx, "ReadFile", rd);
        detail::add_fn<"std.fs.write_file", &f::write_file>(mod, ctx, "WriteFile", wr);
        detail::add_fn<"std.fs.exists", &f::exists>(mod, ctx, "Exists", rd);
        detail::add_fn<"std.fs.is_dir", &f::is_dir>(mod, ctx, "IsDir", rd);
        detail::add_fn<"std.fs.file_size", &f::file_size>(mod, ctx, "FileSize", rd);
        detail::add_fn<"std.fs.remove", &f::remove_>(mod, ctx, "Remove", wr);
        detail::add_fn<"std.fs.mkdir", &f::mkdir_>(mod, ctx, "MakeDir", wr);
        detail::add_fn<"std.fs.copy", &f::copy_>(mod, ctx, "Copy", rdwr);
        detail::add_fn<"std.fs.rename", &f::rename_>(mod, ctx, "Rename", rdwr);

        ctx.register_ffi_module(mod.build());
    }
} // namespace crank::stdlib
