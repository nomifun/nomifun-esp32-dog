#!/usr/bin/env python3
"""Decode only DOG_PANEL_V1 framebuffer records from a private USB serial log.

This tool does not open the serial port. It never echoes arbitrary input log
lines. Every frame must be a complete 128x64 SH1106 page buffer with 16 ordered
64-byte chunks, a matching frame id, and IEEE CRC-32. Output is the actual panel
submission (1 means OLED pixel on), without LVGL palette/colour reinterpretation.
Keep the source log private: unrelated serial output can contain credentials.
"""

from __future__ import annotations

import argparse
import binascii
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import re
import struct
import sys
import zlib


WIDTH = 128
HEIGHT = 64
FRAME_BYTES = WIDTH * HEIGHT // 8
CHUNK_BYTES = 64
CHUNKS = FRAME_BYTES // CHUNK_BYTES
TOKEN = "DOG_PANEL_V1"
ANSI = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")
BEGIN = re.compile(
    r"DOG_PANEL_V1 BEGIN id=([0-9]+) label=([A-Za-z0-9_-]{1,48}) "
    r"width=128 height=64 bytes=1024 chunks=16 crc32=([0-9a-fA-F]{8}) "
    r"seq=([0-9]+) after=([0-9]+) success=1 "
    r"layout=sh1106-page-lsb polarity=1"
)
DATA = re.compile(r"DOG_PANEL_V1 DATA id=([0-9]+) part=([0-9]+)/16 hex=([0-9a-fA-F]{128})")
END = re.compile(r"DOG_PANEL_V1 END id=([0-9]+) crc32=([0-9a-fA-F]{8})")


class CaptureError(ValueError):
    """A bounded explanation that never includes arbitrary source-log text."""


@dataclass(frozen=True)
class PanelFrame:
    frame_id: int
    label: str
    data: bytes
    crc32: int
    sequence: int
    request_after: int


def _record(line: str) -> str | None:
    clean = ANSI.sub("", line).strip()
    if "DogPanelCapture:" in clean:
        payload = clean.split("DogPanelCapture:", 1)[1].strip()
        return payload if payload.startswith(TOKEN) else None
    return clean if clean.startswith(TOKEN) else None


def _frame_id(value: str, line_number: int) -> int:
    if len(value) > 10 or not 0 < int(value) <= 0xFFFFFFFF:
        raise CaptureError(f"line {line_number}: invalid frame id")
    return int(value)


def _sequence(value: str, line_number: int) -> int:
    if len(value) > 10 or not 0 <= int(value) <= 0xFFFFFFFF:
        raise CaptureError(f"line {line_number}: invalid successful-draw sequence")
    return int(value)


def parse_capture_lines(lines) -> list[PanelFrame]:
    """Validate the entire stream before producing any renderable frames."""
    frames: list[PanelFrame] = []
    active_id = None
    active_label = ""
    expected_crc = 0
    chunks: list[bytes] = []
    previous_id = 0
    previous_sequence = 0
    active_sequence = request_after = 0
    for line_number, line in enumerate(lines, 1):
        record = _record(line)
        if record is None:
            continue
        begin = BEGIN.fullmatch(record)
        if begin:
            if active_id is not None:
                raise CaptureError(f"line {line_number}: new frame before previous frame ended")
            active_id = _frame_id(begin[1], line_number)
            if active_id <= previous_id:
                raise CaptureError(f"line {line_number}: duplicate or decreasing frame id")
            active_label = begin[2]
            expected_crc = int(begin[3], 16)
            active_sequence = _sequence(begin[4], line_number)
            request_after = _sequence(begin[5], line_number)
            if active_sequence <= request_after or active_sequence <= previous_sequence:
                raise CaptureError(f"line {line_number}: capture is not a new successful full draw")
            chunks = []
            continue
        data = DATA.fullmatch(record)
        if data:
            frame_id = _frame_id(data[1], line_number)
            if active_id is None or frame_id != active_id:
                raise CaptureError(f"line {line_number}: data frame id does not match an open frame")
            # Avoid parsing unbounded integers from malformed input.
            if len(data[2]) > 2 or int(data[2]) != len(chunks) or len(chunks) >= CHUNKS:
                raise CaptureError(f"line {line_number}: missing, duplicate, or unordered chunk")
            chunks.append(bytes.fromhex(data[3]))
            continue
        end = END.fullmatch(record)
        if end:
            frame_id = _frame_id(end[1], line_number)
            if active_id is None or frame_id != active_id:
                raise CaptureError(f"line {line_number}: end frame id does not match an open frame")
            payload = b"".join(chunks)
            if len(chunks) != CHUNKS or len(payload) != FRAME_BYTES:
                raise CaptureError(f"line {line_number}: incomplete framebuffer")
            actual_crc = binascii.crc32(payload) & 0xFFFFFFFF
            if expected_crc != int(end[2], 16) or actual_crc != expected_crc:
                raise CaptureError(f"line {line_number}: framebuffer CRC mismatch")
            frames.append(PanelFrame(active_id, active_label, payload, actual_crc,
                                     active_sequence, request_after))
            previous_id = active_id
            previous_sequence = active_sequence
            active_id = None
            continue
        raise CaptureError(f"line {line_number}: malformed panel capture record")
    if active_id is not None:
        raise CaptureError("end of log: incomplete framebuffer record")
    if not frames:
        raise CaptureError("no complete panel capture records found")
    return frames


def panel_pixels(data: bytes) -> bytes:
    """Return row-major 8-bit luminance pixels from page-major bit0-first data."""
    if len(data) != FRAME_BYTES:
        raise CaptureError("framebuffer must contain exactly 1024 bytes")
    return bytes(
        255 if data[(y // 8) * WIDTH + x] & (1 << (y % 8)) else 0
        for y in range(HEIGHT) for x in range(WIDTH)
    )


def _png_chunk(kind: bytes, data: bytes) -> bytes:
    return (struct.pack(">I", len(data)) + kind + data +
            struct.pack(">I", binascii.crc32(kind + data) & 0xFFFFFFFF))


def png_bytes(pixels: bytes, scale: int = 1) -> bytes:
    """Encode grayscale PNG with integer nearest-neighbour scaling, without PIL."""
    if len(pixels) != WIDTH * HEIGHT:
        raise CaptureError("pixel raster has an invalid length")
    if isinstance(scale, bool) or not isinstance(scale, int) or not 1 <= scale <= 16:
        raise CaptureError("scale must be an integer from 1 to 16")
    rows = []
    for y in range(HEIGHT):
        row = b"".join(bytes([value]) * scale for value in pixels[y * WIDTH:(y + 1) * WIDTH])
        rows.extend([b"\0" + row] * scale)
    header = struct.pack(">IIBBBBB", WIDTH * scale, HEIGHT * scale, 8, 0, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + _png_chunk(b"IHDR", header) +
            _png_chunk(b"IDAT", zlib.compress(b"".join(rows), 9)) + _png_chunk(b"IEND", b""))


def write_frames(frames: list[PanelFrame], output: Path, scale: int = 4) -> dict:
    if not frames:
        raise CaptureError("no frames to write")
    # Validate before making output files, including a bad scale/byte length.
    rendered = []
    for frame in frames:
        pixels = panel_pixels(frame.data)
        rendered.append((frame, png_bytes(pixels), png_bytes(pixels, scale)))
    output.mkdir(parents=True, exist_ok=True)
    entries = []
    for frame, native, enlarged in rendered:
        stem = f"{frame.frame_id:04d}-{frame.label}"
        native_name = f"{stem}.png"
        enlarged_name = f"{stem}-{scale}x.png"
        (output / native_name).write_bytes(native)
        (output / enlarged_name).write_bytes(enlarged)
        entries.append({
            "frame_id": frame.frame_id,
            "label": frame.label,
            "width": WIDTH,
            "height": HEIGHT,
            "bytes": FRAME_BYTES,
            "crc32": f"{frame.crc32:08x}",
            "successful_draw_sequence": frame.sequence,
            "request_after_sequence": frame.request_after,
            "sha256": hashlib.sha256(frame.data).hexdigest(),
            "native_png": native_name,
            "enlarged_png": enlarged_name,
        })
    manifest = {
        "format": TOKEN,
        "source": "SH1106 draw_bitmap successful full-screen submission",
        "layout": "page-major, bit0 at top of each 8-pixel page, 1=OLED on",
        "scale": scale,
        "frames": entries,
    }
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("serial_log", type=Path, help="existing private serial log (never echoed)")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--scale", default=4, type=int)
    args = parser.parse_args(argv)
    try:
        with args.serial_log.open("r", encoding="utf-8", errors="replace") as stream:
            frames = parse_capture_lines(stream)
        manifest = write_frames(frames, args.output, args.scale)
    except (CaptureError, OSError) as exc:
        # CaptureError messages contain only line numbers and fixed reasons;
        # filesystem failures do not include input file contents.
        print(f"panel capture failed: {exc}", file=sys.stderr)
        return 1
    print(json.dumps({"frames": len(manifest["frames"]), "output": str(args.output)}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
