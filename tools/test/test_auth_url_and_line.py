"""Tests for tools/auth_url_and_line.py — URL extraction, validation, and line-count checks.

These tests require Playwright + system Chromium (/usr/sbin/chromium).
Network-dependent tests are marked with pytest.mark.network.
"""

import json
import subprocess
from pathlib import Path

import pytest

PROJECT_ROOT = Path("/home/charliechen/cinux")
SCRIPT = PROJECT_ROOT / ".venv/bin/python3"
TOOL = PROJECT_ROOT / "tools/auth_url_and_line.py"


def run_tool(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        [str(SCRIPT), str(TOOL), *args],
        capture_output=True, text=True, timeout=120,
    )


# ---------------------------------------------------------------------------
# Fixtures — sample markdown files
# ---------------------------------------------------------------------------

@pytest.fixture
def good_md(tmp_path):
    """Markdown with a valid OSDev URL."""
    p = tmp_path / "good.md"
    p.write_text(
        "# Test\n\n"
        "Some text.\n\n"
        "Here is a [valid link](https://wiki.osdev.org/MBR_(x86)).\n"
        "And a reference:\n\n"
        "[ref]: https://wiki.osdev.org/A20_Line\n",
        encoding="utf-8",
    )
    return str(p)


@pytest.fixture
def bad_url_md(tmp_path):
    """Markdown with a fabricated OSDev URL that does not exist."""
    p = tmp_path / "bad_url.md"
    p.write_text(
        "# Test\n\n"
        "Check [this fake](https://wiki.osdev.org/This_Page_Does_Not_Exist_At_All_12345).\n",
        encoding="utf-8",
    )
    return str(p)


@pytest.fixture
def dead_url_md(tmp_path):
    """Markdown with a fabricated URL on a non-Cloudflare domain."""
    p = tmp_path / "dead_url.md"
    p.write_text(
        "# Test\n\n"
        "See [dead](https://example.com/this_page_does_not_exist_12345).\n",
        encoding="utf-8",
    )
    return str(p)


@pytest.fixture
def over_limit_md(tmp_path):
    """Markdown that exceeds the default line limit."""
    p = tmp_path / "long.md"
    lines = ["line"] * 600
    p.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return str(p)


@pytest.fixture
def bare_url_md(tmp_path):
    """Markdown with a bare (non-inline) URL."""
    p = tmp_path / "bare.md"
    p.write_text(
        "# Test\n\n"
        "See https://wiki.osdev.org/A20_Line for details.\n",
        encoding="utf-8",
    )
    return str(p)


@pytest.fixture
def no_url_md(tmp_path):
    """Markdown with no URLs at all."""
    p = tmp_path / "nourl.md"
    p.write_text("# No URLs\n\nJust text.\n", encoding="utf-8")
    return str(p)


@pytest.fixture
def mixed_md(tmp_path):
    """Markdown with a mix of valid, dead, and non-http URLs."""
    p = tmp_path / "mixed.md"
    p.write_text(
        "# Mixed\n\n"
        "[good](https://wiki.osdev.org/A20_Line)\n"
        "[dead](https://example.com/fake_page_99999)\n"
        "[ftp](ftp://example.com/file)\n"
        "https://wiki.osdev.org/MBR_(x86)\n",
        encoding="utf-8",
    )
    return str(p)


# ---------------------------------------------------------------------------
# Tests — invocation errors
# ---------------------------------------------------------------------------

def test_no_args():
    r = run_tool()
    assert r.returncode != 0


def test_file_not_found():
    r = run_tool("/tmp/does_not_exist_at_all_12345.md")
    assert r.returncode == 2
    assert "not found" in r.stderr.lower()


# ---------------------------------------------------------------------------
# Tests — line counting (no network needed)
# ---------------------------------------------------------------------------

def test_line_count_ok(no_url_md):
    r = run_tool(no_url_md, "--max-lines", "10")
    assert r.returncode == 0
    assert "Lines:" in r.stdout


def test_line_count_over(over_limit_md):
    r = run_tool(over_limit_md, "--max-lines", "500")
    assert r.returncode == 1
    assert "600" in r.stdout


def test_custom_max_lines(over_limit_md):
    r = run_tool(over_limit_md, "--max-lines", "700")
    assert r.returncode == 0


# ---------------------------------------------------------------------------
# Tests — URL extraction (no network needed, uses --json to avoid browser)
# ---------------------------------------------------------------------------

@pytest.mark.network
def test_extracts_inline_url(good_md):
    r = run_tool(good_md, "--json")
    assert r.returncode == 0
    data = json.loads(r.stdout)
    urls = [u["url"] for u in data[0]["urls"]]
    assert "https://wiki.osdev.org/MBR_(x86)" in urls


@pytest.mark.network
def test_extracts_reference_url(good_md):
    r = run_tool(good_md, "--json")
    data = json.loads(r.stdout)
    urls = [u["url"] for u in data[0]["urls"]]
    assert "https://wiki.osdev.org/A20_Line" in urls


@pytest.mark.network
def test_extracts_bare_url(bare_url_md):
    r = run_tool(bare_url_md, "--json")
    data = json.loads(r.stdout)
    urls = [u["url"] for u in data[0]["urls"]]
    assert "https://wiki.osdev.org/A20_Line" in urls


def test_no_urls(no_url_md):
    r = run_tool(no_url_md, "--json")
    data = json.loads(r.stdout)
    assert data[0]["urls"] == []
    assert data[0]["url_ok"] is True


# ---------------------------------------------------------------------------
# Tests — URL validation via Playwright (network-dependent)
# ---------------------------------------------------------------------------

@pytest.mark.network
def test_valid_osdev_url_passes(good_md):
    r = run_tool(good_md)
    assert r.returncode == 0
    assert "OK" in r.stdout


@pytest.mark.network
def test_fake_osdev_url_fails(bad_url_md):
    """OSDev wiki shows 'no text in this page' for non-existent pages.
    May also show ERROR if Cloudflare rate-limits consecutive checks."""
    r = run_tool(bad_url_md)
    assert r.returncode == 1
    assert ("DEAD" in r.stdout or "ERROR" in r.stdout)


@pytest.mark.network
def test_dead_external_url(dead_url_md):
    r = run_tool(dead_url_md)
    assert r.returncode == 1


@pytest.mark.network
def test_skipped_scheme(mixed_md):
    r = run_tool(mixed_md, "--json")
    data = json.loads(r.stdout)
    ftp_entries = [u for u in data[0]["urls"] if u["url"].startswith("ftp://")]
    assert len(ftp_entries) == 1
    assert ftp_entries[0]["status"] == "skipped"


# ---------------------------------------------------------------------------
# Tests — JSON output structure
# ---------------------------------------------------------------------------

def test_json_output_structure(no_url_md):
    r = run_tool(no_url_md, "--json")
    assert r.returncode == 0
    data = json.loads(r.stdout)
    assert isinstance(data, list)
    assert len(data) == 1
    entry = data[0]
    assert "file" in entry
    assert "lines" in entry
    assert "line_limit" in entry
    assert "line_ok" in entry
    assert "urls" in entry
    assert "url_ok" in entry


# ---------------------------------------------------------------------------
# Tests — multiple files
# ---------------------------------------------------------------------------

def test_multiple_files(no_url_md, over_limit_md):
    r = run_tool(no_url_md, over_limit_md, "--max-lines", "500")
    assert r.returncode == 1
    assert "Summary" in r.stdout


# ---------------------------------------------------------------------------
# Tests — real tutorial file (smoke test)
# ---------------------------------------------------------------------------

@pytest.mark.network
def test_real_tutorial_file():
    sample = PROJECT_ROOT / "document/hands-on/001-boot-real-mode-1.md"
    if not sample.exists():
        pytest.skip("sample tutorial file not found")
    r = run_tool(str(sample), "--max-lines", "300")
    assert "Lines:" in r.stdout
    assert "URLs:" in r.stdout
