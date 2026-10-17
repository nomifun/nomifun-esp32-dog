"""Offline native Windows acceptance using actual board C++ and bundled LVGL.

No device, WSL, package installation or network access is used. The unchanged
board source is copied to the output build directory so quoted includes resolve
against explicit ESP/LvglDisplay/media/NVS substitutes. Production OledDisplay
is compiled too. Software preview uses actual LVGL RGB565 at 128x64 then the
port's blue > 16 inverted vertical-page conversion. The complete 14px Puhui C
font is a substitute; these previews are not device screenshots.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import zlib


ROOT = Path(__file__).resolve().parents[2]
TEST = Path(__file__).resolve().parent / "display_host"
FIRMWARE = ROOT
LVGL = FIRMWARE / "managed_components/lvgl__lvgl"
FONTS = FIRMWARE / "managed_components/78__xiaozhi-fonts"
PORT_SOURCE = FIRMWARE / "managed_components/espressif__esp_lvgl_port/src/lvgl9/esp_lvgl_port_disp.c"
VC_BASE = Path("C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC")
SDK = Path("C:/Program Files (x86)/Windows Kits/10")


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def tree_digest(paths):
    value = hashlib.sha256()
    for path in sorted(paths):
        value.update(str(path.relative_to(ROOT)).replace("\\", "/").encode())
        value.update(path.read_bytes())
    return value.hexdigest()


def environment():
    vc = sorted(VC_BASE.iterdir())[-1]
    sdk_version = sorted((SDK / "Lib").iterdir())[-1].name
    env = os.environ.copy()
    compiler = vc / "bin/Hostx64/x64"
    env["PATH"] = str(compiler) + os.pathsep + env["PATH"]
    env["INCLUDE"] = os.pathsep.join(str(path) for path in (
        vc / "include", SDK / "Include" / sdk_version / "ucrt",
        SDK / "Include" / sdk_version / "shared", SDK / "Include" / sdk_version / "um"))
    env["LIB"] = os.pathsep.join(str(path) for path in (
        vc / "lib/x64", SDK / "Lib" / sdk_version / "ucrt/x64", SDK / "Lib" / sdk_version / "um/x64"))
    return compiler / "cl.exe", compiler / "link.exe", env


def png(pgm):
    raw = pgm.read_bytes().split(b"\n", 3)[3]
    def chunk(tag, value):
        return struct.pack(">I", len(value)) + tag + value + struct.pack(">I", zlib.crc32(tag + value))
    rows = b"".join(b"\0" + raw[y*128:(y+1)*128] for y in range(64))
    data = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 128, 64, 8, 0, 0, 0, 0))
    data += chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b"")
    pgm.with_suffix(".png").write_bytes(data)


def software_preview(output):
    """Clearly labelled software output; never present it as a device capture."""
    try:
        from PIL import Image, ImageDraw, ImageFont
    except ImportError:
        return
    scale, cell_w, cell_h = 4, 540, 306
    frames=(("apps_0","Apps: chat"),("apps_1","Apps: emotion"),("music_0","Music"),("reset_selected","Network reset confirmation"))
    canvas = Image.new("RGB", (cell_w*2, 110+cell_h*len(frames)), "#e8edf2")
    draw = ImageDraw.Draw(canvas)
    font_path = Path("C:/Windows/Fonts/segoeui.ttf")
    font = ImageFont.truetype(str(font_path), 20) if font_path.exists() else ImageFont.load_default()
    draw.text((20,12),"SOFTWARE PREVIEW - NOT AN ON-DEVICE SCREENSHOT",fill="#9a2020",font=font)
    draw.text((20,40),"Real OLED layout + LVGL; C font substitute, ESP/media/NVS substitutes",fill="#17202a",font=font)
    draw.text((20,76),"Logical RGB565 (grayscale preview)",fill="#17202a",font=font)
    draw.text((cell_w+20,76),"OLED page bits: 1 lit / 0 dark",fill="#17202a",font=font)
    for row, (name, title) in enumerate(frames):
        for col, suffix in enumerate(("_logical", "")):
            file = output / (name + suffix + ".png")
            if file.exists():
                image = Image.open(file).convert("RGB").resize((128*scale, 64*scale), Image.Resampling.NEAREST)
                x, y = col*cell_w+14, 110+row*cell_h
                draw.text((x, y), title, fill="#17202a", font=font)
                canvas.paste(image, (x, y+32))
    canvas.save(output / "SOFTWARE_PREVIEW.png")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--phase", choices=("before", "after"), default="after")
    parser.add_argument("--source", type=Path, help="Saved source directory; defaults to actual current firmware")
    parser.add_argument("--output", type=Path, default=ROOT / "build/dog_host/display_host")
    args = parser.parse_args()
    source = (args.source or FIRMWARE / "main/boards/zfsn-esp32s3-dog").resolve()
    output = args.output.resolve() / args.phase
    output.mkdir(parents=True, exist_ok=True)
    for name in ("geometry.jsonl", "scrolling.jsonl"):
        (output / name).unlink(missing_ok=True)
    build = output / "build"
    build.mkdir(exist_ok=True)
    cache = args.output.resolve() / "lvgl-build"
    cache.mkdir(exist_ok=True)
    for name in ("display_board.cc", "display_board.h"):
        shutil.copy2(source / name, build / name)
    for name in ("oled_display.cc", "oled_display.h"):
        shutil.copy2(FIRMWARE / "main/display" / name, build / name)
    for header in source.glob("pixel_icons.h"):
        shutil.copy2(header,build / header.name)
    cl, link, env = environment()
    common = ["/nologo", "/utf-8", "/O1", "/W0", "/D_CRT_SECURE_NO_WARNINGS", "/DLV_CONF_INCLUDE_SIMPLE", "/DLV_KCONFIG_IGNORE",
              "/DBUILTIN_TEXT_FONT=font_puhui_14_1", "/DBUILTIN_ICON_FONT=font_awesome_14_1",
              "/I" + str(TEST), "/I" + str(TEST / "stubs"), "/I" + str(LVGL), "/I" + str(FONTS / "include")]
    diagnostics = []
    config_hash = tree_digest([TEST / "lv_conf.h", *(LVGL / "src").rglob("*.h"), *(FONTS / "include").glob("*.h")])
    c_sources = list((LVGL / "src").rglob("*.c")) + [FONTS / "src/font_puhui_14_1.c", FONTS / "src/font_awesome_30_1.c", FONTS / "src/font_awesome_14_1.c"]
    def compile_c(path):
        key = hashlib.sha256((str(path) + digest(path) + config_hash).encode()).hexdigest()[:24]
        obj = cache / (key + ".obj")
        if obj.exists():
            return obj, None
        result = subprocess.run([str(cl), *common, "/std:c11", "/c", str(path), "/Fo" + str(obj)],
                                env=env, cwd=cache, capture_output=True, text=True, encoding="utf-8", errors="replace")
        return obj, None if result.returncode == 0 else result.stdout + result.stderr
    print(f"Compiling/caching {len(c_sources)} actual LVGL/font sources with installed MSVC", flush=True)
    with ThreadPoolExecutor(max_workers=8) as pool:
        results = list(pool.map(compile_c, c_sources))
    objects = [obj for obj, error in results if not error]
    errors = [error for obj, error in results if error]
    status = 1
    stdout = stderr = ""
    if not errors:
        # MSVC encodes member access in mangled names; expose internals equally
        # in both TUs. This changes access control only, never production logic.
        prelude = (TEST / "test.cc").read_text(encoding="utf-8").split('#define private public')[0]
        port = PORT_SOURCE.read_text(encoding="utf-8")
        transform = port[port.index("static void _lvgl_port_transform_monochrome("):port.index("void lvgl_port_rotate_area(")]
        (build / "actual_port_transform.inc").write_text(transform,encoding="utf-8")
        wrapper = build / "board_for_test.cc"
        wrapper.write_text(prelude + '#define private public\n#define protected public\n#include "display_board.cc"\n'
                           + '#include "oled_display.cc"\n'
                           + 'struct lvgl_port_display_ctx_t { uint8_t* oled_buffer; };\n#include "actual_port_transform.inc"\n'
                           + 'void HostPortTransform(lv_display_t* display,const lv_area_t* area,uint8_t* buffer) { _lvgl_port_transform_monochrome(display,area,&buffer); }\n'
                           + 'bool HostValidGif(const std::vector<uint8_t>& bytes) { return ValidGif(bytes); }\n', encoding="utf-8")
        for path in (wrapper, TEST / "test.cc", FONTS / "src/font_awesome.c"):
            obj = build / (path.stem + ".obj")
            board_header=(build / "display_board.h").read_text(encoding="utf-8")
            interface_flags = ["/DDISPLAY_HOST_NEW_UI"] if "void ExitMenu()" in board_header else []
            if "menu_counter_" in board_header:
                interface_flags.append("/DDISPLAY_HOST_CAROUSEL")
            command = [str(cl), *common, *interface_flags, "/TP", "/std:c++20", "/EHsc", "/I" + str(build), "/c", str(path), "/Fo" + str(obj)]
            result = subprocess.run(command, env=env, cwd=build, capture_output=True, text=True, encoding="utf-8", errors="replace")
            diagnostics.append(result.stdout + result.stderr)
            if result.returncode:
                errors.append(result.stdout + result.stderr)
                break
            objects.append(obj)
    if not errors:
        exe = build / "display-host.exe"
        response = build / "link.rsp"
        response.write_text("\n".join('"' + str(p) + '"' for p in objects), encoding="utf-8")
        result = subprocess.run([str(link), "/nologo", "/OUT:" + str(exe), "@" + str(response)], env=env, cwd=build,
                                capture_output=True, text=True, encoding="utf-8", errors="replace")
        diagnostics.append(result.stdout + result.stderr)
        if result.returncode:
            errors.append(result.stdout + result.stderr)
        else:
            try:
                result = subprocess.run([str(exe), str(output), args.phase], env=env, cwd=build,
                                        capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=90)
                status, stdout, stderr = result.returncode, result.stdout, result.stderr
            except subprocess.TimeoutExpired:
                stderr = "Display host timed out after 90 seconds; LVGL assertion or infinite loop requires investigation"
    if errors:
        stderr = "\n".join(errors)
    (output / "build.log").write_text("\n".join(diagnostics + errors), encoding="utf-8")
    geometry = output / "geometry.jsonl"
    captures=[]
    if geometry.exists():
        for line in geometry.read_text(encoding="utf-8").splitlines():
            try:
                captures.append(json.loads(line)["capture"])
            except json.JSONDecodeError:
                # Preserve an assertion failure even if it interrupted the
                # final geometry record before its closing JSON delimiter.
                if status==0:
                    raise
    for name in captures:
        png(output / (name + ".pgm"))
        png(output / (name + "_logical.pgm"))
    if status == 0:
        software_preview(output)
    data = {"status": status, "phase": args.phase, "source": str(source), "toolchain": str(cl),
            "subject": "SOFTWARE PREVIEW: actual display_board.cc + actual oled_display.cc + bundled LVGL RGB565 rendering at 128x64, converted to OLED pagebits with actual blue > 16 inverted threshold; NOT DEVICE SCREENSHOT",
            "font": "existing font_puhui_14_1 C source, complete 14px Puhui, line height 18; not the 32-bit cbin font loader",
            "font_stage": "Layout preview substitutes complete C font for actual BUILTIN_TEXT_FONT basic font; Assets always unavailable, so early on-device font loading is not proven. Device font validation belongs to the on-device panel capture.",
            "substitutes": "LvglDisplay timer/lifecycle, font/theme wrappers, ESP port lifecycle/log/random, assets unavailable fallback, application state, codec volume, in-memory Settings; no device access",
            "limits": "Does not prove OLED driver, orientation, on-device font loading, physical buttons, timers, media/SD/NVS failures or physical device acceptance",
            "preview_warning": "All images here are SOFTWARE PREVIEWS with a C font substitute; actual device output must be checked separately with a panel capture",
            "lvgl_source_sha256": tree_digest(list((LVGL / "src").rglob("*.c")) + list((LVGL / "src").rglob("*.h"))),
            "stubs_sha256": tree_digest(list((TEST / "stubs").rglob("*.h"))),
            "hashes": {str(path.relative_to(ROOT)): digest(path) for path in (source / "display_board.cc", source / "display_board.h", FIRMWARE / "main/display/oled_display.cc", FIRMWARE / "main/display/oled_display.h", PORT_SOURCE, TEST / "test.cc", TEST / "lv_conf.h", FONTS / "src/font_puhui_14_1.c", *source.glob("pixel_icons.h"))},
            "stdout": stdout, "stderr": stderr, "png_count": len(captures), "captures": captures}
    (output / "result.json").write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8")
    print(stdout + stderr)
    print("Saved " + str(output / "result.json"))
    return status


if __name__ == "__main__":
    sys.exit(main())
