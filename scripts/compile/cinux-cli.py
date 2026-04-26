#!/usr/bin/env python3
"""
cinux-cli.py - Cinux OS build/run automation CLI.

Usage:
    python scripts/compile/cinux-cli.py configure --type Release --tests
    python scripts/compile/cinux-cli.py build -j8
    python scripts/compile/cinux-cli.py run run-debug
    python scripts/compile/cinux-cli.py test --scope host
    python scripts/compile/cinux-cli.py check
    python scripts/compile/cinux-cli.py all --target run
"""

from __future__ import annotations

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cinux_utils import (
    CINUX_VERSION,
    EXIT_SUCCESS,
    EXIT_FAILURE,
    EXIT_CONFIG_ERROR,
    Logger,
    find_project_root,
    detect_job_count,
    CMakeOptions,
    CMakeRunner,
    check_toolchain,
    format_check_results,
    run_ctest,
    TestSuiteResult,
    load_build_info,
    VALID_TARGETS,
    TEST_TARGETS,
    check_venv,
)


def _create_common_parser() -> argparse.ArgumentParser:
    """Shared flags that appear on every subcommand via parent parser."""
    common = argparse.ArgumentParser(add_help=False)
    out = common.add_mutually_exclusive_group()
    out.add_argument("-q", "--quiet", action="store_true", help="Suppress non-error output")
    out.add_argument("-v", "--verbose", action="store_true", help="Show debug-level output")
    common.add_argument("--build-dir", default="build", help="Build directory (default: build)")
    common.add_argument("--project-root", default=None, help="Project root (auto-detected)")
    return common


def create_parser() -> argparse.ArgumentParser:
    common = _create_common_parser()

    parser = argparse.ArgumentParser(
        prog="cinux-cli",
        description="Cinux OS build/run automation CLI",
        parents=[common],
    )
    parser.add_argument("--version", action="version", version=f"cinux-cli {CINUX_VERSION}")

    sub = parser.add_subparsers(dest="command", required=True)

    # -- configure --
    p_cfg = sub.add_parser("configure", help="Configure CMake", parents=[common])
    _add_cmake_options(p_cfg)

    # -- build --
    p_build = sub.add_parser("build", help="Build the project", parents=[common])
    p_build.add_argument("--target", default=None, help="Build target name")
    p_build.add_argument("-j", "--jobs", type=int, default=None, help="Parallel job count")

    # -- run --
    p_run = sub.add_parser("run", help="Run a build target", parents=[common])
    p_run.add_argument("target", choices=VALID_TARGETS, help="Target to run")

    # -- test --
    p_test = sub.add_parser("test", help="Run test suite", parents=[common])
    p_test.add_argument(
        "--scope",
        choices=["host", "kernel", "all"],
        default="all",
        help="Which test scope to run (default: all)",
    )
    p_test.add_argument("--test-verbose", action="store_true", help="Verbose test output")
    p_test.add_argument("--filter", default=None, help="CTest -R regex filter")
    p_test.add_argument("--timeout", type=int, default=None, help="Per-test timeout in seconds")

    # -- clean --
    sub.add_parser("clean", help="Remove build directory", parents=[common])

    # -- check --
    sub.add_parser("check", help="Check toolchain prerequisites", parents=[common])

    # -- all --
    p_all = sub.add_parser("all", help="Configure + build + run", parents=[common])
    _add_cmake_options(p_all)
    p_all.add_argument("--target", default="run", help="Target to run (default: run)")
    p_all.add_argument("-j", "--jobs", type=int, default=None, help="Parallel job count")

    return parser


def _add_cmake_options(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--type", choices=["Release", "Debug"], default="Release", dest="build_type")
    parser.add_argument("--tests", action="store_true", default=True, dest="tests")
    parser.add_argument("--no-tests", action="store_false", dest="tests")
    parser.add_argument("--gui", action="store_true", default=True, dest="gui")
    parser.add_argument("--no-gui", action="store_false", dest="gui")
    parser.add_argument("--toolchain", default=None, help="CMAKE_TOOLCHAIN_FILE")
    parser.add_argument("-D", action="append", dest="cmake_defs", default=[], help="Extra CMake definitions")


def _make_options(args: argparse.Namespace) -> CMakeOptions:
    extra = []
    for d in getattr(args, "cmake_defs", []) or []:
        if not d.startswith("-D"):
            extra.append(f"-D{d}")
        else:
            extra.append(d)
    return CMakeOptions(
        build_type=getattr(args, "build_type", "Release"),
        build_tests=getattr(args, "tests", True),
        gui=getattr(args, "gui", True),
        toolchain_file=getattr(args, "toolchain", None),
        extra_args=extra or None,
    )


# ---------------------------------------------------------------------------
# Subcommand handlers
# ---------------------------------------------------------------------------

def cmd_configure(args: argparse.Namespace, runner: CMakeRunner, logger: Logger) -> int:
    options = _make_options(args)
    logger.step(1, 1, f"Configuring CMake ({options.build_type} mode)")
    result = runner.configure(options)
    if result.returncode != 0:
        logger.error("CMake configure failed")
        return EXIT_CONFIG_ERROR
    logger.success(f"Configured in {runner.build_dir}")
    return EXIT_SUCCESS


def cmd_build(args: argparse.Namespace, runner: CMakeRunner, logger: Logger) -> int:
    target = args.target
    jobs = args.jobs or detect_job_count()
    label = f"target '{target}'" if target else "all targets"
    logger.step(1, 1, f"Building {label} ({jobs} jobs)")
    result = runner.build(target=target, jobs=jobs)
    if result.returncode != 0:
        logger.error(f"Build failed with exit code {result.returncode}")
        return EXIT_FAILURE
    info = load_build_info(runner.build_dir)
    duration = info.build_time if info else 0.0
    logger.success(f"Build completed in {duration:.1f}s")
    return EXIT_SUCCESS


def cmd_run(args: argparse.Namespace, runner: CMakeRunner, logger: Logger) -> int:
    target = args.target
    logger.step(1, 1, f"Running target '{target}'")
    result = runner.build(target=target, jobs=detect_job_count())
    if result.returncode != 0:
        logger.error(f"Target '{target}' exited with code {result.returncode}")
        return EXIT_FAILURE
    logger.success(f"Target '{target}' completed")
    return EXIT_SUCCESS


def cmd_test(args: argparse.Namespace, runner: CMakeRunner, logger: Logger) -> int:
    scope = args.scope
    any_failed = False

    if scope in ("host", "all"):
        logger.step(1 if scope == "all" else 1, 2 if scope == "all" else 1, "Running host tests")
        build_result = runner.build(target="test_host", jobs=detect_job_count())
        if build_result.returncode != 0:
            logger.error("Failed to build test_host target")
            any_failed = True
        else:
            proc, host_result = run_ctest(
                runner.build_dir,
                verbose=getattr(args, "test_verbose", False),
                filter_regex=args.filter,
                timeout=args.timeout,
            )
            _print_test_summary("Host", host_result, logger)
            if host_result.failed > 0:
                any_failed = True

    if scope in ("kernel", "all"):
        step_n = 2 if scope == "all" else 1
        step_t = 2 if scope == "all" else 1
        for target in TEST_TARGETS:
            logger.step(step_n, step_t, f"Running kernel test: {target}")
            proc = runner.build(target=target, jobs=detect_job_count())
            passed = proc.returncode == 0
            tag = "PASSED" if passed else "FAILED"
            (logger.success if passed else logger.error)(f"{target}: {tag}")
            if not passed:
                any_failed = True

    return EXIT_SUCCESS if not any_failed else EXIT_FAILURE


def cmd_clean(args: argparse.Namespace, runner: CMakeRunner, logger: Logger) -> int:
    runner.clean()
    logger.success(f"Cleaned {runner.build_dir}")
    return EXIT_SUCCESS


def cmd_check(args: argparse.Namespace, logger: Logger) -> int:
    results = check_toolchain()
    logger.info("Checking toolchain...")
    print(format_check_results(results))
    all_found = all(r.found for r in results)
    if all_found:
        logger.success("All required tools are installed!")
    else:
        logger.error("Some tools are missing. See above for install hints.")
    return EXIT_SUCCESS if all_found else EXIT_FAILURE


def cmd_all(args: argparse.Namespace, runner: CMakeRunner, logger: Logger) -> int:
    options = _make_options(args)
    target = args.target
    jobs = args.jobs or detect_job_count()

    logger.step(1, 3, f"Configuring CMake ({options.build_type} mode)")
    result = runner.configure(options)
    if result.returncode != 0:
        logger.error("CMake configure failed")
        return EXIT_CONFIG_ERROR

    logger.step(2, 3, f"Building ({jobs} jobs)")
    result = runner.build(jobs=jobs)
    if result.returncode != 0:
        logger.error("Build failed")
        return EXIT_FAILURE

    logger.step(3, 3, f"Running target '{target}'")
    result = runner.build(target=target, jobs=jobs)
    if result.returncode != 0:
        logger.error(f"Target '{target}' failed")
        return EXIT_FAILURE

    logger.success("All steps completed!")
    return EXIT_SUCCESS


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _print_test_summary(label: str, result: TestSuiteResult, logger: Logger) -> None:
    if result.failed == 0:
        logger.success(f"{label}: {result.passed}/{result.total} passed ({result.duration:.2f}s)")
    else:
        logger.error(f"{label}: {result.failed}/{result.total} FAILED ({result.duration:.2f}s)")
        for t in result.tests:
            if t.status == "Failed":
                logger.error(f"  - {t.name}")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

HANDLERS = {
    "configure": cmd_configure,
    "build": cmd_build,
    "run": cmd_run,
    "test": cmd_test,
    "clean": cmd_clean,
    "check": cmd_check,
    "all": cmd_all,
}


def main() -> int:
    parser = create_parser()
    args = parser.parse_args()

    logger = Logger(quiet=args.quiet, verbose=args.verbose)

    if args.command == "check":
        return cmd_check(args, logger)

    try:
        project_root = find_project_root(args.project_root)
    except FileNotFoundError as e:
        logger.error(str(e))
        return EXIT_CONFIG_ERROR

    check_venv(project_root)

    runner = CMakeRunner(project_root, args.build_dir, logger)

    handler = HANDLERS.get(args.command)
    if handler:
        return handler(args, runner, logger)

    parser.print_help()
    return EXIT_CONFIG_ERROR


if __name__ == "__main__":
    sys.exit(main())
