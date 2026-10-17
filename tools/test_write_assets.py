"""Offline tests for the original-assets write path; no hardware is opened."""

import copy
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import device_workflow as workflow
import write_assets as writer
import update_app
import test_update_app as app_tests
from test_update_app import selected_ota, typed_receipt


class WriteAssetsTest(unittest.TestCase):
    make_build = app_tests.AppStageTest.make_build
    make_backup = app_tests.AppStageTest.make_backup
    tearDown = app_tests.AppStageTest.tearDown

    def setUp(self):
        app_tests.AppStageTest.setUp(self)
        (self.package / "config").mkdir()
        for filename in ("config/sdkconfig.json", "project_description.json", "flasher_args.json"):
            (self.package / filename).write_bytes((self.old_build / filename).read_bytes())
        self.refresh_package(self.package)

    def refresh_package(self, package):
        plan = workflow.validate_build(package)
        files = [{"file": update_app.PACKAGE_FILES[item["name"]],
                  **{key: item[key] for key in ("offset", "size", "sha256")}}
                 for item in plan["segments"]]
        workflow.save_json(package / "manifest.json", {"plan": plan, "files": files})
        workflow.save_json(self.previous_receipt, typed_receipt(self.manifest, plan))
        return plan

    def prepare(self):
        return writer.stage(self.manifest_path, self.previous_receipt, self.package,
                              self.backups / "assets-stage")

    def args(self, stage):
        return SimpleNamespace(manifest=self.manifest_path, previous_receipt=self.previous_receipt,
                               previous_package=self.package, build=stage, port="COM9", baud=460800)

    def test_stage_keeps_application_metadata_and_writes_exact_assets_bytes(self):
        with patch.object(workflow, "EspTool") as tool, patch.object(workflow, "require_usb_port") as port:
            output, manifest = self.prepare()
        tool.assert_not_called(); port.assert_not_called()
        self.assertEqual((output / "assets.bin").read_bytes(), writer.dog_assets())
        self.assertEqual(manifest["allowed_write_segments"], ["assets"])
        for name in ("bootloader", "partition-table", "otadata", "app"):
            filename = update_app.PACKAGE_FILES[name]
            self.assertEqual((output / filename).read_bytes(), (self.package / filename).read_bytes())
        for filename in ("config/sdkconfig.json", "project_description.json"):
            self.assertEqual((output / filename).read_bytes(), (self.package / filename).read_bytes())
        self.assertEqual(manifest["plan"], workflow.validate_build(output))
        writer.validate_inputs(self.args(output))

    def test_original_source_corruption_is_refused_before_serial_access(self):
        bad = self.root / "corrupt-assets.bin"; bad.write_bytes(b"bad")
        with patch.object(writer, "DOG_ASSETS", bad), \
             patch.object(workflow, "EspTool") as tool, \
             self.assertRaisesRegex(workflow.WorkflowError, "assets SHA256"):
            self.prepare()
        tool.assert_not_called()

    def test_tampered_assets_payload_is_refused_before_serial_access(self):
        output, _ = self.prepare()
        (output / "assets.bin").write_bytes(b"tampered")
        with patch.object(workflow, "EspTool") as tool, \
             self.assertRaises(workflow.WorkflowError):
            writer.write(self.args(output))
        tool.assert_not_called()

    def test_coherent_changed_application_is_still_refused(self):
        output, _ = self.prepare()
        (output / "app.bin").write_bytes((self.new_build / "app.bin").read_bytes())
        manifest = workflow.load_json(output / "manifest.json")
        plan = workflow.validate_build(output); manifest["plan"] = plan
        for record in manifest["files"]:
            if record["file"] == "app.bin":
                item = next(item for item in plan["segments"] if item["name"] == "app")
                record.update({key: item[key] for key in ("size", "sha256")})
        workflow.save_json(output / "manifest.json", manifest)
        with patch.object(workflow, "EspTool") as tool, \
             self.assertRaisesRegex(workflow.WorkflowError, "refuses changed app"):
            writer.write(self.args(output))
        tool.assert_not_called()

    def exercise_write(self, *, wrong_device=False, wrong_ota=False, changed_live_app=False,
                         corrupt_app_after_write=False):
        output, _ = self.prepare()
        args = self.args(output)
        flash = bytearray(b"\xff" * workflow.FLASH_SIZE)
        prior_plan = workflow.validate_build(self.package)
        for item in prior_plan["segments"]:
            flash[item["offset"]:item["offset"] + item["size"]] = Path(item["file"]).read_bytes()
        flash[0xD000:0xF000] = selected_ota(2 if wrong_ota else 1)
        if changed_live_app:
            flash[0x20000] ^= 1
        owner = self

        class Tool:
            def __init__(self, port, baud, directory):
                self.directory = directory; self.calls = []

            def run(self, command, *values, **kwargs):
                self.calls.append((command, values, kwargs))
                if command == "read_flash":
                    offset, size = int(values[0], 0), int(values[1], 0)
                    Path(values[2]).write_bytes(flash[offset:offset + size])
                elif command == "verify_flash":
                    count = 0
                    for index in range(2, len(values), 2):
                        offset, path = int(values[index], 0), Path(values[index + 1])
                        data = path.read_bytes()
                        workflow.require(bytes(flash[offset:offset + len(data)]) == data,
                                         "Flash digest mismatch")
                        count += 1
                    return "-- verify OK (digest matched)\n" * count
                elif command == "write_flash":
                    receipt = workflow.load_json(self.directory / "receipt.json")
                    owner.assertEqual(receipt["status"], "writing")
                    owner.assertEqual(receipt["write"]["sha256"], writer.DOG_ASSETS_SHA256)
                    owner.assertEqual(values[-2], "0xc20000")
                    offset, data = int(values[-2], 0), Path(values[-1]).read_bytes()
                    flash[offset:offset + len(data)] = data
                    if corrupt_app_after_write:
                        flash[0x20000] ^= 1
                return ""

        tool = Tool(args.port, args.baud, self.backups / "unused")
        device = copy.deepcopy(self.manifest["device"])
        if wrong_device:
            device["mac"] = "ff:ee:dd:cc:bb:aa"

        def use_tool(port, baud, directory):
            tool.directory = directory
            return tool

        with patch.object(workflow, "require_usb_port"), \
             patch.object(workflow, "EspTool", side_effect=use_tool), \
             patch.object(workflow, "probe", return_value=device), patch("builtins.print"):
            error = None; receipt = None
            try:
                _, receipt = writer.write(args)
            except workflow.WorkflowError as exc:
                error = exc
        return error, tool, receipt

    def test_one_assets_write_and_every_region_verified_before_reset(self):
        error, tool, receipt = self.exercise_write()
        self.assertIsNone(error)
        writes = [item for item in tool.calls if item[0] == "write_flash"]
        self.assertEqual(len(writes), 1)
        self.assertEqual(list(writes[0][1][:-2]), workflow.WRITE_ARGS)
        verifies = [item for item in tool.calls if item[0] == "verify_flash"]
        self.assertEqual({int(verifies[0][1][i], 0) for i in range(2, len(verifies[0][1]), 2)},
                         {0, 0x8000, 0x20000, 0xC20000, 0x620000})
        self.assertEqual({int(verifies[-2][1][i], 0) for i in range(2, len(verifies[-2][1]), 2)},
                         {0, 0x8000, 0x20000, 0x620000})
        self.assertEqual(tool.calls[-1][2], {"after": "hard_reset"})
        self.assertEqual(receipt["status"], "verified")
        update_app.checked_receipt(receipt, self.manifest)
        update_app.validate_package(self.backups / "assets-stage",
                                    update_app.checked_segments(receipt["plan"]))

    def test_wrong_device_prevents_write(self):
        error, tool, _ = self.exercise_write(wrong_device=True)
        self.assertIsNotNone(error)
        self.assertFalse(any(item[0] == "write_flash" for item in tool.calls))

    def test_changed_live_app_prevents_write(self):
        error, tool, _ = self.exercise_write(changed_live_app=True)
        self.assertIsNotNone(error)
        self.assertFalse(any(item[0] == "write_flash" for item in tool.calls))

    def test_other_ota_selection_prevents_write(self):
        error, tool, _ = self.exercise_write(wrong_ota=True)
        self.assertIsNotNone(error)
        self.assertFalse(any(item[0] == "write_flash" for item in tool.calls))

    def test_postwrite_application_corruption_cannot_produce_verified_receipt(self):
        error, tool, receipt = self.exercise_write(corrupt_app_after_write=True)
        self.assertIsNotNone(error); self.assertIsNone(receipt)
        self.assertEqual(workflow.load_json(tool.directory / "receipt.json")["status"], "writing")
        self.assertFalse(any(item[2].get("after") == "hard_reset" for item in tool.calls))


if __name__ == "__main__":
    unittest.main()
