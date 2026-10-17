"""Exercise the real SSID manager with host NVS failure injection; no device I/O."""

import os
from pathlib import Path
import subprocess
import tempfile

import test_saved_add as host
import test_provisioning_storage as storage

CASES = r'''
#include "ssid_manager.cc"
int main() {
    reset();
    auto& manager = SsidManager::GetInstance();
    auto seed = [&]() {
        fail_event.clear(); occurrence = 0;
        manager.Clear();
        check(manager.AddSsid("Home", "old-secret"), "Initial credentials did not save");
        events.clear();
    };
    seed();
    check(manager.AddSsid("New network", "new-secret"), "New credentials failed");
    check(stored["wifi"]["ssid"] == "New network" && stored["wifi"]["password"] == "new-secret", "New credentials not committed");
    check(manager.GetSsidList().front().ssid == "New network", "New SSID absent in memory");
    ++cases;
    check(manager.AddSsid("New network", "changed-secret"), "Existing credentials update failed");
    check(stored["wifi"]["password"] == "changed-secret", "Updated password not committed"); ++cases;

    for (const char* event : {"open:wifi", "set:wifi:ssid", "set:wifi:password",
                              "erase_key:wifi:ssid2", "erase_key:wifi:password2", "commit:wifi"}) {
        for (const bool existing : {false, true}) {
            seed();
            const auto previous = manager.GetSsidList();
            const auto previous_store = stored;
            fail_event = event; occurrence = 0; fail_occurrence = 1;
            // Existing one-entry updates clear suffix1; new two-entry writes clear suffix2.
            if (existing && fail_event == "erase_key:wifi:ssid2") fail_event = "erase_key:wifi:ssid1";
            if (existing && fail_event == "erase_key:wifi:password2") fail_event = "erase_key:wifi:password1";
            check(!manager.AddSsid(existing ? "Home" : "New network", "changed"), "Failed NVS persistence reported success");
            check(manager.GetSsidList().size() == previous.size(), "Failed save changed in-memory SSID count");
            check(manager.GetSsidList().front().ssid == previous.front().ssid &&
                  manager.GetSsidList().front().password == previous.front().password,
                  "Failed save changed in-memory credentials");
            check(stored == previous_store, "Failed transactional save changed committed values");
            check(handles.empty(), "NVS handle leaked on failed SSID save"); ++cases;
        }
    }
    for (const char* event : {"open:wifi", "set:wifi:ssid", "set:wifi:password",
                              "erase_key:wifi:ssid1", "erase_key:wifi:password1", "commit:wifi"}) {
        seed();
        check(manager.AddSsid("Second", "other-secret"), "Failed to prepare two saved networks");
        const auto previous = manager.GetSsidList();
        const auto previous_store = stored;
        fail_event = event; occurrence = 0; fail_occurrence = 1;
        check(!manager.RemoveSsid(0), "Failed delete reported success");
        check(manager.GetSsidList().size() == previous.size() &&
              manager.GetSsidList()[0].ssid == previous[0].ssid &&
              manager.GetSsidList()[1].ssid == previous[1].ssid,
              "Failed delete changed in-memory network list");
        check(stored == previous_store, "Failed delete changed committed credentials");
        check(handles.empty(), "Delete failure leaked NVS handle"); ++cases;
    }
    seed();
    check(manager.AddSsid("Second", "other-secret"), "Failed to prepare deletion");
    check(manager.RemoveSsid(0), "Valid delete failed");
    check(manager.GetSsidList().size() == 1 && manager.GetSsidList()[0].ssid == "Home" &&
          stored["wifi"]["ssid"] == "Home" && stored["wifi"].count("ssid1") == 0,
          "Successful deletion was not durable"); ++cases;
    for (int index : {-1, 1, 100}) {
        const auto previous_store = stored;
        check(!manager.RemoveSsid(index), "Invalid deletion index reported success");
        check(stored == previous_store && manager.GetSsidList().size() == 1, "Invalid deletion changed state"); ++cases;
    }
    seed();
    check(stored["wifi"]["ota_url"] == "https://old.example/ota", "SSID save changed OTA URL");
    check(stored["petdog"]["angle_settings"] == "right-rear-adjusted", "SSID save changed calibration"); ++cases;
    std::cout << cases << " SSID persistence cases passed\n";
}
'''


def main():
    env = os.environ.copy()
    compiler_bin = host.VC / "bin" / "Hostx64" / "x64"
    env["PATH"] = str(compiler_bin) + os.pathsep + env["PATH"]
    env["INCLUDE"] = os.pathsep.join(str(path) for path in
        (host.VC / "include", host.SDK / "Include" / host.SDK_VERSION / "ucrt",
         host.SDK / "Include" / host.SDK_VERSION / "shared", host.SDK / "Include" / host.SDK_VERSION / "um"))
    env["LIB"] = os.pathsep.join(str(path) for path in
        (host.VC / "lib" / "x64", host.SDK / "Lib" / host.SDK_VERSION / "ucrt" / "x64",
         host.SDK / "Lib" / host.SDK_VERSION / "um" / "x64"))
    with tempfile.TemporaryDirectory(prefix="ssid-host-", dir=Path(__file__).parent) as temporary:
        directory = Path(temporary)
        (directory / "nvs.h").write_text(storage.NVS_HEADER + '\nconstexpr int NVS_READONLY = 0;\n', encoding="utf-8")
        (directory / "nvs_flash.h").write_text('#include "nvs.h"\n', encoding="utf-8")
        (directory / "esp_log.h").write_text('#define ESP_LOGI(...) ((void)0)\n#define ESP_LOGW(...) ((void)0)\n#define ESP_LOGE(...) ((void)0)\n', encoding="utf-8")
        test = directory / "ssid_host.cc"
        test.write_text(storage.SOURCE.split('int main() {')[0] + CASES, encoding="utf-8")
        exe = directory / "ssid_host.exe"
        json_source = host.IDF / "components" / "json" / "cJSON"
        result = subprocess.run([str(compiler_bin / "cl.exe"), "/nologo", "/std:c++17", "/EHsc", "/utf-8",
                                 "/D_CRT_SECURE_NO_WARNINGS", "/I" + str(directory),
                                 "/I" + str(host.COMPONENT), "/I" + str(host.COMPONENT / "include"),
                                 "/I" + str(json_source), str(test), str(json_source / "cJSON.c"),
                                 "/Fe:" + str(exe), "/Fo:" + str(directory) + os.sep],
                                env=env, cwd=directory, capture_output=True, text=True, encoding="utf-8", errors="replace")
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        subprocess.run([str(exe)], cwd=directory, check=True)


if __name__ == "__main__":
    main()
