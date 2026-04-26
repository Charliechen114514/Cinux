/**
 * @file kernel/fs/path.cpp
 * @brief Path resolution and canonicalization implementation
 */

#include "kernel/fs/path.hpp"

#include "kernel/lib/string.hpp"

namespace cinux::fs {

void path_canonicalize(char* buf) {
    if (buf == nullptr || buf[0] == '\0') {
        return;
    }

    uint32_t len = static_cast<uint32_t>(strlen(buf));

    char     out[PATH_MAX];
    uint32_t out_pos = 0;

    // Always produce a leading '/' — result is an absolute path
    out[out_pos++] = '/';

    uint32_t i = 0;

    // Skip leading slashes
    while (i < len && buf[i] == '/') {
        ++i;
    }

    while (i < len) {
        // Extract the next component
        uint32_t comp_start = i;
        while (i < len && buf[i] != '/') {
            ++i;
        }
        uint32_t comp_len = i - comp_start;

        // Skip trailing/duplicate slashes
        while (i < len && buf[i] == '/') {
            ++i;
        }

        if (comp_len == 0) {
            continue;
        }

        // Handle "." — skip
        if (comp_len == 1 && buf[comp_start] == '.') {
            continue;
        }

        // Handle ".." — pop one component
        if (comp_len == 2 && buf[comp_start] == '.' && buf[comp_start + 1] == '.') {
            if (out_pos > 1) {
                --out_pos;
                while (out_pos > 0 && out[out_pos - 1] != '/') {
                    --out_pos;
                }
                // Remove the '/' separator unless it's the root '/'
                if (out_pos > 1) {
                    --out_pos;
                }
            }
            continue;
        }

        // Normal component: append '/'
        if (out_pos > 0 && out[out_pos - 1] != '/') {
            out[out_pos++] = '/';
        }

        // Copy component
        for (uint32_t j = 0; j < comp_len && out_pos < PATH_MAX - 1; ++j) {
            out[out_pos++] = buf[comp_start + j];
        }
    }

    // Ensure at least "/"
    if (out_pos == 0) {
        out[out_pos++] = '/';
    }

    out[out_pos] = '\0';

    memcpy(buf, out, out_pos + 1);
}

bool path_resolve(const char* cwd, const char* path, char* out) {
    if (cwd == nullptr || path == nullptr || out == nullptr) {
        return false;
    }

    // Absolute path: copy and canonicalise
    if (path[0] == '/') {
        uint32_t i = 0;
        while (path[i] != '\0' && i < PATH_MAX - 1) {
            out[i] = path[i];
            ++i;
        }
        out[i] = '\0';

        path_canonicalize(out);
        return true;
    }

    // Relative path: cwd + "/" + path
    uint32_t pos = 0;

    // Copy cwd
    while (cwd[pos] != '\0' && pos < PATH_MAX - 2) {
        out[pos] = cwd[pos];
        ++pos;
    }

    // Add separator if cwd doesn't end with '/'
    if (pos > 0 && out[pos - 1] != '/' && pos < PATH_MAX - 2) {
        out[pos++] = '/';
    }

    // Append path
    uint32_t j = 0;
    while (path[j] != '\0' && pos < PATH_MAX - 1) {
        out[pos++] = path[j++];
    }
    out[pos] = '\0';

    path_canonicalize(out);
    return true;
}

}  // namespace cinux::fs
