"""Narrow native tests for the actual BoardDog menu service callback lambdas."""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
import subprocess


TEST = Path(__file__).resolve().parent
ROOT = TEST.parents[2]
MAIN = ROOT / "main"
BOARD = MAIN / "boards/zfsn-esp32s3-dog/board_dog.cc"
OUTPUT = ROOT / "build/dog_host/board_menu_host"
VC_BASE = Path("C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC")
SDK = Path("C:/Program Files (x86)/Windows Kits/10")


def menu_callbacks(source):
    start = source.index("            display_->SetMenuCallbacks(")
    end = source.index("\n            });", start) + len("\n            });")
    return source[start:end]


def function(source, signature):
    start = source.index(signature + " {")
    end = source.index("\n}", start) + 2
    return source[start:end]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    if os.name != "nt":
        raise SystemExit("Uses only existing Windows MSVC; never installs tools")
    OUTPUT.mkdir(parents=True, exist_ok=True)
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
    board_hash = digest(BOARD)
    app_hash = digest(MAIN / "application.cc")
    callbacks = menu_callbacks(BOARD.read_text(encoding="utf-8"))
    app = (MAIN / "application.cc").read_text(encoding="utf-8")
    app_signatures = (
        "void Application::SetChatActive(bool active)",
        "void Application::Schedule(std::function<void()> callback)",
        "void Application::SetListeningMode(ListeningMode mode)",
    )
    app_bodies = "\n\n".join(function(app, signature) for signature in app_signatures)
    (OUTPUT / "actual_menu_callbacks.inc").write_text(callbacks + "\n", encoding="utf-8")
    (OUTPUT / "actual_chat_entry.inc").write_text(app_bodies + "\n", encoding="utf-8")
    enums = []
    for path, name in ((MAIN / "application.h", "AecMode"),
                       (MAIN / "protocols/protocol.h", "ListeningMode"),
                       (MAIN / "protocols/protocol.h", "AbortReason")):
        match = re.search(r"enum " + name + r" \{[\s\S]*?\};", path.read_text(encoding="utf-8"))
        if not match:
            raise RuntimeError("Actual enum not found: " + name)
        enums.append(match.group())
    (OUTPUT / "actual_enums.inc").write_text("\n".join(enums), encoding="utf-8")
    results = []
    for profile, flags in (("production-dog", []),):
        exe = OUTPUT / (profile + ".exe")
        build = subprocess.run([str(compiler), "/nologo", "/std:c++20", "/EHsc", "/MD", "/W4", "/utf-8",
                                *flags, "/I" + str(OUTPUT), "/I" + str(MAIN),
                                str(TEST / "test.cc"), "/Fe:" + str(exe)], cwd=OUTPUT, env=env,
                               capture_output=True, encoding="utf-8", errors="replace", timeout=60)
        (OUTPUT / (profile + "-build.log")).write_text(build.stdout + build.stderr, encoding="utf-8")
        run = None
        if build.returncode == 0:
            run = subprocess.run([str(exe)], cwd=OUTPUT, env=env, capture_output=True,
                                 encoding="utf-8", errors="replace", timeout=15)
            (OUTPUT / (profile + "-test.log")).write_text(run.stdout + run.stderr, encoding="utf-8")
        value = {"profile": profile, "build_exit": build.returncode,
                 "test_exit": run.returncode if run else None,
                 "stdout": run.stdout if run else build.stdout,
                 "stderr": run.stderr if run else build.stderr}
        results.append(value)
        print(profile + ": " + value["stdout"] + value["stderr"], end="")
    current_callbacks = menu_callbacks(BOARD.read_text(encoding="utf-8"))
    current_app = (MAIN / "application.cc").read_text(encoding="utf-8")
    current_bodies = "\n\n".join(function(current_app, signature) for signature in app_signatures)
    stable = callbacks == current_callbacks and app_bodies == current_bodies
    receipt = {
        "status": "passed" if stable and all(value["test_exit"] == 0 for value in results) else "failed",
        "subject": "Actual BoardDog SetMenuCallbacks action/info lambdas, actual Application SetChatActive/Schedule/SetListeningMode",
        "sampled_methods_stable_during_run": stable,
        "board_file_sha256_before": board_hash, "board_file_sha256_after": digest(BOARD),
        "application_file_sha256_before": app_hash, "application_file_sha256_after": digest(MAIN / "application.cc"),
        "test_sha256": digest(TEST / "test.cc"),
        "callback_snippet_sha256": digest(OUTPUT / "actual_menu_callbacks.inc"),
        "chat_entry_snippet_sha256": digest(OUTPUT / "actual_chat_entry.inc"),
        "toolchain": str(compiler), "results": results,
        "limits": "Narrow routing/dispatch/metadata test. Media methods are service substitutes; Worker playback behavior is covered separately by media_host. App transport/audio/state effects are substitutes around actual chat-entry guards. No OLED, GPIO, serial, network, device or user configuration access. This does not repeat the 44-case chat-control suite or display navigation acceptance.",
    }
    (OUTPUT / "result.json").write_text(json.dumps(receipt, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(OUTPUT / "result.json")
    return 0 if receipt["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
