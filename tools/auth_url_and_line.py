#!/home/charliechen/cinux/.venv/bin/python3
"""auth_url_and_line.py — Validate URLs and line counts in tutorial markdown files.

Uses Playwright (headless Chromium) to bypass Cloudflare and verify each URL by
checking the actual rendered page content.  Also enforces the per-file line-count
policy.

Usage:
    auth_url_and_line.py FILE [FILE ...] [--max-lines N] [--timeout SECS] [--json]

Exit codes:
    0  all URLs alive, all files within line limit
    1  one or more URLs dead or line-count exceeded
    2  invocation / dependency error
"""

import argparse
import json
import re
import sys
from pathlib import Path
from typing import NamedTuple
from urllib.parse import urlparse

try:
    from playwright.sync_api import sync_playwright
except ImportError:
    print("ERROR: playwright is not installed.", file=sys.stderr)
    print("       Use .venv/bin/python3 to run this script (playwright is in the venv):", file=sys.stderr)
    print("         .venv/bin/python3 tools/auth_url_and_line.py <files...>", file=sys.stderr)
    sys.exit(2)

# ---------------------------------------------------------------------------
# Data structures
# ---------------------------------------------------------------------------

class URLResult(NamedTuple):
    url: str
    status: str        # "ok", "dead", "error", "skipped"
    code: int          # HTTP status code or 0
    detail: str        # human-readable detail


class FileReport(NamedTuple):
    path: str
    lines: int
    urls: list[URLResult]


# ---------------------------------------------------------------------------
# URL extraction
# ---------------------------------------------------------------------------

# Inline links: [text](url) — supports one level of nested parens in URL
_RE_INLINE = re.compile(r'\[(?:[^\]]*)\]\(([^)\s(]+(?:\([^)\s]*\))*)\)')
# Reference-style link definitions: [label]: url
_RE_REF_DEF = re.compile(r'^\s*\[[^\]]+\]:\s+<?(\S+?)>?(?:\s|$)', re.MULTILINE)
# Bare URLs (http/https only, not already inside markdown syntax)
_RE_BARE = re.compile(r'(?<!\()(https?://[^\s\)>]+(?:\([^\s\)>]*\))*)')


def extract_urls(text: str) -> list[str]:
    raw: list[str] = []
    raw.extend(_RE_INLINE.findall(text))
    raw.extend(_RE_REF_DEF.findall(text))
    for m in _RE_BARE.findall(text):
        if m not in raw:
            raw.append(m)
    # deduplicate, filter to http/https only, preserve order
    seen: set[str] = set()
    urls: list[str] = []
    for u in raw:
        u = u.rstrip(".,;:>")
        if u not in seen and u.startswith(("http://", "https://")):
            seen.add(u)
            urls.append(u)
    return urls


# ---------------------------------------------------------------------------
# URL validation via Playwright
# ---------------------------------------------------------------------------

_CHECKABLE_SCHEMES = {"http", "https"}

# Phrases that indicate a wiki page does not exist
_DEAD_PAGE_MARKERS = [
    "there is currently no text in this page",
    "this page does not exist",
    "page not found",
    "does not exist",
    "no text in this page",
    "the requested page",
    "404",
]

# System Chromium path (Arch Linux)
_CHROMIUM_PATH = "/usr/sbin/chromium"


def _check_one(page, url: str, timeout: int) -> URLResult:
    """Check a single URL using a shared Playwright page."""
    parsed = urlparse(url)
    if parsed.scheme not in _CHECKABLE_SCHEMES:
        return URLResult(url, "skipped", 0, f"scheme '{parsed.scheme}' not checked")

    try:
        resp = page.goto(url, timeout=timeout * 1000, wait_until="commit")
        code = resp.status if resp else 0

        # 403 = CF/bot protection → cannot verify automatically, mark uncertain.
        # No point waiting for the JS challenge to resolve — it never does in
        # automated contexts.  The user will manually verify these afterwards.
        if code == 403:
            return URLResult(url, "uncertain", code, "HTTP 403 — needs manual check")

        title = page.title().lower()
        body = page.inner_text("body").lower()[:2000]

        # MediaWiki / wiki "page does not exist" detection
        if any(marker in body for marker in _DEAD_PAGE_MARKERS):
            return URLResult(url, "dead", code, f"page says: not found (title: {page.title()[:60]})")

        # HTTP-level errors with no meaningful content
        if code >= 400:
            return URLResult(url, "dead", code, f"HTTP {code}")

        return URLResult(url, "ok", code, f"title: {page.title()[:60]}")

    except Exception as exc:
        msg = str(exc)
        if "Timeout" in msg and len(msg) > 120:
            msg = msg[:120] + "..."
        return URLResult(url, "error", 0, msg)


def check_urls(urls: list[str], timeout: int, *, progress: bool = False) -> list[URLResult]:
    if not urls:
        return []

    results: list[URLResult] = []
    with sync_playwright() as p:
        browser = p.chromium.launch(
            executable_path=_CHROMIUM_PATH,
            headless=True,
            args=[
                "--no-sandbox",
                "--disable-gpu",
            ],
        )
        ctx = browser.new_context(
            viewport={"width": 1920, "height": 1080},
        )
        page = ctx.new_page()

        for i, url in enumerate(urls, 1):
            if progress:
                tag = f"[{i}/{len(urls)}]"
                print(f"  {tag} checking {url} ...", flush=True)
            r = _check_one(page, url, timeout)
            if progress:
                c = _color(r.status)
                label = _status_label(r)
                print(f"  {tag} {c}{label:<22}{_COLOR_RESET} {r.detail}", flush=True)
                if r.status not in ("ok", "skipped"):
                    print(f"  {'':>22} {r.url}", flush=True)
            results.append(r)

        browser.close()

    return results


# ---------------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------------

_COLOR_RED = "\033[31m"
_COLOR_GREEN = "\033[32m"
_COLOR_YELLOW = "\033[33m"
_COLOR_BOLD = "\033[1m"
_COLOR_RESET = "\033[0m"


def _color(status: str) -> str:
    if status == "ok":
        return _COLOR_GREEN
    if status in ("skipped", "uncertain"):
        return _COLOR_YELLOW
    return _COLOR_RED


def _status_label(r: URLResult) -> str:
    tag = r.status.upper()
    if r.code:
        tag += f" ({r.code})"
    return tag


def print_report(report: FileReport, max_lines: int) -> None:
    p = report.path
    lines = report.lines
    lc_color = _COLOR_GREEN if lines <= max_lines else _COLOR_RED
    print(f"\n{_COLOR_BOLD}{'=' * 60}{_COLOR_RESET}")
    print(f"  {p}")
    print(f"  Lines: {lc_color}{lines}{_COLOR_RESET} / {max_lines} limit")
    print(f"  URLs:  {len(report.urls)}")
    print(f"{_COLOR_BOLD}{'=' * 60}{_COLOR_RESET}")

    for r in report.urls:
        c = _color(r.status)
        label = _status_label(r)
        print(f"  {c}{label:<22}{_COLOR_RESET} {r.detail}")
        if r.status not in ("ok", "skipped"):
            print(f"  {'':>22} {r.url}")
    print()


def print_json_report(reports: list[FileReport], max_lines: int) -> None:
    out = []
    for r in reports:
        out.append({
            "file": r.path,
            "lines": r.lines,
            "line_limit": max_lines,
            "line_ok": r.lines <= max_lines,
            "urls": [
                {"url": u.url, "status": u.status, "code": u.code, "detail": u.detail}
                for u in r.urls
            ],
            "url_ok": all(u.status in ("ok", "skipped") for u in r.urls),
        })
    print(json.dumps(out, indent=2, ensure_ascii=False))


def print_summary(reports: list[FileReport], max_lines: int) -> None:
    total_urls = sum(len(r.urls) for r in reports)
    bad_urls = [u for r in reports for u in r.urls if u.status == "dead" or u.status == "error"]
    uncertain_urls = [u for r in reports for u in r.urls if u.status == "uncertain"]
    over_limit = [r for r in reports if r.lines > max_lines]

    print(f"{_COLOR_BOLD}Summary{_COLOR_RESET}")
    print(f"  Files checked:    {len(reports)}")
    print(f"  URLs checked:     {total_urls}")
    print(f"  Dead/error:       {_COLOR_RED if bad_urls else _COLOR_GREEN}{len(bad_urls)}{_COLOR_RESET}")
    print(f"  Uncertain (403):  {_COLOR_YELLOW if uncertain_urls else _COLOR_GREEN}{len(uncertain_urls)}{_COLOR_RESET}")
    print(f"  Over line limit:  {_COLOR_RED if over_limit else _COLOR_GREEN}{len(over_limit)}{_COLOR_RESET}")

    # Deduplicated list of all URLs needing manual review
    needs_review = []
    seen_urls: set[str] = set()
    for u in bad_urls + uncertain_urls:
        if u.url not in seen_urls:
            seen_urls.add(u.url)
            needs_review.append(u)

    if needs_review:
        print(f"\n{_COLOR_BOLD}URLs needing manual review ({len(needs_review)} unique):{_COLOR_RESET}")
        for u in needs_review:
            c = _color(u.status)
            label = _status_label(u)
            files = [r.path for r in reports for uu in r.urls if uu.url == u.url]
            print(f"  {c}{label:<22}{_COLOR_RESET} {u.url}")
            print(f"    {u.detail}")
            print(f"    Referenced in: {', '.join(files)}")

    if over_limit:
        print(f"\n{_COLOR_RED}Over line limit:{_COLOR_RESET}")
        for r in over_limit:
            print(f"  {r.path}: {r.lines} lines (limit {max_lines})")

    print()


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(
        description="Validate URLs and line counts in markdown tutorial files.",
    )
    parser.add_argument("files", nargs="+", metavar="FILE",
                        help="Markdown file(s) to check")
    parser.add_argument("--max-lines", type=int, default=500,
                        help="Maximum allowed lines per file (default: 500)")
    parser.add_argument("--timeout", type=int, default=15,
                        help="Page load timeout in seconds (default: 15)")
    parser.add_argument("--json", action="store_true",
                        help="Output results as JSON")
    args = parser.parse_args()

    # Validate input files
    paths: list[Path] = []
    for f in args.files:
        p = Path(f)
        if not p.exists():
            print(f"ERROR: file not found: {f}", file=sys.stderr)
            return 2
        if not p.is_file():
            print(f"ERROR: not a regular file: {f}", file=sys.stderr)
            return 2
        paths.append(p)

    reports: list[FileReport] = []
    for idx, p in enumerate(paths, 1):
        if not args.json:
            print(f"{_COLOR_BOLD}Checking file [{idx}/{len(paths)}]:{ _COLOR_RESET} {p}", flush=True)
        text = p.read_text(encoding="utf-8")
        line_count = text.count("\n") + (1 if text and not text.endswith("\n") else 0)
        urls = extract_urls(text)
        if not args.json and urls:
            print(f"  Found {len(urls)} URL(s), checking ...", flush=True)
        results = check_urls(urls, args.timeout, progress=not args.json and bool(urls))
        report = FileReport(str(p), line_count, results)
        reports.append(report)

        # Print per-file result immediately (non-JSON mode)
        if not args.json:
            lc_color = _COLOR_GREEN if line_count <= args.max_lines else _COLOR_RED
            print(f"  Lines: {lc_color}{line_count}{_COLOR_RESET} / {args.max_lines} limit", flush=True)
            print(flush=True)

    # Final output
    if args.json:
        print_json_report(reports, args.max_lines)
    elif len(reports) > 1:
        print_summary(reports, args.max_lines)

    # Determine exit code
    has_bad_url = any(
        u.status in ("dead", "error")
        for r in reports
        for u in r.urls
    )
    has_over_limit = any(r.lines > args.max_lines for r in reports)

    return 1 if (has_bad_url or has_over_limit) else 0


if __name__ == "__main__":
    sys.exit(main())
