#!/usr/bin/env python3
"""Write the board's original assets image through one guarded write.

Stage is offline. Write verifies the deployed package and original OTA1, captures
current settings, and writes only the assets image at 0xc20000. It never erases
the device or changes application, bootloader, partition table, or OTA selection.
"""

from __future__ import annotations

import argparse
import copy
from datetime import datetime, timezone
import json
from pathlib import Path
import subprocess
import sys

import device_workflow as workflow
import update_app


DOG_ASSETS = workflow.ROOT / "main/boards/zfsn-esp32s3-dog/assets/dog_assets.bin"
DOG_ASSETS_SHA256 = "b3e37000e6eef19864e32dccfb7f7be286f1901a1ca4a154870d77be1d4e3d9e"
DOG_ASSETS_SIZE = 624371
ASSETS_OFFSET = 0xC20000


def dog_assets():
    data = workflow.local_path(DOG_ASSETS).read_bytes()
    workflow.require(len(data) == DOG_ASSETS_SIZE and workflow.sha256(data) == DOG_ASSETS_SHA256,
                     "Original assets SHA256/size mismatch")
    return data


def previous_inputs(manifest_path, receipt_path, package):
    manifest, backup = workflow.validate_backup(manifest_path)
    receipt = workflow.load_json(workflow.local_path(receipt_path, workflow.BACKUPS))
    prior = update_app.checked_receipt(receipt, manifest)
    package_manifest, _, paths = update_app.validate_package(package, prior)
    workflow.require(workflow.validate_build(package) == package_manifest["plan"],
                     "Previous package build metadata differs from its plan")
    return manifest, backup, receipt, prior, paths, package_manifest


def stage(manifest_path, receipt_path, package, output=None):
    data = dog_assets()
    package = workflow.local_path(package)
    _, _, _, prior, paths, package_manifest = previous_inputs(manifest_path, receipt_path, package)
    workflow.require(prior["assets"]["sha256"] != DOG_ASSETS_SHA256,
                     "Verified deployment already has the original assets")
    if output is None:
        output = workflow.BACKUPS / (datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ") + "-assets-stage")
    output = workflow.local_path(output, workflow.BACKUPS)
    workflow.require(not output.exists(), "Assets stage already exists; refusing to overwrite")
    workflow.private_directory(output)
    (output / "config").mkdir()
    metadata = {"flash_settings": copy.deepcopy(workflow.SETTINGS),
                "write_flash_args": list(workflow.WRITE_ARGS),
                "extra_esptool_args": {"chip": "esp32s3"}, "flash_files": {},
                "deployment_kind": "assets-only-update-stage"}
    files = []
    for name, (offset, _) in workflow.SEGMENTS.items():
        contents = data if name == "assets" else paths[name].read_bytes()
        if name != "assets":
            workflow.require(len(contents) == prior[name]["size"] and
                             workflow.sha256(contents) == prior[name]["sha256"],
                             f"Prior {name} changed during assets staging")
        filename = update_app.PACKAGE_FILES[name]
        (output / filename).write_bytes(contents)
        metadata[name] = {"offset": hex(offset), "file": filename, "encrypted": "false"}
        metadata["flash_files"][hex(offset)] = filename
        files.append({"file": filename, "offset": offset, "size": len(contents),
                      "sha256": workflow.sha256(contents)})
    for filename in ("config/sdkconfig.json", "project_description.json"):
        (output / filename).write_bytes((package / filename).read_bytes())
    workflow.save_json(output / "flasher_args.json", metadata)
    plan = workflow.validate_build(output)
    staged = update_app.checked_segments(plan)
    for name in workflow.SEGMENTS:
        if name != "assets":
            workflow.require(update_app.signature(staged[name]) == update_app.signature(prior[name]),
                             f"Assets stage changed {name}")
    workflow.require(package_manifest == update_app.validate_package(package, prior)[0],
                     "Previous package changed during assets staging")
    workflow.require(dog_assets() == data, "Assets source changed during staging")
    manifest = {"schema": 1, "kind": "assets-only-update-stage", "status": "prepared_not_flashed",
                "firmware_profile": "production", "created_utc": workflow.utc_now(),
                "previous_package": str(package),
                "previous_receipt": str(Path(receipt_path).resolve()),
                "source_assets": str(DOG_ASSETS), "purpose": "write-original-assets",
                "plan": plan, "files": files, "write": update_app.signature(staged["assets"]),
                "allowed_write_segments": ["assets"],
                "retained_non_assets_segments": [name for name in workflow.SEGMENTS if name != "assets"]}
    workflow.save_json(output / "manifest.json", manifest)
    (output / "README.txt").write_text(
        "Original dog assets write stage; preparation is offline.\n"
        "Only assets.bin at 0xc20000 may be written using write_assets.py write.\n"
        "Other files describe the unchanged deployment. Do not flash all five artifacts.\n",
        encoding="utf-8")
    update_app.validate_package(output, staged)
    return output, manifest


def validate_inputs(args):
    manifest, backup, receipt, prior, paths, package_manifest = previous_inputs(
        args.manifest, args.previous_receipt, args.previous_package)
    stage_manifest, current, _ = update_app.validate_package(args.build)
    plan = workflow.validate_build(args.build)
    workflow.require(stage_manifest["plan"] == plan, "Assets stage metadata differs from plan")
    workflow.require(stage_manifest.get("allowed_write_segments") == ["assets"],
                     "Assets stage does not restrict writes to assets")
    for name in workflow.SEGMENTS:
        if name != "assets":
            workflow.require(update_app.signature(current[name]) == update_app.signature(prior[name]),
                             f"Assets write refuses changed {name}")
    workflow.require(current["assets"]["offset"] == ASSETS_OFFSET and
                     current["assets"]["size"] == DOG_ASSETS_SIZE and
                     current["assets"]["sha256"] == DOG_ASSETS_SHA256 and
                     Path(current["assets"]["file"]).read_bytes() == dog_assets(),
                     "Write payload is not the original assets")
    workflow.require(prior["assets"]["sha256"] != DOG_ASSETS_SHA256,
                     "Deployed assets already match the original image")
    return manifest, backup, receipt, prior, paths, plan, current, package_manifest, stage_manifest


def write(args):
    inputs = validate_inputs(args)
    manifest, backup, previous_receipt, prior, paths, plan, current, package_manifest, stage_manifest = inputs
    workflow.require_usb_port(args.port)
    directory = workflow.BACKUPS / (datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ") + "-assets-write")
    workflow.private_directory(directory)
    tool = workflow.EspTool(args.port, args.baud, directory)
    device = workflow.probe(tool)
    workflow.require(device == manifest["device"], "Connected MAC/flash/security differs from original backup")
    payload = directory / "new-assets.bin"
    payload.write_bytes(dog_assets())
    references = []
    for name in ("bootloader", "partition-table", "app", "assets"):
        reference = directory / f"previous-{name}.bin"
        data = paths[name].read_bytes()
        workflow.require(len(data) == prior[name]["size"] and workflow.sha256(data) == prior[name]["sha256"],
                         f"Prior {name} changed during device preflight")
        reference.write_bytes(data)
        references.append((prior[name]["offset"], reference))
    ota1 = next(item for item in workflow.LAYOUT if item["name"] == "ota_1")
    with backup.open("rb") as stream:
        stream.seek(ota1["offset"])
        original_ota1 = stream.read(ota1["size"])
    workflow.require(len(original_ota1) == ota1["size"], "Original OTA1 backup is truncated")
    ota1_reference = directory / "original-ota_1.bin"
    ota1_reference.write_bytes(original_ota1)
    workflow.verify(tool, references + [(ota1["offset"], ota1_reference)])
    preserved = {}
    for name in ("nvs", "phy_init", "otadata"):
        region = next(item for item in workflow.LAYOUT if item["name"] == name)
        path = update_app.read_region(tool, directory, name, region["offset"], region["size"])
        preserved[name] = {"offset": region["offset"], "size": region["size"],
                           "file": path.name, "sha256": workflow.sha256(path.read_bytes())}
    selection = update_app.validate_otadata((directory / preserved["otadata"]["file"]).read_bytes())
    preserved_pairs = [(record["offset"], directory / record["file"]) for record in preserved.values()]
    workflow.verify(tool, preserved_pairs)
    workflow.require(inputs == validate_inputs(args), "Write inputs changed during device preflight")
    receipt = {"schema": 1, "kind": "assets-only-update", "status": "writing",
               "firmware_profile": "production", "purpose": "write-original-assets",
               "started_utc": workflow.utc_now(), "device": device,
               "original_backup_manifest": str(Path(args.manifest).resolve()),
               "backup_sha256": manifest["sha256"],
               "previous_receipt": str(Path(args.previous_receipt).resolve()),
               "previous_package": str(Path(args.previous_package).resolve()),
               "package": str(Path(args.build).resolve()), "plan": plan,
               "previous_assets_sha256": prior["assets"]["sha256"],
               "write": {**update_app.signature(current["assets"]), "file": payload.name},
               "ota_selection": selection, "preserved": preserved,
               "verified_unchanged_ota1_sha256": workflow.sha256(original_ota1)}
    receipt_path = directory / "receipt.json"
    workflow.save_json(receipt_path, receipt)
    workflow.validate_ranges([current["assets"]])
    # Check every local verification snapshot again before the only flash write.
    workflow.require(payload.read_bytes() == dog_assets(), "Assets snapshot changed before write")
    for name in ("bootloader", "partition-table", "app", "assets"):
        workflow.require(workflow.sha256((directory / f"previous-{name}.bin").read_bytes()) == prior[name]["sha256"],
                         f"Prior {name} snapshot changed before write")
    workflow.require(ota1_reference.read_bytes() == original_ota1, "OTA1 snapshot changed before write")
    for record in preserved.values():
        data = (directory / record["file"]).read_bytes()
        workflow.require(len(data) == record["size"] and workflow.sha256(data) == record["sha256"],
                         "Preserved snapshot changed before write")
    tool.run("write_flash", *workflow.WRITE_ARGS, "0xc20000", payload)
    workflow.verify(tool, [(ASSETS_OFFSET, payload)])
    unchanged_pairs = [pair for pair in references if pair[0] != ASSETS_OFFSET]
    workflow.verify(tool, unchanged_pairs + [(ota1["offset"], ota1_reference)])
    workflow.verify(tool, preserved_pairs, after="hard_reset")
    receipt.update(status="verified", completed_utc=workflow.utc_now(),
                   preserved_verified=["nvs", "phy_init", "otadata"],
                   untouched_regions_verified=["bootloader", "partition-table", "app", "ota_1"])
    update_app.checked_receipt(receipt, manifest)
    workflow.save_json(receipt_path, receipt)
    print(json.dumps({"status": "verified", "receipt": str(receipt_path),
                      "package": str(Path(args.build).resolve()), "assets_sha256": DOG_ASSETS_SHA256,
                      "write_offset": "0xc20000"}, indent=2))
    return receipt_path, receipt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    for command in ("stage", "write"):
        sub = commands.add_parser(command)
        sub.add_argument("--manifest", type=Path, required=True)
        sub.add_argument("--previous-receipt", type=Path, required=True)
        sub.add_argument("--previous-package", type=Path, required=True)
        if command == "stage":
            sub.add_argument("--output", type=Path)
        else:
            sub.add_argument("--build", type=Path, required=True)
            sub.add_argument("--port", required=True)
            sub.add_argument("--baud", type=int, default=460800)
    args = parser.parse_args()
    if args.command == "stage":
        output, manifest = stage(args.manifest, args.previous_receipt, args.previous_package, args.output)
        print(json.dumps({"stage": str(output), "status": manifest["status"], "write": manifest["write"]}, indent=2))
    else:
        workflow.require(0 < args.baud <= 2000000, "Invalid baud rate")
        write(args)


if __name__ == "__main__":
    try:
        main()
    except (workflow.WorkflowError, OSError, ValueError, subprocess.SubprocessError) as exc:
        print(f"Stopped: {exc}", file=sys.stderr)
        sys.exit(2)
