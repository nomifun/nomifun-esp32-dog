"""Exercise actual BoardDog button/volume methods with Windows host substitutes."""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import subprocess


TEST = Path(__file__).resolve().parent
ROOT = TEST.parents[2]
SOURCE = ROOT / "main/boards/zfsn-esp32s3-dog/board_dog.cc"
OUTPUT = ROOT / "build/dog_host/board_buttons_host"
VC_BASE = Path("C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC")
SDK = Path("C:/Program Files (x86)/Windows Kits/10")


def main():
    if os.name != "nt":
        raise SystemExit("This runner uses only the existing Windows MSVC toolchain")
    vc = sorted(VC_BASE.iterdir())[-1]
    sdk_version = sorted((SDK / "Lib").iterdir())[-1].name
    compiler = vc / "bin/Hostx64/x64/cl.exe"
    env = os.environ.copy()
    env["PATH"] = str(compiler.parent) + os.pathsep + env["PATH"]
    env["INCLUDE"] = os.pathsep.join(str(path) for path in (
        vc / "include", SDK / "Include" / sdk_version / "ucrt",
        SDK / "Include" / sdk_version / "shared", SDK / "Include" / sdk_version / "um"))
    env["LIB"] = os.pathsep.join(str(path) for path in (
        vc / "lib/x64", SDK / "Lib" / sdk_version / "ucrt/x64", SDK / "Lib" / sdk_version / "um/x64"))
    OUTPUT.mkdir(parents=True, exist_ok=True)
    source_bytes = SOURCE.read_bytes()
    content = source_bytes.decode("utf-8").replace("\r\n", "\n")
    snippets = []
    for start, end in (
        ("    void ChangeVolume(int delta) {", "    void InitializeI2c() {"),
        ("    void InitializeVolumeTimers() {", "    void InitializeButtons() {"),
        ("    void InitializeButtons() {", "    void InitializeSdCard() {"),
    ):
        first = content.index(start)
        last = content.index(end, first + len(start))
        snippets.append(content[first:last])
    actual = OUTPUT / "actual_board_buttons.inc"
    actual.write_text("\n".join(snippets), encoding="utf-8")
    results = []
    for profile, flags in (("production-dog", []),):
        exe = OUTPUT / (profile + ".exe")
        command = [str(compiler), "/nologo", "/std:c++20", "/EHsc", "/MD", "/W4", "/utf-8",
                   *flags, "/I" + str(OUTPUT), str(TEST / "test.cc"), "/Fe:" + str(exe)]
        build = subprocess.run(command, cwd=OUTPUT, env=env, capture_output=True,
                               encoding="utf-8", errors="replace", timeout=60)
        (OUTPUT / (profile + "-build.log")).write_text(build.stdout + build.stderr, encoding="utf-8")
        run = None
        if build.returncode == 0:
            run = subprocess.run([str(exe)], cwd=OUTPUT, env=env, capture_output=True,
                                 encoding="utf-8", errors="replace", timeout=15)
            (OUTPUT / (profile + "-test.log")).write_text(run.stdout + run.stderr, encoding="utf-8")
        result = {"profile": profile, "build_exit": build.returncode,
                  "test_exit": run.returncode if run else None,
                  "stdout": run.stdout if run else build.stdout,
                  "stderr": run.stderr if run else build.stderr}
        results.append(result)
        print(profile + ": " + result["stdout"] + result["stderr"], end="")
    report = {
        "status": "passed" if all(item["test_exit"] == 0 for item in results) else "failed",
        "subject": "Actual BoardDog ChangeVolume, InitializeVolumeTimers, MusicSwitchTimerCb and InitializeButtons",
        "source_sha256": hashlib.sha256(source_bytes).hexdigest(),
        "source_changed_during_run": SOURCE.read_bytes() != source_bytes,
        "test_sha256": hashlib.sha256((TEST / "test.cc").read_bytes()).hexdigest(),
        "extracted_sha256": hashlib.sha256(actual.read_bytes()).hexdigest(),
        "compiler": str(compiler), "results": results,
        "limits": "Button/timer callbacks and main-queue ordering use host substitutes; no GPIO, device, real ESP timers, OLED, media, NVS or network is accessed.",
    }
    (OUTPUT / "result.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(OUTPUT / "result.json")
    return 0 if report["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
