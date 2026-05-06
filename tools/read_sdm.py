#!/home/charliechen/cinux/.venv/bin/python3
"""read_sdm.py — Read-only Intel SDM PDF reader for Cinux tutorial agents.

Extracts text, metadata, TOC, and search results from Intel SDM PDFs with
strict output size control to prevent truncation. Only allows reading files
under document/reference/intel/.

Usage:
    read_sdm.py PDF_PATH [--info | --toc | --pages START END |
                           --search KEYWORD | --chapter NAME]
                          [--max-chars N] [--context N]
"""

import argparse
import sys
from pathlib import Path

try:
    import fitz  # PyMuPDF
except ImportError:
    print("ERROR: PyMuPDF (fitz) is not installed.", file=sys.stderr)
    print("", file=sys.stderr)
    print("Run:  python3 -m venv .venv", file=sys.stderr)
    print("      .venv/bin/pip install PyMuPDF", file=sys.stderr)
    print("Then: .venv/bin/python3 tools/read_sdm.py ...", file=sys.stderr)
    sys.exit(1)

ALLOWED_BASE = Path("/home/charliechen/cinux/document/reference/intel").resolve()


def validate_path(pdf_path: str) -> Path:
    resolved = Path(pdf_path).resolve()
    if not str(resolved).startswith(str(ALLOWED_BASE)):
        print(f"ERROR: Path outside allowed directory: {resolved}", file=sys.stderr)
        print(f"       Only files under {ALLOWED_BASE} are permitted.", file=sys.stderr)
        sys.exit(1)
    if not resolved.exists():
        print(f"ERROR: File not found: {resolved}", file=sys.stderr)
        sys.exit(1)
    if resolved.suffix.lower() != ".pdf":
        print(f"ERROR: Not a PDF file: {resolved}", file=sys.stderr)
        sys.exit(1)
    return resolved


def get_info(doc: fitz.Document) -> str:
    meta = doc.metadata
    filename = Path(doc.name).name
    lines = [
        f"# {meta.get('title', filename) or filename}",
        "",
        "| Field | Value |",
        "|-------|-------|",
        f"| File | `{filename}` |",
        f"| Title | {meta.get('title') or 'N/A'} |",
        f"| Author | {meta.get('author') or 'N/A'} |",
        f"| Pages | {doc.page_count} |",
        f"| Format | {meta.get('format') or 'N/A'} |",
        f"| Encryption | {meta.get('encryption') or 'N/A'} |",
        "",
        f"> Source: Intel SDM | document/reference/intel/{filename}",
    ]
    return "\n".join(lines)


def get_toc(doc: fitz.Document, max_chars: int) -> str:
    toc = doc.get_toc()
    if not toc:
        return "No table of contents found. Use --pages to browse content directly."

    lines = ["# Table of Contents", ""]
    char_count = 0
    for level, title, page in toc:
        indent = "  " * (level - 1)
        line = f"{indent}- **{title}** (p.{page})"
        char_count += len(line) + 1
        if char_count > max_chars:
            lines.append(f"\n> TOC truncated at {max_chars} chars. "
                         f"Use --search to find specific topics.")
            break
        lines.append(line)
    else:
        lines.append(f"\n> Total entries: {len(toc)}")
    return "\n".join(lines)


def extract_pages(doc: fitz.Document, start: int, end: int, max_chars: int) -> str:
    start = max(1, start)
    end = min(doc.page_count, end)
    if start > end:
        return f"ERROR: Invalid page range {start}-{end}"

    filename = Path(doc.name).name
    lines = [f"# Pages {start}-{end}", ""]
    char_count = sum(len(l) + 1 for l in lines)

    for page_num in range(start - 1, end):
        page = doc[page_num]
        text = page.get_text("text")
        header = f"\n## Page {page_num + 1}\n"

        if char_count + len(header) + len(text) > max_chars:
            remaining = max_chars - char_count - len(header) - 100
            if remaining > 200:
                lines.append(header)
                lines.append(text[:remaining])
                lines.append(f"\n> Truncated at {max_chars} chars. "
                             f"Remaining pages: {end - page_num - 1}")
            else:
                lines.append(f"\n> Truncated before page {page_num + 1}. "
                             f"Remaining: {end - page_num} pages.")
            break

        lines.append(header)
        lines.append(text)
        char_count += len(header) + len(text) + 1
    else:
        lines.append(f"\n> Source: Intel SDM | {filename} | Pages {start}-{end}")

    return "\n".join(lines)


def search_keyword(doc: fitz.Document, keyword: str, max_chars: int,
                   context_lines: int) -> str:
    kw_lower = keyword.lower()
    filename = Path(doc.name).name
    lines = [f'# Search: "{keyword}"', ""]
    char_count = sum(len(l) + 1 for l in lines)
    match_count = 0

    for page_num in range(doc.page_count):
        text = doc[page_num].get_text("text")
        text_lines = text.split("\n")

        for i, line in enumerate(text_lines):
            if kw_lower in line.lower():
                match_count += 1
                ctx_start = max(0, i - context_lines)
                ctx_end = min(len(text_lines), i + context_lines + 1)
                context = "\n".join(text_lines[ctx_start:ctx_end])
                entry = f"\n### Page {page_num + 1}, Line {i + 1}\n```\n{context}\n```\n"

                if char_count + len(entry) > max_chars:
                    lines.append(f"\n> Truncated at {max_chars} chars. "
                                 f"Matches so far: {match_count - 1}")
                    lines.append(f"> Searched {page_num + 1}/{doc.page_count} pages.")
                    return "\n".join(lines)

                lines.append(entry)
                char_count += len(entry)

    if match_count == 0:
        lines.append(f'No matches for "{keyword}" across {doc.page_count} pages.')
    else:
        lines.append(f"\n> Found {match_count} match(es) in {filename}.")
    return "\n".join(lines)


def extract_chapter(doc: fitz.Document, chapter_name: str, max_chars: int) -> str:
    toc = doc.get_toc()
    if not toc:
        return "No TOC available. Use --search or --pages instead."

    name_lower = chapter_name.lower()
    matches = [(i, lvl, title, pg)
               for i, (lvl, title, pg) in enumerate(toc)
               if name_lower in title.lower()]

    if not matches:
        return (f'Chapter "{chapter_name}" not found in TOC.\n'
                "Use --toc to list chapters, or --search to find content.")

    best_idx, best_level, best_title, best_page = min(
        matches, key=lambda m: (len(m[2]), m[0]))

    end_page = doc.page_count
    for j in range(best_idx + 1, len(toc)):
        if toc[j][0] <= best_level:
            end_page = toc[j][2] - 1
            break

    header = (f"# Chapter: {best_title}\n"
              f"> Pages {best_page}-{end_page} (TOC entry)\n")
    content = extract_pages(doc, best_page, end_page, max_chars - len(header) - 100)
    return header + "\n" + content


def main():
    parser = argparse.ArgumentParser(
        description="Intel SDM PDF Reader (read-only, safe)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("pdf_path", help="Path to SDM PDF file")
    parser.add_argument("--info", action="store_true",
                        help="Show metadata + page count")
    parser.add_argument("--toc", action="store_true",
                        help="Show table of contents")
    parser.add_argument("--pages", nargs=2, type=int, metavar=("START", "END"),
                        help="Extract text from page range (1-based)")
    parser.add_argument("--search", metavar="KEYWORD",
                        help="Search for keyword across pages")
    parser.add_argument("--chapter", metavar="NAME",
                        help="Extract chapter by name from TOC")
    parser.add_argument("--max-chars", type=int, default=15000,
                        help="Maximum output characters (default: 15000)")
    parser.add_argument("--context", type=int, default=3,
                        help="Context lines around search matches (default: 3)")
    args = parser.parse_args()

    has_op = args.info or args.toc or args.pages or args.search or args.chapter
    if not has_op:
        args.info = True

    pdf_path = validate_path(args.pdf_path)
    doc = fitz.open(str(pdf_path))

    try:
        if args.info:
            output = get_info(doc)
        elif args.toc:
            output = get_toc(doc, args.max_chars)
        elif args.pages:
            output = extract_pages(doc, args.pages[0], args.pages[1], args.max_chars)
        elif args.search:
            output = search_keyword(doc, args.search, args.max_chars, args.context)
        elif args.chapter:
            output = extract_chapter(doc, args.chapter, args.max_chars)
        else:
            output = get_info(doc)

        print(output)
    finally:
        doc.close()


if __name__ == "__main__":
    main()
