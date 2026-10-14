#!/usr/bin/env python3
"""Tests for the TTS streaming chunker in bridge/server.py.

Runs on the standard library alone — no pytest, no piper, no faster_whisper —
so it can be run anywhere the repo is checked out:

    python3 bridge/tests/test_chunker.py

server.py imports faster_whisper, requests, websockets, etc. which are not
present on every machine (and are not installed on the build host), so the
`_extract_complete_sentences` source is lifted out with `ast` and exec'd rather
than importing the module. That still exercises the shipped code text rather
than a copy of it.
"""

from __future__ import annotations

import ast
import pathlib
import sys

SERVER_PY = pathlib.Path(__file__).resolve().parents[1] / "server.py"


def load_chunker():
    src = SERVER_PY.read_text(encoding="utf-8")
    tree = ast.parse(src)

    wanted_consts = {"TTS_STRONG_END", "TTS_WEAK_END"}
    pieces: list[str] = []
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(
            isinstance(t, ast.Name) and t.id in wanted_consts for t in node.targets
        ):
            pieces.append(ast.get_source_segment(src, node))
        if isinstance(node, ast.ClassDef):
            for sub in node.body:
                if isinstance(sub, ast.FunctionDef) and sub.name == "_extract_complete_sentences":
                    body = ast.get_source_segment(src, sub)
                    # Re-indent the method body back to module level.
                    dedented = "\n".join(
                        line[4:] if line.startswith("    ") else line
                        for line in body.splitlines()
                    )
                    pieces.append("@staticmethod\n" + dedented)

    if len(pieces) < 3:
        raise SystemExit(f"could not extract the chunker from {SERVER_PY}")

    ns: dict = {}
    exec("\n\n".join(pieces), ns)  # noqa: S102 — trusted, in-repo source
    return ns["_extract_complete_sentences"]


chunk = load_chunker()
failures: list[str] = []


def check(name: str, got, want) -> None:
    if got != want:
        failures.append(f"{name}\n    got:  {got!r}\n    want: {want!r}")


# --- strong terminators always split -----------------------------------------
check(
    "two sentences, floor 0",
    chunk("Hello there. How are you? Fine", 0),
    (["Hello there.", "How are you?"], " Fine"),
)
check("trailing dot", chunk("All done.", 0), (["All done."], ""))
check("empty", chunk("", 60), ([], ""))
check("no terminator stays pending", chunk("Hello world", 60), ([], "Hello world"))

# --- weak splits are gated on the length floor -------------------------------
bare = "This clause is quite long indeed"  # 32 chars
check("weak below floor", chunk(f"{bare}, tail", 60), ([], f"{bare}, tail"))
check("weak at floor", chunk(f"{bare}, tail", len(bare) + 1), ([f"{bare},"], " tail"))
check(
    "colon split",
    chunk("Here are the options (pick one): alpha", 30),
    (["Here are the options (pick one):"], " alpha"),
)
check(
    "semicolon split",
    chunk("First thing over here is done; second", 30),
    (["First thing over here is done;"], " second"),
)

# --- the floor is measured from the start of the current chunk ---------------
chunks, rest = chunk("One, two, three, four, five, six, seven, eight, nine.", 10)
check("floor first chunk", chunks[0], "One, two, three,")
check("floor second chunk", chunks[1], "four, five,")
check("floor remainder", rest, "")
check(
    "floor rejoins",
    " ".join(chunks) + " " + rest,
    "One, two, three, four, five, six, seven, eight, nine. ",
)

# --- numbers are not boundaries ---------------------------------------------
check("decimals survive", chunk("Pi is 3.14 roughly. Next", 60), (["Pi is 3.14 roughly."], " Next"))
check("thousands survive", chunk("It cost 12,000 dollars and", 10), ([], "It cost 12,000 dollars and"))
check("digit comma guard", chunk("I saw 3, then 4, and then", 8), ([], "I saw 3, then 4, and then"))

# --- odd punctuation ---------------------------------------------------------
check("bang run", chunk("Wow!!! Really", 0), (["Wow!!!"], " Really"))
check("newline boundary", chunk("Line one.\nLine two", 0), (["Line one."], "\nLine two"))
# Preserved quirk, not a regression: the previous regex
# `(.+?[.!?])(?=\s|$)` also broke after "e.g.".
check(
    "abbreviation splits as before",
    chunk("Use e.g. this bit. Done", 60),
    (["Use e.g.", "this bit."], " Done"),
)
# A terminator must be followed by whitespace or end-of-text to count.
check("mid-token dot ignored", chunk("Call 1.800 now", 0), ([], "Call 1.800 now"))
# A comma at index 0 must not index-warp to the last character: with the old
# `text[i - 1].isdigit()` the '7' at the end of the string would be consulted,
# the split would be skipped, and the remainder would come back as ", abc7".
# The remainder keeps its leading space; the caller strips it.
check("leading comma not skipped", chunk(", abc7", 1), ([], " abc7"))
# Punctuation-only fragments are dropped rather than sent to the synthesizer;
# the spaces between them stay in the remainder for the caller to strip.
check("punctuation only dropped", chunk("... , ... then words", 1), ([], " then words"))
check("ellipsis alone dropped", chunk("...", 0), ([], ""))

if failures:
    print("FAILURES:\n" + "\n".join(failures))
    sys.exit(1)

print(f"all chunker tests passed ({SERVER_PY})")
