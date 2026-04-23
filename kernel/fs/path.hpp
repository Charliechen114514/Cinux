/**
 * @file kernel/fs/path.hpp
 * @brief Path resolution and canonicalization utilities
 *
 * Provides helpers to canonicalise paths (collapse . .. //) and to
 * resolve relative paths against a per-process cwd before passing
 * them to the VFS mount layer.
 *
 * Namespace: cinux::fs
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

namespace cinux::fs {

/// Maximum absolute path length (including NUL terminator)
static constexpr uint32_t PATH_MAX = 4096;

/**
 * @brief Canonicalise a path in-place
 *
 * Collapses duplicate slashes, resolves "." and ".." components.
 * The result is an absolute path with no trailing slash (except for "/").
 *
 * @param buf  Mutable buffer containing the path to canonicalise
 */
void path_canonicalize(char* buf);

/**
 * @brief Resolve a (possibly relative) path against a cwd
 *
 * If @p path starts with '/', it is treated as absolute and copied
 * directly to @p out.  Otherwise, @p cwd + "/" + @p path is
 * assembled and then canonicalised.
 *
 * @param cwd   Current working directory (absolute, NUL-terminated)
 * @param path  User-supplied path (may be relative)
 * @param out   Output buffer (at least PATH_MAX bytes)
 * @return true on success, false if the result would overflow
 */
bool path_resolve(const char* cwd, const char* path, char* out);

}  // namespace cinux::fs
