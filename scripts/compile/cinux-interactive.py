#!/usr/bin/env python3
"""
cinux-interactive.py - Cinux OS interactive developer tool.

A TUI-style menu-driven interface for building, running, and testing
the Cinux OS kernel. Uses the 'rich' library for beautiful terminal output.
"""

from __future__ import annotations

import json
import os
import platform
import shutil
import signal
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cinux_utils import (
    CINUX_VERSION,
    EXIT_SUCCESS,
    EXIT_FAILURE,
    EXIT_CONFIG_ERROR,
    Logger,
    find_project_root,
    detect_platform,
    is_wsl,
    detect_job_count,
    CMakeOptions,
    CMakeRunner,
    check_toolchain,
    format_check_results,
    ToolCheckResult,
    run_ctest,
    TestSuiteResult,
    BuildInfo,
    load_build_info,
    save_build_info,
    load_profiles,
    save_profiles,
    PROFILES_FILE,
    VALID_TARGETS,
    RUN_TARGETS,
    TEST_TARGETS,
    check_venv,
)

try:
    from rich.console import Console
    from rich.panel import Panel
    from rich.table import Table
    from rich.prompt import Prompt, Confirm
    from rich.text import Text
    from rich import box

    HAS_RICH = True
except ImportError:
    HAS_RICH = False

# Fallback simple console if rich not available
if not HAS_RICH:
    class _FakeConsole:
        def print(self, *args, **kwargs):
            print(*args)

    Console = _FakeConsole  # type: ignore[assignment, misc]

    class Panel:
        def __init__(self, content, **kwargs):
            self.content = content

    class Table:
        def __init__(self, **kwargs):
            self.rows = []
        def add_column(self, *a, **kw): pass
        def add_row(self, *a, **kw): self.rows.append(a)

    class Prompt:
        @staticmethod
        def ask(msg, choices=None, default=""):
            hint = f" [{'/'.join(choices)}]" if choices else ""
            hint += f" (default: {default})" if default else ""
            while True:
                val = input(f"{msg}{hint}: ").strip()
                if not val and default:
                    return default
                if not choices or val in choices:
                    return val
                print(f"Invalid choice. Valid: {', '.join(choices)}")

        @staticmethod
        def choose(msg, options, default=""):
            return Prompt.ask(msg, choices=[str(i) for i in range(1, len(options) + 1)], default=default)

    class Confirm:
        @staticmethod
        def ask(msg, default=False):
            val = input(f"{msg} [{'Y/n' if default else 'y/N'}]: ").strip().lower()
            if not val:
                return default
            return val in ("y", "yes")

    class Text:
        def __init__(self, content="", **kwargs):
            self.content = content


console = Console() if HAS_RICH else _FakeConsole()


# ---------------------------------------------------------------------------
# Target definitions for sub-menus
# ---------------------------------------------------------------------------

RUN_MENU_OPTIONS = [
    ("run", "Production kernel"),
    ("run-debug", "With GDB server (:1234)"),
    ("run-kernel-test", "Kernel tests (auto-exit)"),
    ("run-kernel-test-interactive", "Kernel tests (interactive)"),
    ("run-kernel-test-debug", "Kernel tests + GDB"),
    ("run-big-kernel-test", "Big kernel GDT/IDT tests"),
    ("run-stress-test", "1GB stress test"),
]

TEST_MENU_OPTIONS = [
    ("host", "Host tests only (CTest)"),
    ("run-kernel-test", "Kernel test (QEMU, auto-exit)"),
    ("run-big-kernel-test", "Big kernel test (GDT/IDT)"),
    ("run-stress-test", "Stress test (1GB)"),
    ("all", "ALL tests (host + kernel)"),
    ("verbose", "Verbose host tests"),
]


# ---------------------------------------------------------------------------
# Main Application
# ---------------------------------------------------------------------------

class CinuxInteractive:
    def __init__(self):
        try:
            self.project_root = find_project_root()
        except FileNotFoundError as e:
            self._fatal(str(e))

        self.platform = detect_platform()
        self.console = console
        self.logger = Logger()
        self.build_dir = "build"
        self.runner = CMakeRunner(self.project_root, self.build_dir, self.logger)
        self.options = CMakeOptions()
        self._apply_active_profile()
        self._setup_signals()

    # ------------------------------------------------------------------
    # Main loop
    # ------------------------------------------------------------------

    def run(self) -> int:
        check_venv(self.project_root)
        if not self._check_first_time():
            return EXIT_SUCCESS

        while True:
            self._show_menu()
            choice = Prompt.ask("Choose an option", default="").strip().lower()
            if choice in ("q", "quit", "exit"):
                self._print_goodbye()
                break
            self._handle(choice)
        return EXIT_SUCCESS

    # ------------------------------------------------------------------
    # Menu display
    # ------------------------------------------------------------------

    def _show_menu(self) -> None:
        self.console.print()
        info = load_build_info(self.runner.build_dir)

        if HAS_RICH:
            py_ver = f"{sys.version_info.major}.{sys.version_info.minor}"
            plat = "WSL2" if is_wsl() else self.platform.capitalize()
            bt = self.options.build_type
            ts = "ON" if self.options.build_tests else "OFF"
            gu = "ON" if self.options.gui else "OFF"
            age = info.age_str() if info else "never"

            header = Text()
            header.append(f"  Cinux OS v{CINUX_VERSION}", style="bold cyan")
            header.append(f"  |  {plat}  |  Python {py_ver}\n")
            header.append(f"  Build: {bt}  |  Tests: {ts}  |  GUI: {gu}  |  Last: {age}")

            self.console.print(Panel(header, box=box.HEAVY, border_style="cyan"))

            menu = Text()
            items = [
                ("1", "Quick Build & Run"),
                ("2", "Quick Build & Test (all)"),
                ("3", "Configure Build"),
                ("4", "Build Only"),
                ("5", "Run Kernel (QEMU)"),
                ("6", "Run Tests"),
                ("7", "Debug Session (GDB)"),
                ("8", "Check Toolchain"),
                ("9", "Clean Build"),
            ]
            for key, label in items:
                menu.append(f"  [{key}] ", style="bold yellow")
                menu.append(f"{label}\n")
            menu.append("\n")
            menu.append("  [s] ", style="bold yellow")
            menu.append("Build Status    ")
            menu.append("[p] ", style="bold yellow")
            menu.append("Save Profile    ")
            menu.append("[l] ", style="bold yellow")
            menu.append("Load Profile\n")
            menu.append("  [q] ", style="bold yellow")
            menu.append("Quit")
            self.console.print(menu)
        else:
            bt = self.options.build_type
            ts = "ON" if self.options.build_tests else "OFF"
            gu = "ON" if self.options.gui else "OFF"
            print(f"\n=== Cinux OS v{CINUX_VERSION} | Build: {bt} | Tests: {ts} | GUI: {gu} ===\n")
            print("  [1] Quick Build & Run")
            print("  [2] Quick Build & Test (all)")
            print("  [3] Configure Build")
            print("  [4] Build Only")
            print("  [5] Run Kernel (QEMU)")
            print("  [6] Run Tests")
            print("  [7] Debug Session (GDB)")
            print("  [8] Check Toolchain")
            print("  [9] Clean Build")
            print("  [s] Build Status  [p] Save Profile  [l] Load Profile")
            print("  [q] Quit")

    def _handle(self, choice: str) -> None:
        actions = {
            "1": self._action_quick_build_run,
            "2": self._action_quick_build_test,
            "3": self._action_configure,
            "4": self._action_build,
            "5": self._action_run,
            "6": self._action_test,
            "7": self._action_debug,
            "8": self._action_check,
            "9": self._action_clean,
            "s": self._action_status,
            "p": self._action_save_profile,
            "l": self._action_load_profile,
        }
        fn = actions.get(choice)
        if fn:
            try:
                fn()
            except KeyboardInterrupt:
                self.console.print("\n[yitalic]Interrupted.[/italic]")
        else:
            self.console.print("[red]Invalid option.[/red]")

    # ------------------------------------------------------------------
    # Actions
    # ------------------------------------------------------------------

    def _action_quick_build_run(self) -> None:
        """Configure (if needed) + build + run."""
        if not self.runner.is_configured():
            self._do_configure()
        self._do_build()
        self._do_run_target("run")

    def _action_quick_build_test(self) -> None:
        """Configure (if needed) + build + run all tests."""
        if not self.runner.is_configured():
            self._do_configure()
        self._do_build()
        self._do_test_all()

    def _action_configure(self) -> None:
        """Configure build sub-menu."""
        while True:
            bt = self.options.build_type
            ts = "ON" if self.options.build_tests else "OFF"
            gu = "ON" if self.options.gui else "OFF"
            if HAS_RICH:
                self.console.print(f"\n[bold]Build Configuration[/bold]  (current: {bt} | Tests: {ts} | GUI: {gu})")
            else:
                print(f"\nBuild Configuration  (current: {bt} | Tests: {ts} | GUI: {gu})")

            print("  [1] Build type: Release")
            print("  [2] Build type: Debug")
            print("  [3] Toggle tests")
            print("  [4] Toggle GUI")
            print("  [5] Apply and configure")
            print("  [b] Back")

            choice = Prompt.ask("Choose", default="5").strip().lower()
            if choice == "1":
                self.options.build_type = "Release"
            elif choice == "2":
                self.options.build_type = "Debug"
            elif choice == "3":
                self.options.build_tests = not self.options.build_tests
            elif choice == "4":
                self.options.gui = not self.options.gui
            elif choice == "5":
                self._do_configure()
                return
            elif choice in ("b", "back"):
                return

    def _action_build(self) -> None:
        self._do_build()

    def _action_run(self) -> None:
        """Run sub-menu: pick a target."""
        self._print_sub_menu("Run which target?", RUN_MENU_OPTIONS)
        choice = Prompt.ask("Choose", default="1").strip()
        idx = int(choice) - 1 if choice.isdigit() else -1
        if 0 <= idx < len(RUN_MENU_OPTIONS):
            self._do_run_target(RUN_MENU_OPTIONS[idx][0])
        elif choice in ("b", "back"):
            return

    def _action_test(self) -> None:
        """Test sub-menu."""
        self._print_sub_menu("Run which tests?", TEST_MENU_OPTIONS)
        choice = Prompt.ask("Choose", default="5").strip()
        idx = int(choice) - 1 if choice.isdigit() else -1
        if 0 <= idx < len(TEST_MENU_OPTIONS):
            key = TEST_MENU_OPTIONS[idx][0]
            if key == "host":
                self._do_test_host()
            elif key == "all":
                self._do_test_all()
            elif key == "verbose":
                self._do_test_host(verbose=True)
            else:
                self._do_run_target(key)
        elif choice in ("b", "back"):
            return

    def _action_debug(self) -> None:
        """Debug session: switch to Debug, build, launch QEMU with GDB stub."""
        if HAS_RICH:
            self.console.print("\n[bold yellow]Starting debug session...[/bold yellow]")
        else:
            print("\nStarting debug session...")

        saved = self.options.build_type
        self.options.build_type = "Debug"
        self._do_configure()
        self._do_build()

        self.console.print()
        self.console.print("[cyan]QEMU starting with GDB stub on :1234[/cyan]")
        self.console.print("[dim]In another terminal: gdb -ex 'target remote :1234' build/kernel.elf[/dim]")
        self.console.print("[dim]Press Ctrl+C to stop QEMU when done.[/dim]\n")

        self.runner.build(target="run-debug", jobs=detect_job_count())

        self.options.build_type = saved
        self._prompt_continue()

    def _action_check(self) -> None:
        results = check_toolchain()
        if HAS_RICH:
            table = Table(title="Toolchain Check", box=box.SIMPLE)
            table.add_column("Status", width=8)
            table.add_column("Tool")
            table.add_column("Version")
            table.add_column("Path")
            for r in results:
                status = "[green]OK[/green]" if r.found else "[red]MISSING[/red]"
                ver = r.version or "-"
                path = r.path or r.install_hint
                table.add_row(status, r.name, ver, path)
            self.console.print(table)
        else:
            print(format_check_results(results))

        all_ok = all(r.found for r in results)
        if all_ok:
            self.console.print("[green]All tools present![/green]")
        else:
            self.console.print("[red]Some tools missing. See install hints above.[/red]")
        self._prompt_continue()

    def _action_clean(self) -> None:
        if Confirm.ask(f"Remove {self.runner.build_dir}?", default=False):
            self.runner.clean()
            self.console.print("[green]Build directory cleaned.[/green]")

    def _action_status(self) -> None:
        info = load_build_info(self.runner.build_dir)
        configured = self.runner.is_configured()

        if HAS_RICH:
            table = Table(title="Build Status", box=box.SIMPLE)
            table.add_column("Property")
            table.add_column("Value")
            table.add_row("Configured", "[green]Yes[/green]" if configured else "[red]No[/red]")
            if info:
                table.add_row("Build type", info.build_type)
                table.add_row("Tests", "ON" if info.tests_enabled else "OFF")
                table.add_row("GUI", "ON" if info.gui_enabled else "OFF")
                table.add_row("Last build", info.age_str())
            else:
                table.add_row("Info", "No build info found")
            self.console.print(table)
        else:
            print(f"  Configured: {'Yes' if configured else 'No'}")
            if info:
                print(f"  Build type: {info.build_type}")
                print(f"  Tests: {'ON' if info.tests_enabled else 'OFF'}")
                print(f"  GUI: {'ON' if info.gui_enabled else 'OFF'}")
                print(f"  Last build: {info.age_str()}")
        self._prompt_continue()

    def _action_save_profile(self) -> None:
        name = Prompt.ask("[bold]Profile name[/bold]", default=self.options.build_type.lower())
        profiles = load_profiles(self.project_root)
        profiles["profiles"][name] = {
            "build_type": self.options.build_type,
            "build_tests": self.options.build_tests,
            "gui": self.options.gui,
            "build_dir": self.build_dir,
        }
        profiles["active_profile"] = name
        save_profiles(self.project_root, profiles)
        self.console.print(f"[green]Profile '{name}' saved.[/green]")

    def _action_load_profile(self) -> None:
        profiles = load_profiles(self.project_root)
        names = list(profiles.get("profiles", {}).keys())
        if not names:
            self.console.print("[yellow]No saved profiles.[/yellow]")
            return
        print("  Available profiles:")
        for i, n in enumerate(names, 1):
            active = " (active)" if n == profiles.get("active_profile") else ""
            print(f"    [{i}] {n}{active}")
        print("    [b] Back")
        choice = Prompt.ask("Choose", default="").strip()
        if choice in ("b", "back"):
            return
        idx = int(choice) - 1 if choice.isdigit() else -1
        if 0 <= idx < len(names):
            name = names[idx]
            p = profiles["profiles"][name]
            self.options.build_type = p.get("build_type", "Release")
            self.options.build_tests = p.get("build_tests", True)
            self.options.gui = p.get("gui", True)
            self.build_dir = p.get("build_dir", "build")
            self.runner = CMakeRunner(self.project_root, self.build_dir, self.logger)
            profiles["active_profile"] = name
            save_profiles(self.project_root, profiles)
            self.console.print(f"[green]Loaded profile '{name}'.[/green]")

    # ------------------------------------------------------------------
    # Core operations
    # ------------------------------------------------------------------

    def _do_configure(self) -> bool:
        self.logger.step(1, 1, f"Configuring CMake ({self.options.build_type} mode)")
        result = self.runner.configure(self.options)
        if result.returncode != 0:
            self.console.print("[red]CMake configure failed.[/red]")
            return False
        self.console.print("[green]CMake configured successfully.[/green]")
        return True

    def _do_build(self, target: str | None = None) -> bool:
        jobs = detect_job_count()
        label = f"target '{target}'" if target else "project"
        self.logger.step(1, 1, f"Building {label} ({jobs} jobs)")
        result = self.runner.build(target=target, jobs=jobs)
        if result.returncode != 0:
            self.console.print(f"[red]Build failed (exit code {result.returncode}).[/red]")
            return False
        info = load_build_info(self.runner.build_dir)
        dur = info.build_time if info else 0.0
        self.console.print(f"[green]Build completed in {dur:.1f}s[/green]")
        return True

    def _do_run_target(self, target: str) -> None:
        self.logger.step(1, 1, f"Running '{target}'")
        result = self.runner.build(target=target, jobs=detect_job_count())
        if result.returncode != 0:
            self.console.print(f"[red]Target '{target}' exited with code {result.returncode}[/red]")
        else:
            self.console.print(f"[green]Target '{target}' completed.[/green]")

    def _do_test_host(self, verbose: bool = False) -> None:
        self.logger.step(1, 1, "Running host tests")
        build_result = self.runner.build(target="test_host", jobs=detect_job_count())
        if build_result.returncode != 0:
            self.console.print("[red]Failed to build test_host.[/red]")
            return
        proc, result = run_ctest(self.runner.build_dir, verbose=verbose)
        self._show_test_results("Host Tests", result)

    def _do_test_all(self) -> None:
        self._do_test_host()
        for target in TEST_TARGETS:
            self.logger.step(1, 1, f"Running kernel test: {target}")
            proc = self.runner.build(target=target, jobs=detect_job_count())
            passed = proc.returncode == 0
            tag = "PASSED" if passed else "FAILED"
            color = "green" if passed else "red"
            self.console.print(f"  [{color}]{target}: {tag}[/{color}]")

    def _show_test_results(self, label: str, result: TestSuiteResult) -> None:
        if HAS_RICH:
            table = Table(title=label, box=box.SIMPLE)
            table.add_column("Status", width=8)
            table.add_column("Test")
            table.add_column("Time", justify="right")
            for t in result.tests:
                s = "[green]PASS[/green]" if t.status == "Passed" else "[red]FAIL[/red]"
                table.add_row(s, t.name, f"{t.duration:.3f}s")

            summary = (
                f"[green]{result.passed}/{result.total} passed[/green]"
                if result.failed == 0
                else f"[red]{result.failed}/{result.total} FAILED[/red]"
            )
            self.console.print(table)
            self.console.print(f"  {summary} ({result.duration:.2f}s)")
        else:
            for t in result.tests:
                tag = "PASS" if t.status == "Passed" else "FAIL"
                print(f"  [{tag}] {t.name} ({t.duration:.3f}s)")
            if result.failed == 0:
                print(f"  {result.passed}/{result.total} passed ({result.duration:.2f}s)")
            else:
                print(f"  {result.failed}/{result.total} FAILED ({result.duration:.2f}s)")

    # ------------------------------------------------------------------
    # First-time setup
    # ------------------------------------------------------------------

    def _check_first_time(self) -> bool:
        """Return True to continue, False to exit."""
        build_path = os.path.join(self.project_root, self.runner.build_dir)
        if os.path.isdir(build_path):
            return True

        if HAS_RICH:
            self.console.print(Panel(
                "[bold]Welcome to Cinux![/bold]\n\n"
                "It looks like this is your first build.\n"
                "Let me check your toolchain first...",
                title="First-Time Setup",
                border_style="green",
            ))
        else:
            print("\n=== Welcome to Cinux! ===")
            print("First build detected. Checking toolchain...\n")

        results = check_toolchain()
        if HAS_RICH:
            for r in results:
                tag = "[green]OK[/green]" if r.found else "[red]MISSING[/red]"
                self.console.print(f"  {tag} {r.name}{'  (' + r.version + ')' if r.version and r.found else ''}")
        else:
            print(format_check_results(results))

        all_ok = all(r.found for r in results)
        if not all_ok:
            self.console.print("\n[red]Some tools are missing. Install them first, then re-run.[/red]")
            return False

        self.console.print("\n[green]All tools present![/green]")
        if Confirm.ask("\nConfigure with defaults (Release, Tests ON, GUI ON)?", default=True):
            self._do_configure()
            self._do_build()
            self.console.print("\n[bold green]Setup complete![/bold green]")
            self._prompt_continue()
        return True

    # ------------------------------------------------------------------
    # Profile helpers
    # ------------------------------------------------------------------

    def _apply_active_profile(self) -> None:
        profiles = load_profiles(self.project_root)
        active = profiles.get("active_profile")
        if active and active in profiles.get("profiles", {}):
            p = profiles["profiles"][active]
            self.options.build_type = p.get("build_type", "Release")
            self.options.build_tests = p.get("build_tests", True)
            self.options.gui = p.get("gui", True)
            self.build_dir = p.get("build_dir", "build")
            self.runner = CMakeRunner(self.project_root, self.build_dir, self.logger)

    # ------------------------------------------------------------------
    # UI helpers
    # ------------------------------------------------------------------

    def _print_sub_menu(self, title: str, options: list) -> None:
        print(f"\n  {title}")
        for i, (key, desc) in enumerate(options, 1):
            print(f"    [{i}] {desc}")
        print("    [b] Back")

    def _prompt_continue(self) -> None:
        Prompt.ask("\n  Press Enter to continue", default="")

    def _print_goodbye(self) -> None:
        self.console.print("\n[dim]Goodbye![/dim]")

    def _fatal(self, msg: str) -> None:
        self.console.print(f"[red]ERROR:[/red] {msg}", file=sys.stderr)
        sys.exit(EXIT_CONFIG_ERROR)

    def _setup_signals(self) -> None:
        def handler(sig, frame):
            raise KeyboardInterrupt()
        signal.signal(signal.SIGINT, handler)


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main() -> int:
    app = CinuxInteractive()
    return app.run()


if __name__ == "__main__":
    sys.exit(main())
