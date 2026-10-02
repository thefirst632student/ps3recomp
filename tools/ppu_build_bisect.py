#!/usr/bin/env python3
"""Build-time function splitter for generated PPU translation units.

This tool is deliberately separate from ppu_lifter.py.  It never edits the
lifter output in place; instead it reads one existing ppu_recomp_NNN.cpp and
writes temporary compilation shards to a caller-selected directory.

The selected function slice can then be compiled at a different optimisation
level by CMake while the original recompiled/ tree remains read-only.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import sys
from dataclasses import dataclass
from pathlib import Path

FUNC_RE = re.compile(
    r"(?m)^void\s+(func_[0-9A-Fa-f]+)\(ppu_context\*\s+ctx\)\s*\{\s*$"
)
TABLE_RE = re.compile(r"(?m)^const\s+func_entry\s+function_table\[\]\s*=\s*\{\s*$")


@dataclass(frozen=True)
class FunctionSpan:
    name: str
    start: int
    end: int


def _select_range(count: int, path: str) -> tuple[int, int]:
    """Return [start, length] for an A/B binary-search path.

    A always takes ceil(n/2), B takes the remainder.  Repeating the rule makes
    paths deterministic for odd counts and mirrors the prior CMake bisects.
    """
    start = 0
    length = count
    for depth, branch in enumerate(path, 1):
        if branch not in "AB":
            raise ValueError(f"invalid branch {branch!r} at depth {depth}; use only A/B")
        if length <= 0:
            raise ValueError(
                f"path {path!r} descends past an empty slice at depth {depth}"
            )
        a_len = (length + 1) // 2
        if branch == "A":
            length = a_len
        else:
            start += a_len
            length -= a_len
    return start, length


def _function_spans(text: str) -> tuple[str, list[FunctionSpan], str]:
    matches = list(FUNC_RE.finditer(text))
    if not matches:
        raise ValueError("no lifted function definitions matching 'void func_XXXXXXXX(ppu_context* ctx)' found")

    table = TABLE_RE.search(text, matches[-1].end())
    if not table:
        raise ValueError("could not locate 'const func_entry function_table[] = {' after lifted functions")

    preamble = text[: matches[0].start()]
    tail = text[table.start() :]
    spans: list[FunctionSpan] = []
    for i, m in enumerate(matches):
        end = matches[i + 1].start() if i + 1 < len(matches) else table.start()
        spans.append(FunctionSpan(m.group(1), m.start(), end))

    return preamble, spans, tail


def _write_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    # newline="" preserves CRLF/LF sequences already present in text.
    with path.open("w", encoding="utf-8", newline="") as f:
        f.write(text)


def _compose(preamble: str, source: str, spans: list[FunctionSpan], tail: str = "") -> str:
    pieces = [preamble]
    pieces.extend(source[s.start : s.end] for s in spans)
    if tail:
        pieces.append(tail)
    return "".join(pieces)


def _display_name(span: FunctionSpan) -> str:
    m = re.fullmatch(r"func_([0-9A-Fa-f]+)", span.name)
    if not m:
        return span.name
    return f"{span.name} (guest 0x{int(m.group(1), 16):08X})"


def split_source(
    input_path: Path,
    output_dir: Path,
    function_path: str,
    selected_optimization: str,
) -> dict:
    raw = input_path.read_bytes()
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise ValueError(f"{input_path} is not UTF-8: {exc}") from exc

    preamble, functions, tail = _function_spans(text)
    start, length = _select_range(len(functions), function_path)
    if length <= 0:
        raise ValueError(f"function path {function_path!r} selected an empty slice")

    before = functions[:start]
    selected = functions[start : start + length]
    after = functions[start + length :]

    if output_dir.exists():
        shutil.rmtree(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    stem = input_path.stem
    outputs: list[dict[str, object]] = []

    if before:
        pre_path = output_dir / f"{stem}_av_00_pre.cpp"
        _write_text(pre_path, _compose(preamble, text, before))
        outputs.append({"role": "pre", "path": str(pre_path), "functions": len(before), "optimization": "release"})

    selected_tag = "o1" if selected_optimization == "o1" else "selected"
    selected_path = output_dir / f"{stem}_av_01_{selected_tag}.cpp"
    _write_text(selected_path, _compose(preamble, text, selected))
    outputs.append(
        {
            "role": "selected",
            "path": str(selected_path),
            "functions": len(selected),
            "optimization": selected_optimization,
        }
    )

    # The function table must exist exactly once.  Keep it in the final shard;
    # this shard is emitted even when there are no functions after the slice.
    post_path = output_dir / f"{stem}_av_02_post.cpp"
    _write_text(post_path, _compose(preamble, text, after, tail))
    outputs.append({"role": "post", "path": str(post_path), "functions": len(after), "optimization": "release"})

    # Structural safety checks before CMake ever sees the shards.
    generated_text = "\n".join(Path(o["path"]).read_text(encoding="utf-8") for o in outputs)
    generated_names = FUNC_RE.findall(generated_text)
    original_names = [f.name for f in functions]
    if generated_names != original_names:
        raise RuntimeError("generated shards do not preserve the original lifted-function order exactly")
    if generated_text.count("const func_entry function_table[] = {") != 1:
        raise RuntimeError("generated shards must contain function_table exactly once")
    if generated_text.count("const uint64_t function_table_count") != 1:
        raise RuntimeError("generated shards must contain function_table_count exactly once")

    manifest = {
        "schema": 1,
        "input": str(input_path),
        "input_sha256": hashlib.sha256(raw).hexdigest(),
        "function_count": len(functions),
        "function_path": function_path,
        "selected_start_index": start,
        "selected_count": length,
        "selected_optimization": selected_optimization,
        "selected_first": selected[0].name,
        "selected_last": selected[-1].name,
        "selected_functions": [f.name for f in selected],
        "outputs": outputs,
    }
    (output_dir / "bisect_manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )

    summary_lines = [
        "PS3Recomp build-time PPU function bisect",
        f"input: {input_path}",
        f"input sha256: {manifest['input_sha256']}",
        f"functions: {len(functions)}",
        f"path: {function_path}",
        f"selected: {length}/{len(functions)} functions (indices {start}..{start + length - 1})",
        f"selected optimization: {selected_optimization}",
        f"first selected: {_display_name(selected[0])}",
        f"last selected:  {_display_name(selected[-1])}",
        "generated shards:",
    ]
    for out in outputs:
        summary_lines.append(
            f"  {Path(str(out['path'])).name}: {out['functions']} functions, {out['optimization']}"
        )
    if len(selected) <= 16:
        summary_lines.append("selected function(s):")
        summary_lines.extend(f"  [{start + i}] {_display_name(fn)}" for i, fn in enumerate(selected))
    summary = "\n".join(summary_lines) + "\n"
    (output_dir / "bisect_manifest.txt").write_text(summary, encoding="utf-8")
    print(summary, end="")

    return manifest


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Split an existing ppu_recomp_NNN.cpp into temporary function shards without modifying recompiled/."
    )
    parser.add_argument("--input", required=True, type=Path, help="existing generated ppu_recomp_NNN.cpp")
    parser.add_argument("--output-dir", required=True, type=Path, help="temporary output directory")
    parser.add_argument(
        "--function-path",
        default="A",
        help="binary-search path using A/B (A=ceil first half, B=remainder); default: A",
    )
    parser.add_argument(
        "--selected-optimization",
        choices=("release", "o1"),
        default="release",
        help="label selected shard as normal Release (control) or O1; default: release",
    )
    args = parser.parse_args(argv)

    try:
        split_source(
            args.input.resolve(),
            args.output_dir.resolve(),
            args.function_path.strip().upper(),
            args.selected_optimization,
        )
    except (OSError, ValueError, RuntimeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
