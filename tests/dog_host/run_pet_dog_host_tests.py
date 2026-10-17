"""Run the current PetDog C++ implementation against host hardware substitutes.

Windows uses the existing default WSL distribution and its gcc/g++. No packages
are installed. On Linux the same runner invokes the installed compilers directly.
The only production-source substitution is the SD custom_action directory.
"""

import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from datetime import datetime


EXECUTOR = r'''
from pathlib import Path
import json
import shutil
import subprocess
import sys
import tempfile

staging = Path(sys.argv[1])
cjson = Path(sys.argv[2])
with tempfile.TemporaryDirectory(prefix="pet-dog-host-") as temporary:
    build = Path(temporary)
    shutil.copytree(staging, build, dirs_exist_ok=True)
    (build / "sdcard").mkdir()
    source_file = build / "pet_dog.cc"
    source = source_file.read_text(encoding="utf-8")
    token = '"/sdcard/custom_action"'
    assert source.count(token) == 1, "SD path substitution must match exactly once"
    source_file.write_text(source.replace(token, json.dumps(str(build / "sdcard" / "custom_action"))), encoding="utf-8")
    commands = [
        ["gcc", "-c", "-O1", str(cjson / "cJSON.c"), "-o", "cJSON.o"],
        ["g++", "-std=c++20", "-O1", "-g", "-pthread", "-Wall", "-Wextra", "-Werror",
         "-Wno-unused-parameter", "-Wno-misleading-indentation", "-I.", "-Istubs",
         "-I" + str(cjson), "pet_dog.cc", "test.cc", "cJSON.o", "-o", "test"],
        [str(build / "test")],
    ]
    for label, command in zip(["cJSON compile", "PetDog host compile", "behavior assertions"], commands):
        result = subprocess.run(command, cwd=build, text=True, capture_output=True)
        if result.stdout:
            print(result.stdout, end="")
        if result.stderr:
            print(result.stderr, end="", file=sys.stderr)
        print(label + " exit=" + str(result.returncode), flush=True)
        if result.returncode:
            raise SystemExit(result.returncode)
'''


def linux_path(path: Path) -> str:
    """Translate an absolute Windows drive path to the default WSL mount path."""
    if os.name != "nt":
        return str(path)
    resolved = str(path.resolve()).replace("\\", "/")
    if len(resolved) < 3 or resolved[1:3] != ":/":
        raise ValueError("Expected an absolute Windows drive path: " + resolved)
    return "/mnt/" + resolved[0].lower() + "/" + resolved[3:]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--idf-path", type=Path,
        default=Path(os.environ["IDF_PATH"]) if os.environ.get("IDF_PATH") else None,
        help="Installed ESP-IDF checkout containing components/json/cJSON; defaults to IDF_PATH")
    parser.add_argument("--wsl-distribution", help="Optional installed WSL distribution name")
    parser.add_argument("--log", type=Path,
        default=Path(__file__).resolve().parents[2] / "build/dog_host/pet_dog_host_wsl/pet_dog_host_latest.log")
    args = parser.parse_args()
    if args.idf_path is None:
        parser.error("--idf-path or the IDF_PATH environment variable is required (components/json/cJSON)")
    tests = Path(__file__).resolve().parent
    board = tests.parents[1] / "main/boards/zfsn-esp32s3-dog"
    cjson = args.idf_path.resolve() / "components/json/cJSON"
    for file in [board / "pet_dog.cc", board / "pet_dog.h", cjson / "cJSON.c", cjson / "cJSON.h"]:
        if not file.is_file():
            parser.error("Required existing source not found: " + str(file))

    lines = [
        "PetDog host behavior verification",
        "Started: " + datetime.now().astimezone().isoformat(timespec="seconds"),
        "Command: " + subprocess.list2cmdline([sys.executable, str(Path(__file__).resolve()), *sys.argv[1:]]),
        "ESP-IDF cJSON source: " + str(cjson),
        "Source substitution: SD custom_action path only; actual PetDog C++ compiled unchanged otherwise.",
        "Hardware substitutes: ESP logging/LEDC, NVS persistence, FreeRTOS tasks/notifications, Application/Board/Display.",
        "Cases: nested and flat calibration; bounds/integer/shape/JSON/NUL validation; NVS commit failure rollback and reload;",
        "       SD action CRUD/overwrite; path traversal/empty steps/negative delay/8192-byte limit; invalid commands;",
        "       100-second action accepted within 1 second; no further PWM writes after stop; command replacement; two workers per instance.",
    ]
    for file in [board / "pet_dog.h", board / "pet_dog.cc", tests / "pet_dog_host/test.cc", tests / "pet_dog_host/stubs/fake.h", cjson / "cJSON.c"]:
        lines.append("SHA256 " + str(file) + " " + hashlib.sha256(file.read_bytes()).hexdigest())

    with tempfile.TemporaryDirectory(prefix="pet-dog-host-stage-") as directory:
        staging = Path(directory)
        shutil.copytree(tests / "pet_dog_host", staging, dirs_exist_ok=True)
        for name in ["pet_dog.h", "pet_dog.cc"]:
            shutil.copyfile(board / name, staging / name)
        if os.name == "nt":
            command = ["wsl"]
            if args.wsl_distribution:
                command += ["--distribution", args.wsl_distribution]
            command += ["--exec", "python3", "-c", EXECUTOR, linux_path(staging), linux_path(cjson)]
        else:
            command = [sys.executable, "-c", EXECUTOR, str(staging), str(cjson)]
        environment = os.environ.copy()
        environment["WSL_UTF8"] = "1"
        try:
            result = subprocess.run(command, text=True, encoding="utf-8", errors="replace",
                                    capture_output=True, env=environment, timeout=120)
            lines += [result.stdout.rstrip(), "Host runner exit=" + str(result.returncode)]
            if result.stderr:
                lines += ["Compiler/WSL diagnostics:", result.stderr.rstrip()]
            exit_code = result.returncode
        except (OSError, subprocess.TimeoutExpired) as error:
            lines += ["Host runner failure: " + str(error), "Host runner exit=1"]
            exit_code = 1

    lines += ["Scope: host substitutes prove application behavior; they do not prove ESP32 timing, real SD/FatFS failures, servo calibration, flash, or device acceptance."]
    log = "\n".join(line for line in lines if line) + "\n"
    args.log.resolve().parent.mkdir(parents=True, exist_ok=True)
    args.log.write_text(log, encoding="utf-8")
    print(log)
    print("Saved log: " + str(args.log.resolve()))
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
