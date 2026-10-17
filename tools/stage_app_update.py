#!/usr/bin/env python3
"""Prepare an offline, private app-only deployment snapshot.

The stage uses the newly compiled app/config metadata and the four non-app
artifacts from an immutable prior package. A newly compiled bootloader is
validated but is never selected for writing. This tool never opens a serial
port or accesses a network. Use update_app.py for the single OTA0 write.
"""

from __future__ import annotations

import argparse
import copy
from datetime import datetime, timezone
import json
from pathlib import Path
import sys

import device_workflow as workflow
import update_app


def stage(build, previous_package, output=None):
    build = workflow.local_path(build)
    previous_package = workflow.local_path(previous_package)
    source_plan = workflow.validate_build(build)
    source_segments = update_app.checked_segments(source_plan)
    package_manifest, prior, prior_paths = update_app.validate_package(previous_package)
    for name in ("partition-table", "otadata", "assets"):
        workflow.require(update_app.signature(source_segments[name]) == update_app.signature(prior[name]),
                         f"App-only stage refuses changed {name}")
    workflow.require(source_segments["app"]["sha256"] != prior["app"]["sha256"],
                     "App image is unchanged; no stage needed")

    if output is None:
        output = workflow.BACKUPS / (datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ") + "-app-stage")
    output = workflow.local_path(output, workflow.BACKUPS)
    workflow.require(not output.exists(), "App stage output already exists; refusing to overwrite")
    workflow.private_directory(output)
    (output / "config").mkdir()
    metadata = {"flash_settings": copy.deepcopy(workflow.SETTINGS),
                "write_flash_args": list(workflow.WRITE_ARGS),
                "extra_esptool_args": {"chip": "esp32s3"}, "flash_files": {},
                "deployment_kind": "app-only-update-stage"}
    files = []
    for name, (offset, _) in workflow.SEGMENTS.items():
        chosen = source_segments[name] if name == "app" else prior[name]
        source = Path(chosen["file"]) if name == "app" else prior_paths[name]
        data = source.read_bytes()
        workflow.require(len(data) == chosen["size"] and workflow.sha256(data) == chosen["sha256"],
                         f"{name} changed while preparing app stage")
        filename = update_app.PACKAGE_FILES[name]
        (output / filename).write_bytes(data)
        metadata[name] = {"offset": hex(offset), "file": filename, "encrypted": "false"}
        metadata["flash_files"][hex(offset)] = filename
        files.append({"file": filename, "offset": offset, "size": len(data),
                      "sha256": workflow.sha256(data)})
    for filename in ("config/sdkconfig.json", "project_description.json"):
        (output / filename).write_bytes((build / filename).read_bytes())
    workflow.save_json(output / "flasher_args.json", metadata)

    plan = workflow.validate_build(output)
    staged = update_app.checked_segments(plan)
    for name in workflow.SEGMENTS:
        expected = source_segments[name] if name == "app" else prior[name]
        workflow.require(update_app.signature(staged[name]) == update_app.signature(expected),
                         f"Staged {name} differs from chosen app-only artifact")
    workflow.require(source_plan == workflow.validate_build(build), "Source build changed during staging")
    workflow.require(package_manifest == update_app.validate_package(previous_package)[0],
                     "Previous package changed during staging")
    manifest = {"schema": 1, "kind": "app-only-update-stage", "status": "prepared_not_flashed",
                "firmware_profile": update_app.FIRMWARE_PROFILE,
                "created_utc": workflow.utc_now(), "previous_package": str(previous_package),
                "source_build": str(build), "plan": plan, "files": files,
                "write": update_app.signature(staged["app"]),
                "allowed_write_segments": ["app"],
                "retained_non_app_segments": [name for name in workflow.SEGMENTS if name != "app"],
                "source_build_plan": source_plan,
                "source_build_bootloader_selected_for_write": False}
    workflow.save_json(output / "manifest.json", manifest)
    (output / "README.txt").write_text(
        "Offline app-only deployment stage; no device has been opened or flashed.\n"
        "Only app.bin at 0x20000 is eligible for writing with update_app.py.\n"
        "The other four artifacts describe the immutable retained deployment.\n"
        "The newly built bootloader is not part of the selected write.\n"
        "flasher_args.json has five artifacts for offline validate_build compatibility;\n"
        "do not use device_workflow.py flash or a generic five-segment flash command.\n"
        "No source archive is created or replaced by this staging operation.\n", encoding="utf-8")
    # Exercise the same package checks that the following app-only step uses.
    update_app.validate_package(output, staged)
    return output, manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--previous-package", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    output, manifest = stage(args.build, args.previous_package, args.output)
    print(json.dumps({"stage": str(output), "kind": manifest["kind"],
                      "firmware_profile": manifest["firmware_profile"],
                      "status": manifest["status"], "write": manifest["write"],
                      "retained_non_app_segments": manifest["retained_non_app_segments"]}, indent=2))


if __name__ == "__main__":
    try:
        main()
    except (workflow.WorkflowError, OSError, ValueError) as exc:
        print(f"Stopped: {exc}", file=sys.stderr)
        sys.exit(2)
