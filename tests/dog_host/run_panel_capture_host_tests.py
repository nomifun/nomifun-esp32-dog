"""Exercise the actual capture wrapper with explicit host panel/RTOS/log stubs.

Uses installed MSVC only, no USB/network/build-tree access. The enabled variant
exports the real C++ protocol; the Python decoder validates it end to end. The
disabled variant proves no callback replacement or capture behavior is present.
"""

from pathlib import Path
import subprocess
import sys

from run_display_host_tests import ROOT, FIRMWARE, environment

sys.path.insert(0, str(ROOT / "tools"))
import capture_panel_frames as capture


STUBS = {
    "esp_err.h": """#pragma once
using esp_err_t = int;
constexpr int ESP_OK=0, ESP_ERR_INVALID_ARG=1, ESP_ERR_INVALID_STATE=2,
 ESP_ERR_NO_MEM=3, ESP_ERR_TIMEOUT=4, ESP_ERR_NOT_FINISHED=5, ESP_ERR_NOT_SUPPORTED=6;
""",
    "esp_lcd_panel_ops.h": """#pragma once
#include "esp_err.h"
struct esp_lcd_panel_t {
 esp_err_t (*draw_bitmap)(esp_lcd_panel_t*,int,int,int,int,const void*);
};
using esp_lcd_panel_handle_t = esp_lcd_panel_t*;
""",
    "esp_lcd_panel_interface.h": '#pragma once\n#include "esp_lcd_panel_ops.h"\n',
    "sdkconfig.h": "#pragma once\n",
    "esp_log.h": """#pragma once
void HostCaptureLog(const char* format, ...);
#define ESP_LOGI(tag, format, ...) HostCaptureLog(format, __VA_ARGS__)
""",
    "freertos/FreeRTOS.h": """#pragma once
using TickType_t=unsigned;
#define pdMS_TO_TICKS(ms) (ms)
#define pdTRUE 1
""",
    "freertos/semphr.h": """#pragma once
#include "FreeRTOS.h"
struct StaticSemaphore_t { bool held=false; };
using SemaphoreHandle_t=StaticSemaphore_t*;
inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t* value) {return value;}
inline int xSemaphoreTake(SemaphoreHandle_t value, TickType_t) {
 if(value->held) return 0;
 value->held=true; return pdTRUE;
}
inline void xSemaphoreGive(SemaphoreHandle_t value) {value->held=false;}
""",
}

HOST = r'''#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "panel_capture.h"
#include "esp_lcd_panel_interface.h"

FILE* log_file=nullptr;
int assertions=0;
void Check(bool condition, const char* reason) {
 ++assertions;
 if(!condition) {std::fprintf(stderr,"failed: %s\n",reason); std::exit(1);}
}
void HostCaptureLog(const char* format, ...) {
 va_list args; va_start(args,format); std::vfprintf(log_file,format,args); va_end(args);
 std::fputc('\n',log_file);
}
esp_err_t next_result=ESP_OK;
int draw_calls=0;
int last_x0=-1,last_y0=-1,last_x1=-1,last_y1=-1;
const void* last_data=nullptr;
bool request_during_draw=false;
esp_err_t Original(esp_lcd_panel_t*,int x0,int y0,int x1,int y1,const void* data) {
 ++draw_calls;last_x0=x0;last_y0=y0;last_x1=x1;last_y1=y1;last_data=data;
 if(request_during_draw) {
  request_during_draw=false;
  Check(RequestDogPanelFrame("requested-in-flight")==ESP_OK,"request during in-flight draw");
 }
 return next_result;
}

int main(int argc,char** argv) {
 Check(argc==2,"log path");
 log_file=std::fopen(argv[1],"wb");Check(log_file!=nullptr,"log open");
 esp_lcd_panel_t panel{Original};
#ifdef CONFIG_ZFSN_UI_CAPTURE
 Check(RequestDogPanelFrame("before-install")==ESP_ERR_INVALID_STATE,"request before install");
 Check(DumpDogPanelFrame()==ESP_ERR_INVALID_STATE,"dump before install");
 Check(InstallDogPanelCapture(nullptr)==ESP_ERR_INVALID_ARG,"invalid install");
 Check(InstallDogPanelCapture(&panel)==ESP_OK,"install");
 Check(InstallDogPanelCapture(&panel)==ESP_OK,"idempotent same panel");
 esp_lcd_panel_t another{Original};
 Check(InstallDogPanelCapture(&another)==ESP_ERR_INVALID_STATE,"single panel only");
 Check(DumpDogPanelFrame()==ESP_ERR_NOT_FINISHED,"no old frame exported");
 Check(RequestDogPanelFrame(nullptr)==ESP_ERR_INVALID_ARG,"null label");
 Check(RequestDogPanelFrame("")==ESP_ERR_INVALID_ARG,"empty label");
 Check(RequestDogPanelFrame("bad label")==ESP_ERR_INVALID_ARG,"unsafe label");
 Check(RequestDogPanelFrame(std::string(49,'a').c_str())==ESP_ERR_INVALID_ARG,"long label");
 std::array<unsigned char,1024> pixels{};
 for(int i=0;i<1024;++i) pixels[i]=static_cast<unsigned char>(i);
 Check(panel.draw_bitmap(&panel,0,0,128,64,pixels.data())==ESP_OK,"original success passthrough");
 Check(draw_calls==1&&last_data==pixels.data(),"original called with original buffer");
 Check(DumpDogPanelFrame()==ESP_ERR_NOT_FINISHED,"unrequested full frame ignored");
 Check(RequestDogPanelFrame("host-page-map")==ESP_OK,"request");
 Check(RequestDogPanelFrame("must-not-overwrite")==ESP_ERR_INVALID_STATE,"pending cannot overwrite");
 next_result=77;
 Check(panel.draw_bitmap(&panel,0,0,128,64,pixels.data())==77,"original failure passthrough");
 Check(DumpDogPanelFrame()==ESP_ERR_NOT_FINISHED,"failed frame not accepted");
 next_result=ESP_OK;
 Check(panel.draw_bitmap(&panel,3,8,67,16,pixels.data())==ESP_OK,"partial result passthrough");
 Check(last_x0==3&&last_y0==8&&last_x1==67&&last_y1==16,"partial bounds preserved");
 Check(DumpDogPanelFrame()==ESP_ERR_NOT_FINISHED,"partial frame not accepted");
 Check(panel.draw_bitmap(&panel,0,0,128,64,pixels.data())==ESP_OK,"fresh full frame accepted");
 Check(RequestDogPanelFrame("unread-overwrite")==ESP_ERR_INVALID_STATE,"unread cannot overwrite");
 pixels.fill(0); // Driver/LVGL buffer reuse cannot mutate the captured copy.
 Check(DumpDogPanelFrame()==ESP_OK,"dump first frame");
 Check(DumpDogPanelFrame()==ESP_ERR_NOT_FINISHED,"frame consumed exactly once");
 // The request made inside Original must not capture that already-running draw.
 request_during_draw=true;
 Check(panel.draw_bitmap(&panel,0,0,128,64,pixels.data())==ESP_OK,"in-flight original return");
 Check(DumpDogPanelFrame()==ESP_ERR_NOT_FINISHED,"in-flight old frame excluded");
 pixels.fill(0xff);
 Check(panel.draw_bitmap(&panel,0,0,128,64,pixels.data())==ESP_OK,"subsequent original return");
 Check(DumpDogPanelFrame()==ESP_OK,"dump fresh subsequent frame");
 Check(draw_calls==6,"all original calls retained");
#else
 Check(InstallDogPanelCapture(&panel)==ESP_ERR_NOT_SUPPORTED,"disabled install");
 Check(panel.draw_bitmap==Original,"disabled callback unchanged");
 Check(RequestDogPanelFrame("disabled")==ESP_ERR_NOT_SUPPORTED,"disabled request");
 Check(DumpDogPanelFrame()==ESP_ERR_NOT_SUPPORTED,"disabled dump");
#endif
 std::fclose(log_file);
 std::printf("host_assertions=%d\n",assertions);
 return 0;
}
'''


def main():
    output = ROOT / "build/dog_host/panel_capture_host"
    output.mkdir(parents=True, exist_ok=True)
    stubs = output / "stubs"
    for name, text in STUBS.items():
        path = stubs / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
    host = output / "host.cc"
    host.write_text('#include <string>\n' + HOST, encoding="utf-8")
    cl, _, env = environment()
    source = FIRMWARE / "main/boards/zfsn-esp32s3-dog/panel_capture.cc"
    for enabled in (True, False):
        name = "enabled" if enabled else "disabled"
        binary = output / f"{name}.exe"
        build = output / name
        build.mkdir(exist_ok=True)
        arguments = [str(cl), "/nologo", "/utf-8", "/std:c++17", "/EHsc", "/W4", "/WX",
                     "/D_CRT_SECURE_NO_WARNINGS", "/I" + str(stubs),
                     "/I" + str(source.parent), str(source), str(host), "/Fe" + str(binary),
                     "/Fo" + str(build) + "/"]
        if enabled:
            arguments.append("/DCONFIG_ZFSN_UI_CAPTURE=1")
        compiled = subprocess.run(arguments, cwd=build, env=env, capture_output=True, text=True)
        if compiled.returncode:
            print(compiled.stdout + compiled.stderr)
            return compiled.returncode
        captured = output / f"{name}.log"
        result = subprocess.run([str(binary), str(captured)], capture_output=True, text=True)
        print(name + ": " + result.stdout.strip())
        if result.returncode:
            print(result.stderr)
            return result.returncode
        if enabled:
            frames = capture.parse_capture_lines(captured.read_text(encoding="utf-8").splitlines())
            assert len(frames) == 2
            assert frames[0].data == bytes(range(256)) * 4
            assert (frames[0].request_after, frames[0].sequence) == (1, 2)
            assert frames[1].data == b"\xff" * 1024
            assert frames[1].label == "requested-in-flight"
            assert (frames[1].request_after, frames[1].sequence) == (2, 4)
            capture.write_frames(frames, output / "decoded")
        else:
            assert captured.read_bytes() == b""
    print("Actual C++ wrapper protocol decoded with matching fresh sequences, CRC and pixels.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
