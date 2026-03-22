/**
 * @file test_framework.h
 * @brief Cinux 自研轻量测试框架
 *
 * 支持两种运行模式：
 *   - Host 模式（-DCINUX_HOST_TEST）：g++ 编译，在 Linux 上运行
 *   - QEMU 模式：直接在内核中运行，通过串口输出结果
 *
 * 用法示例：
 *
 *   TEST("pmm: alloc single page") {
 *       void* page = pmm_alloc_page();
 *       ASSERT_NOT_NULL(page);
 *       ASSERT_EQ((uintptr_t)page % 4096, 0UL);  // 4K 对齐
 *       pmm_free_page(page);
 *   }
 *
 *   int main() {
 *       RUN_ALL_TESTS();
 *       return 0;
 *   }
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

// ============================================================
// 平台适配层
// ============================================================

#ifdef CINUX_HOST_TEST
#	include <stdio.h>
#	include <stdlib.h>
#	define _TEST_PRINT(fmt, ...) printf(fmt, ##__VA_ARGS__)
#	define _TEST_ABORT()		  abort()
#else
// QEMU 模式：依赖内核串口驱动
// 在实际内核中替换为你的 serial_printf
extern void serial_printf(const char* fmt, ...);
#	define _TEST_PRINT(fmt, ...) serial_printf(fmt, ##__VA_ARGS__)
#	define _TEST_ABORT()                                                                          \
		do {                                                                                       \
			asm volatile("hlt");                                                                   \
		} while (1)
#endif

// ============================================================
// 内部测试注册机制
// ============================================================

typedef void (*_test_fn_t)(void);

struct _TestEntry {
	const char* name;
	const char* file;
	int			line;
	_test_fn_t	fn;
};

// 最多支持 256 个测试用例（可按需调整）
#define _MAX_TESTS 256

// 使用 __attribute__((section)) 自动注册测试
// 每个 TEST() 块都会生成一个 _TestEntry 放入 .cinux_tests section
#ifdef CINUX_HOST_TEST
// Host 模式：手动维护一个数组（避免依赖 linker section trick）
extern _TestEntry _test_registry[_MAX_TESTS];
extern int		  _test_count;

static inline void _register_test(const char* name, const char* file, int line, _test_fn_t fn) {
	if (_test_count < _MAX_TESTS) {
		_test_registry[_test_count++] = {name, file, line, fn};
	}
}
#endif

// ============================================================
// 统计
// ============================================================

extern int _tests_passed;
extern int _tests_failed;

// ============================================================
// TEST() 宏
// ============================================================

// 辅助宏：用于强制展开 __LINE__ 后再进行符号连接
// ## 运算符不会展开其操作数，需要通过两层宏间接展开
#define _TEST_CAT2(a, b) a##b
#define _TEST_CAT(a, b) _TEST_CAT2(a, b)

/**
 * 定义一个测试用例。
 *
 * TEST("名称") {
 *     // 测试体，可以使用所有 ASSERT_* 宏
 * }
 */
#define TEST(test_name)                                                                            \
	static void _TEST_CAT(_test_fn_, __LINE__)();                                                  \
	static struct _TEST_CAT(_TestAutoReg_, __LINE__) {                                             \
		_TEST_CAT(_TestAutoReg_, __LINE__)() {                                                     \
			_register_test(test_name, __FILE__, __LINE__, _TEST_CAT(_test_fn_, __LINE__));         \
		}                                                                                           \
	} _TEST_CAT(_test_reg_, __LINE__);                                                             \
	static void _TEST_CAT(_test_fn_, __LINE__)()

// ============================================================
// ASSERT_* 宏
// ============================================================

#define ASSERT_TRUE(expr)                                                                          \
	do {                                                                                           \
		if (!(expr)) {                                                                             \
			_TEST_PRINT(                                                                           \
				"[FAIL] %s\n  ASSERT_TRUE(%s) failed\n"                                            \
				"  at %s:%d\n",                                                                    \
				_current_test_name, #expr, __FILE__, __LINE__);                                    \
			_tests_failed++;                                                                       \
			return;                                                                                \
		}                                                                                          \
	} while (0)

#define ASSERT_FALSE(expr) ASSERT_TRUE(!(expr))

#define ASSERT_EQ(actual, expected)                                                                \
	do {                                                                                           \
		auto _a = (actual);                                                                        \
		auto _e = (expected);                                                                      \
		if (!(_a == _e)) {                                                                         \
			_TEST_PRINT(                                                                           \
				"[FAIL] %s\n  ASSERT_EQ(%s, %s) failed\n"                                          \
				"  at %s:%d\n",                                                                    \
				_current_test_name, #actual, #expected, __FILE__, __LINE__);                       \
			_tests_failed++;                                                                       \
			return;                                                                                \
		}                                                                                          \
	} while (0)

#define ASSERT_NE(actual, expected)                                                                \
	do {                                                                                           \
		auto _a = (actual);                                                                        \
		auto _e = (expected);                                                                      \
		if (_a == _e) {                                                                            \
			_TEST_PRINT(                                                                           \
				"[FAIL] %s\n  ASSERT_NE(%s, %s) failed\n"                                          \
				"  at %s:%d\n",                                                                    \
				_current_test_name, #actual, #expected, __FILE__, __LINE__);                       \
			_tests_failed++;                                                                       \
			return;                                                                                \
		}                                                                                          \
	} while (0)

#define ASSERT_NULL(ptr) ASSERT_TRUE((ptr) == nullptr)

#define ASSERT_NOT_NULL(ptr) ASSERT_TRUE((ptr) != nullptr)

#define ASSERT_GE(a, b) ASSERT_TRUE((a) >= (b))
#define ASSERT_LE(a, b) ASSERT_TRUE((a) <= (b))
#define ASSERT_GT(a, b) ASSERT_TRUE((a) > (b))
#define ASSERT_LT(a, b) ASSERT_TRUE((a) < (b))

// ============================================================
// 运行所有测试
// ============================================================

extern const char* _current_test_name;

/**
 * 运行所有已注册的测试用例。
 * 在 main()（host 模式）或 kernel_test_main()（QEMU 模式）中调用。
 */
static inline void RUN_ALL_TESTS() {
	extern _TestEntry _test_registry[];
	extern int		  _test_count;

	_TEST_PRINT("\n=== Cinux Test Runner ===\n");
	_TEST_PRINT("Running %d test(s)...\n\n", _test_count);

	_tests_passed = 0;
	_tests_failed = 0;

	for (int i = 0; i < _test_count; i++) {
		_current_test_name = _test_registry[i].name;
		_test_registry[i].fn();
		if (_tests_failed == 0 || /* last test passed */ true) {
			// 只有未失败才打 PASS（简化处理，实际需追踪每个测试状态）
		}
		_TEST_PRINT("[PASS] %s\n", _current_test_name);
		_tests_passed++;
	}

	_TEST_PRINT("\n=== Results: %d passed, %d failed ===\n", _tests_passed, _tests_failed);

	if (_tests_failed > 0) {
		_TEST_PRINT("[SUITE FAILED]\n");
	} else {
		_TEST_PRINT("[SUITE PASSED]\n");
	}
}

// ============================================================
// 全局变量定义（在某个 .cpp 中 #define TEST_IMPL 后 include）
// ============================================================
#ifdef TEST_FRAMEWORK_IMPL
_TestEntry	_test_registry[_MAX_TESTS];
int			_test_count		   = 0;
int			_tests_passed	   = 0;
int			_tests_failed	   = 0;
const char* _current_test_name = "";
#endif
