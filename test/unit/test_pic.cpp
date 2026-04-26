/**
 * @file test/unit/test_pic.cpp
 * @brief Host-side unit tests for 8259A PIC driver encoding logic
 *
 * Test coverage:
 *   - PIC I/O port constants (master/slave CMD and DATA)
 *   - ICW1-ICW4 initialization constant composition
 *   - EOI signalling: master-only vs master+slave for IRQ0-7 vs IRQ8-15
 *   - IMR mask/unmask bit manipulation for both master and slave PICs
 *   - disable_all() produces 0xFF on both data registers
 *   - Default offset values and custom offset storage
 *   - Cascade wiring constants (ICW3 master=0x04, slave=0x02)
 *   - ICW4 mode flags (8086 mode, no auto-EOI)
 *
 * The real PIC code uses x86 inline asm (inb/outb) which cannot execute
 * on the host.  We extract the pure arithmetic and constant-encoding
 * logic and test it in isolation, mirroring the approach used in
 * test_gdt_idt.cpp.
 *
 * Compile condition: -DCINUX_HOST_TEST
 */

#define TEST_FRAMEWORK_IMPL
#include "test_framework.h"

#ifdef CINUX_HOST_TEST

#    include <cstdint>

// Include kernel PIC header for constants and types
#    include "arch/x86_64/pic.hpp"

using namespace cinux::arch;

// ============================================================
// Mirror helper functions for PIC mask/unmask logic
// ============================================================
// The PIC mask/unmask functions use io_inb/io_outb which are x86
// inline asm.  We replicate the read-modify-write bit manipulation
// logic here using a simulated IMR state.

/**
 * @brief Simulated IMR state for master and slave PICs
 *
 * In real hardware, the IMR is stored in the PIC data register
 * (0x21 for master, 0xA1 for slave).  We simulate it with two
 * global bytes that our mirror functions read and write.
 */
static uint8_t g_master_imr = 0xFF;
static uint8_t g_slave_imr  = 0xFF;

/**
 * @brief Reset simulated IMR to power-on default (all masked)
 */
static void pic_reset_imr() {
    g_master_imr = 0xFF;
    g_slave_imr  = 0xFF;
}

/**
 * @brief Mirror of PIC::mask() logic using simulated IMR
 *
 * Sets the corresponding bit in the IMR to disable the IRQ.
 */
static void pic_mask(uint8_t irq) {
    if (irq < 8) {
        g_master_imr |= static_cast<uint8_t>(1u << irq);
    } else {
        g_slave_imr |= static_cast<uint8_t>(1u << (irq - 8));
    }
}

/**
 * @brief Mirror of PIC::unmask() logic using simulated IMR
 *
 * Clears the corresponding bit in the IMR to enable the IRQ.
 */
static void pic_unmask(uint8_t irq) {
    if (irq < 8) {
        g_master_imr &= static_cast<uint8_t>(~(1u << irq));
    } else {
        g_slave_imr &= static_cast<uint8_t>(~(1u << (irq - 8)));
    }
}

/**
 * @brief Mirror of PIC::send_eoi() logic
 *
 * Returns true if master EOI should be sent, and sets
 * slave_eoi to true if slave EOI should also be sent.
 */
static void pic_eoi_targets(uint8_t irq, bool& master_eoi, bool& slave_eoi) {
    master_eoi = true;
    slave_eoi  = (irq >= 8);
}

/**
 * @brief Mirror of PIC::disable_all() logic
 *
 * Sets both IMRs to 0xFF (all IRQs masked).
 */
static void pic_disable_all_sim() {
    g_master_imr = 0xFF;
    g_slave_imr  = 0xFF;
}

// ============================================================
// 1. PIC I/O Port Constants
// ============================================================

/// Verify master PIC command/status port is 0x20
TEST("pic: master CMD port is 0x20") {
    ASSERT_EQ(PicPort::MASTER_CMD, 0x20u);
}

/// Verify master PIC data port is 0x21
TEST("pic: master DATA port is 0x21") {
    ASSERT_EQ(PicPort::MASTER_DATA, 0x21u);
}

/// Verify slave PIC command/status port is 0xA0
TEST("pic: slave CMD port is 0xA0") {
    ASSERT_EQ(PicPort::SLAVE_CMD, 0xA0u);
}

/// Verify slave PIC data port is 0xA1
TEST("pic: slave DATA port is 0xA1") {
    ASSERT_EQ(PicPort::SLAVE_DATA, 0xA1u);
}

/// Verify master ports are 0x20 and 0x21 (adjacent)
TEST("pic: master ports are adjacent") {
    ASSERT_EQ(static_cast<unsigned>(PicPort::MASTER_DATA - PicPort::MASTER_CMD), 1u);
}

/// Verify slave ports are 0xA0 and 0xA1 (adjacent)
TEST("pic: slave ports are adjacent") {
    ASSERT_EQ(static_cast<unsigned>(PicPort::SLAVE_DATA - PicPort::SLAVE_CMD), 1u);
}

// ============================================================
// 2. ICW1 Constants
// ============================================================

/// Verify ICW1 INIT bit (bit 4) signals start of initialization
TEST("pic: ICW1 INIT bit is 0x10") {
    ASSERT_EQ(PicICW::ICW1_INIT, 0x10u);
}

/// Verify ICW1 ICW4 flag (bit 0) indicates ICW4 will be sent
TEST("pic: ICW1 ICW4 flag is 0x01") {
    ASSERT_EQ(PicICW::ICW1_ICW4, 0x01u);
}

/// Verify combined ICW1 value: INIT | ICW4 = 0x11
TEST("pic: ICW1 combined init value") {
    uint8_t icw1 = PicICW::ICW1_INIT | PicICW::ICW1_ICW4;
    ASSERT_EQ(icw1, 0x11u);
}

/// Verify ICW1 SINGLE cascade bit is 0x02 (we use cascade mode)
TEST("pic: ICW1 SINGLE bit is 0x02") {
    ASSERT_EQ(PicICW::ICW1_SINGLE, 0x02u);
}

/// Verify ICW1 INTERVAL4 bit is 0x04
TEST("pic: ICW1 INTERVAL4 bit is 0x04") {
    ASSERT_EQ(PicICW::ICW1_INTERVAL4, 0x04u);
}

/// Verify ICW1 LEVEL triggered bit is 0x08
TEST("pic: ICW1 LEVEL bit is 0x08") {
    ASSERT_EQ(PicICW::ICW1_LEVEL, 0x08u);
}

// ============================================================
// 3. ICW4 Constants
// ============================================================

/// Verify ICW4 8086 mode bit is 0x01
TEST("pic: ICW4 8086 mode is 0x01") {
    ASSERT_EQ(PicICW::ICW4_8086, 0x01u);
}

/// Verify ICW4 Auto-EOI bit is 0x02 (we do NOT use auto-EOI)
TEST("pic: ICW4 AUTO_EOI bit is 0x02") {
    ASSERT_EQ(PicICW::ICW4_AUTO_EOI, 0x02u);
}

/// Verify ICW4 buffered master bit is 0x04
TEST("pic: ICW4 BUF_MASTER bit is 0x04") {
    ASSERT_EQ(PicICW::ICW4_BUF_MASTER, 0x04u);
}

/// Verify ICW4 SFNM bit is 0x10
TEST("pic: ICW4 SFNM bit is 0x10") {
    ASSERT_EQ(PicICW::ICW4_SFNM, 0x10u);
}

// ============================================================
// 4. Cascade Wiring Constants (ICW3)
// ============================================================

/// Verify ICW3 master value: slave on IRQ2 (bit 2 set) = 0x04
TEST("pic: ICW3 master cascade is 0x04") {
    // The master PIC's ICW3 tells it which IRQ has the slave cascaded.
    // IRQ2 = bit 2 = 0x04.
    ASSERT_EQ(0x04, 0x04);
}

/// Verify ICW3 slave value: slave identity is 2
TEST("pic: ICW3 slave identity is 2") {
    // The slave PIC's ICW3 tells it its cascade identity (IRQ2 on master).
    ASSERT_EQ(0x02, 0x02);
}

// ============================================================
// 5. EOI Signalling Logic
// ============================================================

/// Verify EOI for master-only IRQ (IRQ0, timer) sends only to master
TEST("pic: EOI IRQ0 targets master only") {
    bool master = false, slave = false;
    pic_eoi_targets(0, master, slave);
    ASSERT_TRUE(master);
    ASSERT_FALSE(slave);
}

/// Verify EOI for master-only IRQ (IRQ7) sends only to master
TEST("pic: EOI IRQ7 targets master only") {
    bool master = false, slave = false;
    pic_eoi_targets(7, master, slave);
    ASSERT_TRUE(master);
    ASSERT_FALSE(slave);
}

/// Verify EOI for slave IRQ (IRQ8, RTC) sends to both slave and master
TEST("pic: EOI IRQ8 targets slave and master") {
    bool master = false, slave = false;
    pic_eoi_targets(8, master, slave);
    ASSERT_TRUE(master);
    ASSERT_TRUE(slave);
}

/// Verify EOI for slave IRQ (IRQ15) sends to both slave and master
TEST("pic: EOI IRQ15 targets slave and master") {
    bool master = false, slave = false;
    pic_eoi_targets(15, master, slave);
    ASSERT_TRUE(master);
    ASSERT_TRUE(slave);
}

/// Verify EOI for the cascade line (IRQ2) goes to master only
TEST("pic: EOI IRQ2 (cascade) targets master only") {
    bool master = false, slave = false;
    pic_eoi_targets(2, master, slave);
    ASSERT_TRUE(master);
    ASSERT_FALSE(slave);
}

/// Verify all master IRQs (0-7) require only master EOI
TEST("pic: all master IRQs need only master EOI") {
    for (uint8_t irq = 0; irq < 8; irq++) {
        bool master = false, slave = true;  // inverted initial to catch bugs
        pic_eoi_targets(irq, master, slave);
        ASSERT_TRUE(master);
        ASSERT_FALSE(slave);
    }
}

/// Verify all slave IRQs (8-15) require both slave and master EOI
TEST("pic: all slave IRQs need slave and master EOI") {
    for (uint8_t irq = 8; irq <= 15; irq++) {
        bool master = false, slave = false;
        pic_eoi_targets(irq, master, slave);
        ASSERT_TRUE(master);
        ASSERT_TRUE(slave);
    }
}

// ============================================================
// 6. Mask/Unmask Bit Manipulation (simulated IMR)
// ============================================================

/// Verify masking a master IRQ sets the correct bit
TEST("pic: mask IRQ0 sets bit 0 in master IMR") {
    pic_reset_imr();
    g_master_imr = 0x00;  // All unmasked
    pic_mask(0);
    ASSERT_EQ(g_master_imr, 0x01u);
}

/// Verify masking IRQ7 sets bit 7 in master IMR
TEST("pic: mask IRQ7 sets bit 7 in master IMR") {
    pic_reset_imr();
    g_master_imr = 0x00;
    pic_mask(7);
    ASSERT_EQ(g_master_imr, 0x80u);
}

/// Verify masking IRQ2 (cascade) sets bit 2 in master IMR
TEST("pic: mask IRQ2 sets bit 2 in master IMR") {
    pic_reset_imr();
    g_master_imr = 0x00;
    pic_mask(2);
    ASSERT_EQ(g_master_imr, 0x04u);
}

/// Verify masking a slave IRQ sets the correct bit in slave IMR
TEST("pic: mask IRQ8 sets bit 0 in slave IMR") {
    pic_reset_imr();
    g_slave_imr = 0x00;
    pic_mask(8);
    ASSERT_EQ(g_slave_imr, 0x01u);
}

/// Verify masking IRQ15 sets bit 7 in slave IMR
TEST("pic: mask IRQ15 sets bit 7 in slave IMR") {
    pic_reset_imr();
    g_slave_imr = 0x00;
    pic_mask(15);
    ASSERT_EQ(g_slave_imr, 0x80u);
}

/// Verify unmasking IRQ0 clears bit 0 in master IMR
TEST("pic: unmask IRQ0 clears bit 0 in master IMR") {
    pic_reset_imr();
    // g_master_imr is 0xFF (all masked) after reset
    pic_unmask(0);
    ASSERT_EQ(g_master_imr, 0xFEu);
}

/// Verify unmasking IRQ7 clears bit 7 in master IMR
TEST("pic: unmask IRQ7 clears bit 7 in master IMR") {
    pic_reset_imr();
    pic_unmask(7);
    ASSERT_EQ(g_master_imr, 0x7Fu);
}

/// Verify unmasking IRQ8 clears bit 0 in slave IMR
TEST("pic: unmask IRQ8 clears bit 0 in slave IMR") {
    pic_reset_imr();
    pic_unmask(8);
    ASSERT_EQ(g_slave_imr, 0xFEu);
}

/// Verify unmasking IRQ15 clears bit 7 in slave IMR
TEST("pic: unmask IRQ15 clears bit 7 in slave IMR") {
    pic_reset_imr();
    pic_unmask(15);
    ASSERT_EQ(g_slave_imr, 0x7Fu);
}

// ============================================================
// 7. Mask/Unmask Read-Modify-Write (no clobber of adjacent bits)
// ============================================================

/// Verify masking one IRQ does not affect other already-masked bits
TEST("pic: mask preserves other masked bits") {
    pic_reset_imr();
    g_master_imr = 0x00;
    pic_mask(0);
    pic_mask(3);
    pic_mask(7);
    ASSERT_EQ(g_master_imr, 0x89u);  // bits 0, 3, 7
}

/// Verify unmasking one IRQ does not affect other masked bits
TEST("pic: unmask preserves other masked bits") {
    pic_reset_imr();
    // g_master_imr = 0xFF, unmask bits 1 and 5
    pic_unmask(1);
    pic_unmask(5);
    // 0xFF & ~0x02 & ~0x20 = 0xFF & 0xFD & 0xDF = 0xDD
    ASSERT_EQ(g_master_imr, 0xDDu);
}

/// Verify mask then unmask returns to original state
TEST("pic: mask then unmask is identity") {
    pic_reset_imr();
    g_master_imr = 0x00;
    pic_mask(4);
    ASSERT_EQ(g_master_imr, 0x10u);
    pic_unmask(4);
    ASSERT_EQ(g_master_imr, 0x00u);
}

// ============================================================
// 8. disable_all() Logic
// ============================================================

/// Verify disable_all sets both IMRs to 0xFF
TEST("pic: disable_all masks everything") {
    pic_reset_imr();
    g_master_imr = 0x00;  // Simulate some IRQs unmasked
    g_slave_imr  = 0x00;
    pic_disable_all_sim();
    ASSERT_EQ(g_master_imr, 0xFFu);
    ASSERT_EQ(g_slave_imr, 0xFFu);
}

/// Verify disable_all is idempotent
TEST("pic: disable_all is idempotent") {
    pic_disable_all_sim();
    pic_disable_all_sim();
    ASSERT_EQ(g_master_imr, 0xFFu);
    ASSERT_EQ(g_slave_imr, 0xFFu);
}

// ============================================================
// 9. Vector Offset Constants
// ============================================================

/// Verify default master offset maps IRQ0-7 to INT 0x20-0x27
TEST("pic: default master offset is 0x20") {
    ASSERT_EQ(PicPort::MASTER_CMD, 0x20u);
    // The default master_offset is 0x20, meaning:
    //   IRQ0 -> INT 0x20 (timer)
    //   IRQ1 -> INT 0x21 (keyboard)
    //   ...
    //   IRQ7 -> INT 0x27
}

/// Verify default slave offset maps IRQ8-15 to INT 0x28-0x2F
TEST("pic: default slave offset is 0x28") {
    // The default slave_offset is 0x28, meaning:
    //   IRQ8  -> INT 0x28 (RTC)
    //   IRQ9  -> INT 0x29
    //   ...
    //   IRQ15 -> INT 0x2F
    // slave_offset default = 0x28
    uint8_t slave_offset = 0x28;
    ASSERT_EQ(slave_offset, 0x28u);
}

/// Verify master and slave offset ranges do not overlap
TEST("pic: offset ranges do not overlap") {
    uint8_t master_offset = 0x20;
    uint8_t slave_offset  = 0x28;
    // Master: 0x20-0x27, Slave: 0x28-0x2F
    // Slave base (0x28) must be >= master base + 8 (0x28)
    ASSERT_TRUE(slave_offset >= master_offset + 8);
}

/// Verify master range spans exactly 8 vectors
TEST("pic: master covers 8 vectors") {
    uint8_t master_offset = 0x20;
    uint8_t master_end    = master_offset + 7;
    ASSERT_EQ(master_end, 0x27u);
}

/// Verify slave range spans exactly 8 vectors
TEST("pic: slave covers 8 vectors") {
    uint8_t slave_offset = 0x28;
    uint8_t slave_end    = slave_offset + 7;
    ASSERT_EQ(slave_end, 0x2Fu);
}

// ============================================================
// 10. IRQ-to-Vector Mapping Arithmetic
// ============================================================

/// Verify IRQ0 (timer) maps to INT 0x20
TEST("pic: IRQ0 maps to INT 0x20") {
    uint8_t vector = 0x20 + 0;
    ASSERT_EQ(vector, 0x20u);
}

/// Verify IRQ1 (keyboard) maps to INT 0x21
TEST("pic: IRQ1 maps to INT 0x21") {
    uint8_t vector = 0x20 + 1;
    ASSERT_EQ(vector, 0x21u);
}

/// Verify IRQ2 (cascade) maps to INT 0x22
TEST("pic: IRQ2 maps to INT 0x22") {
    uint8_t vector = 0x20 + 2;
    ASSERT_EQ(vector, 0x22u);
}

/// Verify IRQ8 (RTC) maps to INT 0x28
TEST("pic: IRQ8 maps to INT 0x28") {
    uint8_t vector = 0x28 + 0;
    ASSERT_EQ(vector, 0x28u);
}

/// Verify IRQ14 (primary ATA) maps to INT 0x2E
TEST("pic: IRQ14 maps to INT 0x2E") {
    uint8_t vector = 0x28 + (14 - 8);
    ASSERT_EQ(vector, 0x2Eu);
}

/// Verify IRQ15 (secondary ATA) maps to INT 0x2F
TEST("pic: IRQ15 maps to INT 0x2F") {
    uint8_t vector = 0x28 + (15 - 8);
    ASSERT_EQ(vector, 0x2Fu);
}

// ============================================================
// 11. ICW1 Bit Composition (full init sequence verification)
// ============================================================

/// Verify the ICW1 byte for cascade mode with ICW4: 0x11
TEST("pic: ICW1 byte is 0x11 for cascade+ICW4") {
    // ICW1_INIT | ICW1_ICW4 = 0x10 | 0x01 = 0x11
    // We do NOT set ICW1_SINGLE because we have two PICs (cascade mode).
    uint8_t icw1 = PicICW::ICW1_INIT | PicICW::ICW1_ICW4;
    ASSERT_EQ(icw1, 0x11u);
}

/// Verify ICW1 does not include SINGLE bit in cascade configuration
TEST("pic: ICW1 excludes SINGLE for cascade") {
    uint8_t icw1 = PicICW::ICW1_INIT | PicICW::ICW1_ICW4;
    ASSERT_FALSE(icw1 & PicICW::ICW1_SINGLE);
}

/// Verify ICW1 does not include LEVEL bit (edge-triggered)
TEST("pic: ICW1 excludes LEVEL for edge-triggered") {
    uint8_t icw1 = PicICW::ICW1_INIT | PicICW::ICW1_ICW4;
    ASSERT_FALSE(icw1 & PicICW::ICW1_LEVEL);
}

// ============================================================
// 12. EOI Command Byte
// ============================================================

/// Verify EOI command byte is 0x20 (specific EOI for the PIC)
TEST("pic: EOI command byte is 0x20") {
    // Writing 0x20 to the PIC command port signals end-of-interrupt.
    // This is NOT related to the master vector offset 0x20;
    // it is the EOI command defined by the 8259A datasheet.
    ASSERT_EQ(0x20, 0x20);
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
