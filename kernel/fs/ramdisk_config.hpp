/**
 * @file kernel/fs/ramdisk_config.hpp
 * @brief POSIX ustar (tar) header definition and conversion helpers
 *
 * Defines the on-disk layout of a ustar archive entry (512-byte header)
 * and provides octal-to-integer conversion for size/mode fields.
 *
 * Reference: POSIX.1-1988 "ustar" interchange format
 *
 * Namespace: cinux::fs
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

namespace cinux::fs {

// ============================================================
// Ustar Block Size
// ============================================================

/// Size of a single tar record block in bytes
static constexpr uint32_t USTAR_BLOCK_SIZE = 512;

// ============================================================
// Ustar Type Flag Constants
// ============================================================

namespace UstarType {
constexpr char REGULAR    = '0';  ///< Regular file
constexpr char HARDLINK   = '1';  ///< Hard link
constexpr char SYMLINK    = '2';  ///< Symbolic link
constexpr char CHARDEV    = '3';  ///< Character device
constexpr char BLOCKDEV   = '4';  ///< Block device
constexpr char DIRECTORY  = '5';  ///< Directory
constexpr char FIFO       = '6';  ///< FIFO / named pipe
constexpr char CONTIGUOUS = '7';  ///< Contiguous file (regular equivalent)
}  // namespace UstarType

/// Expected magic string at offset 257 in a valid ustar header
static constexpr char USTAR_MAGIC[] = "ustar";

// ============================================================
// Ustar Header Structure
// ============================================================

/**
 * @brief POSIX ustar archive header (512 bytes, packed)
 *
 * Represents a single entry header in a ustar-format tar archive.
 * All string fields are null-terminated ASCII.  Numeric fields
 * (mode, uid, gid, size, mtime, checksum) are stored as
 * octal ASCII strings.
 *
 * Layout (offsets in bytes):
 *   0-99     name[100]    Entry pathname
 *   100-107  mode[8]      File mode (octal)
 *   108-115  uid[8]       Owner UID (octal)
 *   116-123  gid[8]       Group GID (octal)
 *   124-135  size[12]     File size in bytes (octal)
 *   136-147  mtime[12]    Modification time (octal)
 *   148-155  checksum[8]  Header checksum (octal)
 *   156      typeflag     Entry type character
 *   157-256  linkname[100] Target of link (if applicable)
 *   257-262  magic[6]     "ustar\0"
 *   263-264  version[2]   "00"
 *   265-296  uname[32]    Owner user name
 *   297-328  gname[32]    Owner group name
 *   329-336  devmajor[8]  Device major (octal)
 *   337-344  devminor[8]  Device minor (octal)
 *   345-499  prefix[155]  Path prefix for long names
 *   500-511  padding[12]  Reserved / padding
 */
struct [[gnu::packed]] UstarHeader {
    char    name[100];     ///< 0:   Entry pathname (null-terminated)
    char    mode[8];       ///< 100: File mode in octal ASCII
    char    uid[8];        ///< 108: Owner user ID in octal ASCII
    char    gid[8];        ///< 116: Owner group ID in octal ASCII
    char    size[12];      ///< 124: File size in octal ASCII
    char    mtime[12];     ///< 136: Modification timestamp in octal ASCII
    char    checksum[8];   ///< 148: Header checksum in octal ASCII
    char    typeflag;      ///< 156: Entry type flag
    char    linkname[100]; ///< 157: Link target name
    char    magic[6];      ///< 257: "ustar\0"
    char    version[2];    ///< 263: Version "00"
    char    uname[32];     ///< 265: Owner user name
    char    gname[32];     ///< 297: Owner group name
    char    devmajor[8];   ///< 329: Device major number (octal)
    char    devminor[8];   ///< 337: Device minor number (octal)
    char    prefix[155];   ///< 345: Path prefix for long names
    char    padding[12];   ///< 500: Reserved / padding
};

static_assert(sizeof(UstarHeader) == USTAR_BLOCK_SIZE,
              "UstarHeader must be exactly 512 bytes");

// ============================================================
// Conversion Helpers
// ============================================================

/**
 * @brief Convert an octal ASCII string to an unsigned integer
 *
 * Ustar numeric fields are stored as null- or space-terminated
 * octal ASCII strings.  This function parses up to `len` characters,
 * stopping at the first null or space terminator.
 *
 * @param s   Pointer to the octal string (need not be null-terminated)
 * @param len Maximum number of characters to examine
 * @return    Parsed unsigned 64-bit integer value
 */
uint64_t octal_to_uint(const char* s, size_t len);

}  // namespace cinux::fs
