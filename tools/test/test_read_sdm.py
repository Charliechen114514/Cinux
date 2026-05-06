import subprocess
from pathlib import Path

from conftest import run_tool, VOL3A, VOL1


def test_info(vol3a):
    r = run_tool(vol3a, "--info")
    assert r.returncode == 0
    assert "Pages" in r.stdout
    assert "Field" in r.stdout


def test_toc(vol3a):
    r = run_tool(vol3a, "--toc")
    assert r.returncode == 0
    assert "Table of Contents" in r.stdout or "No table of contents" in r.stdout


def test_pages_range(vol3a):
    r = run_tool(vol3a, "--pages", "1", "3")
    assert r.returncode == 0
    assert "Page 1" in r.stdout
    assert "Page 3" in r.stdout


def test_pages_invalid(vol3a):
    r = run_tool(vol3a, "--pages", "9999", "10000")
    assert r.returncode == 0
    assert "Invalid page range" in r.stdout or len(r.stdout.strip()) > 0


def test_search_found(vol3a):
    r = run_tool(vol3a, "--search", "interrupt")
    assert r.returncode == 0
    assert "match" in r.stdout.lower() or "Match" in r.stdout


def test_search_not_found(vol3a):
    r = run_tool(vol3a, "--search", "xyzzy_nonexistent_keyword_42")
    assert r.returncode == 0
    assert "No matches" in r.stdout


def test_chapter_found(vol3a):
    r = run_tool(vol3a, "--chapter", "interrupt")
    assert r.returncode == 0
    assert "Chapter:" in r.stdout or "chapter" in r.stdout.lower()


def test_chapter_not_found(vol3a):
    r = run_tool(vol3a, "--chapter", "xyzzy_nonexistent")
    assert r.returncode == 0
    assert "not found" in r.stdout.lower()


def test_path_traversal():
    r = run_tool("/etc/passwd", "--info")
    assert r.returncode == 1
    assert "outside allowed" in r.stderr.lower() or "not found" in r.stderr.lower()


def test_non_pdf(tmp_path):
    txt = tmp_path / "test.txt"
    txt.write_text("hello")
    r = run_tool(str(txt), "--info")
    assert r.returncode == 1
    assert "Not a PDF" in r.stderr or "outside allowed" in r.stderr


def test_max_chars(vol3a):
    r = run_tool(vol3a, "--pages", "1", "100", "--max-chars", "200")
    assert r.returncode == 0
    assert len(r.stdout) <= 800  # generous buffer for headers + notice


def test_nonexistent_file():
    r = run_tool("/home/charliechen/cinux/document/reference/intel/nope.pdf", "--info")
    assert r.returncode == 1
    assert "not found" in r.stderr.lower() or "File not found" in r.stderr
