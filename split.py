#!/usr/bin/env python3
"""
move_file_comment_after_guard.py

Moves the leading Doxygen "@file" comment block of every .h file so it
sits at the LATEST right after the include guard:

    #ifndef X_H
    #define X_H

    /**
     * @file x.h
     * @brief ...
     */

    <rest of the file>

If the header has no #ifndef/#define guard, the block is moved to the
very top of the file instead. Headers with no "@file" doc block at all
are left untouched.

Usage:
    python3 move_file_comment_after_guard.py [root_dir]

root_dir defaults to the current directory; recurses into subdirectories
(including parser/, examples/, tests/, ...) looking for *.h files.
"""

import re
import sys
from pathlib import Path

BLOCK_RE = re.compile(r'/\*\*.*?\*/', re.DOTALL)
GUARD_RE = re.compile(r'#ifndef\s+(\w+)\s*\n#define\s+\1\s*\n')


def find_file_block(text: str):
    """Return the re.Match for the first /** ... */ block containing @file, or None."""
    for m in BLOCK_RE.finditer(text):
        if '@file' in m.group(0):
            return m
    return None


def strip_block(text: str, block_m) -> str:
    """
    Remove the matched comment block from text and collapse the
    whitespace gap it leaves behind to at most one blank line, so
    repeated moves don't accumulate stray blank lines.
    """
    without = text[:block_m.start()] + text[block_m.end():]
    return re.sub(r'\n{3,}', '\n\n', without)


def insert_after_guard(text: str, block_text: str) -> str:
    """
    Insert block_text right after the #ifndef/#define guard, or at the
    top of the file if no guard is found. Normalises to exactly one
    blank line before and after the inserted block.
    """
    guard_m = GUARD_RE.search(text)

    if guard_m:
        head = text[:guard_m.end()]
        tail = text[guard_m.end():].lstrip('\n')
        return f"{head}\n{block_text}\n\n{tail}"

    # no include guard: comment goes at the very top of the file
    tail = text.lstrip('\n')
    return f"{block_text}\n\n{tail}"


def process_file(path: Path) -> bool:
    original = path.read_text(encoding='utf-8')

    block_m = find_file_block(original)
    if block_m is None:
        return False  # no @file doc block: nothing to move

    block_text = block_m.group(0)
    without_block = strip_block(original, block_m)
    new_text = insert_after_guard(without_block, block_text)

    if new_text == original:
        return False  # already in canonical position

    path.write_text(new_text, encoding='utf-8')
    return True


def main():
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path('.')
    if not root.exists():
        print(f"error: path '{root}' does not exist")
        sys.exit(1)

    changed = 0
    for path in sorted(root.rglob('*.h')):
        if process_file(path):
            changed += 1
            print(f"[moved] {path}")

    print(f"\nDone: {changed} header(s) fixed.")


if __name__ == '__main__':
    main()