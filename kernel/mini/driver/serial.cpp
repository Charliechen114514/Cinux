/* ==============================================================
 * Cinux Mini Kernel - Serial Port Driver Implementation
 * ============================================================== */

#include "serial.h"

#include "driver/io.h"

namespace cinux::mini::serial {
// ============================================================
// Constructor - Initialize and configure serial port
// ============================================================
Serial::Serial(uint16_t port) : base_port(port) {
	__asm__ volatile("movb $0x5C, %%al; outb %%al, $0xE9" ::: "eax");  // '\' - before init
	init();
	__asm__ volatile("movb $0x27, %%al; outb %%al, $0xE9" ::: "eax");  // ''' - after init
}

// ============================================================
// Initialize serial port to 115200 8N1 (QEMU default)
// ============================================================
void Serial::init() {
	__asm__ volatile("movb $0x5B, %%al; outb %%al, $0xE9" ::: "eax");  // '[' - init start
	// 禁用中断
	io::outb(base_port + SerialReg::IER, 0x00);
	__asm__ volatile("movb $0x31, %%al; outb %%al, $0xE9" ::: "eax");  // '1' - IER done

	// QEMU 默认已经是 115200 8N1，这里设置 LCR 确保配置
	// LCR = 0x03: 8 bits, no parity, 1 stop bit
	io::outb(base_port + SerialReg::LCR, 0x03);
	__asm__ volatile("movb $0x32, %%al; outb %%al, $0xE9" ::: "eax");  // '2' - LCR done

	// 启用 FIFO，清除缓冲区，设置 14 字节阈值
	io::outb(base_port + SerialReg::FCR, 0xC7);
	__asm__ volatile("movb $0x33, %%al; outb %%al, $0xE9" ::: "eax");  // '3' - FCR done

	// 设置 Modem 控制寄存器 (RTS + DTR)
	io::outb(base_port + SerialReg::MCR, 0x03);
	__asm__ volatile("movb $0x34, %%al; outb %%al, $0xE9" ::: "eax");  // '4' - MCR done
}

// ============================================================
// Write a single character (blocking poll)
// ============================================================
void Serial::putc(char c) {
	// 等待发送缓冲区就绪
	while (!is_tx_ready()) {
		// Simple spin-wait
		__asm__ volatile("pause");
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
