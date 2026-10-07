#!/usr/bin/env python3
"""Generate nexus-page BBCode from README.md user-facing content.

Reads the block between <!-- nexus:start --> and <!-- nexus:end --> in README.md,
converts it to BBCode, substitutes it into the <!-- generated:start/end --> region
of docs/nexus-page.md in memory, and prints the result to stdout.

Usage:
    python3 scripts/generate-nexus-page.py
    python3 scripts/generate-nexus-page.py --copy
"""

import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

README = Path(__file__).parent.parent / "README.md"
NEXUS_TEMPLATE = Path(__file__).parent.parent / "docs" / "nexus-page.md"

NEXUS_START = "<!-- nexus:start -->"
NEXUS_END = "<!-- nexus:end -->"
GEN_START = "<!-- generated:start -->"
GEN_END = "<!-- generated:end -->"
BBCODE_FENCE_OPEN = "```bbcode"
BBCODE_FENCE_CLOSE = "```"
CLIPBOARD_COMMANDS = [
    ["wl-copy"],
    ["xclip", "-selection", "clipboard"],
    ["xsel", "--clipboard", "--input"],
    ["pbcopy"],
]
CLIPBOARD_TIMEOUT_SECONDS = 10


def extract_block(text: str, start_marker: str, end_marker: str) -> str:
    start = text.find(start_marker)
    if start == -1:
        sys.exit(f"error: marker not found: {start_marker!r}")
    end = text.find(end_marker, start + len(start_marker))
    if end == -1:
        sys.exit(f"error: marker not found: {end_marker!r}")
    return text[start + len(start_marker):end].strip()


def md_to_bbcode(md: str) -> str:
    lines = md.splitlines()
    output: list[str] = []
    i = 0
    first_heading = True

    while i < len(lines):
        line = lines[i]

        # ## Heading
        if line.startswith("## "):
            heading = line[3:].strip()
            if not first_heading:
                output.append("")
                output.append("[line]")
                output.append("")
            first_heading = False
            output.append(f"[size=4][b][color=#B8953E]{heading}[/color][/b][/size]")
            i += 1
            continue

        # Fenced code block (```lang or plain ```)
        if line.startswith("```"):
            fence_start = i
            i += 1
            code_lines: list[str] = []
            while i < len(lines) and not lines[i].startswith("```"):
                code_lines.append(lines[i])
                i += 1
            if i >= len(lines):
                sys.exit(f"error: unterminated fenced code block opened at line {fence_start + 1}")
            i += 1  # skip closing ```
            output.append("[code]")
            output.extend(code_lines)
            output.append("[/code]")
            continue

        # Markdown pipe table — render data rows as a list (header and separator skipped)
        if line.startswith("|"):
            table_lines: list[str] = []
            while i < len(lines) and lines[i].startswith("|"):
                table_lines.append(lines[i])
                i += 1
            output.append("[list]")
            for row in table_lines[2:]:  # skip header row [0] and separator row [1]
                # Split on unescaped pipes only, then unescape `\|` within each cell.
                raw = re.split(r"(?<!\\)\|", row.strip().strip("|"))
                cells = [convert_inline(c.strip().replace("\\|", "|")) for c in raw]
                output.append(f"[*]{' — '.join(cells)}")
            output.append("[/list]")
            continue

        # Unordered list block
        if line.startswith("- "):
            items: list[str] = []
            while i < len(lines) and lines[i].startswith("- "):
                items.append(convert_inline(lines[i][2:].strip()))
                i += 1
            output.append("[list]")
            for item in items:
                output.append(f"[*]{item}")
            output.append("[/list]")
            continue

        # Ordered list block — may be preceded by a bold label line
        if re.match(r"\d+\. ", line):
            items = []
            while i < len(lines) and re.match(r"\d+\. ", lines[i]):
                items.append(convert_inline(re.sub(r"^\d+\. ", "", lines[i])))
                i += 1
            output.append("[list=1]")
            for item in items:
                output.append(f"[*]{item}")
            output.append("[/list]")
            continue

        # Bold label (e.g. **Mod manager (recommended):**)
        if re.match(r"\*\*.+\*\*", line):
            output.append(convert_inline(line))
            i += 1
            continue

        # Blank line
        if line.strip() == "":
            output.append("")
            i += 1
            continue

        # Plain paragraph (pass through inline conversion)
        stripped = line.strip()
        if stripped and not stripped.startswith("<!--"):
            output.append(convert_inline(line))

        i += 1

    # Collapse consecutive blank lines to single blank
    result = re.sub(r"\n{3,}", "\n\n", "\n".join(output))
    return result.strip()


def convert_inline(text: str) -> str:
    # [text](url) → [url=url]text[/url]
    # Note: link text containing ] or URLs containing unbalanced ) are not supported.
    text = re.sub(r"\[([^\]]+)\]\(([^()]*(?:\([^)]*\)[^()]*)*)\)", r"[url=\2]\1[/url]", text)
    # **text** → [b]text[/b]
    text = re.sub(r"\*\*(.+?)\*\*", r"[b]\1[/b]", text)
    # `code` → [font=Courier New]code[/font]
    text = re.sub(r"`([^`]+)`", r"[font=Courier New]\1[/font]", text)
    return text


def extract_bbcode_fence(text: str) -> str:
    start = text.find(BBCODE_FENCE_OPEN)
    if start == -1:
        sys.exit(f"error: {BBCODE_FENCE_OPEN!r} fence not found in nexus-page.md")
    newline = text.find("\n", start)
    if newline == -1:
        sys.exit(f"error: no newline after {BBCODE_FENCE_OPEN!r} in nexus-page.md")
    start = newline + 1
    end = text.find(f"\n{BBCODE_FENCE_CLOSE}", start)
    if end == -1:
        sys.exit("error: closing ``` fence not found in nexus-page.md")
    return text[start:end]


def inject_generated(bbcode_content: str, generated: str) -> str:
    start = bbcode_content.find(GEN_START)
    if start == -1:
        sys.exit(f"error: marker not found: {GEN_START!r}")
    end = bbcode_content.find(GEN_END, start + len(GEN_START))
    if end == -1:
        sys.exit(f"error: marker not found: {GEN_END!r}")
    return (
        bbcode_content[:start]
        + GEN_START + "\n"
        + generated + "\n"
        + bbcode_content[end:]
    )


def try_copy(command: list[str], text: str) -> bool:
    # Forked clipboard servers hold inherited pipes open, so stderr goes to a file.
    with tempfile.TemporaryFile() as stderr:
        try:
            subprocess.run(
                command,
                input=text.encode("utf-8"),
                stdout=subprocess.DEVNULL,
                stderr=stderr,
                check=True,
                timeout=CLIPBOARD_TIMEOUT_SECONDS,
            )
        except (OSError, subprocess.SubprocessError) as exc:
            stderr.seek(0)
            detail = stderr.read().decode("utf-8", errors="replace").strip() or str(exc)
            print(f"warning: {command[0]} failed: {detail}", file=sys.stderr)
            return False
    return True


def copy_to_clipboard(text: str) -> None:
    installed = [cmd for cmd in CLIPBOARD_COMMANDS if shutil.which(cmd[0])]
    if not installed:
        tried = ", ".join(cmd[0] for cmd in CLIPBOARD_COMMANDS)
        sys.exit(f"error: no clipboard tool found (tried {tried})")
    if any(try_copy(cmd, text) for cmd in installed):
        return
    sys.exit("error: every installed clipboard tool failed")


def main() -> None:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--copy",
        action="store_true",
        help="copy the output to the clipboard instead of printing it",
    )
    args = parser.parse_args()

    readme = README.read_text(encoding="utf-8")
    template = NEXUS_TEMPLATE.read_text(encoding="utf-8")

    md_block = extract_block(readme, NEXUS_START, NEXUS_END)
    bbcode_generated = md_to_bbcode(md_block)

    bbcode_content = extract_bbcode_fence(template)
    result = inject_generated(bbcode_content, bbcode_generated)
    result = result.replace(GEN_START + "\n", "", 1).replace("\n" + GEN_END, "", 1)

    if not args.copy:
        print(result)
        return

    copy_to_clipboard(result + "\n")
    print("copied to clipboard", file=sys.stderr)


if __name__ == "__main__":
    main()
