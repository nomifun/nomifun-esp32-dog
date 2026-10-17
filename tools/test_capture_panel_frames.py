"""Packet integrity, physical page-bit mapping, and private-log handling checks."""

import binascii
from contextlib import redirect_stderr, redirect_stdout
import io
import json
from pathlib import Path
import struct
import tempfile
import unittest
import zlib

import capture_panel_frames as capture


def packet(data=None, frame_id=1, label="settings-volume", sequence=None, after=None):
    data = bytes(range(256)) * 4 if data is None else data
    sequence = frame_id if sequence is None else sequence
    after = sequence - 1 if after is None else after
    crc = binascii.crc32(data) & 0xFFFFFFFF
    lines = [f"DOG_PANEL_V1 BEGIN id={frame_id} label={label} width=128 height=64 "
             f"bytes=1024 chunks=16 crc32={crc:08x} seq={sequence} after={after} "
             "success=1 layout=sh1106-page-lsb polarity=1"]
    for part in range(16):
        lines.append(f"DOG_PANEL_V1 DATA id={frame_id} part={part}/16 "
                     f"hex={data[part * 64:(part + 1) * 64].hex()}")
    lines.append(f"DOG_PANEL_V1 END id={frame_id} crc32={crc:08x}")
    return lines


class PacketTest(unittest.TestCase):
    def test_firmware_protocol_and_ansi_logs_roundtrip_without_source_text(self):
        source = "sensitive-wifi-password-never-echo-this"
        lines = [f"I (23) Other: {source}"]
        lines += [f"\x1b[0;32mI (99) DogPanelCapture: {line}\x1b[0m\r\n" for line in packet()]
        lines += ["I (100) Other: DOG_PANEL_V1 this is not a capture record"]
        frames = capture.parse_capture_lines(lines)
        self.assertEqual(len(frames), 1)
        self.assertEqual(frames[0].data, bytes(range(256)) * 4)
        self.assertEqual(frames[0].label, "settings-volume")

    def test_multiple_complete_frames(self):
        frames = capture.parse_capture_lines(packet() + packet(frame_id=2, label="apps"))
        self.assertEqual([frame.frame_id for frame in frames], [1, 2])

    def test_empty_noisy_or_truncated_logs_fail(self):
        for lines in ([], ["unrelated"], packet()[:-1], packet()[:1]):
            with self.subTest(lines=len(lines)), self.assertRaises(capture.CaptureError):
                capture.parse_capture_lines(lines)

    def test_missing_duplicate_unordered_or_extra_chunks_fail(self):
        for edit in (
            lambda lines: lines[:3] + lines[4:],
            lambda lines: lines[:3] + [lines[2]] + lines[3:],
            lambda lines: [lines[0], lines[2], lines[1]] + lines[3:],
            lambda lines: lines[:-1] + [lines[-2]] + lines[-1:],
        ):
            with self.subTest(edit=edit), self.assertRaises(capture.CaptureError):
                capture.parse_capture_lines(edit(packet()))

    def test_frame_id_mismatch_duplicate_or_overflow_fail(self):
        data_wrong = packet(); data_wrong[2] = data_wrong[2].replace("id=1", "id=2")
        end_wrong = packet(); end_wrong[-1] = end_wrong[-1].replace("id=1", "id=2")
        for lines in (data_wrong, end_wrong, packet() + packet(), packet(frame_id=0),
                      packet(frame_id=0x100000000), packet(frame_id=10**30)):
            with self.subTest(lines=len(lines)), self.assertRaises(capture.CaptureError):
                capture.parse_capture_lines(lines)

    def test_restarting_unfinished_frame_is_not_silently_skipped(self):
        with self.assertRaises(capture.CaptureError):
            capture.parse_capture_lines(packet()[:4] + packet(frame_id=2))

    def test_stale_unsuccessful_or_overflow_sequence_fails(self):
        for lines in (packet(sequence=5, after=5), packet(sequence=4, after=5),
                      packet(sequence=0, after=0), packet(sequence=0x100000000),
                      packet(sequence=7) + packet(frame_id=2, sequence=7),
                      [line.replace("success=1", "success=0") for line in packet()]):
            with self.subTest(lines=len(lines)), self.assertRaises(capture.CaptureError):
                capture.parse_capture_lines(lines)

    def test_wrong_geometry_layout_polarity_label_and_chunk_length_fail(self):
        replacements = ((0, "width=128", "width=127"), (0, "height=64", "height=32"),
                        (0, "bytes=1024", "bytes=1023"), (0, "chunks=16", "chunks=15"),
                        (0, "polarity=1", "polarity=0"),
                        (0, "sh1106-page-lsb", "row-msb"),
                        (0, "settings-volume", "../escape"),
                        (0, "settings-volume", "a" * 49),
                        (1, "000102", "0001"), (1, "000102", "zz0102"))
        for index, old, new in replacements:
            lines = packet(); lines[index] = lines[index].replace(old, new)
            with self.subTest(new=new), self.assertRaises(capture.CaptureError):
                capture.parse_capture_lines(lines)

    def test_begin_end_crc_and_actual_payload_must_all_match(self):
        for index in (0, -1):
            lines = packet(); lines[index] = lines[index].replace("crc32=", "crc32=0", 1)
            with self.subTest(index=index), self.assertRaises(capture.CaptureError):
                capture.parse_capture_lines(lines)
        lines = packet(); lines[1] = lines[1].replace("000102", "010102")
        with self.assertRaisesRegex(capture.CaptureError, "CRC mismatch"):
            capture.parse_capture_lines(lines)

    def test_exception_does_not_echo_malformed_record_or_unrelated_secrets(self):
        secret = "NEVER_ECHO_SECRET"
        with self.assertRaises(capture.CaptureError) as error:
            capture.parse_capture_lines(["unrelated " + secret, "DOG_PANEL_V1 " + secret])
        self.assertNotIn(secret, str(error.exception))


class PixelTest(unittest.TestCase):
    def test_each_page_and_bit_maps_to_exact_physical_pixel(self):
        data = bytearray(1024)
        points = ((0, 0), (127, 0), (0, 7), (53, 8), (64, 31), (127, 63))
        for x, y in points:
            data[(y // 8) * 128 + x] |= 1 << (y % 8)
        pixels = capture.panel_pixels(bytes(data))
        lit = {(index % 128, index // 128) for index, value in enumerate(pixels) if value}
        self.assertEqual(lit, set(points))

    def test_zero_is_black_and_one_is_white(self):
        self.assertEqual(set(capture.panel_pixels(bytes(1024))), {0})
        self.assertEqual(set(capture.panel_pixels(b"\xff" * 1024)), {255})

    def test_wrong_frame_length_and_invalid_scale_fail(self):
        for length in (0, 1023, 1025):
            with self.subTest(length=length), self.assertRaises(capture.CaptureError):
                capture.panel_pixels(bytes(length))
        for scale in (0, 17, 1.5, True):
            with self.subTest(scale=scale), self.assertRaises(capture.CaptureError):
                capture.png_bytes(bytes(8192), scale)

    def test_png_integrity_and_nearest_neighbour_mapping(self):
        pixels = capture.panel_pixels(b"\x01" + bytes(1023))
        blob = capture.png_bytes(pixels, 4)
        self.assertEqual(blob[:8], b"\x89PNG\r\n\x1a\n")
        offset, chunks = 8, {}
        while offset < len(blob):
            length = struct.unpack_from(">I", blob, offset)[0]
            kind = blob[offset + 4:offset + 8]
            payload = blob[offset + 8:offset + 8 + length]
            crc = struct.unpack_from(">I", blob, offset + 8 + length)[0]
            self.assertEqual(crc, binascii.crc32(kind + payload) & 0xFFFFFFFF)
            chunks[kind] = payload
            offset += length + 12
        self.assertEqual(struct.unpack(">IIBBBBB", chunks[b"IHDR"]), (512, 256, 8, 0, 0, 0, 0))
        raster = zlib.decompress(chunks[b"IDAT"])
        self.assertEqual(len(raster), 256 * 513)
        for y in range(256):
            row = raster[y * 513:(y + 1) * 513]
            self.assertEqual(row[0], 0)
            self.assertEqual(row[1:5], b"\xff" * 4 if y < 4 else bytes(4))
            self.assertEqual(set(row[5:]), {0})


class CliTest(unittest.TestCase):
    def test_native_scaled_png_and_safe_manifest(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "private.log"
            secret = "DO_NOT_PUBLISH_THIS_TOKEN"
            source.write_text(secret + "\n" + "\n".join(packet()), encoding="utf-8")
            output = io.StringIO()
            with redirect_stdout(output):
                code = capture.main([str(source), "--output", str(root / "frames")])
            self.assertEqual(code, 0)
            self.assertNotIn(secret, output.getvalue())
            self.assertEqual(json.loads(output.getvalue())["frames"], 1)
            manifest = (root / "frames/manifest.json").read_text(encoding="utf-8")
            self.assertNotIn(secret, manifest)
            self.assertEqual(len(list((root / "frames").glob("*.png"))), 2)

    def test_corrupt_log_creates_no_partial_output_and_echoes_no_secret(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "private.log"
            source.write_text("\n".join(packet() + ["DOG_PANEL_V1 SECRET"]), encoding="utf-8")
            errors = io.StringIO()
            with redirect_stderr(errors):
                code = capture.main([str(source), "--output", str(root / "frames")])
            self.assertEqual(code, 1)
            self.assertNotIn("SECRET", errors.getvalue())
            self.assertFalse((root / "frames").exists())


if __name__ == "__main__":
    unittest.main()
