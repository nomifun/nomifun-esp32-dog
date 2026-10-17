"""Compile real MCP dispatch and dog HTTP handlers against offline substitutes.

Uses the installed MSVC/cJSON already used by provisioning host tests. No device,
network, firmware flashing or ESP-IDF installation occurs. Selected production
function bodies are copied verbatim, with real public headers and parsers.
"""

import os
from pathlib import Path
import subprocess
import sys
import tempfile


TESTS = Path(__file__).resolve().parent
SOURCE = TESTS.parents[1]
BOARD = SOURCE / "main/boards/zfsn-esp32s3-dog"
sys.path.insert(0, str(SOURCE / "components/78__esp-wifi-connect/tests"))
import test_saved_add as host


def section(source, start, end=None):
    begin = source.index(start)
    return source[begin:source.index(end, begin) if end else len(source)]


def main():
    env = os.environ.copy()
    compiler_bin = host.VC / "bin/Hostx64/x64"
    env["PATH"] = str(compiler_bin) + os.pathsep + env["PATH"]
    env["INCLUDE"] = os.pathsep.join(str(path) for path in (
        host.VC / "include", host.SDK / "Include" / host.SDK_VERSION / "ucrt",
        host.SDK / "Include" / host.SDK_VERSION / "shared", host.SDK / "Include" / host.SDK_VERSION / "um"))
    env["LIB"] = os.pathsep.join(str(path) for path in (
        host.VC / "lib/x64", host.SDK / "Lib" / host.SDK_VERSION / "ucrt/x64",
        host.SDK / "Lib" / host.SDK_VERSION / "um/x64"))
    mcp = (SOURCE / "main/mcp_server.cc").read_text(encoding="utf-8")
    web = (BOARD / "web_control_server.cc").read_text(encoding="utf-8")
    portal = (SOURCE / "components/78__esp-wifi-connect/wifi_configuration_ap.cc").read_text(encoding="utf-8")
    delete_begin = portal.index(".handler = ", portal.index("httpd_uri_t saved_delete")) + len(".handler = ")
    delete_end = portal.index(",\n        .user_ctx = ", delete_begin)
    implementations = {
        "mcp": section(mcp, "void McpServer::DoToolCall("),
        "web": "\n".join([
            section(web, "std::string PrintJson(", "void Cors("),
            section(web, "bool FormBoolean(", "void RebootAfterResponse("),
            section(web, "esp_err_t WebControlServer::Move(", "esp_err_t WebControlServer::Emotion("),
            section(web, "esp_err_t WebControlServer::CustomEmotion(", "esp_err_t WebControlServer::WifiSettings("),
            section(web, "esp_err_t WebControlServer::WifiSettings(", "esp_err_t WebControlServer::SettingsHandler("),
            section(web, "esp_err_t WebControlServer::SettingsHandler(", "esp_err_t WebControlServer::OtaUpload("),
        ]),
        "saved_delete": "auto handler = " + portal[delete_begin:delete_end] + ";\n",
    }
    with tempfile.TemporaryDirectory(prefix="dog-control-host-") as temporary:
        directory = Path(temporary)
        (directory / "mbedtls").mkdir()
        (directory / "mbedtls/base64.h").write_text(
            "#pragma once\n#include <cstddef>\ninline int mbedtls_base64_encode(unsigned char*, size_t, size_t*, const unsigned char*, size_t) { return 0; }\n",
            encoding="utf-8")
        (directory / "esp_http_server.h").write_text((TESTS / "control_host/http_stub.h").read_text(encoding="utf-8"), encoding="utf-8")
        for name, implementation in implementations.items():
            template = (TESTS / "control_host" / (name + "_test.cc")).read_text(encoding="utf-8")
            assert template.count("// INSERT_PRODUCTION_FUNCTIONS") == 1
            test = directory / (name + "_test.cc")
            test.write_text(template.replace("// INSERT_PRODUCTION_FUNCTIONS", implementation), encoding="utf-8")
            executable = directory / (name + "_test.exe")
            cjson = host.IDF / "components/json/cJSON"
            result = subprocess.run([
                str(compiler_bin / "cl.exe"), "/nologo", "/std:c++20", "/EHsc", "/utf-8",
                "/D_CRT_SECURE_NO_WARNINGS", "/I" + str(directory), "/I" + str(cjson),
                "/I" + str(SOURCE / "main"), "/I" + str(BOARD), str(test), str(cjson / "cJSON.c"),
                "/Fe:" + str(executable), "/Fo:" + str(directory) + os.sep,
            ], env=env, cwd=directory, capture_output=True, text=True, encoding="utf-8", errors="replace")
            if result.returncode:
                raise RuntimeError(result.stdout + result.stderr)
            subprocess.run([str(executable)], cwd=directory, check=True)
    print("Scope: host dispatch/validation/side-effect assertions; real ESP timing, storage, OTA and hardware require device acceptance.")


if __name__ == "__main__":
    main()
