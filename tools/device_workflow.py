#!/usr/bin/env python3
"""Project-specific ESP32-S3 dog backup and guarded flashing workflow.

`prepare` and `check-backup` are offline; `list` only enumerates ports.
All other commands require an explicit USB port and can reset/open that device.
Backups and monitor logs contain private device data; never publish them.
No arbitrary esptool options, mass erase, encryption, or force path is exposed.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import io
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import time
from datetime import datetime, timezone


ROOT = Path(__file__).resolve().parents[1]
BACKUPS = ROOT / ".private" / "backups"
BUILD = ROOT / "build"
FLASH_SIZE = 16 * 1024 * 1024
SECTOR = 0x1000
LAYOUT = [
    {"name": "nvs", "type": 1, "subtype": 2, "offset": 0x9000, "size": 0x4000, "flags": 0},
    {"name": "otadata", "type": 1, "subtype": 0, "offset": 0xD000, "size": 0x2000, "flags": 0},
    {"name": "phy_init", "type": 1, "subtype": 1, "offset": 0xF000, "size": 0x1000, "flags": 0},
    {"name": "ota_0", "type": 0, "subtype": 0x10, "offset": 0x20000, "size": 0x600000, "flags": 0},
    {"name": "ota_1", "type": 0, "subtype": 0x11, "offset": 0x620000, "size": 0x600000, "flags": 0},
    {"name": "assets", "type": 1, "subtype": 0x82, "offset": 0xC20000, "size": 0x3E0000, "flags": 0},
]
SEGMENTS = {
    "bootloader": (0, 0x8000),
    "partition-table": (0x8000, 0x1000),
    "otadata": (0xD000, 0x2000),
    "app": (0x20000, 0x600000),
    "assets": (0xC20000, 0x3E0000),
}
SETTINGS = {"flash_mode": "dio", "flash_size": "16MB", "flash_freq": "80m"}
WRITE_ARGS = ["--flash_mode", "dio", "--flash_size", "16MB", "--flash_freq", "80m"]


class WorkflowError(Exception):
    pass


def require(condition, message):
    if not condition:
        raise WorkflowError(message)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def utc_now():
    return datetime.now(timezone.utc).isoformat()


def local_path(value, base=ROOT):
    path = Path(value).resolve()
    require(path.is_relative_to(base.resolve()), f"Path must stay inside {base}")
    return path


def child_path(base, name):
    require(isinstance(name, str) and name, "Missing relative file path")
    require(not Path(name).is_absolute(), "Absolute artifact paths are not allowed")
    return local_path(base / name, base)


def load_json(path):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise WorkflowError(f"Invalid JSON file: {path}") from exc


def save_json(path, value):
    # A crash must not leave a partially written manifest that looks complete.
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)


def parse_partition_table(data):
    require(len(data) >= 0xC00, "Partition table is truncated")
    entries = []
    digest_seen = False
    for offset in range(0, 0xC00, 32):
        block = data[offset:offset + 32]
        if block == b"\xff" * 32:
            break
        if block[:2] == b"\xeb\xeb":
            require(block[2:16] == b"\xff" * 14, "Invalid partition MD5 header")
            require(hashlib.md5(data[:offset]).digest() == block[16:], "Partition MD5 mismatch")
            require(data[offset + 32:0xC00] == b"\xff" * (0xC00 - offset - 32), "Unexpected partition data after MD5")
            digest_seen = True
            break
        magic, kind, subtype, start, size, label, flags = struct.unpack("<HBBII16sI", block)
        require(magic == 0x50AA, "Invalid partition magic")
        try:
            name = label.split(b"\0", 1)[0].decode("ascii")
        except UnicodeDecodeError as exc:
            raise WorkflowError("Invalid partition label") from exc
        entries.append({"name": name, "type": kind, "subtype": subtype,
                        "offset": start, "size": size, "flags": flags})
    require(digest_seen, "Partition table needs a verified MD5 trailer")
    validate_layout(entries)
    return entries


def validate_layout(entries):
    previous_end = 0x9000
    names = set()
    for entry in sorted(entries, key=lambda item: item["offset"]):
        start, size = entry["offset"], entry["size"]
        require(entry["name"] not in names, "Duplicate partition name")
        names.add(entry["name"])
        require(start >= previous_end and size > 0, "Partition overlap or invalid size")
        require(start % SECTOR == 0 and size % SECTOR == 0, "Partition is not sector aligned")
        require(start + size <= FLASH_SIZE, "Partition exceeds flash size")
        previous_end = start + size
    require(entries == LAYOUT, "Partition layout differs from expected dog layout; refusing migration")


def validate_image_header(data, name):
    require(len(data) >= 24 and data[0] == 0xE9, f"{name} is not an ESP image")
    require(data[2] == 2 and data[3] == 0x4F, f"{name} must be DIO / 16MB / 80MHz")
    require(struct.unpack_from("<H", data, 12)[0] == 9, f"{name} must target ESP32-S3")
    # Loading an image is purely offline; esptool exposes checksum and SHA256.
    from esptool.bin_image import ESP32S3FirmwareImage
    try:
        image = ESP32S3FirmwareImage(io.BytesIO(data))
        require(image.checksum == image.calculate_checksum(), f"{name} checksum mismatch")
        require(image.append_digest and image.stored_digest == image.calc_digest, f"{name} image SHA256 mismatch")
    except WorkflowError:
        raise
    except Exception as exc:
        raise WorkflowError(f"{name} image is malformed") from exc


def validate_ranges(segments):
    previous_end = 0
    for segment in sorted(segments, key=lambda item: item["offset"]):
        start, size = segment["offset"], segment["size"]
        erase_end = (start + size + SECTOR - 1) // SECTOR * SECTOR
        require(start % SECTOR == 0 and size > 0, "Invalid write segment alignment or size")
        require(start >= previous_end, "Write segments or erase sectors overlap")
        require(erase_end <= FLASH_SIZE, "Write extends beyond physical flash")
        for part in LAYOUT:
            if part["name"] in ("nvs", "phy_init", "ota_1"):
                require(erase_end <= part["offset"] or start >= part["offset"] + part["size"],
                        f"Write would erase preserved {part['name']}")
        previous_end = erase_end


def validate_build(build):
    build = local_path(build)
    metadata = load_json(build / "flasher_args.json")
    config = load_json(build / "config" / "sdkconfig.json")
    project = load_json(build / "project_description.json")
    require(metadata.get("flash_settings") == SETTINGS, "Unexpected flash settings")
    require(metadata.get("write_flash_args") == WRITE_ARGS, "Unexpected write_flash options")
    require(metadata.get("extra_esptool_args", {}).get("chip") == "esp32s3", "Wrong esptool chip")
    require(project.get("target") == "esp32s3" and project.get("project_name") == "xiaozhi", "Wrong build target/project")
    expected = {"IDF_TARGET": "esp32s3", "BOARD_TYPE_ZFSN_ESP32S3_DOG": True,
                "ESPTOOLPY_FLASHMODE": "dio", "ESPTOOLPY_FLASHSIZE": "16MB", "ESPTOOLPY_FLASHFREQ": "80m"}
    require(all(config.get(key) == value for key, value in expected.items()), "Build is not configured for 16MB DIO dog")
    flash_files = metadata.get("flash_files")
    require(isinstance(flash_files, dict) and len(flash_files) == len(SEGMENTS), "Exactly five dog flash segments required")
    segments = []
    seen = {}
    for name, (expected_offset, capacity) in SEGMENTS.items():
        item = metadata.get(name, {})
        require(item.get("encrypted") == "false", "Encrypted write unsupported")
        require(int(item.get("offset", "-1"), 0) == expected_offset, f"Wrong {name} offset")
        relative_file = item.get("file")
        require(isinstance(relative_file, str) and relative_file and not Path(relative_file).is_absolute(),
                "Build artifact needs a relative project path")
        file = local_path(build / relative_file)
        require(file.is_file(), f"Missing build artifact: {file}")
        data = file.read_bytes()
        require(0 < len(data) <= capacity, f"{name} exceeds allocated region")
        if name in ("bootloader", "app"):
            validate_image_header(data, name)
        if name == "partition-table":
            parse_partition_table(data)
        if name == "otadata":
            require(len(data) == 0x2000 and data == b"\xff" * 0x2000, "Initial OTA data must select ota_0")
        seen[expected_offset] = item["file"]
        segments.append({"name": name, "offset": expected_offset, "size": len(data),
                         "file": str(file), "sha256": sha256(data)})
    require({int(key, 0): value for key, value in flash_files.items()} == seen, "flash_files disagrees with named segments")
    validate_ranges(segments)
    return {"build": str(build), "chip": "esp32s3", "settings": SETTINGS,
            "layout": LAYOUT, "preserved": ["nvs", "phy_init", "ota_1"], "segments": sorted(segments, key=lambda item: item["offset"])}


def port_record(port):
    return {key: getattr(port, key, None) for key in
            ("device", "description", "hwid", "vid", "pid", "manufacturer", "product", "serial_number")}


def list_ports():
    from serial.tools import list_ports as serial_ports
    return [port_record(port) for port in serial_ports.comports()]


def require_usb_port(name, ports=None):
    matches = [item for item in (list_ports() if ports is None else ports)
               if str(item["device"]).casefold() == name.casefold()]
    require(len(matches) == 1, "Explicit port is missing or ambiguous")
    item = matches[0]
    description = " ".join(str(item.get(key) or "") for key in ("description", "hwid", "manufacturer", "product"))
    require(item.get("vid") is not None and item.get("pid") is not None, "Only enumerated USB serial devices are permitted")
    require(item["vid"] != 0x8086 and not re.search(r"bluetooth|蓝牙|intel", description, re.I), "Bluetooth/Intel port refused")
    return item


def private_directory(path):
    local_path(path, BACKUPS)
    path.mkdir(parents=True, mode=0o700, exist_ok=False)
    if os.name == "nt":
        identity = subprocess.run(["whoami", "/user", "/fo", "csv", "/nh"], check=True,
                                  capture_output=True, text=True).stdout
        sid = list(csv.reader(io.StringIO(identity.strip())))[0][1]
        require(re.fullmatch(r"S-1-[0-9-]+", sid) is not None, "Cannot determine private backup owner")
        subprocess.run(["icacls", str(path), "/inheritance:r", "/grant:r", f"*{sid}:(OI)(CI)F",
                        "/grant:r", "*S-1-5-18:(OI)(CI)F"], check=True, capture_output=True)
    else:
        path.chmod(0o700)


class EspTool:
    def __init__(self, port, baud, directory):
        self.port, self.baud, self.directory = port, baud, directory

    def run(self, command, *arguments, after="no_reset", timeout=600):
        require(command in {"chip_id", "flash_id", "get_security_info", "read_flash", "verify_flash", "write_flash"}, "Command is not allowed")
        forbidden = {"--force", "--erase-all", "--erase_all", "--encrypt", "erase_flash", "erase_region"}
        require(not forbidden.intersection(map(str, arguments)), "Unsafe esptool option refused")
        argv = [sys.executable, "-m", "esptool", "--chip", "esp32s3", "--port", self.port,
                "--baud", str(self.baud), "--before", "default_reset", "--after", after,
                "--connect-attempts", "3", command, *map(str, arguments)]
        print(f"esptool {command}...", flush=True)
        result = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                timeout=timeout, text=True, encoding="utf-8", errors="replace")
        log = self.directory / f"{time.time_ns()}-{command}.log"
        log.write_text(result.stdout, encoding="utf-8")
        require(result.returncode == 0, f"esptool {command} failed; private diagnostic log: {log}")
        return result.stdout


def parse_probe(chip, flash, security):
    require(re.search(r"Chip is ESP32-S3\b", chip), "Probe did not confirm ESP32-S3")
    mac = re.search(r"\bMAC:\s*([0-9a-f]{2}(?::[0-9a-f]{2}){5})\b", chip, re.I)
    require(mac is not None, "Probe returned no device MAC")
    size = re.search(r"Detected flash size:\s*(\d+)MB\b", flash)
    require(size is not None and int(size[1]) * 1024 * 1024 == FLASH_SIZE, "Physical flash must be detected as 16MB")
    require(re.search(r"^Secure Boot: Disabled\s*$", security, re.M), "Secure Boot not confirmed disabled")
    require(re.search(r"^Flash Encryption: Disabled\s*$", security, re.M), "Flash encryption not confirmed disabled")
    flags = re.search(r"^Flags:\s*(0x[0-9a-f]+)\b", security, re.M | re.I)
    require(flags is not None and not int(flags[1], 16) & 0x5, "Secure download/boot mode unsupported")
    require("Secure Download Mode" not in chip + flash + security, "Secure download mode unsupported")
    return {"chip": "esp32s3", "mac": mac[1].lower(), "flash_size": FLASH_SIZE,
            "security": {"secure_boot": False, "flash_encryption": False, "flags": int(flags[1], 16)}}


def probe(tool):
    return parse_probe(tool.run("chip_id"), tool.run("flash_id"), tool.run("get_security_info"))


def verify(tool, pairs, after="no_reset"):
    arguments = [value for offset, file in pairs for value in (hex(offset), str(file))]
    result = tool.run("verify_flash", "--diff", "no", *arguments, after=after)
    require(result.count("-- verify OK (digest matched)") == len(pairs), "Flash verification did not confirm every region")


def validate_backup(manifest_path):
    path = local_path(manifest_path, BACKUPS)
    manifest = load_json(path)
    require(manifest.get("schema") == 1 and manifest.get("status") == "verified", "Backup is incomplete or unverified")
    identity = manifest.get("device", {})
    require(identity.get("chip") == "esp32s3" and identity.get("flash_size") == FLASH_SIZE, "Backup device target mismatch")
    require(re.fullmatch(r"[0-9a-f]{2}(?::[0-9a-f]{2}){5}", identity.get("mac", "")) is not None, "Invalid backup MAC")
    security = identity.get("security", {})
    require(security.get("secure_boot") is False and security.get("flash_encryption") is False,
            "Backup security state unsupported")
    require(isinstance(security.get("flags"), int) and not security["flags"] & 0x5, "Backup security flags unsupported")
    backup = child_path(path.parent, manifest.get("file"))
    require(backup.is_file() and backup.stat().st_size == FLASH_SIZE, "Backup must contain the whole physical flash")
    data = backup.read_bytes()
    require(sha256(data) == manifest.get("sha256"), "Full backup SHA256 mismatch")
    require(manifest.get("verified_sha256") == manifest["sha256"], "Backup verification receipt mismatch")
    require(manifest.get("layout") == parse_partition_table(data[0x8000:0x9000]), "Backup partition layout mismatch")
    require(set(manifest.get("preserved", {})) == {"nvs", "phy_init"}, "Unexpected preserved region manifest")
    for name in ("nvs", "phy_init"):
        part = next(item for item in LAYOUT if item["name"] == name)
        record = manifest.get("preserved", {}).get(name, {})
        contents = data[part["offset"]:part["offset"] + part["size"]]
        require(record.get("sha256") == sha256(contents), f"Backup {name} hash mismatch")
        separate = child_path(path.parent, record.get("file"))
        require(separate.is_file() and separate.read_bytes() == contents, f"Backup {name} artifact mismatch")
    return manifest, backup


def backup_device(args):
    port = require_usb_port(args.port)
    directory = BACKUPS / (datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ") + "-backup")
    private_directory(directory)
    tool = EspTool(args.port, args.baud, directory)
    device = probe(tool)
    identified_directory = directory.with_name(directory.name + "-" + device["mac"].replace(":", ""))
    directory.rename(identified_directory)
    directory = identified_directory
    tool.directory = directory
    backup = directory / "flash-16mb.bin"
    tool.run("read_flash", "0", "ALL", backup)
    require(backup.stat().st_size == FLASH_SIZE, "Readback length differs from detected physical flash")
    data = backup.read_bytes()
    layout = parse_partition_table(data[0x8000:0x9000])
    digest = sha256(data)
    manifest = {"schema": 1, "status": "read", "created_utc": utc_now(), "port": port,
                "device": device, "file": backup.name, "sha256": digest, "layout": layout, "preserved": {}}
    for name in ("nvs", "phy_init"):
        part = next(item for item in layout if item["name"] == name)
        contents = data[part["offset"]:part["offset"] + part["size"]]
        file = directory / f"{name}.bin"
        file.write_bytes(contents)
        manifest["preserved"][name] = {"file": file.name, "sha256": sha256(contents)}
    path = directory / "manifest.json"
    save_json(path, manifest)
    verify(tool, [(0, backup)])
    manifest.update(status="verified", verified_sha256=digest, verified_utc=utc_now())
    save_json(path, manifest)
    validate_backup(path)
    print(json.dumps({"backup_manifest": str(path), "device": device, "sha256": digest}, indent=2))


def probe_device(args):
    require_usb_port(args.port)
    directory = BACKUPS / (datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ") + "-probe")
    private_directory(directory)
    device = probe(EspTool(args.port, args.baud, directory))
    save_json(directory / "probe.json", {"created_utc": utc_now(), "device": device})
    print(json.dumps({"device": device, "private_diagnostics": str(directory)}, indent=2))


def flash_device(args):
    plan = validate_build(args.build)
    manifest, backup = validate_backup(args.manifest)
    require_usb_port(args.port)
    # All command logs and the write receipt stay beside the private backup.
    tool = EspTool(args.port, args.baud, backup.parent)
    device = probe(tool)
    require(device == manifest["device"], "Connected device MAC/flash/security differs from verified backup")
    # No firmware write is reached unless the ENTIRE live flash still matches.
    # If normal application activity changed NVS, take a new backup first.
    verify(tool, [(0, backup)])
    # Recheck the artifacts immediately before use; do not forward arbitrary JSON arguments.
    require(plan == validate_build(args.build), "Build changed during preflight")
    require(manifest == validate_backup(args.manifest)[0], "Backup changed during preflight")
    pairs = [(item["offset"], item["file"]) for item in plan["segments"]]
    arguments = [value for offset, file in pairs for value in (hex(offset), file)]
    receipt = {"schema": 1, "started_utc": utc_now(), "status": "writing", "device": device,
               "backup_sha256": manifest["sha256"], "plan": plan}
    receipt_path = backup.parent / f"flash-{time.time_ns()}.json"
    save_json(receipt_path, receipt)
    tool.run("write_flash", *WRITE_ARGS, *arguments)
    verify(tool, pairs)
    preserved = [(next(item["offset"] for item in LAYOUT if item["name"] == name),
                  child_path(backup.parent, record["file"])) for name, record in manifest["preserved"].items()]
    verify(tool, preserved, after="hard_reset")
    receipt.update(status="verified", completed_utc=utc_now(), preserved_verified=["nvs", "phy_init"])
    save_json(receipt_path, receipt)
    print(json.dumps({"flash_receipt": str(receipt_path), "status": "verified", "preserved": ["nvs", "phy_init", "ota_1"]}, indent=2))


def monitor_device(args):
    require_usb_port(args.port)
    require(0 < args.seconds <= 300, "Monitor duration must be 1..300 seconds")
    import serial
    directory = BACKUPS / (datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ") + "-monitor")
    private_directory(directory)
    path = directory / "serial.log"
    # Set line states before opening: monitor should not intentionally reset/enter bootloader.
    stream = serial.Serial(port=None, baudrate=args.baud, timeout=0.25)
    stream.dtr = False
    stream.rts = False
    stream.port = args.port
    total = 0
    started = time.monotonic()
    with stream, path.open("wb") as output:
        while time.monotonic() - started < args.seconds:
            block = stream.read(min(max(stream.in_waiting, 1), 65536))
            total += len(block)
            output.write(block)
    print(json.dumps({"private_log": str(path), "bytes": total, "seconds": args.seconds}, indent=2))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("list", help="Enumerate serial metadata only; never opens ports")
    prepare = commands.add_parser("prepare", help="Offline validation of built dog images; no serial access")
    prepare.add_argument("--build", type=Path, default=BUILD)
    check = commands.add_parser("check-backup", help="Offline manifest/hash/layout validation; no serial access")
    check.add_argument("--manifest", type=Path, required=True)
    for command in ("probe", "backup", "flash", "monitor"):
        sub = commands.add_parser(command)
        sub.add_argument("--port", required=True, help="Explicit enumerated USB serial port")
        sub.add_argument("--baud", type=int, default=115200 if command == "monitor" else 460800)
        if command == "flash":
            sub.add_argument("--manifest", type=Path, required=True)
            sub.add_argument("--build", type=Path, default=BUILD)
        if command == "monitor":
            sub.add_argument("--seconds", type=float, default=30)
    args = parser.parse_args(argv)
    if args.command == "list":
        print(json.dumps(list_ports(), ensure_ascii=False, indent=2))
    elif args.command == "prepare":
        print(json.dumps(validate_build(args.build), indent=2))
    elif args.command == "check-backup":
        manifest, backup = validate_backup(args.manifest)
        print(json.dumps({"status": "verified-offline", "backup": str(backup), "device": manifest["device"], "sha256": manifest["sha256"]}, indent=2))
    else:
        require(0 < args.baud <= 2000000, "Invalid serial baud rate")
        {"probe": probe_device, "backup": backup_device, "flash": flash_device, "monitor": monitor_device}[args.command](args)


if __name__ == "__main__":
    try:
        main()
    except (WorkflowError, OSError, ValueError, subprocess.SubprocessError) as error:
        print(f"Stopped: {error}", file=sys.stderr)
        sys.exit(2)
