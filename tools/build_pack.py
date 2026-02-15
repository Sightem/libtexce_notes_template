#!/usr/bin/env python3
from __future__ import annotations

import argparse
import bisect
import json
import re
import struct
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

OS_VAR_MAX_SIZE = 65512
INDEX_NAME = "NTXIDX"
PART_PREFIX = "NTX"

SPLIT_NONE = 0
SPLIT_SENTENCE = 1
SPLIT_PARAGRAPH = 2
SPLIT_WHITESPACE = 3
SPLIT_HARD = 4

PART_HEADER_FMT = "<4sHHHHHHHHHH"
PART_ENTRY_FMT = "<HHBBH"
INDEX_HEADER_FMT = "<4sHHHHI"
INDEX_ENTRY_FIXED_FMT = "<HHHHIBB"

PART_HEADER_SIZE = struct.calcsize(PART_HEADER_FMT)
PART_ENTRY_SIZE = struct.calcsize(PART_ENTRY_FMT)
INDEX_HEADER_SIZE = struct.calcsize(INDEX_HEADER_FMT)
INDEX_ENTRY_FIXED_SIZE = struct.calcsize(INDEX_ENTRY_FIXED_FMT)


@dataclass
class Chunk:
    text: str
    kind: int
    idx: int

    @property
    def data(self) -> bytes:
        return self.text.encode("utf-8")


@dataclass
class NoteBuild:
    note_id: int
    title: str
    source: Path
    chunks: list[Chunk]
    first_part_id: int = 0
    part_count: int = 0


@dataclass
class PartBuild:
    name: str
    note_id: int
    part_index: int
    part_count: int
    chunks: list[Chunk]
    payload: bytes


class LoudWarningCollector:
    def __init__(self) -> None:
        self.items: list[str] = []

    def warn(self, message: str) -> None:
        self.items.append(message)

    def emit(self) -> None:
        if not self.items:
            return
        print("\n=== WARNINGS (non-fatal) ===", file=sys.stderr)
        for idx, msg in enumerate(self.items, start=1):
            print(f"[{idx}] {msg}", file=sys.stderr)


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Build libtexce notes pack AppVars")
    p.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    p.add_argument("--notes-dir", type=Path)
    p.add_argument("--out-raw", type=Path)
    p.add_argument("--out-8xv", type=Path)
    p.add_argument("--target-bytes", type=int, default=40960)
    p.add_argument("--hard-bytes", type=int, default=49152)
    p.add_argument("--skip-convbin", action="store_true")
    p.add_argument("--latex-commands", type=Path)
    return p.parse_args()


def load_supported_commands(path: Path) -> set[str]:
    if not path.exists():
        return set()
    commands: set[str] = set()
    bullet_re = re.compile(r"^\s*-\s+\\+([A-Za-z]+)\s*$")
    for line in path.read_text(encoding="utf-8").splitlines():
        m = bullet_re.match(line)
        if m:
            commands.add(m.group(1))
    return commands


def collect_used_commands(text: str) -> set[str]:
    return set(re.findall(r"\\([A-Za-z]+)", text))


def _last_gt_start(bounds: list[int], upper: int, start: int) -> int | None:
    i = bisect.bisect_right(bounds, upper) - 1
    while i >= 0:
        if bounds[i] > start:
            return bounds[i]
        i -= 1
    return None


def compute_boundaries(text: str) -> tuple[list[int], list[int], list[int]]:
    sentence: list[int] = []
    whitespace: list[int] = []

    in_inline = False
    in_display = False
    i = 0
    n = len(text)

    while i < n:
        ch = text[i]
        prev = text[i - 1] if i > 0 else ""

        if ch == "$" and prev != "\\":
            if i + 1 < n and text[i + 1] == "$":
                in_display = not in_display
                i += 2
                continue
            if not in_display:
                in_inline = not in_inline
                i += 1
                continue

        in_math = in_inline or in_display

        if not in_math:
            if ch.isspace():
                whitespace.append(i + 1)

            if ch in ".?!":
                j = i + 1
                if j >= n or text[j].isspace():
                    k = j
                    while k < n and text[k].isspace():
                        k += 1
                    if k >= n or not text[k].islower():
                        sentence.append(j)

        i += 1

    paragraph = [m.end() for m in re.finditer(r"(?:\r?\n[ \t]*){2,}", text)]

    sentence = sorted(set(sentence))
    paragraph = sorted(set(paragraph))
    whitespace = sorted(set(whitespace))
    return sentence, paragraph, whitespace


def split_text_deterministic(text: str, target: int, hard: int, warnings: LoudWarningCollector, source: str) -> list[Chunk]:
    if target <= 0 or hard <= 0:
        raise ValueError("target and hard must be positive")
    if target > hard:
        raise ValueError("target must be <= hard")

    sentence_bounds, paragraph_bounds, ws_bounds = compute_boundaries(text)
    chunks: list[Chunk] = []

    start = 0
    n = len(text)
    idx = 0

    while start < n:
        remaining = n - start
        if remaining <= target:
            chunks.append(Chunk(text=text[start:n], kind=SPLIT_NONE, idx=idx))
            idx += 1
            break

        preferred_end = min(start + target, n)
        hard_end = min(start + hard, n)

        boundary = _last_gt_start(sentence_bounds, preferred_end, start)
        kind = SPLIT_SENTENCE

        if boundary is None:
            boundary = _last_gt_start(paragraph_bounds, preferred_end, start)
            kind = SPLIT_PARAGRAPH

        if boundary is None:
            boundary = _last_gt_start(ws_bounds, preferred_end, start)
            kind = SPLIT_WHITESPACE

        if boundary is None:
            boundary = _last_gt_start(ws_bounds, hard_end, start)
            kind = SPLIT_WHITESPACE

        if boundary is None or boundary <= start:
            boundary = hard_end
            kind = SPLIT_HARD
            warnings.warn(
                f"{source}: hard split at byte {boundary}; consider inserting separators to improve chunking"
            )

        chunk_text = text[start:boundary]
        chunks.append(Chunk(text=chunk_text, kind=kind, idx=idx))
        idx += 1
        start = boundary

    for c in chunks:
        if len(c.data) > hard:
            warnings.warn(
                f"{source}: chunk {c.idx} is {len(c.data)} bytes (> hard cap {hard}); runtime may fail"
            )

    return chunks


def partition_into_parts(chunks: list[Chunk]) -> list[list[Chunk]]:
    parts: list[list[Chunk]] = []
    cur: list[Chunk] = []
    cur_payload = 0

    for chunk in chunks:
        c_len = len(chunk.data)
        next_count = len(cur) + 1
        next_payload = cur_payload + c_len
        next_size = PART_HEADER_SIZE + (next_count * PART_ENTRY_SIZE) + next_payload

        if next_size > OS_VAR_MAX_SIZE and cur:
            parts.append(cur)
            cur = []
            cur_payload = 0
            next_count = 1
            next_payload = c_len
            next_size = PART_HEADER_SIZE + PART_ENTRY_SIZE + c_len

        if next_size > OS_VAR_MAX_SIZE:
            raise RuntimeError(
                f"single chunk too large for AppVar ({next_size} > {OS_VAR_MAX_SIZE}); lower hard limit"
            )

        cur.append(chunk)
        cur_payload = next_payload

    if cur:
        parts.append(cur)

    return parts


def build_part_blob(note_id: int, part_index: int, part_count: int, chunks: list[Chunk]) -> bytes:
    payload_parts: list[bytes] = []
    entries: list[bytes] = []
    rel = 0

    for chunk in chunks:
        data = chunk.data
        payload_parts.append(data)
        entry = struct.pack(
            PART_ENTRY_FMT,
            rel,
            len(data),
            chunk.kind,
            0,
            chunk.idx,
        )
        entries.append(entry)
        rel += len(data)

    payload = b"".join(payload_parts)
    chunk_table_offset = PART_HEADER_SIZE
    payload_offset = PART_HEADER_SIZE + (len(entries) * PART_ENTRY_SIZE)

    header = struct.pack(
        PART_HEADER_FMT,
        b"NTXP",
        1,
        PART_HEADER_SIZE,
        note_id,
        part_index,
        part_count,
        len(entries),
        chunk_table_offset,
        payload_offset,
        len(payload),
        0,
    )

    blob = header + b"".join(entries) + payload
    if len(blob) > OS_VAR_MAX_SIZE:
        raise RuntimeError(f"part blob exceeded OS var max: {len(blob)}")
    return blob


def build_index_blob(notes: list[NoteBuild]) -> bytes:
    entries = bytearray()

    for note in notes:
        title_bytes = note.title.encode("utf-8")
        if len(title_bytes) > 255:
            title_bytes = title_bytes[:255]

        fixed = struct.pack(
            INDEX_ENTRY_FIXED_FMT,
            note.note_id,
            note.first_part_id,
            note.part_count,
            len(note.chunks),
            sum(len(c.data) for c in note.chunks),
            len(title_bytes),
            0,
        )
        entries.extend(fixed)
        entries.extend(title_bytes)

    header = struct.pack(
        INDEX_HEADER_FMT,
        b"NTXI",
        1,
        INDEX_HEADER_SIZE,
        len(notes),
        0,
        0,
    )

    blob = bytes(header) + bytes(entries)
    if len(blob) > OS_VAR_MAX_SIZE:
        raise RuntimeError(f"index blob exceeded OS var max: {len(blob)}")
    return blob


def ensure_dir(path: Path) -> None:
    path.mkdir(parents=True, exist_ok=True)


def write_blob(path: Path, data: bytes) -> None:
    path.write_bytes(data)


def run_convbin(bin_path: Path, out_path: Path, name: str) -> None:
    cmd = [
        "convbin",
        "-j",
        "bin",
        "-i",
        str(bin_path),
        "-k",
        "8xv",
        "-o",
        str(out_path),
        "-n",
        name,
        "-r",
    ]
    subprocess.run(cmd, check=True)


def discover_note_files(notes_dir: Path) -> list[Path]:
    if not notes_dir.exists() or not notes_dir.is_dir():
        raise FileNotFoundError(f"notes directory not found: {notes_dir}")

    files: list[Path] = []
    for p in sorted(notes_dir.iterdir()):
        if not p.is_file():
            continue
        if p.name.startswith("."):
            continue
        if p.name.lower() == "manifest.json":
            continue
        files.append(p)

    if not files:
        raise ValueError(f"no note files found in {notes_dir}")
    return files


def derive_title_from_filename(path: Path) -> str:
    return path.stem


def build_notes(args: argparse.Namespace) -> int:
    root: Path = args.root.resolve()
    notes_dir = (args.notes_dir or (root / "notes")).resolve()
    out_raw = (args.out_raw or (root / "dist/raw")).resolve()
    out_8xv = (args.out_8xv or (root / "dist/8xv")).resolve()
    latex_cmd_path = (args.latex_commands or (root / "LATEX_COMMANDS_SUPPORTED.md")).resolve()

    ensure_dir(out_raw)
    ensure_dir(out_8xv)

    warnings = LoudWarningCollector()

    supported = load_supported_commands(latex_cmd_path)
    if not latex_cmd_path.exists():
        warnings.warn(f"no supported-command list found at {latex_cmd_path}; command validation skipped")
    elif not supported:
        warnings.warn(
            f"supported-command list exists but no commands were parsed at {latex_cmd_path}; validation skipped"
        )

    note_files = discover_note_files(notes_dir)
    notes: list[NoteBuild] = []

    for i, source in enumerate(note_files, start=1):
        title = derive_title_from_filename(source)
        if not title:
            title = source.name

        try:
            source_rel = str(source.relative_to(root))
        except ValueError:
            source_rel = str(source)

        text = source.read_text(encoding="utf-8")

        used_cmds = collect_used_commands(text)
        if supported:
            unknown = sorted(cmd for cmd in used_cmds if cmd not in supported)
            if unknown:
                warnings.warn(
                    f"{source_rel}: unsupported commands detected ({', '.join(unknown)})"
                )

        chunks = split_text_deterministic(
            text=text,
            target=args.target_bytes,
            hard=args.hard_bytes,
            warnings=warnings,
            source=source_rel,
        )

        notes.append(
            NoteBuild(
                note_id=i,
                title=title,
                source=source,
                chunks=chunks,
            )
        )

    part_builds: list[PartBuild] = []
    next_part_id = 1

    for note in notes:
        note_parts = partition_into_parts(note.chunks)
        note.first_part_id = next_part_id
        note.part_count = len(note_parts)

        for p_idx, p_chunks in enumerate(note_parts):
            part_id = next_part_id
            next_part_id += 1
            part_name = f"{PART_PREFIX}{part_id:04d}"
            payload = build_part_blob(
                note_id=note.note_id,
                part_index=p_idx,
                part_count=len(note_parts),
                chunks=p_chunks,
            )
            part_builds.append(
                PartBuild(
                    name=part_name,
                    note_id=note.note_id,
                    part_index=p_idx,
                    part_count=len(note_parts),
                    chunks=p_chunks,
                    payload=payload,
                )
            )

    idx_blob = build_index_blob(notes)
    idx_raw = out_raw / f"{INDEX_NAME}.bin"
    write_blob(idx_raw, idx_blob)

    for part in part_builds:
        write_blob(out_raw / f"{part.name}.bin", part.payload)

    if not args.skip_convbin:
        run_convbin(idx_raw, out_8xv / f"{INDEX_NAME}.8xv", INDEX_NAME)
        for part in part_builds:
            run_convbin(out_raw / f"{part.name}.bin", out_8xv / f"{part.name}.8xv", part.name)

    build_index = {
        "index_appvar": INDEX_NAME,
        "notes_dir": str(notes_dir),
        "notes": [
            {
                "note_id": n.note_id,
                "title": n.title,
                "first_part_id": n.first_part_id,
                "part_count": n.part_count,
                "total_chunks": len(n.chunks),
                "source": str(n.source),
            }
            for n in notes
        ],
        "part_count": len(part_builds),
        "artifacts": {
            "raw_dir": str(out_raw),
            "x8v_dir": str(out_8xv),
        },
    }
    (root / "dist/pack_manifest.json").write_text(json.dumps(build_index, indent=2), encoding="utf-8")

    print(f"Built index: {idx_raw}")
    print(f"Built parts: {len(part_builds)}")
    if not args.skip_convbin:
        print(f"Generated AppVars in: {out_8xv}")

    warnings.emit()
    return 0


def main() -> int:
    args = parse_args()
    try:
        return build_notes(args)
    except subprocess.CalledProcessError as e:
        print(f"convbin failed: {e}", file=sys.stderr)
        return 2
    except Exception as e:
        print(f"build failed: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
