#pragma once

// =============================================================================
// lithe_execution/store/blob_store.hpp — Lithe artifact blob store (impl-3)
//
// Lithe's artifact blob store is a refinement of the generic
// containers::content_store (G3). This file exposes the Lithe-specific aliases
// and the artifact_store concept that names the blob store's role in the
// artifact pipeline.
//
// Provides:
//   artifact_store concept       — content_store refinement bound to blob_address
//   blob_address alias           — re-exported from containers::content_store
//   blob_handle alias            — re-exported
//   store_error alias            — re-exported
//   filesystem_blob_store alias  — filesystem_content_store under the lithe namespace
//
// Relationship to content_store.hpp (G3):
//   artifact_store IS a content_store. filesystem_blob_store IS a
//   filesystem_content_store. The type aliases keep the Lithe store layer
//   decoupled from the generic container while reusing its implementation.
//
// No virtual, no macros. Header-only C++23.
// =============================================================================

#include "containers/content_store.hpp"  // generic G3

namespace lithe::execution::store {
    // Re-export generic types into the Lithe store namespace.
    using blob_address = containers::blob_address;
    using blob_handle = containers::blob_handle;
    using store_error = containers::store_error;

    // artifact_store concept — any content_store is an artifact_store.
    template <class S>
    concept artifact_store = containers::content_store<S>;

    // filesystem_blob_store — the default durable artifact blob store.
    // Sharded directory layout, atomic-rename put, Setu zero-copy get.
    using filesystem_blob_store = containers::filesystem_content_store;

    static_assert(artifact_store<filesystem_blob_store>);
} // namespace lithe::execution::store
