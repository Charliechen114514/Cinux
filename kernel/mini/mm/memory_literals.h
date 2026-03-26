/**
 * @file kernel/mini/mm/memory_literals.h
 * @brief Custom Literal Operators for Memory Sizes
 *
 * Provides constexpr user-defined literal operators for KB, MB, GB, TB.
 * Freestanding-compatible - requires no standard library.
 */

#pragma once

#include <stdint.h>

namespace cinux::mini::mm::literals {

/**
 * @brief Kilobyte literal operator
 * @param value Numeric value in kilobytes
 * @return Equivalent bytes as uint64_t
 *
 * Example: 4_KB → 4096
 */
constexpr uint64_t operator""_KB(unsigned long long value) {
	return value * 1024ULL;
}

/**
 * @brief Megabyte literal operator
 * @param value Numeric value in megabytes
 * @return Equivalent bytes as uint64_t
 *
 * Example: 1_MB → 1048576
 */
constexpr uint64_t operator""_MB(unsigned long long value) {
	return value * 1024ULL * 1024ULL;
}

/**
 * @brief Gigabyte literal operator
 * @param value Numeric value in gigabytes
 * @return Equivalent bytes as uint64_t
 *
 * Example: 4_GB → 4294967296
 */
constexpr uint64_t operator""_GB(unsigned long long value) {
	return value * 1024ULL * 1024ULL * 1024ULL;
}

/**
 * @brief Terabyte literal operator
 * @param value Numeric value in terabytes
 * @return Equivalent bytes as uint64_t
 *
 * Example: 1_TB → 1099511627776
 */
constexpr uint64_t operator""_TB(unsigned long long value) {
	return value * 1024ULL * 1024ULL * 1024ULL * 1024ULL;
}

} // namespace cinux::mini::mm::literals

// Import literals into cinux::mini::mm namespace for convenience
namespace cinux::mini::mm {
	using namespace literals;
}
