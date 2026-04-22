/**
 * @file test/unit/test_ramdisk.cpp
 * @brief Host-side unit tests for ramdisk / ustar archive parser (026)
 *
 * Test coverage:
 *   - UstarHeader: struct size is exactly 512 bytes, packed layout
 *   - UstarHeader: field offsets match POSIX ustar specification
 *   - UstarType constants: REGULAR, DIRECTORY, CONTIGUOUS, etc.
 *   - USTAR_MAGIC: expected value "ustar"
 *   - octal_to_uint: empty, single digit, multi-digit, null-terminated,
 *     space-terminated, max-length, all-zeros, large values
 *   - is_valid_ustar: valid magic, invalid magic, partial, all-zero
 *   - data_blocks: zero size, exact block, partial block, large file
 *   - ramdisk_mount: single regular file, directory entries, mixed entries,
 *     empty archive, end-of-archive detection, invalid magic detection,
 *     contiguous file handling, file data skipping
 *
 * Pure arithmetic and logic -- no kernel code linked.  The kernel
 * ramdisk.cpp depends on linker symbols (_binary_initrd_start/end) and
 * kprintf, so we re-implement the pure logic (octal conversion, header
 * validation, mount traversal) and test against hand-crafted ustar
 * archives in memory.
 *
 * Compile condition: -DCINUX_HOST_TEST
 */

#define TEST_FRAMEWORK_IMPL
#include "test_framework.h"

#ifdef CINUX_HOST_TEST

#include <cstddef>
#include <cstdint>
#include <cstring>

// Include kernel headers for struct/constant definitions
#include "fs/ramdisk_config.hpp"
#include "fs/ramdisk.hpp"

using namespace cinux::fs;

// ============================================================
// Re-implement internal helpers for host testing
//
// The kernel ramdisk.cpp uses anonymous-namespace helpers and
// kprintf, which cannot execute on the host.  We mirror the pure
// logic here for isolated testing.
// ============================================================

namespace ramdisk_test {

// ---------- octal_to_uint (mirrors kernel implementation) ----------

uint64_t octal_to_uint(const char* s, size_t len) {
    uint64_t result = 0;
    for (size_t i = 0; i < len; ++i) {
        char c = s[i];
        if (c == '\0' || c == ' ') {
            break;
        }
        result = (result << 3) + static_cast<uint64_t>(c - '0');
    }
    return result;
}

// ---------- is_valid_ustar (mirrors kernel implementation) ----------

bool is_valid_ustar(const UstarHeader* hdr) {
    for (uint32_t i = 0; i < 5; ++i) {
        if (hdr->magic[i] != USTAR_MAGIC[i]) {
            return false;
        }
    }
    return true;
}

// ---------- data_blocks (mirrors kernel implementation) ----------

uint32_t data_blocks(uint64_t size) {
    if (size == 0) {
        return 0;
    }
    return static_cast<uint32_t>((size + USTAR_BLOCK_SIZE - 1) / USTAR_BLOCK_SIZE);
}

// ---------- Ustar archive builder helper ----------

/**
 * @brief Build a valid ustar header in a caller-supplied buffer
 *
 * @param buf      Must be at least 512 bytes, will be zeroed
 * @param name     Entry name (max 99 chars)
 * @param size     File size in bytes (stored as octal in header)
 * @param typeflag Type flag character ('0' for file, '5' for dir, etc.)
 */
void build_ustar_header(void* buf, const char* name,
                        uint64_t size, char typeflag) {
    auto* hdr = static_cast<UstarHeader*>(std::memset(buf, 0, sizeof(UstarHeader)));

    // Copy name
    size_t name_len = std::strlen(name);
    if (name_len > 99) {
        name_len = 99;
    }
    std::memcpy(hdr->name, name, name_len);

    // Encode size as octal ASCII (space/null-terminated)
    // ustar convention: octal digits left-padded with '0', terminated by
    // NUL or space.  We place a NUL at the end of the field.
    char size_buf[13];
    std::memset(size_buf, '0', 12);
    size_buf[12] = '\0';

    if (size == 0) {
        // "0\0" -- single zero digit followed by NUL
        size_buf[0] = '0';
        size_buf[1] = '\0';
    } else {
        // Convert to octal string, writing digits from right to left
        size_t pos = 11;  // last position before the NUL terminator
        uint64_t v = size;
        while (v > 0 && pos > 0) {
            size_buf[pos] = '0' + static_cast<char>(v & 7);
            --pos;
            v >>= 3;
        }
        // Everything before the first octal digit is already '0' (padding)
    }
    std::memcpy(hdr->size, size_buf, 12);

    // Set magic
    std::memcpy(hdr->magic, "ustar", 6);
    std::memcpy(hdr->version, "00", 2);

    // Set typeflag
    hdr->typeflag = typeflag;
}

// ---------- mount logic (mirrors kernel Ramdisk::mount) ----------

/**
 * @brief Mount result structure for host testing
 */
struct MountResult {
    uint32_t entry_count;       ///< Number of file entries found
    uint32_t dir_count;         ///< Number of directory entries found
    bool     stopped_bad_magic; ///< True if stopped due to invalid magic
};

/**
 * @brief Simulate Ramdisk::mount() on a raw archive buffer
 *
 * Mirrors the kernel mount() logic: iterate through ustar entries,
 * count files (REGULAR + CONTIGUOUS) and directories, detect
 * end-of-archive and invalid magic.
 *
 * @param base  Pointer to the start of the archive
 * @param size  Size of the archive in bytes
 * @return      MountResult with counts and status
 */
MountResult mount_archive(const uint8_t* base, uint64_t size) {
    MountResult result{};
    uint64_t offset = 0;

    while (offset + sizeof(UstarHeader) <= size) {
        auto* hdr = reinterpret_cast<const UstarHeader*>(base + offset);

        // End-of-archive: zero name[0]
        if (hdr->name[0] == '\0') {
            break;
        }

        // Validate ustar magic
        if (!is_valid_ustar(hdr)) {
            result.stopped_bad_magic = true;
            break;
        }

        // Parse file size
        uint64_t file_size = octal_to_uint(hdr->size, sizeof(hdr->size));

        // Count by type
        char type = hdr->typeflag;
        if (type == UstarType::REGULAR || type == UstarType::CONTIGUOUS) {
            ++result.entry_count;
        } else if (type == UstarType::DIRECTORY) {
            ++result.dir_count;
        }

        // Advance past header + data blocks
        uint32_t blocks = data_blocks(file_size);
        offset += sizeof(UstarHeader) + static_cast<uint64_t>(blocks) * USTAR_BLOCK_SIZE;
    }

    return result;
}

}  // namespace ramdisk_test

// ============================================================
// 1. UstarHeader struct size and layout
// ============================================================

TEST("ramdisk_ustar_header: sizeof(UstarHeader) is 512") {
    ASSERT_EQ(sizeof(UstarHeader), 512ULL);
}

TEST("ramdisk_ustar_header: name field offset is 0") {
    UstarHeader hdr{};
    auto* base = reinterpret_cast<const uint8_t*>(&hdr);
    auto* name = reinterpret_cast<const uint8_t*>(hdr.name);
    ASSERT_EQ(static_cast<size_t>(name - base), 0ULL);
}

TEST("ramdisk_ustar_header: mode field offset is 100") {
    UstarHeader hdr{};
    auto* base = reinterpret_cast<const uint8_t*>(&hdr);
    auto* mode = reinterpret_cast<const uint8_t*>(hdr.mode);
    ASSERT_EQ(static_cast<size_t>(mode - base), 100ULL);
}

TEST("ramdisk_ustar_header: size field offset is 124") {
    UstarHeader hdr{};
    auto* base = reinterpret_cast<const uint8_t*>(&hdr);
    auto* sz = reinterpret_cast<const uint8_t*>(hdr.size);
    ASSERT_EQ(static_cast<size_t>(sz - base), 124ULL);
}

TEST("ramdisk_ustar_header: typeflag offset is 156") {
    UstarHeader hdr{};
    auto* base = reinterpret_cast<const uint8_t*>(&hdr);
    auto* tf = reinterpret_cast<const uint8_t*>(&hdr.typeflag);
    ASSERT_EQ(static_cast<size_t>(tf - base), 156ULL);
}

TEST("ramdisk_ustar_header: magic field offset is 257") {
    UstarHeader hdr{};
    auto* base = reinterpret_cast<const uint8_t*>(&hdr);
    auto* mag = reinterpret_cast<const uint8_t*>(hdr.magic);
    ASSERT_EQ(static_cast<size_t>(mag - base), 257ULL);
}

TEST("ramdisk_ustar_header: prefix field offset is 345") {
    UstarHeader hdr{};
    auto* base = reinterpret_cast<const uint8_t*>(&hdr);
    auto* pfx = reinterpret_cast<const uint8_t*>(hdr.prefix);
    ASSERT_EQ(static_cast<size_t>(pfx - base), 345ULL);
}

// ============================================================
// 2. UstarType constants
// ============================================================

TEST("ramdisk_ustar_type: REGULAR is '0'") {
    ASSERT_EQ(UstarType::REGULAR, '0');
}

TEST("ramdisk_ustar_type: HARDLINK is '1'") {
    ASSERT_EQ(UstarType::HARDLINK, '1');
}

TEST("ramdisk_ustar_type: SYMLINK is '2'") {
    ASSERT_EQ(UstarType::SYMLINK, '2');
}

TEST("ramdisk_ustar_type: CHARDEV is '3'") {
    ASSERT_EQ(UstarType::CHARDEV, '3');
}

TEST("ramdisk_ustar_type: BLOCKDEV is '4'") {
    ASSERT_EQ(UstarType::BLOCKDEV, '4');
}

TEST("ramdisk_ustar_type: DIRECTORY is '5'") {
    ASSERT_EQ(UstarType::DIRECTORY, '5');
}

TEST("ramdisk_ustar_type: FIFO is '6'") {
    ASSERT_EQ(UstarType::FIFO, '6');
}

TEST("ramdisk_ustar_type: CONTIGUOUS is '7'") {
    ASSERT_EQ(UstarType::CONTIGUOUS, '7');
}

// ============================================================
// 3. USTAR_MAGIC constant
// ============================================================

TEST("ramdisk_magic: USTAR_MAGIC is 'ustar'") {
    // Compare first 5 characters (excluding null terminator)
    ASSERT_TRUE(std::memcmp(USTAR_MAGIC, "ustar", 5) == 0);
}

TEST("ramdisk_magic: USTAR_MAGIC is null-terminated") {
    ASSERT_EQ(USTAR_MAGIC[5], '\0');
}

// ============================================================
// 4. USTAR_BLOCK_SIZE constant
// ============================================================

TEST("ramdisk_block_size: USTAR_BLOCK_SIZE is 512") {
    ASSERT_EQ(USTAR_BLOCK_SIZE, 512U);
}

// ============================================================
// 5. octal_to_uint: normal path
// ============================================================

TEST("ramdisk_octal: single digit '0' returns 0") {
    ASSERT_EQ(ramdisk_test::octal_to_uint("0", 1), 0ULL);
}

TEST("ramdisk_octal: single digit '7' returns 7") {
    ASSERT_EQ(ramdisk_test::octal_to_uint("7", 1), 7ULL);
}

TEST("ramdisk_octal: two digits '10' returns 8") {
    ASSERT_EQ(ramdisk_test::octal_to_uint("10", 2), 8ULL);
}

TEST("ramdisk_octal: '12' returns 10") {
    ASSERT_EQ(ramdisk_test::octal_to_uint("12", 2), 10ULL);
}

TEST("ramdisk_octal: '777' returns 511") {
    ASSERT_EQ(ramdisk_test::octal_to_uint("777", 3), 511ULL);
}

TEST("ramdisk_octal: '377' returns 255") {
    ASSERT_EQ(ramdisk_test::octal_to_uint("377", 3), 255ULL);
}

TEST("ramdisk_octal: typical file size '00000001244' (12-char field)") {
    // Octal "1244" = 1*512 + 2*64 + 4*8 + 4 = 512 + 128 + 32 + 4 = 676
    ASSERT_EQ(ramdisk_test::octal_to_uint("00000001244", 11), 676ULL);
}

TEST("ramdisk_octal: large value '77777777777' returns expected") {
    // 11 octal digits of 7: 8^11 - 1 = 8589934591
    ASSERT_EQ(ramdisk_test::octal_to_uint("77777777777", 11), 8589934591ULL);
}

// ============================================================
// 6. octal_to_uint: null terminator stops parsing
// ============================================================

TEST("ramdisk_octal: stops at null terminator within len") {
    // "12\0" with len=3: should parse "12" only
    char buf[] = {'1', '2', '\0', '4', '5'};
    ASSERT_EQ(ramdisk_test::octal_to_uint(buf, 5), 10ULL);
}

TEST("ramdisk_octal: stops at space terminator") {
    // "12 45" with len=5: should parse "12" only
    char buf[] = {'1', '2', ' ', '4', '5'};
    ASSERT_EQ(ramdisk_test::octal_to_uint(buf, 5), 10ULL);
}

TEST("ramdisk_octal: leading space returns 0") {
    // A space at position 0 means immediate stop
    char buf[] = {' ', '1', '2'};
    ASSERT_EQ(ramdisk_test::octal_to_uint(buf, 3), 0ULL);
}

TEST("ramdisk_octal: all spaces returns 0") {
    char buf[] = {' ', ' ', ' ', ' '};
    ASSERT_EQ(ramdisk_test::octal_to_uint(buf, 4), 0ULL);
}

// ============================================================
// 7. octal_to_uint: boundary conditions
// ============================================================

TEST("ramdisk_octal: zero length returns 0") {
    ASSERT_EQ(ramdisk_test::octal_to_uint("12345", 0), 0ULL);
}

TEST("ramdisk_octal: empty string returns 0") {
    ASSERT_EQ(ramdisk_test::octal_to_uint("", 0), 0ULL);
}

TEST("ramdisk_octal: len=1 with digit '3' returns 3") {
    ASSERT_EQ(ramdisk_test::octal_to_uint("3", 1), 3ULL);
}

TEST("ramdisk_octal: len longer than string (null stops early)") {
    // String "377\0" with len=10: stops at null
    char buf[10] = {'3', '7', '7'};
    ASSERT_EQ(ramdisk_test::octal_to_uint(buf, 10), 255ULL);
}

// ============================================================
// 8. octal_to_uint: typical ustar size field encoding
// ============================================================

TEST("ramdisk_octal: typical 12-char size field for 4096 bytes") {
    // 4096 in octal = 10000
    char buf[12];
    std::memset(buf, '0', 12);
    std::memcpy(buf + 6, "10000", 5);
    buf[11] = '\0';
    // octal "10000" = 4096
    ASSERT_EQ(ramdisk_test::octal_to_uint(buf, 12), 4096ULL);
}

TEST("ramdisk_octal: typical 12-char size field for 1 byte") {
    char buf[12];
    std::memset(buf, '0', 12);
    buf[10] = '1';
    buf[11] = '\0';
    ASSERT_EQ(ramdisk_test::octal_to_uint(buf, 12), 1ULL);
}

TEST("ramdisk_octal: typical 12-char size field for 0 bytes") {
    // All zeros with null terminator
    char buf[12];
    std::memset(buf, '0', 12);
    buf[11] = '\0';
    // "0000000000\0" parses as 0 because first char is '0' (value 0),
    // then stops at null after processing all zeros
    ASSERT_EQ(ramdisk_test::octal_to_uint(buf, 12), 0ULL);
}

// ============================================================
// 9. is_valid_ustar: normal and edge cases
// ============================================================

TEST("ramdisk_valid_ustar: valid magic returns true") {
    UstarHeader hdr{};
    std::memcpy(hdr.magic, "ustar", 6);
    ASSERT_TRUE(ramdisk_test::is_valid_ustar(&hdr));
}

TEST("ramdisk_valid_ustar: wrong magic returns false") {
    UstarHeader hdr{};
    std::memcpy(hdr.magic, "bstar", 6);
    ASSERT_FALSE(ramdisk_test::is_valid_ustar(&hdr));
}

TEST("ramdisk_valid_ustar: empty magic returns false") {
    UstarHeader hdr{};
    std::memset(hdr.magic, 0, 6);
    ASSERT_FALSE(ramdisk_test::is_valid_ustar(&hdr));
}

TEST("ramdisk_valid_ustar: partial magic 'ust' returns false") {
    UstarHeader hdr{};
    std::memcpy(hdr.magic, "ust", 3);
    hdr.magic[3] = 'a';
    hdr.magic[4] = 'r';
    // Actually this IS "ustar" if we set all 5 chars correctly
    // Let's make it truly partial: "ustx\0"
    hdr.magic[3] = 'x';
    hdr.magic[4] = '\0';
    ASSERT_FALSE(ramdisk_test::is_valid_ustar(&hdr));
}

TEST("ramdisk_valid_ustar: magic with wrong 5th char returns false") {
    UstarHeader hdr{};
    std::memcpy(hdr.magic, "ustax", 5);
    hdr.magic[5] = '\0';
    ASSERT_FALSE(ramdisk_test::is_valid_ustar(&hdr));
}

TEST("ramdisk_valid_ustar: all-zero header returns false") {
    UstarHeader hdr{};
    std::memset(&hdr, 0, sizeof(hdr));
    ASSERT_FALSE(ramdisk_test::is_valid_ustar(&hdr));
}

// ============================================================
// 10. data_blocks: normal and boundary cases
// ============================================================

TEST("ramdisk_data_blocks: zero size returns 0") {
    ASSERT_EQ(ramdisk_test::data_blocks(0), 0U);
}

TEST("ramdisk_data_blocks: 1 byte returns 1 block") {
    ASSERT_EQ(ramdisk_test::data_blocks(1), 1U);
}

TEST("ramdisk_data_blocks: 512 bytes returns 1 block") {
    ASSERT_EQ(ramdisk_test::data_blocks(512), 1U);
}

TEST("ramdisk_data_blocks: 513 bytes returns 2 blocks") {
    ASSERT_EQ(ramdisk_test::data_blocks(513), 2U);
}

TEST("ramdisk_data_blocks: 1023 bytes returns 2 blocks") {
    ASSERT_EQ(ramdisk_test::data_blocks(1023), 2U);
}

TEST("ramdisk_data_blocks: 1024 bytes returns 2 blocks") {
    ASSERT_EQ(ramdisk_test::data_blocks(1024), 2U);
}

TEST("ramdisk_data_blocks: 1025 bytes returns 3 blocks") {
    ASSERT_EQ(ramdisk_test::data_blocks(1025), 3U);
}

TEST("ramdisk_data_blocks: 4096 bytes returns 8 blocks") {
    ASSERT_EQ(ramdisk_test::data_blocks(4096), 8U);
}

TEST("ramdisk_data_blocks: large file 1 MiB returns 2048 blocks") {
    ASSERT_EQ(ramdisk_test::data_blocks(1024 * 1024), 2048U);
}

// ============================================================
// 11. mount_archive: single regular file
// ============================================================

TEST("ramdisk_mount: single regular file") {
    // Build a minimal archive: one 512-byte header + no data blocks (size=0)
    uint8_t archive[1024];
    std::memset(archive, 0, sizeof(archive));
    ramdisk_test::build_ustar_header(archive, "hello.txt", 0, UstarType::REGULAR);

    auto result = ramdisk_test::mount_archive(archive, sizeof(archive));
    ASSERT_EQ(result.entry_count, 1U);
    ASSERT_EQ(result.dir_count, 0U);
    ASSERT_FALSE(result.stopped_bad_magic);
}

TEST("ramdisk_mount: single regular file with data") {
    // Header (512) + 1 data block (512) = 1024 bytes, then a zero header
    uint8_t archive[2048];
    std::memset(archive, 0, sizeof(archive));
    ramdisk_test::build_ustar_header(archive, "data.bin", 100, UstarType::REGULAR);
    // Fill some data bytes
    for (int i = 0; i < 100; ++i) {
        archive[512 + i] = static_cast<uint8_t>(i);
    }
    // Second header is all-zero (end-of-archive)

    auto result = ramdisk_test::mount_archive(archive, sizeof(archive));
    ASSERT_EQ(result.entry_count, 1U);
    ASSERT_FALSE(result.stopped_bad_magic);
}

TEST("ramdisk_mount: single contiguous file ('7') counted as file") {
    uint8_t archive[1024];
    std::memset(archive, 0, sizeof(archive));
    ramdisk_test::build_ustar_header(archive, "contiguous.dat", 0, UstarType::CONTIGUOUS);

    auto result = ramdisk_test::mount_archive(archive, sizeof(archive));
    ASSERT_EQ(result.entry_count, 1U);
    ASSERT_EQ(result.dir_count, 0U);
}

// ============================================================
// 12. mount_archive: directory entries
// ============================================================

TEST("ramdisk_mount: single directory entry") {
    uint8_t archive[1024];
    std::memset(archive, 0, sizeof(archive));
    ramdisk_test::build_ustar_header(archive, "mydir/", 0, UstarType::DIRECTORY);

    auto result = ramdisk_test::mount_archive(archive, sizeof(archive));
    ASSERT_EQ(result.entry_count, 0U);
    ASSERT_EQ(result.dir_count, 1U);
}

// ============================================================
// 13. mount_archive: mixed entries (dir + file)
// ============================================================

TEST("ramdisk_mount: directory followed by file") {
    // Header 1: directory (512 bytes, no data)
    // Header 2: file (512 bytes, no data)
    // Header 3: all-zero (end-of-archive)
    uint8_t archive[1536];
    std::memset(archive, 0, sizeof(archive));
    ramdisk_test::build_ustar_header(archive, "dir/", 0, UstarType::DIRECTORY);
    ramdisk_test::build_ustar_header(archive + 512, "dir/file.txt", 0, UstarType::REGULAR);
    // Third header is all-zero

    auto result = ramdisk_test::mount_archive(archive, sizeof(archive));
    ASSERT_EQ(result.entry_count, 1U);
    ASSERT_EQ(result.dir_count, 1U);
}

TEST("ramdisk_mount: multiple files with data") {
    // File 1: header (512) + 512 bytes data (size=200, rounds to 1 block)
    // File 2: header (512) + 0 bytes data (size=0)
    // End-of-archive: all-zero header
    uint8_t archive[2048];
    std::memset(archive, 0, sizeof(archive));
    ramdisk_test::build_ustar_header(archive, "file1.txt", 200, UstarType::REGULAR);
    ramdisk_test::build_ustar_header(archive + 512 + 512, "file2.txt", 0, UstarType::REGULAR);

    auto result = ramdisk_test::mount_archive(archive, sizeof(archive));
    ASSERT_EQ(result.entry_count, 2U);
    ASSERT_FALSE(result.stopped_bad_magic);
}

// ============================================================
// 14. mount_archive: empty archive (all zero bytes)
// ============================================================

TEST("ramdisk_mount: all-zero archive returns zero entries") {
    uint8_t archive[1024];
    std::memset(archive, 0, sizeof(archive));

    auto result = ramdisk_test::mount_archive(archive, sizeof(archive));
    ASSERT_EQ(result.entry_count, 0U);
    ASSERT_EQ(result.dir_count, 0U);
    ASSERT_FALSE(result.stopped_bad_magic);
}

TEST("ramdisk_mount: zero-size archive returns zero entries") {
    auto result = ramdisk_test::mount_archive(nullptr, 0);
    ASSERT_EQ(result.entry_count, 0U);
    ASSERT_EQ(result.dir_count, 0U);
    ASSERT_FALSE(result.stopped_bad_magic);
}

TEST("ramdisk_mount: archive smaller than header returns zero entries") {
    uint8_t small[256];
    std::memset(small, 0, sizeof(small));

    auto result = ramdisk_test::mount_archive(small, sizeof(small));
    ASSERT_EQ(result.entry_count, 0U);
    ASSERT_FALSE(result.stopped_bad_magic);
}

// ============================================================
// 15. mount_archive: invalid magic detection
// ============================================================

TEST("ramdisk_mount: invalid magic stops traversal") {
    uint8_t archive[1024];
    std::memset(archive, 0, sizeof(archive));

    // Build a header with a name but bad magic
    auto* hdr = reinterpret_cast<UstarHeader*>(archive);
    std::memcpy(hdr->name, "badfile.txt", 11);
    std::memcpy(hdr->magic, "bogus", 5);
    hdr->typeflag = UstarType::REGULAR;

    auto result = ramdisk_test::mount_archive(archive, sizeof(archive));
    ASSERT_EQ(result.entry_count, 0U);
    ASSERT_TRUE(result.stopped_bad_magic);
}

TEST("ramdisk_mount: valid entry followed by invalid magic stops") {
    uint8_t archive[1536];
    std::memset(archive, 0, sizeof(archive));

    // First entry: valid
    ramdisk_test::build_ustar_header(archive, "good.txt", 0, UstarType::REGULAR);

    // Second entry: has a name but bad magic
    auto* hdr2 = reinterpret_cast<UstarHeader*>(archive + 512);
    std::memcpy(hdr2->name, "bad.txt", 7);
    std::memcpy(hdr2->magic, "xxxxx", 5);
    hdr2->typeflag = UstarType::REGULAR;

    auto result = ramdisk_test::mount_archive(archive, sizeof(archive));
    ASSERT_EQ(result.entry_count, 1U);  // First valid file counted
    ASSERT_TRUE(result.stopped_bad_magic);
}

// ============================================================
// 16. mount_archive: file data skipping (correct offset advancement)
// ============================================================

TEST("ramdisk_mount: file with 512-byte data correctly skips to next header") {
    // File 1: header (512) + 1 data block (512) [size=512]
    // File 2: header (512) + 0 data blocks [size=0]
    // End-of-archive
    uint8_t archive[2048];
    std::memset(archive, 0, sizeof(archive));

    ramdisk_test::build_ustar_header(archive, "first.bin", 512, UstarType::REGULAR);
    ramdisk_test::build_ustar_header(archive + 512 + 512, "second.bin", 0, UstarType::REGULAR);

    auto result = ramdisk_test::mount_archive(archive, sizeof(archive));
    ASSERT_EQ(result.entry_count, 2U);
}

TEST("ramdisk_mount: file with non-aligned data size correctly rounds up") {
    // File 1: header (512) + 1 data block (512) [size=1, rounds to 1 block]
    // File 2: header (512) [size=0]
    uint8_t archive[2048];
    std::memset(archive, 0, sizeof(archive));

    ramdisk_test::build_ustar_header(archive, "unaligned.bin", 1, UstarType::REGULAR);
    ramdisk_test::build_ustar_header(archive + 512 + 512, "after.bin", 0, UstarType::REGULAR);

    auto result = ramdisk_test::mount_archive(archive, sizeof(archive));
    ASSERT_EQ(result.entry_count, 2U);
}

TEST("ramdisk_mount: file with 1024 bytes uses 2 data blocks") {
    // File 1: header (512) + 2 data blocks (1024) [size=1024]
    // File 2: header (512) [size=0]
    uint8_t archive[2560];
    std::memset(archive, 0, sizeof(archive));

    ramdisk_test::build_ustar_header(archive, "big.bin", 1024, UstarType::REGULAR);
    ramdisk_test::build_ustar_header(archive + 512 + 1024, "next.bin", 0, UstarType::REGULAR);

    auto result = ramdisk_test::mount_archive(archive, sizeof(archive));
    ASSERT_EQ(result.entry_count, 2U);
}

// ============================================================
// 17. mount_archive: entry type filtering
// ============================================================

TEST("ramdisk_mount: hardlink entry not counted as file or dir") {
    uint8_t archive[1024];
    std::memset(archive, 0, sizeof(archive));
    ramdisk_test::build_ustar_header(archive, "link", 0, UstarType::HARDLINK);

    auto result = ramdisk_test::mount_archive(archive, sizeof(archive));
    ASSERT_EQ(result.entry_count, 0U);
    ASSERT_EQ(result.dir_count, 0U);
}

TEST("ramdisk_mount: symlink entry not counted as file or dir") {
    uint8_t archive[1024];
    std::memset(archive, 0, sizeof(archive));
    ramdisk_test::build_ustar_header(archive, "symlink", 0, UstarType::SYMLINK);

    auto result = ramdisk_test::mount_archive(archive, sizeof(archive));
    ASSERT_EQ(result.entry_count, 0U);
    ASSERT_EQ(result.dir_count, 0U);
}

// ============================================================
// 18. RamdiskEntry struct
// ============================================================

TEST("ramdisk_entry: RamdiskEntry struct fields") {
    RamdiskEntry entry{};
    std::memcpy(entry.name, "test.txt", 9);  // includes null terminator
    entry.size = 42;
    entry.data = reinterpret_cast<const void*>(0x1000);

    ASSERT_TRUE(std::strcmp(entry.name, "test.txt") == 0);
    ASSERT_EQ(entry.size, 42ULL);
    ASSERT_TRUE(entry.data == reinterpret_cast<const void*>(0x1000));
}

// ============================================================
// 19. RAMDISK_NAME_MAX constant
// ============================================================

TEST("ramdisk_name_max: RAMDISK_NAME_MAX is 100") {
    ASSERT_EQ(RAMDISK_NAME_MAX, 100U);
}

// ============================================================
// 20. Data-driven: multiple octal conversion cases
// ============================================================

TEST("ramdisk_octal_data_driven: batch of known values") {
    struct TestCase {
        const char* input;
        size_t      len;
        uint64_t    expected;
    };

    const TestCase cases[] = {
        {"0",          1,  0ULL},
        {"1",          1,  1ULL},
        {"7",          1,  7ULL},
        {"10",         2,  8ULL},
        {"17",         2,  15ULL},
        {"20",         2,  16ULL},
        {"77",         2,  63ULL},
        {"100",        3,  64ULL},
        {"200",        3,  128ULL},
        {"400",        3,  256ULL},
        {"777",        3,  511ULL},
        {"1000",       4,  512ULL},
        {"7777",       4,  4095ULL},
        {"10000",      5,  4096ULL},
        {"77777",      5,  32767ULL},
    };

    for (const auto& tc : cases) {
        uint64_t got = ramdisk_test::octal_to_uint(tc.input, tc.len);
        ASSERT_EQ(got, tc.expected);
    }
}

// ============================================================
// 21. Data-driven: data_blocks batch
// ============================================================

TEST("ramdisk_data_blocks_data_driven: batch of known values") {
    struct TestCase {
        uint64_t size;
        uint32_t expected;
    };

    const TestCase cases[] = {
        {0ULL,    0U},
        {1ULL,    1U},
        {511ULL,  1U},
        {512ULL,  1U},
        {513ULL,  2U},
        {1024ULL, 2U},
        {1025ULL, 3U},
        {1536ULL, 3U},
        {1537ULL, 4U},
        {2048ULL, 4U},
    };

    for (const auto& tc : cases) {
        uint32_t got = ramdisk_test::data_blocks(tc.size);
        ASSERT_EQ(got, tc.expected);
    }
}

// ============================================================
// 22. Complex archive: realistic multi-entry scenario
// ============================================================

TEST("ramdisk_mount: realistic multi-entry archive") {
    // Simulate:
    //   dir/            (directory)
    //   dir/hello.txt   (regular, 13 bytes)
    //   dir/world.txt   (regular, 1024 bytes)
    //   readme.txt      (regular, 0 bytes)
    //   [end-of-archive]

    // Layout:
    //   0:      dir/ header        (512)
    //   512:    dir/hello.txt hdr  (512) + 1 data block (512, size=13 rounds up)
    //   1536:   dir/world.txt hdr  (512) + 2 data blocks (1024, size=1024)
    //   3072:   readme.txt header  (512)
    //   3584:   zero header        (512) [end-of-archive]
    //   Total: 4096 bytes

    uint8_t archive[4096];
    std::memset(archive, 0, sizeof(archive));

    ramdisk_test::build_ustar_header(archive + 0,    "dir/",          0,    UstarType::DIRECTORY);
    ramdisk_test::build_ustar_header(archive + 512,   "dir/hello.txt", 13,   UstarType::REGULAR);
    ramdisk_test::build_ustar_header(archive + 1536,  "dir/world.txt", 1024, UstarType::REGULAR);
    ramdisk_test::build_ustar_header(archive + 3072,  "readme.txt",    0,    UstarType::REGULAR);

    auto result = ramdisk_test::mount_archive(archive, sizeof(archive));
    ASSERT_EQ(result.entry_count, 3U);  // 3 regular files
    ASSERT_EQ(result.dir_count, 1U);    // 1 directory
    ASSERT_FALSE(result.stopped_bad_magic);
}

// ============================================================
// Main function
// ============================================================

int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}

#endif  // CINUX_HOST_TEST
