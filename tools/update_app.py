#!/usr/bin/env python3
"""Guarded app-only update following this project's verified five-part flash.

Checks the original full backup, deployment receipt, immutable prior package,
current OTA0 selection, unchanged OTA1 and deployed image digests before writing
only 0x20000. Current NVS/PHY/otadata are captured and verified unchanged.
"""

from __future__ import annotations

import argparse
import binascii
from datetime import datetime, timezone
import json
from pathlib import Path
import struct
import subprocess
import sys

import device_workflow as workflow


PACKAGE_FILES = {"bootloader": "bootloader.bin", "partition-table": "partition-table.bin",
                 "otadata": "otadata.bin", "app": "app.bin", "assets": "assets.bin"}
FIRMWARE_PROFILE = "production"


def signature(segment):
    return {key: segment.get(key) for key in ("name", "offset", "size", "sha256")}


def checked_segments(plan):
    workflow.require(isinstance(plan, dict) and plan.get("chip") == "esp32s3"
                     and plan.get("settings") == workflow.SETTINGS
                     and plan.get("layout") == workflow.LAYOUT,
                     "Prior deployment plan target/settings/layout mismatch")
    segments = plan.get("segments", [])
    workflow.require(isinstance(segments, list) and len(segments) == 5,
                     "Prior deployment needs exactly five segments")
    by_name = {item.get("name"): item for item in segments}
    workflow.require(set(by_name) == set(workflow.SEGMENTS), "Prior deployment segment names mismatch")
    for name, (offset, capacity) in workflow.SEGMENTS.items():
        segment = by_name[name]
        workflow.require(segment.get("offset") == offset and isinstance(segment.get("size"), int)
                         and 0 < segment["size"] <= capacity, f"Invalid prior {name} range")
        workflow.require(isinstance(segment.get("sha256"), str) and len(segment["sha256"]) == 64,
                         f"Missing prior {name} digest")
    workflow.validate_ranges(segments)
    return by_name


def checked_receipt(receipt, manifest):
    """Accept the original flash or an explicitly verified single-segment step.

    A typed app-only receipt proves a different preservation boundary from the
    original five-segment flash. Do not treat arbitrary receipt kinds or a
    partial set of successful verifications as interchangeable.
    """
    workflow.require(receipt.get("schema") == 1 and receipt.get("status") == "verified",
                     "Previous flash receipt is not verified")
    workflow.require(receipt.get("device") == manifest["device"]
                     and receipt.get("backup_sha256") == manifest["sha256"],
                     "Previous deployment does not belong to this original backup/device")
    prior = checked_segments(receipt.get("plan"))
    kind = receipt.get("kind")
    if kind is None:
        workflow.require(set(receipt.get("preserved_verified", [])) == {"nvs", "phy_init"},
                         "Previous deployment lacks NVS/PHY verification")
    else:
        written_names = {"app-only-update": "app", "assets-only-update": "assets"}
        workflow.require(isinstance(kind, str) and kind in written_names,
                         "Unsupported previous deployment receipt kind")
        written_name = written_names[kind]
        preserved = receipt.get("preserved_verified", [])
        untouched = receipt.get("untouched_regions_verified", [])
        workflow.require(isinstance(preserved, list) and len(preserved) == 3 and
                         all(isinstance(name, str) for name in preserved) and
                         set(preserved) == {"nvs", "phy_init", "otadata"},
                         "Previous single-segment update lacks NVS/PHY/OTA-data verification")
        expected_untouched = {"bootloader", "partition-table", "ota_1",
                              "assets" if written_name == "app" else "app"}
        workflow.require(isinstance(untouched, list) and len(untouched) == 4 and
                         all(isinstance(name, str) for name in untouched) and
                         set(untouched) == expected_untouched,
                         "Previous single-segment update lacks complete untouched-region verification")
        workflow.require(isinstance(receipt.get("write"), dict) and
                         signature(receipt["write"]) == signature(prior[written_name]),
                         f"Previous {written_name}-only write does not match its {written_name} plan")
        workflow.require(receipt.get("ota_selection") in (
            {"active_slot": "ota_0", "state": "initial", "sequence": None},
            {"active_slot": "ota_0", "state": "valid", "sequence": 1}),
            "Previous single-segment receipt does not prove the initial OTA0 selection")
    return prior


def validate_package(package, expected=None):
    """Validate all immutable package artifacts, with an optional deployed plan."""
    package = workflow.local_path(package)
    package_manifest = workflow.load_json(package / "manifest.json")
    package_segments = checked_segments(package_manifest.get("plan"))
    if expected is not None:
        workflow.require({name: signature(item) for name, item in package_segments.items()} ==
                         {name: signature(item) for name, item in expected.items()},
                         "Previous package differs from verified deployed receipt")
    files = package_manifest.get("files", [])
    workflow.require(isinstance(files, list) and len(files) == 5, "Previous package needs five file records")
    file_records = {item.get("file"): item for item in files}
    workflow.require(set(file_records) == set(PACKAGE_FILES.values()), "Previous package file list mismatch")
    paths = {}
    for name, filename in PACKAGE_FILES.items():
        path = workflow.child_path(package, filename)
        data = path.read_bytes()
        segment = package_segments[name]
        record = file_records[filename]
        workflow.require(len(data) == segment["size"] and workflow.sha256(data) == segment["sha256"],
                         f"Prior package {name} content differs from deployed receipt")
        workflow.require(all(record.get(key) == segment[key] for key in ("offset", "size", "sha256")),
                         f"Prior package {name} file record mismatch")
        if name in ("bootloader", "app"):
            workflow.validate_image_header(data, name)
        elif name == "partition-table":
            workflow.parse_partition_table(data)
        elif name == "otadata":
            workflow.require(data == b"\xff" * 0x2000, "Prior deployment must have initial OTA0 selection")
        paths[name] = path
    return package_manifest, package_segments, paths


def validate_inputs(args):
    # Fail before original-backup validation or any serial/device operations.
    manifest, backup = workflow.validate_backup(args.manifest)
    receipt_path = workflow.local_path(args.previous_receipt, workflow.BACKUPS)
    receipt = workflow.load_json(receipt_path)
    prior = checked_receipt(receipt, manifest)
    _, _, paths = validate_package(args.previous_package, prior)
    plan = workflow.validate_build(args.build)
    current = checked_segments(plan)
    for name in workflow.SEGMENTS:
        if name != "app":
            workflow.require(signature(current[name]) == signature(prior[name]),
                             f"App-only update refuses changed {name}")
    reflash_unchanged = getattr(args, "reflash_unchanged", False)
    workflow.require(isinstance(reflash_unchanged, bool), "reflash_unchanged must be a boolean")
    workflow.require(current["app"]["sha256"] != prior["app"]["sha256"] or reflash_unchanged,
                     "App image is unchanged; use --reflash-unchanged only for an intentional reflash")
    return manifest, backup, receipt, prior, paths, plan, current


def validate_otadata(data):
    """Accept only this deployment's initial OTA0 state or its first-boot receipt.

    ESP-IDF bootloader_utility.c:set_actual_ota_seq sets seq=1/state=VALID
    in sector0 when the flashed all-FF OTA data first boots OTA0. Any later
    OTA selection, including a return from OTA1, is conservatively refused.
    CRC convention matches IDF components/app_update/otatool.py.
    """
    workflow.require(len(data) == 0x2000, "Current OTA data is truncated")
    if data == b"\xff" * 0x2000:
        return {"active_slot": "ota_0", "state": "initial", "sequence": None}
    crc = binascii.crc32(struct.pack("<I", 1), 0xFFFFFFFF) & 0xFFFFFFFF
    expected = struct.pack("<I20sII", 1, b"\xff" * 20, 2, crc).ljust(0x2000, b"\xff")
    workflow.require(data == expected,
                     "OTA selection differs from initial verified OTA0 deployment; refusing to guess")
    return {"active_slot": "ota_0", "state": "valid", "sequence": 1}


def read_region(tool, directory, name, offset, size):
    path = directory / f"current-{name}.bin"
    tool.run("read_flash", hex(offset), hex(size), path)
    workflow.require(path.is_file() and path.stat().st_size == size, f"Current {name} readback length mismatch")
    return path


def update(args):
    manifest, backup, previous_receipt, prior, paths, plan, current = validate_inputs(args)
    workflow.require_usb_port(args.port)
    directory = workflow.BACKUPS / (datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ") + "-app-update")
    workflow.private_directory(directory)
    tool = workflow.EspTool(args.port, args.baud, directory)
    device = workflow.probe(tool)
    workflow.require(device == manifest["device"], "Connected MAC/flash/security differs from original backup")

    # Snapshots make the actual write independent of build output replacement.
    new_app = directory / "new-app.bin"
    new_app.write_bytes(Path(current["app"]["file"]).read_bytes())
    workflow.require(workflow.sha256(new_app.read_bytes()) == current["app"]["sha256"],
                     "New app changed during validation")
    references = []
    for name in ("bootloader", "partition-table", "app", "assets"):
        reference = directory / f"previous-{name}.bin"
        reference.write_bytes(paths[name].read_bytes())
        workflow.require(workflow.sha256(reference.read_bytes()) == prior[name]["sha256"],
                         f"Prior {name} changed during validation")
        references.append((prior[name]["offset"], reference))
    ota1 = next(item for item in workflow.LAYOUT if item["name"] == "ota_1")
    ota1_reference = directory / "original-ota_1.bin"
    with backup.open("rb") as stream:
        stream.seek(ota1["offset"])
        original_ota1 = stream.read(ota1["size"])
    workflow.require(len(original_ota1) == ota1["size"], "Original OTA1 backup truncated")
    ota1_reference.write_bytes(original_ota1)
    # Device-side MD5 checks avoid reading the entire flash over USB again.
    workflow.verify(tool, references + [(ota1["offset"], ota1_reference)])

    preserved = {}
    for name in ("nvs", "phy_init", "otadata"):
        region = next(item for item in workflow.LAYOUT if item["name"] == name)
        path = read_region(tool, directory, name, region["offset"], region["size"])
        preserved[name] = {"offset": region["offset"], "size": region["size"],
                           "file": path.name, "sha256": workflow.sha256(path.read_bytes())}
    selection = validate_otadata((directory / preserved["otadata"]["file"]).read_bytes())
    workflow.verify(tool, [(record["offset"], directory / record["file"]) for record in preserved.values()])
    workflow.require(plan == workflow.validate_build(args.build), "Build changed during device preflight")
    workflow.require(manifest == workflow.validate_backup(args.manifest)[0], "Original backup changed during preflight")
    workflow.require(previous_receipt == workflow.load_json(workflow.local_path(args.previous_receipt, workflow.BACKUPS)),
                     "Previous receipt changed during device preflight")
    receipt = {"schema": 1, "kind": "app-only-update", "status": "writing",
               "firmware_profile": FIRMWARE_PROFILE,
               "started_utc": workflow.utc_now(), "device": device,
               "original_backup_manifest": str(Path(args.manifest).resolve()),
               "backup_sha256": manifest["sha256"],
               "previous_receipt": str(Path(args.previous_receipt).resolve()),
               "previous_package": str(Path(args.previous_package).resolve()),
               "previous_app_sha256": prior["app"]["sha256"], "plan": plan,
               "reflash_unchanged": getattr(args, "reflash_unchanged", False),
               "app_unchanged": current["app"]["sha256"] == prior["app"]["sha256"],
               "write": {**signature(current["app"]), "file": new_app.name},
               "ota_selection": selection, "preserved": preserved,
               "verified_unchanged_ota1_sha256": workflow.sha256(original_ota1)}
    receipt_path = directory / "receipt.json"
    workflow.save_json(receipt_path, receipt)
    workflow.validate_ranges([current["app"]])
    workflow.require(workflow.sha256(new_app.read_bytes()) == current["app"]["sha256"], "App snapshot changed before write")
    # Sole write command: the application at the fixed OTA0 offset.
    tool.run("write_flash", *workflow.WRITE_ARGS, "0x20000", new_app)
    workflow.verify(tool, [(0x20000, new_app)])
    # Prove current settings and OTA selection survived, then boot the app.
    workflow.verify(tool, [(record["offset"], directory / record["file"]) for record in preserved.values()],
                    after="hard_reset")
    receipt.update(status="verified", completed_utc=workflow.utc_now(),
                   preserved_verified=["nvs", "phy_init", "otadata"],
                   untouched_regions_verified=["bootloader", "partition-table", "assets", "ota_1"])
    workflow.save_json(receipt_path, receipt)
    print(json.dumps({"status": "verified", "app_sha256": current["app"]["sha256"],
                      "firmware_profile": FIRMWARE_PROFILE,
                      "receipt": str(receipt_path), "write_offset": "0x20000",
                      "preserved": ["nvs", "phy_init", "otadata", "ota_1", "bootloader", "partition-table", "assets"]}, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--previous-receipt", type=Path, required=True)
    parser.add_argument("--previous-package", type=Path, required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--baud", type=int, default=460800)
    parser.add_argument("--reflash-unchanged", action="store_true",
                        help="Intentionally rewrite an identical app; all identity and preservation guards still apply")
    args = parser.parse_args()
    workflow.require(0 < args.baud <= 2000000, "Invalid baud rate")
    update(args)


if __name__ == "__main__":
    try:
        main()
    except (workflow.WorkflowError, OSError, ValueError, subprocess.SubprocessError) as exc:
        print(f"Stopped: {exc}", file=sys.stderr)
        sys.exit(2)
