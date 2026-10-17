"""Offline OTA selection and guarded app-only write orchestration checks."""

import binascii
import copy
import hashlib
import json
from pathlib import Path
import struct
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import device_workflow as workflow
import update_app as updater
import stage_app_update as stager
from test_device_workflow import image_blob, partition_blob


def selected_ota(seq=1, state=2):
    crc = binascii.crc32(struct.pack("<I", seq), 0xFFFFFFFF) & 0xFFFFFFFF
    return struct.pack("<I20sII", seq, b"\xff" * 20, state, crc).ljust(0x2000, b"\xff")


class OtaSelectionTest(unittest.TestCase):
    def test_initial_and_bootloader_initialized_ota0(self):
        self.assertEqual(updater.validate_otadata(b"\xff" * 0x2000)["active_slot"], "ota_0")
        self.assertEqual(updater.validate_otadata(selected_ota())["state"], "valid")

    def test_ota1_or_later_ota0_or_unverified_state_is_refused(self):
        for seq, state in ((2, 2), (3, 2), (1, 0), (1, 1), (1, 3), (1, 4), (1, 0xFFFFFFFF)):
            with self.subTest(seq=seq, state=state), self.assertRaises(workflow.WorkflowError):
                updater.validate_otadata(selected_ota(seq, state))

    def test_corrupt_crc_second_entry_or_truncation_is_refused(self):
        crc = bytearray(selected_ota()); crc[28] ^= 1
        second = bytearray(selected_ota()); second[0x1000:0x1020] = selected_ota(2)[:32]
        for value in (bytes(crc), bytes(second), b"\xff" * 32):
            with self.subTest(value=value[:32]), self.assertRaises(workflow.WorkflowError):
                updater.validate_otadata(value)


class WriteGuardTest(unittest.TestCase):
    def run_update(self, *, identity_wrong=False, prior_verify_fails=False, ota=None):
        with tempfile.TemporaryDirectory(dir=Path(__file__).parent) as temporary:
            root = Path(temporary)
            (root / "config").mkdir()
            workflow.save_json(root / "config/sdkconfig.json", {})
            old = root / "old.bin"; old.write_bytes(b"old app")
            new = root / "new.bin"; new.write_bytes(b"new app")
            backup = root / "backup.bin"; backup.write_bytes(b"\xff" * workflow.FLASH_SIZE)
            receipt_file = root / "previous.json"; receipt_file.write_text("{}")
            device = {"chip": "esp32s3", "mac": "00:11:22:33:44:55", "flash_size": workflow.FLASH_SIZE,
                      "security": {"secure_boot": False, "flash_encryption": False, "flags": 0}}
            manifest = {"device": device, "sha256": "backupsha"}
            prior = {name: {"name": name, "offset": offset, "size": 7,
                            "sha256": workflow.sha256(old.read_bytes()), "file": str(old)}
                     for name, (offset, capacity) in workflow.SEGMENTS.items()}
            current = {**prior, "app": {"name": "app", "offset": 0x20000, "size": 7,
                                       "sha256": workflow.sha256(new.read_bytes()), "file": str(new)}}
            plan = {"segments": list(current.values())}
            inputs = manifest, backup, {}, prior, {name: old for name in prior}, plan, current
            args = SimpleNamespace(manifest=root / "manifest.json", previous_receipt=receipt_file,
                                   previous_package=root, build=root, port="COM9", baud=460800)
            class Tool:
                def __init__(self): self.calls = []
                def run(self, command, *values, **kwargs):
                    self.calls.append((command, values, kwargs))
                    if command == "read_flash":
                        path = Path(values[-1]); size = int(values[1], 0)
                        path.write_bytes((selected_ota() if ota is None else ota) if "otadata" in path.name else b"x" * size)
                    return ""
            tool = Tool()
            observed = dict(device)
            if identity_wrong: observed["mac"] = "ff:ee:dd:cc:bb:aa"
            with patch.object(updater, "validate_inputs", return_value=inputs), \
                 patch.object(workflow, "require_usb_port"), \
                 patch.object(workflow, "BACKUPS", root), \
                 patch.object(workflow, "private_directory", side_effect=lambda directory: directory.mkdir()), \
                 patch.object(workflow, "EspTool", return_value=tool), \
                 patch.object(workflow, "probe", return_value=observed), \
                 patch.object(workflow, "verify", side_effect=workflow.WorkflowError("digest mismatch") if prior_verify_fails else None) as verifier, \
                 patch.object(workflow, "validate_build", return_value=plan), \
                 patch.object(workflow, "validate_backup", return_value=(manifest, backup)), \
                 patch.object(workflow, "load_json", return_value={}), patch("builtins.print"):
                error = None
                try: updater.update(args)
                except workflow.WorkflowError as exc: error = exc
                writes = [call for call in tool.calls if call[0] == "write_flash"]
                return error, writes, verifier.call_args_list

    def test_wrong_mac_prevents_write(self):
        error, writes, verifies = self.run_update(identity_wrong=True)
        self.assertIsNotNone(error); self.assertEqual(writes, [])

    def test_changed_live_candidate_prevents_write(self):
        error, writes, verifies = self.run_update(prior_verify_fails=True)
        self.assertIsNotNone(error); self.assertEqual(writes, [])

    def test_cloud_ota_slot_prevents_write(self):
        error, writes, verifies = self.run_update(ota=selected_ota(2))
        self.assertIsNotNone(error); self.assertEqual(writes, [])

    def test_only_fixed_app_written_and_preservation_verified_before_reset(self):
        error, writes, verifies = self.run_update()
        self.assertIsNone(error)
        self.assertEqual(len(writes), 1)
        self.assertEqual(list(writes[0][1][:-2]), workflow.WRITE_ARGS)
        self.assertEqual(writes[0][1][-2], "0x20000")
        self.assertEqual(verifies[-1].kwargs, {"after": "hard_reset"})
        self.assertEqual({offset for offset, path in verifies[-1].args[1]}, {0x9000, 0xD000, 0xF000})
        self.assertEqual(verifies[0].args[1][-1][0], 0x620000)


def typed_receipt(manifest, plan, kind="app-only-update"):
    written_name = "assets" if kind == "assets-only-update" else "app"
    written = next(item for item in plan["segments"] if item["name"] == written_name)
    untouched_name = "app" if written_name == "assets" else "assets"
    return {"schema": 1, "kind": kind, "status": "verified",
            "device": copy.deepcopy(manifest["device"]), "backup_sha256": manifest["sha256"],
            "plan": copy.deepcopy(plan), "write": updater.signature(written),
            "preserved_verified": ["nvs", "phy_init", "otadata"],
            "untouched_regions_verified": ["bootloader", "partition-table", untouched_name, "ota_1"],
            "ota_selection": {"active_slot": "ota_0", "state": "valid", "sequence": 1}}


class ReceiptContractTest(unittest.TestCase):
    def setUp(self):
        self.manifest = {"device": {"mac": "00:11:22:33:44:55"}, "sha256": "b" * 64}
        self.plan = {"chip": "esp32s3", "settings": workflow.SETTINGS, "layout": workflow.LAYOUT,
                     "segments": [{"name": name, "offset": offset, "size": 64,
                                   "sha256": "a" * 64} for name, (offset, _) in workflow.SEGMENTS.items()]}
        self.receipt = typed_receipt(self.manifest, self.plan)

    def test_typed_app_only_receipt_is_accepted(self):
        self.assertEqual(set(updater.checked_receipt(self.receipt, self.manifest)), set(workflow.SEGMENTS))

    def test_typed_assets_only_receipt_is_accepted(self):
        receipt = typed_receipt(self.manifest, self.plan, "assets-only-update")
        self.assertEqual(set(updater.checked_receipt(receipt, self.manifest)), set(workflow.SEGMENTS))

    def test_assets_receipt_requires_exact_preservation_and_untouched_regions(self):
        original = typed_receipt(self.manifest, self.plan, "assets-only-update")
        for field in ("preserved_verified", "untouched_regions_verified"):
            values = original[field]
            for invalid in (None, values[:-1], values + [values[0]], values[:-1] + ["unknown"]):
                receipt = copy.deepcopy(original); receipt[field] = invalid
                with self.subTest(field=field, invalid=invalid), self.assertRaises(workflow.WorkflowError):
                    updater.checked_receipt(receipt, self.manifest)

    def test_assets_receipt_requires_written_assets_and_initial_ota0(self):
        original = typed_receipt(self.manifest, self.plan, "assets-only-update")
        app = next(item for item in self.plan["segments"] if item["name"] == "app")
        assets = next(item for item in self.plan["segments"] if item["name"] == "assets")
        for change in ({"write": None}, {"write": updater.signature(app)},
                       {"write": dict(updater.signature(assets), sha256="c" * 64)},
                       {"write": dict(updater.signature(assets), offset=0x20000)},
                       {"ota_selection": {"active_slot": "ota_1", "state": "valid", "sequence": 2}},
                       {"ota_selection": {"active_slot": "ota_0", "state": "valid", "sequence": 3}}):
            with self.subTest(change=change), self.assertRaises(workflow.WorkflowError):
                updater.checked_receipt(dict(original, **change), self.manifest)

    def test_original_untyped_five_segment_receipt_is_accepted(self):
        del self.receipt["kind"]
        self.receipt["preserved_verified"] = ["nvs", "phy_init"]
        self.assertEqual(set(updater.checked_receipt(self.receipt, self.manifest)), set(workflow.SEGMENTS))

    def test_typed_app_only_requires_otadata_and_every_untouched_region(self):
        for field in ("preserved_verified", "untouched_regions_verified"):
            for missing in self.receipt[field]:
                receipt = copy.deepcopy(self.receipt)
                receipt[field].remove(missing)
                with self.subTest(field=field, missing=missing), self.assertRaises(workflow.WorkflowError):
                    updater.checked_receipt(receipt, self.manifest)

    def test_unknown_receipt_kind_is_rejected_even_with_legacy_verifications(self):
        self.receipt["preserved_verified"] = ["nvs", "phy_init"]
        for kind in ("unverified-custom-update", [], {}):
            self.receipt["kind"] = kind
            with self.subTest(kind=kind), self.assertRaisesRegex(workflow.WorkflowError, "receipt kind"):
                updater.checked_receipt(self.receipt, self.manifest)

    def test_written_app_and_initial_ota0_must_match(self):
        for change in ({"write": None}, {"write": {"name": "app", "offset": 0x620000}},
                       {"ota_selection": {"active_slot": "ota_1", "state": "valid", "sequence": 2}},
                       {"ota_selection": {"active_slot": "ota_0", "state": "valid", "sequence": 3}}):
            with self.subTest(change=change), self.assertRaises(workflow.WorkflowError):
                updater.checked_receipt(dict(self.receipt, **change), self.manifest)

    def test_backup_device_and_verified_status_remain_required(self):
        for change in ({"device": {"mac": "ff:ee:dd:cc:bb:aa"}}, {"backup_sha256": "c" * 64},
                       {"status": "writing"}, {"schema": 2}):
            with self.subTest(change=change), self.assertRaises(workflow.WorkflowError):
                updater.checked_receipt(dict(self.receipt, **change), self.manifest)


def different_image(seed):
    data = bytearray(image_blob()[:48])
    data[32] = seed
    checksum = 0xEF
    for value in data[32:36]: checksum ^= value
    data[47] = checksum
    return bytes(data) + hashlib.sha256(data).digest()


class AppStageTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="app-stage-test-", dir=Path(__file__).parent)
        self.root = Path(self.temporary.name)
        self.backups = self.root / "backups"; self.backups.mkdir()
        self.backups_patch = patch.object(workflow, "BACKUPS", self.backups)
        self.backups_patch.start()
        self.private_patch = patch.object(workflow, "private_directory", side_effect=lambda path: path.mkdir())
        self.private_patch.start()
        self.old_build = self.make_build("old-build", 1, 1)
        self.new_build = self.make_build("new-build", 2, 3)
        self.package = self.root / "previous-package"; self.package.mkdir()
        plan = workflow.validate_build(self.old_build)
        files = []
        for segment in plan["segments"]:
            name = updater.PACKAGE_FILES[segment["name"]]
            (self.package / name).write_bytes(Path(segment["file"]).read_bytes())
            files.append({"file": name, **{key: segment[key] for key in ("offset", "size", "sha256")}})
        workflow.save_json(self.package / "manifest.json", {"plan": plan, "files": files})
        self.manifest, self.manifest_path = self.make_backup()
        self.previous_receipt = self.backups / "previous-receipt.json"
        workflow.save_json(self.previous_receipt, typed_receipt(self.manifest, plan))

    def tearDown(self):
        self.private_patch.stop(); self.backups_patch.stop(); self.temporary.cleanup()

    def make_build(self, name, app_seed, boot_seed):
        directory = self.root / name; (directory / "config").mkdir(parents=True)
        metadata = {"flash_settings": workflow.SETTINGS, "write_flash_args": workflow.WRITE_ARGS,
                    "extra_esptool_args": {"chip": "esp32s3"}, "flash_files": {}}
        for segment, (offset, _) in workflow.SEGMENTS.items():
            contents = b"same assets"
            if segment == "app": contents = different_image(app_seed)
            elif segment == "bootloader": contents = different_image(boot_seed)
            elif segment == "partition-table": contents = partition_blob()
            elif segment == "otadata": contents = b"\xff" * 0x2000
            filename = updater.PACKAGE_FILES[segment]
            (directory / filename).write_bytes(contents)
            metadata[segment] = {"offset": hex(offset), "file": filename, "encrypted": "false"}
            metadata["flash_files"][hex(offset)] = filename
        workflow.save_json(directory / "flasher_args.json", metadata)
        workflow.save_json(directory / "project_description.json", {"target": "esp32s3", "project_name": "xiaozhi"})
        workflow.save_json(directory / "config/sdkconfig.json", {"IDF_TARGET": "esp32s3",
            "BOARD_TYPE_ZFSN_ESP32S3_DOG": True, "ESPTOOLPY_FLASHMODE": "dio", "ESPTOOLPY_FLASHSIZE": "16MB",
            "ESPTOOLPY_FLASHFREQ": "80m"})
        return directory

    def make_backup(self):
        directory = self.backups / "original"; directory.mkdir()
        contents = bytearray(b"\xff" * workflow.FLASH_SIZE)
        contents[0x8000:0x8C00] = partition_blob()
        (directory / "flash.bin").write_bytes(contents)
        digest = workflow.sha256(contents)
        manifest = {"schema": 1, "status": "verified", "device": {"chip": "esp32s3",
            "mac": "00:11:22:33:44:55", "flash_size": workflow.FLASH_SIZE,
            "security": {"secure_boot": False, "flash_encryption": False, "flags": 0}},
            "file": "flash.bin", "sha256": digest, "verified_sha256": digest,
            "layout": workflow.LAYOUT, "preserved": {}}
        for name in ("nvs", "phy_init"):
            part = next(item for item in workflow.LAYOUT if item["name"] == name)
            data = contents[part["offset"]:part["offset"] + part["size"]]
            filename = name + ".bin"; (directory / filename).write_bytes(data)
            manifest["preserved"][name] = {"file": filename, "sha256": workflow.sha256(data)}
        path = directory / "manifest.json"; workflow.save_json(path, manifest)
        return manifest, path

    def inputs(self, build, receipt=None, package=None):
        return SimpleNamespace(manifest=self.manifest_path, previous_receipt=receipt or self.previous_receipt,
                               previous_package=package or self.package, build=build)

    def test_new_bootloader_is_retained_from_previous_package_and_only_app_selected(self):
        original_new_boot = (self.new_build / "bootloader.bin").read_bytes()
        output, manifest = stager.stage(self.new_build, self.package, self.backups / "stage")
        self.assertEqual((output / "bootloader.bin").read_bytes(), (self.package / "bootloader.bin").read_bytes())
        self.assertNotEqual((output / "bootloader.bin").read_bytes(), original_new_boot)
        self.assertEqual((output / "app.bin").read_bytes(), (self.new_build / "app.bin").read_bytes())
        self.assertEqual((self.new_build / "bootloader.bin").read_bytes(), original_new_boot)
        self.assertEqual(manifest["allowed_write_segments"], ["app"])
        self.assertFalse(manifest["source_build_bootloader_selected_for_write"])
        self.assertEqual(manifest["status"], "prepared_not_flashed")
        self.assertEqual(manifest["firmware_profile"], "production")
        self.assertFalse((output / "source.zip").exists())
        self.assertEqual(manifest["plan"], workflow.validate_build(output))
        updater.validate_inputs(self.inputs(output))

    def test_raw_changed_bootloader_still_rejected_by_app_updater(self):
        with self.assertRaisesRegex(workflow.WorkflowError, "changed bootloader"):
            updater.validate_inputs(self.inputs(self.new_build))

    def test_changed_partition_assets_or_flash_configuration_is_rejected(self):
        cases = ("partition-table.bin", "assets.bin", "config/sdkconfig.json")
        for filename in cases:
            path = self.new_build / filename; original = path.read_bytes()
            if filename.endswith(".json"):
                data = json.loads(original); data["ESPTOOLPY_FLASHSIZE"] = "8MB"
                path.write_text(json.dumps(data), encoding="utf-8")
            else: path.write_bytes(original + b"x")
            with self.subTest(filename=filename), self.assertRaises(workflow.WorkflowError):
                stager.stage(self.new_build, self.package, self.backups / "rejected-stage")
            path.write_bytes(original)
        self.assertFalse((self.backups / "rejected-stage").exists())

    def test_original_package_digest_and_output_no_overwrite_are_enforced(self):
        app = self.package / "app.bin"; original = app.read_bytes(); app.write_bytes(original + b"bad")
        with self.assertRaisesRegex(workflow.WorkflowError, "content differs"):
            stager.stage(self.new_build, self.package, self.backups / "stage")
        app.write_bytes(original)
        output = self.backups / "stage"; output.mkdir(); (output / "keep").write_text("keep")
        with self.assertRaisesRegex(workflow.WorkflowError, "already exists"):
            stager.stage(self.new_build, self.package, output)
        self.assertEqual((output / "keep").read_text(), "keep")

    def test_app_only_update_chain_keeps_original_backup_and_baseline_boot(self):
        test_stage, test_manifest = stager.stage(self.new_build, self.package, self.backups / "test-stage")
        test_receipt = self.backups / "test-receipt.json"
        workflow.save_json(test_receipt, typed_receipt(self.manifest, test_manifest["plan"]))
        return_stage, return_manifest = stager.stage(self.old_build, test_stage, self.backups / "return-stage")
        updater.validate_inputs(self.inputs(return_stage, receipt=test_receipt, package=test_stage))
        self.assertEqual((return_stage / "app.bin").read_bytes(), (self.package / "app.bin").read_bytes())
        self.assertEqual((return_stage / "bootloader.bin").read_bytes(), (self.package / "bootloader.bin").read_bytes())
        self.assertEqual(return_manifest["allowed_write_segments"], ["app"])
        self.assertEqual(return_manifest["firmware_profile"], "production")

    def test_unchanged_app_requires_explicit_reflash_before_serial_access(self):
        args = self.inputs(self.old_build)
        with patch.object(workflow, "require_usb_port") as serial_check, \
             patch.object(workflow, "EspTool") as tool, \
             self.assertRaisesRegex(workflow.WorkflowError, "App image is unchanged"):
            updater.update(args)
        serial_check.assert_not_called(); tool.assert_not_called()
        args.reflash_unchanged = True
        updater.validate_inputs(args)

    def test_assets_only_chain_can_reflash_unchanged_app_with_all_other_guards(self):
        plan = workflow.validate_build(self.old_build)
        receipt = typed_receipt(self.manifest, plan, "assets-only-update")
        workflow.save_json(self.previous_receipt, receipt)
        args = self.inputs(self.old_build); args.reflash_unchanged = True
        updater.validate_inputs(args)
        assets = self.old_build / "assets.bin"; assets.write_bytes(b"different assets")
        with self.assertRaisesRegex(workflow.WorkflowError, "changed assets"):
            updater.validate_inputs(args)

    def test_reflash_option_requires_boolean(self):
        args = self.inputs(self.old_build); args.reflash_unchanged = "true"
        with self.assertRaisesRegex(workflow.WorkflowError, "reflash_unchanged must be a boolean"):
            updater.validate_inputs(args)


if __name__ == "__main__": unittest.main()
