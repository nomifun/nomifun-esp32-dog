"""Run the existing offline firmware and integration contract suites on Windows.

Uses only the installed Python, Node.js, MSVC and ESP-IDF cJSON sources. The
selected tests use host substitutes: no COM port, Wi-Fi adapter, real network
socket, user application configuration, flashing or dependency download.
"""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[2]
SUITES = ROOT / "tests" / "dog_host"
WIFI_TESTS = ROOT / "components" / "78__esp-wifi-connect" / "tests"
TOOLS = ROOT / "tools"
TOOL_MODULES = (
    "test_device_workflow", "test_update_app",
)
WIFI_MODULES = (
    "test_saved_add", "test_ssid_persistence", "test_provisioning_storage",
    "test_provisioning_http", "test_provisioning_connection",
    "test_wifi_station", "test_wifi_board_recovery",
)


def source_hashes():
    firmware = ROOT
    selected = list(SUITES.rglob("*.cc"))
    selected += list(SUITES.rglob("*.h"))
    selected += list(SUITES.rglob("*.mjs"))
    selected += list(SUITES.rglob("*.py"))
    selected += list(TOOLS.glob("*.py"))
    selected += list(WIFI_TESTS.glob("*.py"))
    selected += list((WIFI_TESTS.parent).glob("*.cc"))
    selected += list((WIFI_TESTS.parent / "include").glob("*.h"))
    selected += list((firmware / "main/boards/zfsn-esp32s3-dog").glob("*.cc"))
    selected += list((firmware / "main/boards/zfsn-esp32s3-dog").glob("*.h"))
    selected += [firmware / "main" / name for name in (
        "application.cc", "application.h", "settings.cc", "settings.h",
        "boards/common/wifi_board.cc", "boards/common/wifi_board.h",
    )]
    return {str(path.relative_to(ROOT)).replace("\\", "/"):
            hashlib.sha256(path.read_bytes()).hexdigest()
            for path in sorted(set(selected)) if path.is_file()}


def run_suite(spec, output, environment):
    name, command, cwd = spec
    started = time.monotonic()
    try:
        result = subprocess.run(command, cwd=cwd, env=environment,
                                capture_output=True, encoding="utf-8",
                                errors="replace", timeout=180)
        content = result.stdout + result.stderr
        status = result.returncode
        error = None
    except (subprocess.TimeoutExpired, OSError) as exc:
        content, status, error = str(exc), None, type(exc).__name__
    log = output / (name + ".log")
    log.write_text(content, encoding="utf-8")
    counts = [int(value) for value in re.findall(
        r"(?:^|\n)(?:[a-z-]+: )?(\d+) [^\n]*?(?:tests|cases) passed", content)]
    unittest_count = re.search(r"Ran (\d+) tests? in ", content)
    if unittest_count:
        counts.append(int(unittest_count.group(1)))
    category = ("network-recovery" if name == "network-reset" or name.startswith("wifi-")
                else {"ota-setup": "startup-ota", "pet-dog": "motion-arbitration",
                      "tools-contracts": "integration-tool-contracts",
                      "board-buttons": "board-key-dispatch"}[name])
    return {
        "suite": name, "status": "passed" if status == 0 else "failed",
        "category": category,
        "exit_code": status, "error": error,
        "elapsed_seconds": round(time.monotonic() - started, 3),
        "reported_case_count": sum(counts) if counts else None,
        "counting_note": ("PetDog reports behavioral groups and 100 arbitration pairs; "
                          "these are not counted as independent test cases."
                          if name == "pet-dog" else None),
        "command": command, "cwd": str(cwd), "log": str(log),
        "summary_lines": [line for line in content.splitlines()
                          if "passed" in line or line.startswith("PASS")
                          or line.startswith("Ran ") or line in ("OK", "FAILED")],
        "failure_tail": content[-4000:] if status != 0 else None,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--label", default="baseline",
                        help="Artifact label for results under build/dog_host")
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args()
    if not re.fullmatch(r"[a-zA-Z0-9_-]+", args.label):
        parser.error("label must contain only letters, digits, underscore or hyphen")
    if os.name != "nt":
        parser.error("This aggregate runner selects the existing Windows-native suites")
    node = shutil.which("node")
    if not node:
        parser.error("Installed Node.js is required; this runner never downloads tools")
    output = ROOT / "build" / "dog_host" / args.label
    output.mkdir(parents=True, exist_ok=True)
    environment = dict(os.environ, PYTHONNOUSERSITE="1", PYTHONDONTWRITEBYTECODE="1",
                       PYTHONIOENCODING="utf-8")
    specs = [(name, [node, str(SUITES / directory / "run.mjs")], ROOT)
             for name, directory in (("network-reset", "network_reset_host"),
                                     ("ota-setup", "ota_setup_host"),
                                     ("pet-dog", "pet_dog_host"))]
    specs += [("wifi-" + module.removeprefix("test_").removeprefix("wifi_").replace("_", "-"),
               [sys.executable, "-s", str(WIFI_TESTS / (module + ".py"))], WIFI_TESTS)
              for module in WIFI_MODULES]
    specs += [("tools-contracts", [sys.executable, "-s", "-m", "unittest", "-v",
                                 *TOOL_MODULES], TOOLS)]
    specs += [("board-buttons", [sys.executable, "-s", str(SUITES / "board_buttons_host/run.py")], ROOT)]
    started_at = datetime.now(timezone.utc).isoformat()
    before = source_hashes()
    results = []
    with ThreadPoolExecutor(max_workers=max(1, min(args.jobs, 4))) as executor:
        futures = [executor.submit(run_suite, spec, output, environment) for spec in specs]
        for future in as_completed(futures):
            result = future.result()
            results.append(result)
            print(f"{result['suite']}: {result['status']} "
                  f"cases={result['reported_case_count']} "
                  f"seconds={result['elapsed_seconds']}", flush=True)
    after = source_hashes()
    changed = [path for path in sorted(set(before) | set(after))
               if before.get(path) != after.get(path)]
    categories = {}
    for item in results:
        group = categories.setdefault(item["category"], {
            "suites_passed": 0, "suites_total": 0,
            "reported_cases_passed": 0, "unquantified_behavior_suites": [],
        })
        group["suites_total"] += 1
        if item["status"] == "passed":
            group["suites_passed"] += 1
            group["reported_cases_passed"] += item["reported_case_count"] or 0
        if item["reported_case_count"] is None:
            group["unquantified_behavior_suites"].append(item["suite"])
    report = {
        "started_at_utc": started_at,
        "finished_at_utc": datetime.now(timezone.utc).isoformat(),
        "python": sys.executable, "node": node,
        "status": "passed" if all(item["status"] == "passed" for item in results) else "failed",
        "suites_passed": sum(item["status"] == "passed" for item in results),
        "suites_total": len(results),
        "categories": categories,
        "source_changed_during_run": changed,
        "source_sha256_before": before, "source_sha256_after": after,
        "limits": ("Offline native host substitutes. This does not establish ESP32 firmware "
                   "linking, real OLED appearance, hardware timing, live cloud/server behavior, "
                   "physical motion, USB/serial, actual network reconnection or device OTA."),
        "results": sorted(results, key=lambda item: item["suite"]),
    }
    receipt = output / "result.json"
    receipt.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"{report['suites_passed']}/{report['suites_total']} suites; "
          f"source changes during run: {len(changed)}")
    for category, group in sorted(categories.items()):
        print(f"  {category}: {group['suites_passed']}/{group['suites_total']} suites; "
              f"{group['reported_cases_passed']} reported cases; "
              f"uncounted: {group['unquantified_behavior_suites']}")
    print(receipt)
    return 0 if report["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
