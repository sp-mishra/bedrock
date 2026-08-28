/*
 * plugin_abi.h — stable C ABI boundary for Lithe plugins (§P15)
 *
 * Pure C header.  Fixed-width types only.  No C++ symbols cross this boundary.
 * A plugin built by any ABI-compatible compiler can link against this contract.
 *
 * ABI contract version: 1.0.0
 * Layout: little-endian; all multi-byte fields LE.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * ABI version stamp — host checks this before calling any thunk
 * ========================================================================= */

#define LITHE_PLUGIN_ABI_MAJOR 1
#define LITHE_PLUGIN_ABI_MINOR 0
#define LITHE_PLUGIN_ABI_PATCH 0

typedef struct lithe_abi_version {
    uint32_t major;
    uint32_t minor;
    uint32_t patch;
} lithe_abi_version;

/* =========================================================================
 * lithe_plugin_descriptor — C-layout identity block for a plugin.
 *
 * Fixed-size char arrays (null-terminated, max lengths excluding NUL shown).
 * domain_bits mirrors lithe::semantic::domain_type bit-mask (uint16_t).
 * ========================================================================= */

#define LITHE_PLUGIN_ID_MAX     127
#define LITHE_PLUGIN_AUTHOR_MAX  63

typedef struct lithe_plugin_descriptor {
    char id[LITHE_PLUGIN_ID_MAX + 1]; /* reverse-DNS identifier */
    char author[LITHE_PLUGIN_AUTHOR_MAX + 1];
    uint32_t version_major;
    uint32_t version_minor;
    uint32_t version_patch;
    uint16_t domain_bits; /* lithe::semantic::domain_type bits */
    uint8_t _pad[2]; /* explicit padding, must be 0 */
} lithe_plugin_descriptor;

/* =========================================================================
 * lithe_plugin_status — integer return codes (no exceptions across boundary)
 * ========================================================================= */

typedef enum lithe_plugin_status {
    LITHE_PLUGIN_OK = 0,
    LITHE_PLUGIN_ERR_ABI_MISMATCH = 1, /* host/plugin ABI major mismatch */
    LITHE_PLUGIN_ERR_INIT_FAILED = 2, /* plugin-side initialisation failed */
    LITHE_PLUGIN_ERR_BAD_SIG = 3, /* signature verification failed */
    LITHE_PLUGIN_ERR_REFUSED = 4, /* plugin refused the profile/constraints */
} lithe_plugin_status;

/* =========================================================================
 * lithe_backend_capability_bits — subset of backend_feature bits the C side
 * can express.  Bit positions match backend_feature enumerator ordinals.
 * ========================================================================= */

typedef uint64_t lithe_backend_capability_bits;

/* =========================================================================
 * lithe_plugin_thunk_table — function-pointer table the plugin exposes.
 *
 * The host obtains this table via lithe_plugin_entry() (below).
 * All pointers must be non-null for a well-formed plugin.
 *
 * Plugin MUST NOT throw exceptions through any of these calls.
 * ========================================================================= */

typedef struct lithe_plugin_thunk_table {
    /* abi_version() — returns the ABI version the plugin was compiled against. */
    lithe_abi_version (*abi_version)(void);

    /* descriptor() — fills *out with the plugin's identity block. */
    lithe_plugin_status (*descriptor)(lithe_plugin_descriptor* out);

    /* capabilities() — returns the backend_capability_bits this plugin provides. */
    lithe_backend_capability_bits (*capabilities)(void);

    /*
     * register_fn / unregister_fn — called by the host to activate/deactivate
     * the plugin's backend.  The host passes an opaque context pointer that the
     * plugin stores and passes back in any cross-boundary call.
     *
     * register_fn:   plugin allocates any internal state; returns LITHE_PLUGIN_OK
     *                or an error code.  The host may call register_fn only once
     *                per loaded plugin instance.
     *
     * unregister_fn: plugin tears down; called when the registration_token is
     *                dropped (after the refcount reaches zero).
     */
    lithe_plugin_status (*register_fn)(void* host_ctx);
    void (*unregister_fn)(void* host_ctx);
} lithe_plugin_thunk_table;

/* =========================================================================
 * lithe_plugin_entry — the single exported entry point.
 *
 * The host dlsym()s "lithe_plugin_entry" from the loaded DSO and calls it
 * exactly once to retrieve the thunk table.  The plugin owns the table's
 * lifetime for as long as the DSO is loaded.
 *
 * Signature (C linkage, no name mangling):
 *   lithe_plugin_status lithe_plugin_entry(lithe_plugin_thunk_table* out);
 * ========================================================================= */

typedef lithe_plugin_status (*lithe_plugin_entry_fn)(lithe_plugin_thunk_table* out);

/* Macro for plugin authors to declare the entry point correctly. */
#define LITHE_PLUGIN_ENTRY_DECL \
    lithe_plugin_status lithe_plugin_entry(lithe_plugin_thunk_table* out)

#ifdef __cplusplus
} /* extern "C" */
#endif
