/**
 * @file test/unit/test_pit.cpp
 * @brief Host-side unit tests for Intel 8254 PIT driver encoding logic
 *
 * Test coverage:
 *   - PIT hardware constants (I/O ports, base frequency, command bits)
 *   - Divisor calculation and clamping logic (16-bit range [1, 65535])
 *   - Command byte composition (0x36 = channel 0 + LSB-then-MSB + square wave)
 *   - Divisor byte splitting (low byte = divisor & 0xFF, high byte = divisor >> 8)
 *   - Tick counter arithmetic (get_ticks, get_uptime_ms)
 *   - Uptime calculation: (tick_count * 1000) / freq_hz
 *   - Per-second tick report threshold: (tick_count % freq_hz) == 0
 *   - Edge cases: minimum frequency (19 Hz), maximum frequency (1193182 Hz),
 *     frequency of 1 Hz (divisor clamped to 65535), very high frequency
 *
 * The real PIT code uses x86 inline asm (io_outb) which cannot execute on
 * the host.  We extract the pure arithmetic -- divisor calculation, command
 * byte composition, and uptime tracking -- and verify it in isolation.
 *
 * Compile condition: -DCINUX_HOST_TEST
 */

#define TEST_FRAMEWORK_IMPL
#include "test_framework.h"

#ifdef CINUX_HOST_TEST

#    include <cstdint>

// Include kernel PIT header for constants and types
#    include "drivers/pit/pit.hpp"

using namespace cinux::drivers;

// ============================================================
// Mirror helper functions for PIT logic
// ============================================================
// The PIT init and IRQ handler use io_outb/io_inb (x86 inline asm).
// We replicate the pure arithmetic portions here for host testing.

/**
 * @brief Mirror of PIT divisor calculation with clamping
 *
 * Replicates the logic from PIT::init():
 *   divisor = BASE_FREQ / freq_hz
 *   clamped to [1, 65535]
 */
static uint32_t pit_calc_divisor(uint32_t freq_hz) {
    uint32_t divisor = PitHW::BASE_FREQ / freq_hz;
    if (divisor > 65535) {
        divisor = 65535;
    }
    if (divisor == 0) {
        divisor = 1;
    }
    return divisor;
}

/**
 * @brief Mirror of PIT command byte composition
 *
 * The command byte for channel 0 square-wave mode is:
 *   CMD_CHANNEL_0 | CMD_LSB_MSB | CMD_MODE_3 | CMD_BINARY
 * which equals 0x36.
 */
static uint8_t pit_command_byte() {
    return PitHW::CMD_CHANNEL_0 | PitHW::CMD_LSB_MSB | PitHW::CMD_MODE_3 | PitHW::CMD_BINARY;
}

/**
 * @brief Split divisor into low and high bytes for I/O port writes
 */
static uint8_t pit_divisor_low(uint32_t divisor) {
    return static_cast<uint8_t>(divisor & 0xFF);
}

static uint8_t pit_divisor_high(uint32_t divisor) {
    return static_cast<uint8_t>((divisor >> 8) & 0xFF);
}

/**
 * @brief Mirror of PIT::get_uptime_ms() calculation
 */
static uint64_t pit_uptime_ms(uint64_t tick_count, uint32_t freq_hz) {
    return (tick_count * 1000) / freq_hz;
}

/**
 * @brief Mirror of per-second tick check: should we print the uptime message?
 */
static bool pit_should_print_tick(uint64_t tick_count, uint32_t freq_hz) {
    return (tick_count % freq_hz) == 0;
}

/**
 * @brief Mirror of seconds calculation from tick count
 */
static uint64_t pit_seconds(uint64_t tick_count, uint32_t freq_hz) {
    return tick_count / freq_hz;
}

// ============================================================
// 1. PIT Hardware Port Constants
// ============================================================

/// Verify channel 0 data port is 0x40
TEST("pit: channel 0 port is 0x40") {
    ASSERT_EQ(PitHW::CHANNEL_0, 0x40u);
}

/// Verify channel 1 data port is 0x41
TEST("pit: channel 1 port is 0x41") {
    ASSERT_EQ(PitHW::CHANNEL_1, 0x41u);
}

/// Verify channel 2 data port is 0x42
TEST("pit: channel 2 port is 0x42") {
    ASSERT_EQ(PitHW::CHANNEL_2, 0x42u);
}

/// Verify command register is 0x43
TEST("pit: command register is 0x43") {
    ASSERT_EQ(PitHW::COMMAND, 0x43u);
}

/// Verify ports are contiguous (0x40-0x43)
TEST("pit: ports are contiguous 0x40-0x43") {
    ASSERT_EQ(PitHW::CHANNEL_0, 0x40u);
    ASSERT_EQ(PitHW::COMMAND, 0x43u);
    ASSERT_EQ(static_cast<unsigned>(PitHW::COMMAND - PitHW::CHANNEL_0), 3u);
}

// ============================================================
// 2. PIT Base Frequency
// ============================================================

/// Verify base frequency is 1193182 Hz
TEST("pit: base frequency is 1193182 Hz") {
    ASSERT_EQ(PitHW::BASE_FREQ, 1193182u);
}

/// Verify base frequency is approximately 1.193182 MHz
TEST("pit: base frequency approximately 1.193 MHz") {
    ASSERT_TRUE(PitHW::BASE_FREQ > 1190000u);
    ASSERT_TRUE(PitHW::BASE_FREQ < 1200000u);
}

// ============================================================
// 3. Command Byte Bit Constants
// ============================================================

/// Verify CMD_BINARY is 0x00 (binary mode, not BCD)
TEST("pit: CMD_BINARY is 0x00") {
    ASSERT_EQ(PitHW::CMD_BINARY, 0x00u);
}

/// Verify CMD_BCD is 0x01
TEST("pit: CMD_BCD is 0x01") {
    ASSERT_EQ(PitHW::CMD_BCD, 0x01u);
}

/// Verify CMD_MODE_2 (rate generator) is 0x04
TEST("pit: CMD_MODE_2 is 0x04") {
    ASSERT_EQ(PitHW::CMD_MODE_2, 0x04u);
}

/// Verify CMD_MODE_3 (square wave) is 0x06
TEST("pit: CMD_MODE_3 is 0x06") {
    ASSERT_EQ(PitHW::CMD_MODE_3, 0x06u);
}

/// Verify CMD_LSB_ONLY is 0x10
TEST("pit: CMD_LSB_ONLY is 0x10") {
    ASSERT_EQ(PitHW::CMD_LSB_ONLY, 0x10u);
}

/// Verify CMD_MSB_ONLY is 0x20
TEST("pit: CMD_MSB_ONLY is 0x20") {
    ASSERT_EQ(PitHW::CMD_MSB_ONLY, 0x20u);
}

/// Verify CMD_LSB_MSB is 0x30
TEST("pit: CMD_LSB_MSB is 0x30") {
    ASSERT_EQ(PitHW::CMD_LSB_MSB, 0x30u);
}

/// Verify CMD_CHANNEL_0 is 0x00 (bits 6-5 = 00)
TEST("pit: CMD_CHANNEL_0 is 0x00") {
    ASSERT_EQ(PitHW::CMD_CHANNEL_0, 0x00u);
}

// ============================================================
// 4. Command Byte Composition (0x36)
// ============================================================

/// Verify full command byte is 0x36
TEST("pit: command byte is 0x36") {
    ASSERT_EQ(pit_command_byte(), 0x36u);
}

/// Verify command byte breakdown: channel 0 (bits 7-6 = 00)
TEST("pit: command byte selects channel 0") {
    uint8_t cmd = pit_command_byte();
    ASSERT_EQ(static_cast<unsigned>(cmd >> 6), 0x00u);
}

/// Verify command byte uses LSB-then-MSB access mode (bits 5-4 = 11)
TEST("pit: command byte uses LSB-then-MSB access") {
    uint8_t cmd = pit_command_byte();
    ASSERT_EQ(static_cast<unsigned>((cmd >> 4) & 0x03), 0x03u);
}

/// Verify command byte uses mode 3 / square wave (bits 3-1 = 011)
TEST("pit: command byte uses square wave mode 3") {
    uint8_t cmd = pit_command_byte();
    ASSERT_EQ(static_cast<unsigned>((cmd >> 1) & 0x07), 0x03u);
}

/// Verify command byte uses binary counting (bit 0 = 0)
TEST("pit: command byte uses binary counting") {
    uint8_t cmd = pit_command_byte();
    ASSERT_FALSE(cmd & 0x01);
}

// ============================================================
// 5. Divisor Calculation
// ============================================================

/// Verify default 100 Hz frequency gives divisor ~11931
TEST("pit: 100 Hz divisor is 11931") {
    uint32_t divisor = pit_calc_divisor(100);
    ASSERT_EQ(divisor, 11931u);  // 1193182 / 100 = 11931
}

/// Verify 1000 Hz frequency gives divisor ~1193
TEST("pit: 1000 Hz divisor is 1193") {
    uint32_t divisor = pit_calc_divisor(1000);
    ASSERT_EQ(divisor, 1193u);  // 1193182 / 1000 = 1193
}

/// Verify maximum frequency (base clock / 1) gives divisor 1
TEST("pit: max frequency divisor is 1") {
    uint32_t divisor = pit_calc_divisor(PitHW::BASE_FREQ);
    ASSERT_EQ(divisor, 1u);
}

/// Verify minimum usable frequency (19 Hz) gives divisor 62799
TEST("pit: 19 Hz divisor is near max 16-bit") {
    uint32_t divisor = pit_calc_divisor(19);
    // 1193182 / 19 = 62799 (still within 16-bit range)
    ASSERT_EQ(divisor, 62799u);
}

/// Verify divisor for 1 Hz is clamped to 65535 (max 16-bit value)
TEST("pit: 1 Hz divisor clamped to 65535") {
    // 1193182 / 1 = 1193182, but this exceeds 16-bit range
    uint32_t divisor = pit_calc_divisor(1);
    ASSERT_EQ(divisor, 65535u);
}

/// Verify divisor for very low frequency (0.01 Hz equivalent) is clamped
TEST("pit: sub-Hz frequency divisor clamped to 65535") {
    // With freq_hz = 1, divisor would be 1193182 > 65535 -> clamped
    uint32_t divisor = pit_calc_divisor(1);
    ASSERT_EQ(divisor, 65535u);
}

/// Verify divisor for extremely high frequency gives 1
TEST("pit: very high frequency divisor is 1") {
    // freq_hz = 1193182 -> divisor = 1
    // freq_hz = 2000000 -> 1193182/2000000 = 0 -> clamped to 1
    uint32_t divisor = pit_calc_divisor(2000000);
    ASSERT_EQ(divisor, 1u);
}

/// Verify divisor for 18.2 Hz (original PC tick rate) is ~65535
TEST("pit: 18 Hz divisor") {
    // 1193182 / 18 = 66287 -> clamped to 65535
    uint32_t divisor = pit_calc_divisor(18);
    ASSERT_EQ(divisor, 65535u);
}

// ============================================================
// 6. Divisor Byte Splitting
// ============================================================

/// Verify 100 Hz divisor (11931) splits correctly
TEST("pit: 100 Hz divisor byte split") {
    uint32_t divisor = pit_calc_divisor(100);
    ASSERT_EQ(divisor, 11931u);
    // 11931 = 0x2E9B
    ASSERT_EQ(pit_divisor_low(divisor), 0x9Bu);
    ASSERT_EQ(pit_divisor_high(divisor), 0x2Eu);
}

/// Verify 1000 Hz divisor (1193) splits correctly
TEST("pit: 1000 Hz divisor byte split") {
    uint32_t divisor = pit_calc_divisor(1000);
    ASSERT_EQ(divisor, 1193u);
    // 1193 = 0x04A9
    ASSERT_EQ(pit_divisor_low(divisor), 0xA9u);
    ASSERT_EQ(pit_divisor_high(divisor), 0x04u);
}

/// Verify divisor 1 splits to (low=1, high=0)
TEST("pit: divisor 1 byte split") {
    ASSERT_EQ(pit_divisor_low(1), 0x01u);
    ASSERT_EQ(pit_divisor_high(1), 0x00u);
}

/// Verify divisor 65535 (0xFFFF) splits to (low=0xFF, high=0xFF)
TEST("pit: divisor 65535 byte split") {
    ASSERT_EQ(pit_divisor_low(65535), 0xFFu);
    ASSERT_EQ(pit_divisor_high(65535), 0xFFu);
}

/// Verify divisor 256 splits to (low=0, high=1)
TEST("pit: divisor 256 byte split") {
    ASSERT_EQ(pit_divisor_low(256), 0x00u);
    ASSERT_EQ(pit_divisor_high(256), 0x01u);
}

/// Verify round-trip: reconstructing divisor from low + high bytes
TEST("pit: divisor round-trip from byte split") {
    uint32_t divisor       = pit_calc_divisor(100);
    uint8_t  lo            = pit_divisor_low(divisor);
    uint8_t  hi            = pit_divisor_high(divisor);
    uint32_t reconstructed = static_cast<uint32_t>(lo) | (static_cast<uint32_t>(hi) << 8);
    ASSERT_EQ(reconstructed, divisor);
}

// ============================================================
// 7. Uptime Calculation
// ============================================================

/// Verify uptime at 0 ticks is 0 ms
TEST("pit: uptime 0 ticks is 0 ms") {
    ASSERT_EQ(pit_uptime_ms(0, 100), 0u);
}

/// Verify uptime at 100 ticks (1 second) at 100 Hz is 1000 ms
TEST("pit: uptime 100 ticks at 100 Hz is 1000 ms") {
    ASSERT_EQ(pit_uptime_ms(100, 100), 1000u);
}

/// Verify uptime at 50 ticks (0.5 seconds) at 100 Hz is 500 ms
TEST("pit: uptime 50 ticks at 100 Hz is 500 ms") {
    ASSERT_EQ(pit_uptime_ms(50, 100), 500u);
}

/// Verify uptime at 1 tick at 1000 Hz is 1 ms
TEST("pit: uptime 1 tick at 1000 Hz is 1 ms") {
    ASSERT_EQ(pit_uptime_ms(1, 1000), 1u);
}

/// Verify uptime at 1000 ticks at 1000 Hz is 1000 ms
TEST("pit: uptime 1000 ticks at 1000 Hz is 1000 ms") {
    ASSERT_EQ(pit_uptime_ms(1000, 1000), 1000u);
}

/// Verify uptime at 6000 ticks at 100 Hz is 60000 ms (1 minute)
TEST("pit: uptime 6000 ticks at 100 Hz is 60000 ms") {
    ASSERT_EQ(pit_uptime_ms(6000, 100), 60000u);
}

/// Verify uptime calculation handles large tick counts without overflow
TEST("pit: uptime handles large tick counts") {
    // 1 hour at 100 Hz = 360000 ticks = 3600000 ms
    uint64_t ticks = 360000;
    ASSERT_EQ(pit_uptime_ms(ticks, 100), 3600000u);
}

// ============================================================
// 8. Per-Second Tick Report Threshold
// ============================================================

/// Verify tick 0 triggers a report (modulo is 0)
TEST("pit: tick 0 triggers second report") {
    // tick_count starts at 0 before first increment.
    // After the first increment (tick_count = 1), modulo check is:
    ASSERT_TRUE(pit_should_print_tick(0, 100));
}

/// Verify tick 100 triggers a report at 100 Hz
TEST("pit: tick 100 triggers report at 100 Hz") {
    ASSERT_TRUE(pit_should_print_tick(100, 100));
}

/// Verify tick 99 does NOT trigger a report at 100 Hz
TEST("pit: tick 99 does not trigger report at 100 Hz") {
    ASSERT_FALSE(pit_should_print_tick(99, 100));
}

/// Verify tick 200 triggers a report at 100 Hz (2nd second)
TEST("pit: tick 200 triggers report at 100 Hz") {
    ASSERT_TRUE(pit_should_print_tick(200, 100));
}

/// Verify tick 50 triggers a report at 50 Hz
TEST("pit: tick 50 triggers report at 50 Hz") {
    ASSERT_TRUE(pit_should_print_tick(50, 50));
}

/// Verify tick 51 does NOT trigger a report at 50 Hz
TEST("pit: tick 51 does not trigger report at 50 Hz") {
    ASSERT_FALSE(pit_should_print_tick(51, 50));
}

/// Verify tick 1000 triggers a report at 1000 Hz (1 second elapsed)
TEST("pit: tick 1000 triggers report at 1000 Hz") {
    ASSERT_TRUE(pit_should_print_tick(1000, 1000));
}

/// Verify tick 999 does NOT trigger a report at 1000 Hz
TEST("pit: tick 999 does not trigger report at 1000 Hz") {
    ASSERT_FALSE(pit_should_print_tick(999, 1000));
}

// ============================================================
// 9. Seconds Calculation
// ============================================================

/// Verify 0 ticks = 0 seconds
TEST("pit: 0 ticks is 0 seconds") {
    ASSERT_EQ(pit_seconds(0, 100), 0u);
}

/// Verify 100 ticks at 100 Hz = 1 second
TEST("pit: 100 ticks at 100 Hz is 1 second") {
    ASSERT_EQ(pit_seconds(100, 100), 1u);
}

/// Verify 200 ticks at 100 Hz = 2 seconds
TEST("pit: 200 ticks at 100 Hz is 2 seconds") {
    ASSERT_EQ(pit_seconds(200, 100), 2u);
}

/// Verify 150 ticks at 100 Hz = 1 second (integer division)
TEST("pit: 150 ticks at 100 Hz is 1 second") {
    ASSERT_EQ(pit_seconds(150, 100), 1u);
}

/// Verify 6000 ticks at 100 Hz = 60 seconds (1 minute)
TEST("pit: 6000 ticks at 100 Hz is 60 seconds") {
    ASSERT_EQ(pit_seconds(6000, 100), 60u);
}

// ============================================================
// 10. Frequency and Divisor Relationship
// ============================================================

/// Verify effective frequency is close to requested frequency
TEST("pit: effective 100 Hz close to requested") {
    uint32_t divisor   = pit_calc_divisor(100);
    uint32_t effective = PitHW::BASE_FREQ / divisor;
    // 1193182 / 11931 = 100.00...
    ASSERT_EQ(effective, 100u);
}

/// Verify effective 1000 Hz is exact
TEST("pit: effective 1000 Hz is exact") {
    uint32_t divisor   = pit_calc_divisor(1000);
    uint32_t effective = PitHW::BASE_FREQ / divisor;
    ASSERT_EQ(effective, 1000u);
}

/// Verify clamped 1 Hz effective frequency differs from requested
TEST("pit: clamped 1 Hz effective is not 1 Hz") {
    // Divisor clamped to 65535, effective = 1193182/65535 ~ 18.2 Hz
    uint32_t divisor   = pit_calc_divisor(1);
    uint32_t effective = PitHW::BASE_FREQ / divisor;
    ASSERT_TRUE(effective > 1u);  // Cannot actually achieve 1 Hz
}

// ============================================================
// 11. Mode Constants (mode 0 vs mode 2 vs mode 3)
// ============================================================

/// Verify mode 0 (interrupt on terminal count) is 0x00
TEST("pit: CMD_MODE_0 is 0x00") {
    ASSERT_EQ(PitHW::CMD_MODE_0, 0x00u);
}

/// Verify mode 2 (rate generator) bits occupy bits 3-1 of command
TEST("pit: mode 2 bits are in correct position") {
    // CMD_MODE_2 = 0x04 = bit 2 set (binary 100 in bits 3-1)
    ASSERT_EQ(PitHW::CMD_MODE_2, 0x04u);
}

/// Verify mode 3 (square wave) bits are different from mode 2
TEST("pit: mode 3 differs from mode 2") {
    ASSERT_NE(PitHW::CMD_MODE_3, PitHW::CMD_MODE_2);
}

/// Verify LATCH command is 0x00 (used for read-back, not in our init)
TEST("pit: CMD_LATCH is 0x00") {
    ASSERT_EQ(PitHW::CMD_LATCH, 0x00u);
}

// ============================================================
// Main Function
// ============================================================

/**
 * @brief Test entry point
 */
int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}

#endif  // CINUX_HOST_TEST
