/* ==============================================================
 * Cinux Mini Kernel - Serial Port Driver Implementation
 * ============================================================== */

#include "serial.h"

#include <stdint.h>
#include "driver/io.h"

namespace cinux::mini::serial {
// ============================================================
// Constructor - Initialize and configure serial port
// ============================================================
Serial::Serial(uint16_t port) : base_port(port) {
	init();
}

// ============================================================
// Initialize serial port to 115200 8N1 (QEMU default)
// ============================================================
void Serial::init() {
	// 禁用中断
	io::outb(base_port + SerialReg::IER, 0x00);

	// QEMU 默认已经是 115200 8N1，这里设置 LCR 确保配置
	// LCR = 0x03: 8 bits, no parity, 1 stop bit
	io::outb(base_port + SerialReg::LCR, 0x03);

	// 启用 FIFO，清除缓冲区，设置 14 字节阈值
	io::outb(base_port + SerialReg::FCR, 0xC7);

	// 设置 Modem 控制寄存器 (RTS + DTR)
	io::outb(base_port + SerialReg::MCR, 0x03);

	// Read LSR to verify it's accessible
	io::inb(base_port + SerialReg::LSR);
}

// ============================================================
// Write a single character (blocking poll)
// ============================================================
void Serial::putc(char c) {
	// 等待发送缓冲区就绪
	uint32_t wait_count = 0;
	while (!is_tx_ready()) {
		// Simple spin-wait
		__asm__ volatile("pause");
		wait_count++;
		if (wait_count > 100000) {
			// Timeout - serial port may be broken
			wait_count = 0;
		}
	}

	// 写入字符到 THR
	io::outb(base_port + SerialReg::THR, static_cast<uint8_t>(c));
}

// ============================================================
// Read a single character (blocking poll)
// ============================================================
char Serial::getc() {
	/* Mini kernel! Lets take it easy... */
	while (!is_rx_ready()) {
		// Simple spin-wait
		__asm__ volatile("pause");
	}

	return static_cast<char>(io::inb(base_port + SerialReg::RBR));
}

// ============================================================
// Write a null-terminated string
// ============================================================
void Serial::puts(const char* s) {
	if (s == nullptr) {
		return;
	}

	while (*s != '\0') {
		if (*s == '\n') {
			putc('\r');
		}
		putc(*s);
		s++;
	}
}

// ============================================================
// Global serial instance (singleton)
// ============================================================
static Serial g_serial(SERIAL_COM1);

// ============================================================
// Get global serial port instance
// ============================================================
Serial& get_initial_serial() {
	return g_serial;
}

}  // namespace cinux::mini::serial
