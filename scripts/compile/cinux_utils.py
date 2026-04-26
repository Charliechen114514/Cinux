"""
cinux_utils.py - Shared utilities for Cinux build/run automation scripts.

Provides CMake execution, toolchain checking, CTest parsing, logging,
and platform detection for both cinux-cli.py and cinux-interactive.py.
"""

from __future__ import annotations

import dataclasses
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
from typing import Dict, List, Optional, Tuple

CINUX_VERSION = "0.1.0"

EXIT_SUCCESS = 0
EXIT_FAILURE = 1
EXIT_CONFIG_ERROR = 2

VALID_TARGETS = [
    "run",
    "run-debug",
    "run-gdb",
    "run-kernel-test",
    "run-kernel-test-interactive",
    "run-kernel-test-debug",
    "run-big-kernel-test",
    "run-stress-test",
    "image",
    "test-image",
    "stress-test-image",
    "big-kernel-test-image",
    "test_host",
    "test_verbose",
]

RUN_TARGETS = [
    "run",
    "run-debug",
    "run-gdb",
    "run-kernel-test",
    "run-kernel-test-interactive",
    "run-kernel-test-debug",
    "run-big-kernel-test",
    "run-stress-test",
]

TEST_TARGETS = [
    "run-kernel-test",
    "run-big-kernel-test",
    "run-stress-test",
]

BUILD_INFO_FILE = ".cinux_build_info.json"
PROFILES_FILE = ".cinux_profiles.json"

REQUIRED_TOOLS = [
    ("gcc", "sudo apt install gcc"),
    ("g++", "sudo apt install g++"),
    ("as", "sudo apt install binutils"),
    ("ld", "sudo apt install binutils"),
    ("objcopy", "sudo apt install binutils"),
    ("qemu-system-x86_64", "sudo apt install qemu-system-x86"),
]

CMAKE_MIN_VERSION = "4.1"


# ---------------------------------------------------------------------------
# Color / Logging
# ---------------------------------------------------------------------------

class Colors:
    RED = "\033[0;31m"
    GREEN = "\033[0;32m"
    YELLOW = "\033[1;33m"
    BLUE = "\033[0;34m"
    CYAN = "\033[0;36m"
    BOLD = "\033[1m"
    NC = "\033[0m"


def _supports_color() -> bool:
    if not hasattr(sys.stdout, "isatty"):
        return False
    if not sys.stdout.isatty():
        return False
    return True


class Logger:
    """Colored logging matching scripts/log/logging.sh patterns."""

    def __init__(self, quiet: bool = False, verbose: bool = False):
        self.quiet = quiet
        self.verbose = verbose
        self._color = _supports_color()

    def _c(self, code: str, msg: str) -> str:
        if self._color:
            return f"{code}{msg}{Colors.NC}"
        return msg

    def info(self, msg: str) -> None:
        if not self.quiet:
            print(self._c(Colors.GREEN, f"[INFO] {msg}"))

    def error(self, msg: str) -> None:
        print(self._c(Colors.RED, f"[ERROR] {msg}"), file=sys.stderr)

    def warn(self, msg: str) -> None:
        if not self.quiet:
            print(self._c(Colors.YELLOW, f"[WARN] {msg}"))

    def success(self, msg: str) -> None:
        if not self.quiet:
            print(self._c(Colors.GREEN, f"[SUCCESS] {msg}"))

    def debug(self, msg: str) -> None:
        if self.verbose and not self.quiet:
            print(self._c(Colors.BLUE, f"[DEBUG] {msg}"))

    def cmd(self, msg: str) -> None:
        if not self.quiet:
            print(self._c(Colors.YELLOW, f"[CMD] {msg}"))

    def step(self, n: int, total: int, msg: str) -> None:
        if not self.quiet:
            print(self._c(Colors.CYAN, f"[{n}/{total}] {msg}"))

    def plain(self, msg: str) -> None:
        if not self.quiet:
            print(msg)


# ---------------------------------------------------------------------------
# Platform / Project Detection
# ---------------------------------------------------------------------------

def detect_platform() -> str:
    """Return 'linux', 'wsl', or 'macos'."""
    system = platform.system()
    if system == "Darwin":
        return "macos"
    if system == "Linux":
        try:
            with open("/proc/version", "r") as f:
                version = f.read().lower()
            if "microsoft" in version or "wsl" in version:
                return "wsl"
        except FileNotFoundError:
            pass
        return "linux"
    return system.lower()


def is_wsl() -> bool:
    return detect_platform() == "wsl"


def find_project_root(start: Optional[str] = None) -> str:
    """Walk upward to find the directory containing CMakeLists.txt with project(cinux)."""
    current = os.path.abspath(start or os.path.dirname(os.path.abspath(__file__)))
    for _ in range(20):
        cmake_file = os.path.join(current, "CMakeLists.txt")
        if os.path.isfile(cmake_file):
            try:
                with open(cmake_file, "r") as f:
                    content = f.read()
                if "project(cinux" in content:
                    return current
            except OSError:
                pass
        parent = os.path.dirname(current)
        if parent == current:
            break
        current = parent
    raise FileNotFoundError(
        "Could not find Cinux project root (directory with CMakeLists.txt containing project(cinux))"
    )


def detect_job_count() -> int:
    count = os.cpu_count()
    return count if count else 1


def is_in_venv() -> bool:
    return hasattr(sys, "real_prefix") or (
        hasattr(sys, "base_prefix") and sys.base_prefix != sys.prefix
    )


def check_venv(project_root: str) -> None:
    """Warn if not running inside a virtual environment."""
    if is_in_venv():
        return
    venv_dir = os.path.join(
        os.path.dirname(os.path.abspath(__file__)), ".venv"
    )
    print(
        f"\033[1;33m[WARN]\033[0m Not running inside a Python virtual environment.\n"
        f"  Recommended: run  \033[1mbash scripts/compile/create_env.sh\033[0m  first, then:\n"
        f"  \033[1msource {venv_dir}/bin/activate\033[0m\n",
        file=sys.stderr,
    )


# ---------------------------------------------------------------------------
# CMake Options & Runner
# ---------------------------------------------------------------------------

@dataclasses.dataclass
class CMakeOptions:
    build_type: str = "Release"
    build_tests: bool = True
    gui: bool = True
    toolchain_file: Optional[str] = None
    extra_args: Optional[List[str]] = None

    def to_args(self) -> List[str]:
        args = [
            f"-DCMAKE_BUILD_TYPE={self.build_type}",
            f"-DCINUX_BUILD_TESTS={'ON' if self.build_tests else 'OFF'}",
            f"-DCINUX_GUI={'ON' if self.gui else 'OFF'}",
        ]
        if self.toolchain_file:
            args.append(f"-DCMAKE_TOOLCHAIN_FILE={self.toolchain_file}")
        if self.extra_args:
            args.extend(self.extra_args)
        return args


class CMakeRunner:
    def __init__(
        self,
        project_root: str,
        build_dir: str,
        logger: Logger,
    ):
        self.project_root = project_root
        self.build_dir = (
            build_dir
            if os.path.isabs(build_dir)
            else os.path.join(project_root, build_dir)
        )
        self.logger = logger

    def configure(self, options: CMakeOptions) -> subprocess.CompletedProcess:
        cmd = ["cmake", "-B", self.build_dir] + options.to_args() + ["-S", self.project_root]
        self.logger.cmd(" ".join(cmd))
        result = subprocess.run(cmd, cwd=self.project_root)
        if result.returncode == 0:
            save_build_info(self.build_dir, BuildInfo(
                configure_time=time.time(),
                build_time=0.0,
                build_type=options.build_type,
                tests_enabled=options.build_tests,
                gui_enabled=options.gui,
            ))
        return result

    def build(
        self,
        target: Optional[str] = None,
        jobs: Optional[int] = None,
        capture: bool = False,
    ) -> subprocess.CompletedProcess:
        if not self.is_configured():
            self.logger.error("Project not configured. Run 'configure' first.")
            return subprocess.CompletedProcess([], EXIT_CONFIG_ERROR)

        cmd = ["cmake", "--build", self.build_dir]
        if jobs is None:
            jobs = detect_job_count()
        cmd.append(f"-j{jobs}")
        if target:
            cmd.extend(["--target", target])

        self.logger.cmd(" ".join(cmd))
        start = time.time()
        if capture:
            result = subprocess.run(cmd, cwd=self.project_root, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        else:
            result = subprocess.run(cmd, cwd=self.project_root)
        elapsed = time.time() - start

        if result.returncode == 0:
            info = load_build_info(self.build_dir)
            if info:
                info.build_time = elapsed
                save_build_info(self.build_dir, info)

        return result

    def clean(self) -> None:
        if os.path.isdir(self.build_dir):
            self.logger.info(f"Removing {self.build_dir}")
            shutil.rmtree(self.build_dir)

    def is_configured(self) -> bool:
        return os.path.isfile(os.path.join(self.build_dir, "CMakeCache.txt"))

    def get_cached_option(self, key: str) -> Optional[str]:
        cache = os.path.join(self.build_dir, "CMakeCache.txt")
        if not os.path.isfile(cache):
            return None
        with open(cache, "r") as f:
            for line in f:
                if line.startswith(f"{key}"):
                    if ":" in line and "=" in line:
                        return line.split("=", 1)[1].strip()
        return None


# ---------------------------------------------------------------------------
# Toolchain Checker
# ---------------------------------------------------------------------------

@dataclasses.dataclass
class ToolCheckResult:
    name: str
    found: bool
    path: str = ""
    version: str = ""
    install_hint: str = ""


def _parse_version(version_str: str) -> tuple:
    parts = []
    for p in re.split(r"[.\-]", version_str):
        try:
            parts.append(int(p))
        except ValueError:
            break
    return tuple(parts)


def check_toolchain() -> List[ToolCheckResult]:
    results: List[ToolCheckResult] = []
    for name, hint in REQUIRED_TOOLS:
        path = shutil.which(name)
        if path:
            ver = ""
            try:
                r = subprocess.run(
                    [name, "--version"], capture_output=True, text=True, timeout=5
                )
                first_line = (r.stdout or r.stderr).split("\n")[0]
                m = re.search(r"(\d+\.\d+[\.\-\d]*)", first_line)
                if m:
                    ver = m.group(1)
            except Exception:
                pass
            results.append(ToolCheckResult(name, True, path, ver, hint))
        else:
            results.append(ToolCheckResult(name, False, "", "", hint))

    cmake_path = shutil.which("cmake")
    if cmake_path:
        try:
            r = subprocess.run(
                ["cmake", "--version"], capture_output=True, text=True, timeout=5
            )
            first_line = r.stdout.split("\n")[0]
            m = re.search(r"(\d+\.\d+)", first_line)
            ver = m.group(1) if m else ""
            min_parts = _parse_version(CMAKE_MIN_VERSION)
            cur_parts = _parse_version(ver)
            ok = cur_parts >= min_parts
            results.append(
                ToolCheckResult(
                    f"cmake (>= {CMAKE_MIN_VERSION})",
                    ok,
                    cmake_path,
                    ver,
                    "sudo apt install cmake" if not ok else "",
                )
            )
        except Exception:
            results.append(ToolCheckResult(f"cmake (>= {CMAKE_MIN_VERSION})", False, "", "", "sudo apt install cmake"))
    else:
        results.append(ToolCheckResult(f"cmake (>= {CMAKE_MIN_VERSION})", False, "", "", "sudo apt install cmake"))

    return results


def format_check_results(results: List[ToolCheckResult]) -> str:
    lines = []
    for r in results:
        tag = "[OK]" if r.found else "[MISSING]"
        ver = f" ({r.version})" if r.version and r.found else ""
        hint = f"\n  Install: {r.install_hint}" if not r.found and r.install_hint else ""
        lines.append(f"  {tag} {r.name}{ver}{hint}")
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# CTest Parsing
# ---------------------------------------------------------------------------

@dataclasses.dataclass
class TestResult:
    name: str
    status: str  # "Passed", "Failed", "Not Run"
    duration: float
    output: str = ""

    def to_dict(self) -> Dict:
        return {
            "name": self.name,
            "status": self.status,
            "duration": round(self.duration, 3),
        }


@dataclasses.dataclass
class TestSuiteResult:
    total: int
    passed: int
    failed: int
    not_run: int
    duration: float
    tests: List[TestResult]

    def to_dict(self) -> Dict:
        return {
            "total": self.total,
            "passed": self.passed,
            "failed": self.failed,
            "not_run": self.not_run,
            "duration": round(self.duration, 3),
            "tests": [t.to_dict() for t in self.tests],
        }

    def to_json(self, indent: int = 2) -> str:
        return json.dumps(self.to_dict(), indent=indent)


def parse_ctest_output(stdout: str, stderr: str = "") -> TestSuiteResult:
    """Parse ctest --output-on-failure stdout into structured results."""
    tests: List[TestResult] = []
    total_duration = 0.0

    per_test = re.compile(
        r"(\d+)/\d+\s+Test\s+#\d+:\s+(\S+)\s+\.+\s+(Passed|Failed|\*\*\*Not Run)\s+([\d.]+)\s+sec"
    )
    for m in per_test.finditer(stdout):
        tests.append(TestResult(
            name=m.group(2),
            status="Passed" if m.group(3) == "Passed" else m.group(3).replace("*", "").strip(),
            duration=float(m.group(4)),
        ))

    summary = re.search(r"(\d+)% tests passed,\s+(\d+)\s+tests failed out of\s+(\d+)", stdout)
    if summary:
        total = int(summary.group(3))
        failed = int(summary.group(2))
        passed = total - failed
    else:
        passed = sum(1 for t in tests if t.status == "Passed")
        failed = sum(1 for t in tests if t.status == "Failed")
        total = len(tests)

    dur_match = re.search(r"Total Test time \(real\)\s*=\s*([\d.]+)", stdout)
    if dur_match:
        total_duration = float(dur_match.group(1))

    return TestSuiteResult(
        total=total,
        passed=passed,
        failed=failed,
        not_run=total - passed - failed,
        duration=total_duration,
        tests=tests,
    )


def _find_latest_test_xml(build_dir: str) -> Optional[str]:
    testing_dir = os.path.join(build_dir, "Testing")
    if not os.path.isdir(testing_dir):
        return None
    subdirs = sorted(
        [
            d for d in os.listdir(testing_dir)
            if os.path.isdir(os.path.join(testing_dir, d))
        ],
        reverse=True,
    )
    for d in subdirs:
        xml_path = os.path.join(testing_dir, d, "Test.xml")
        if os.path.isfile(xml_path):
            return xml_path
    return None


def parse_ctest_xml(xml_path: str) -> TestSuiteResult:
    """Parse CTest's Test.xml for reliable structured results."""
    tree = ET.parse(xml_path)
    root = tree.getroot()

    tests: List[TestResult] = []
    total_duration = 0.0

    for test_elem in root.findall(".//Test"):
        # Skip <TestList> entries (no Status attribute)
        if test_elem.get("Status") is None:
            continue
        name_elem = test_elem.find("Name")
        name = name_elem.text if name_elem is not None else "unknown"
        status_raw = test_elem.get("Status", "Not Run")
        status = "Passed" if status_raw == "passed" else "Failed" if status_raw == "failed" else "Not Run"

        duration = 0.0
        results_elem = test_elem.find("Results")
        if results_elem is not None:
            for m in results_elem.findall("NamedMeasurement"):
                if m.get("name") == "Execution Time":
                    val = m.find("Value")
                    if val is not None and val.text:
                        try:
                            duration = float(val.text)
                        except ValueError:
                            pass
        total_duration += duration
        tests.append(TestResult(name=name, status=status, duration=duration))

    total = len(tests)
    passed = sum(1 for t in tests if t.status == "Passed")
    failed = sum(1 for t in tests if t.status == "Failed")

    return TestSuiteResult(
        total=total,
        passed=passed,
        failed=failed,
        not_run=total - passed - failed,
        duration=total_duration,
        tests=tests,
    )


def run_ctest(
    build_dir: str,
    verbose: bool = False,
    filter_regex: Optional[str] = None,
    timeout: Optional[int] = None,
) -> Tuple[subprocess.CompletedProcess, TestSuiteResult]:
    """Run ctest and return (process, parsed results)."""
    cmd = ["ctest", "--output-on-failure", "-T", "Test"]
    if verbose:
        cmd.append("--verbose")
    if filter_regex:
        cmd.extend(["-R", filter_regex])
    if timeout:
        cmd.extend(["--timeout", str(timeout)])

    proc = subprocess.run(cmd, cwd=build_dir, capture_output=True, text=True)

    xml_path = _find_latest_test_xml(build_dir)
    if xml_path:
        result = parse_ctest_xml(xml_path)
    else:
        result = parse_ctest_output(proc.stdout, proc.stderr)

    return proc, result


# ---------------------------------------------------------------------------
# Build Info Persistence
# ---------------------------------------------------------------------------

@dataclasses.dataclass
class BuildInfo:
    configure_time: float = 0.0
    build_time: float = 0.0
    build_type: str = "Release"
    tests_enabled: bool = True
    gui_enabled: bool = True

    def to_dict(self) -> Dict:
        return dataclasses.asdict(self)

    @classmethod
    def from_dict(cls, d: Dict) -> "BuildInfo":
        return cls(**{k: v for k, v in d.items() if k in cls.__dataclass_fields__})

    def age_str(self) -> str:
        if self.build_time <= 0:
            return "never"
        elapsed = time.time() - self.build_time
        if elapsed < 60:
            return f"{elapsed:.0f}s ago"
        if elapsed < 3600:
            return f"{elapsed / 60:.0f} min ago"
        return f"{elapsed / 3600:.1f}h ago"


def save_build_info(build_dir: str, info: BuildInfo) -> None:
    os.makedirs(build_dir, exist_ok=True)
    path = os.path.join(build_dir, BUILD_INFO_FILE)
    with open(path, "w") as f:
        json.dump(info.to_dict(), f, indent=2)


def load_build_info(build_dir: str) -> Optional[BuildInfo]:
    path = os.path.join(build_dir, BUILD_INFO_FILE)
    if not os.path.isfile(path):
        return None
    try:
        with open(path, "r") as f:
            return BuildInfo.from_dict(json.load(f))
    except (json.JSONDecodeError, TypeError):
        return None


# ---------------------------------------------------------------------------
# Profile Management
# ---------------------------------------------------------------------------

def load_profiles(project_root: str) -> Dict:
    path = os.path.join(project_root, PROFILES_FILE)
    if not os.path.isfile(path):
        return {"profiles": {}, "active_profile": None}
    try:
        with open(path, "r") as f:
            return json.load(f)
    except (json.JSONDecodeError, OSError):
        return {"profiles": {}, "active_profile": None}


def save_profiles(project_root: str, profiles: Dict) -> None:
    path = os.path.join(project_root, PROFILES_FILE)
    with open(path, "w") as f:
        json.dump(profiles, f, indent=2)
