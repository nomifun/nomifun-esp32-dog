"""Purely offline refusal tests. No serial APIs or subprocesses are invoked."""

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


def partition_blob(layout=None):
    layout = workflow.LAYOUT if layout is None else layout
    data = b"".join(struct.pack("<HBBII16sI", 0x50AA, part["type"], part["subtype"],
                                part["offset"], part["size"], part["name"].encode(), part["flags"])
                    for part in layout)
    data += b"\xeb\xeb" + b"\xff" * 14 + hashlib.md5(data).digest()
    return data.ljust(0xC00, b"\xff")


def image_blob():
    header = bytearray(24)
    header[:4] = bytes((0xE9, 1, 2, 0x4F))
    struct.pack_into("<H", header, 12, 9)
    header[23] = 1
    data = bytes(header) + struct.pack("<II", 0x3FC88000, 4) + b"\x01\x02\x03\x04"
    data = data.ljust(47, b"\0") + bytes((0xEF ^ 1 ^ 2 ^ 3 ^ 4,))
    return data + hashlib.sha256(data).digest()


class GuardsTest(unittest.TestCase):
    def test_fixed_partition_table(self):
        self.assertEqual(workflow.parse_partition_table(partition_blob()), workflow.LAYOUT)

    def test_partition_digest_mismatch(self):
        data = bytearray(partition_blob())
        data[16] ^= 1
        with self.assertRaisesRegex(workflow.WorkflowError, "MD5 mismatch"):
            workflow.parse_partition_table(data)

    def test_partition_without_digest(self):
        data = partition_blob()[:192].ljust(0xC00, b"\xff")
        with self.assertRaisesRegex(workflow.WorkflowError, "MD5 trailer"):
            workflow.parse_partition_table(data)

    def test_layout_overlap(self):
        layout = copy.deepcopy(workflow.LAYOUT)
        layout[1]["offset"] = 0xC000
        with self.assertRaisesRegex(workflow.WorkflowError, "overlap"):
            workflow.parse_partition_table(partition_blob(layout))

    def test_layout_out_of_bounds(self):
        layout = copy.deepcopy(workflow.LAYOUT)
        layout[-1]["size"] += 0x1000
        with self.assertRaisesRegex(workflow.WorkflowError, "exceeds flash"):
            workflow.parse_partition_table(partition_blob(layout))

    def test_layout_migration_refused(self):
        layout = copy.deepcopy(workflow.LAYOUT)
        layout[3]["offset"] += 0x1000
        layout[3]["size"] -= 0x1000
        with self.assertRaisesRegex(workflow.WorkflowError, "differs"):
            workflow.parse_partition_table(partition_blob(layout))

    def test_write_sector_overlap(self):
        with self.assertRaisesRegex(workflow.WorkflowError, "overlap"):
            workflow.validate_ranges([{"offset": 0, "size": 0x1001}, {"offset": 0x1000, "size": 100}])

    def test_write_sector_touches_nvs(self):
        with self.assertRaisesRegex(workflow.WorkflowError, "preserved nvs"):
            workflow.validate_ranges([{"offset": 0x8000, "size": 0x1001}])

    def test_write_out_of_bounds(self):
        with self.assertRaisesRegex(workflow.WorkflowError, "physical flash"):
            workflow.validate_ranges([{"offset": 0xFFF000, "size": 0x1001}])

    def test_image_integrity(self):
        workflow.validate_image_header(image_blob(), "app")
        damaged = bytearray(image_blob())
        damaged[34] ^= 1
        with self.assertRaises(workflow.WorkflowError):
            workflow.validate_image_header(damaged, "app")

    def test_usb_filter_never_accepts_bluetooth_or_intel(self):
        base = {"device": "COM9", "vid": 0x303A, "pid": 0x1001, "description": "USB JTAG/serial"}
        self.assertEqual(workflow.require_usb_port("COM9", [base]), base)
        for bad in ({"vid": None}, {"description": "Bluetooth serial"}, {"vid": 0x8086}):
            with self.subTest(bad=bad), self.assertRaises(workflow.WorkflowError):
                workflow.require_usb_port("COM9", [dict(base, **bad)])

    def test_probe_requires_actual_chip_mac_flash_security(self):
        chip = "Chip is ESP32-S3 (QFN56)\nMAC: 00:11:22:33:44:55\n"
        flash = "Detected flash size: 16MB\n"
        security = "Flags: 0x00000000 (0b0)\nSecure Boot: Disabled\nFlash Encryption: Disabled\n"
        self.assertEqual(workflow.parse_probe(chip, flash, security)["mac"], "00:11:22:33:44:55")
        for args in ((chip.replace("ESP32-S3", "ESP32-C3"), flash, security),
                     (chip, flash.replace("16MB", "8MB"), security),
                     (chip, flash, security.replace("Flash Encryption: Disabled", "Flash Encryption: Enabled")),
                     (chip, flash, security.replace("0x00000000", "0x00000004"))):
            with self.subTest(args=args), self.assertRaises(workflow.WorkflowError):
                workflow.parse_probe(*args)


class ArtifactsTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="offline-test-", dir=Path(__file__).parent)
        self.root = Path(self.temporary.name)
        self.backups = self.root / "backups"
        self.backups.mkdir()
        self.patch = patch.object(workflow, "BACKUPS", self.backups)
        self.patch.start()
        self.build = self.root / "build"
        (self.build / "config").mkdir(parents=True)
        metadata = {"flash_settings": workflow.SETTINGS, "write_flash_args": workflow.WRITE_ARGS,
                    "extra_esptool_args": {"chip": "esp32s3"}, "flash_files": {}}
        for name, (offset, _) in workflow.SEGMENTS.items():
            file = self.build / f"{name}.bin"
            data = image_blob() if name in ("app", "bootloader") else b"assets"
            if name == "partition-table":
                data = partition_blob()
            if name == "otadata":
                data = b"\xff" * 0x2000
            file.write_bytes(data)
            metadata[name] = {"offset": hex(offset), "file": file.name, "encrypted": "false"}
            metadata["flash_files"][hex(offset)] = file.name
        self.metadata = metadata
        self.write_json(self.build / "flasher_args.json", metadata)
        self.write_json(self.build / "project_description.json", {"target": "esp32s3", "project_name": "xiaozhi"})
        self.config = {"IDF_TARGET": "esp32s3", "BOARD_TYPE_ZFSN_ESP32S3_DOG": True,
                       "ESPTOOLPY_FLASHMODE": "dio", "ESPTOOLPY_FLASHSIZE": "16MB", "ESPTOOLPY_FLASHFREQ": "80m"}
        self.write_json(self.build / "config" / "sdkconfig.json", self.config)

    def tearDown(self):
        self.patch.stop()
        self.temporary.cleanup()

    @staticmethod
    def write_json(path, data):
        path.write_text(json.dumps(data), encoding="utf-8")

    def make_backup(self):
        directory = self.backups / "example"
        directory.mkdir()
        data = bytearray(b"\xff" * workflow.FLASH_SIZE)
        data[0x8000:0x8C00] = partition_blob()
        file = directory / "flash.bin"
        file.write_bytes(data)
        digest = workflow.sha256(data)
        self.manifest = {"schema": 1, "status": "verified", "device": {"chip": "esp32s3",
                         "mac": "00:11:22:33:44:55", "flash_size": workflow.FLASH_SIZE,
                         "security": {"secure_boot": False, "flash_encryption": False, "flags": 0}},
                         "file": file.name, "sha256": digest, "verified_sha256": digest,
                         "layout": workflow.LAYOUT, "preserved": {}}
        for name in ("nvs", "phy_init"):
            part = next(item for item in workflow.LAYOUT if item["name"] == name)
            contents = data[part["offset"]:part["offset"] + part["size"]]
            (directory / f"{name}.bin").write_bytes(contents)
            self.manifest["preserved"][name] = {"file": f"{name}.bin", "sha256": workflow.sha256(contents)}
        self.manifest_path = directory / "manifest.json"
        self.write_json(self.manifest_path, self.manifest)

    def test_valid_build(self):
        self.assertEqual(len(workflow.validate_build(self.build)["segments"]), 5)

    def test_wrong_board_is_refused(self):
        self.config["BOARD_TYPE_ZFSN_ESP32S3_DOG"] = False
        self.write_json(self.build / "config" / "sdkconfig.json", self.config)
        with self.assertRaisesRegex(workflow.WorkflowError, "dog"):
            workflow.validate_build(self.build)

    def test_arbitrary_force_or_erase_arguments_are_refused_offline(self):
        for value in ("--force", "--erase-all"):
            self.metadata["write_flash_args"] = workflow.WRITE_ARGS + [value]
            self.write_json(self.build / "flasher_args.json", self.metadata)
            with self.subTest(value=value), self.assertRaisesRegex(workflow.WorkflowError, "options"):
                workflow.validate_build(self.build)

    def test_nvs_write_is_refused(self):
        self.metadata["app"]["offset"] = "0x9000"
        self.write_json(self.build / "flasher_args.json", self.metadata)
        with self.assertRaisesRegex(workflow.WorkflowError, "app offset"):
            workflow.validate_build(self.build)

    def test_extra_flash_file_is_refused(self):
        self.metadata["flash_files"]["0x9000"] = "app.bin"
        self.write_json(self.build / "flasher_args.json", self.metadata)
        with self.assertRaisesRegex(workflow.WorkflowError, "five"):
            workflow.validate_build(self.build)

    def test_valid_backup(self):
        self.make_backup()
        self.assertEqual(workflow.validate_backup(self.manifest_path)[0], self.manifest)

    def test_wrong_backup_hash(self):
        self.make_backup()
        self.manifest["sha256"] = "0" * 64
        self.write_json(self.manifest_path, self.manifest)
        with self.assertRaisesRegex(workflow.WorkflowError, "SHA256 mismatch"):
            workflow.validate_backup(self.manifest_path)

    def test_incomplete_backup(self):
        self.make_backup()
        self.manifest["status"] = "read"
        self.write_json(self.manifest_path, self.manifest)
        with self.assertRaisesRegex(workflow.WorkflowError, "unverified"):
            workflow.validate_backup(self.manifest_path)

    def test_short_backup(self):
        self.make_backup()
        (self.manifest_path.parent / "flash.bin").write_bytes(b"partial backup")
        with self.assertRaisesRegex(workflow.WorkflowError, "whole physical flash"):
            workflow.validate_backup(self.manifest_path)

    def test_wrong_preserved_artifact(self):
        self.make_backup()
        (self.manifest_path.parent / "nvs.bin").write_bytes(b"damaged")
        with self.assertRaisesRegex(workflow.WorkflowError, "artifact mismatch"):
            workflow.validate_backup(self.manifest_path)

    def test_wrong_connected_mac_prevents_any_write(self):
        self.make_backup()
        device = copy.deepcopy(self.manifest["device"])
        device["mac"] = "aa:bb:cc:dd:ee:ff"
        args = SimpleNamespace(build=self.build, manifest=self.manifest_path, port="COM9", baud=460800)
        with patch.object(workflow, "require_usb_port"), patch.object(workflow, "probe", return_value=device), \
                patch.object(workflow, "EspTool") as tool, self.assertRaisesRegex(workflow.WorkflowError, "MAC"):
            workflow.flash_device(args)
        tool.return_value.run.assert_not_called()

    def test_changed_live_flash_prevents_any_write(self):
        self.make_backup()
        args = SimpleNamespace(build=self.build, manifest=self.manifest_path, port="COM9", baud=460800)
        with patch.object(workflow, "require_usb_port"), \
                patch.object(workflow, "probe", return_value=self.manifest["device"]), \
                patch.object(workflow, "EspTool") as tool, \
                patch.object(workflow, "verify", side_effect=workflow.WorkflowError("live digest mismatch")), \
                self.assertRaisesRegex(workflow.WorkflowError, "live digest mismatch"):
            workflow.flash_device(args)
        tool.return_value.run.assert_not_called()

    def test_no_serial_access_for_offline_commands(self):
        with patch.object(workflow, "list_ports", side_effect=AssertionError("serial accessed")), \
                patch.object(workflow.subprocess, "run", side_effect=AssertionError("subprocess used")), \
                patch("builtins.print"):
            workflow.main(["prepare", "--build", str(self.build)])
            self.make_backup()
            workflow.main(["check-backup", "--manifest", str(self.manifest_path)])


if __name__ == "__main__":
    unittest.main()
