#!/usr/bin/env python3
"""Bounded private serial capture, with an optional USB-JTAG application reset.

No flash, provisioning, movement or HTTP requests are issued by this tool.
Run only when no other process holds the device's serial port. Raw logs may
contain credentials and stay under .private/backups with a restricted directory.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
import re
import sys
import time

import device_workflow as workflow


def summarize(data):
    text = re.sub(r"\x1b\[[0-9;]*m", "", data.decode("utf-8", "replace"))
    def first(pattern):
        match = re.search(pattern, text, re.M | re.I)
        return match[1].strip() if match else None

    states = re.findall(r"Application:\s*STATE:\s*([a-z_]+)", text, re.I)
    # Only selected fields are printed. Never echo arbitrary serial lines,
    # SSIDs, passwords, tokens or activation codes to the public tool output.
    fields = {
        "project_name": first(r"Project name:\s*([^\r\n]+)"),
        "app_version": first(r"App version:\s*([^\r\n]+)"),
        "elf_sha256": first(r"ELF file SHA256:\s*([a-f0-9]+)"),
        "idf_version": first(r"ESP-IDF:\s*([^\r\n]+)"),
        "boot_mode": first(r"boot:0x[0-9a-f]+\s*\(([^)]+)\)"),
        "reset_reason": first(r"rst:0x[0-9a-f]+\s*\(([^)]+)\)"),
        "board_initialized": "ZFSN-ESP32S3-DOG initialized" in text,
        "http_started": "HTTP server started successfully" in text,
        "got_ip": bool(re.search(r"(?:got ip|sta ip|IP_EVENT_STA_GOT_IP|WiFi connected event received)", text, re.I)),
        "ipv4": first(r"(?:got ip:|sta ip:)\s*((?:\d{1,3}\.){3}\d{1,3})"),
        "states": list(dict.fromkeys(states)),
        "sd_mount_failed": "Failed to mount SD card:" in text,
        "psram_mb": first(r"Found\s+(\d+)\s*MB PSRAM"),
        "display_init_failed": bool(re.search(r"(?:Failed to initialize display|esp_lcd_new_panel_io_i2c_v2 failed|i2c_new_master_bus failed)", text)),
        "assets_error": bool(re.search(r"(?:Failed to mmap assets partition|does not match the stored checksum|The index.json file is not (?:found|valid)|Failed to load srmodels.bin|Failed to load fonts.bin)", text)),
        "ota_upgrade_started": "Starting firmware upgrade from URL:" in text,
        "ota_current_is_latest": "Current is the latest version" in text,
        "panic_count": len(re.findall(r"Guru Meditation Error|panic'ed|abort\(\) was called|assert failed:", text, re.I)),
        "backtrace_count": text.count("Backtrace:"),
        "watchdog_count": len(re.findall(r"Task watchdog got triggered|Interrupt wdt timeout|TG[01]WDT_SYS_RST", text, re.I)),
        "brownout_detected": bool(re.search(r"Brownout detector was triggered|BROWNOUT_RST", text, re.I)),
        "download_mode": bool(re.search(r"waiting for download|DOWNLOAD\(USB|DOWNLOAD_BOOT", text, re.I)),
    }
    return fields


def same_device(record, original):
    if (record.get("vid"), record.get("pid")) != (original.get("vid"), original.get("pid")):
        return False
    serial = original.get("serial_number")
    return record.get("serial_number") == serial if serial else record["device"].casefold() == original["device"].casefold()


def open_stream(port, baud):
    import serial
    stream = serial.Serial(port=None, baudrate=baud, timeout=0.1)
    # Avoid accidental download mode on open. HardReset below is deliberate.
    stream.dtr = False
    stream.rts = False
    stream.port = port
    stream.open()
    return stream


def capture(args):
    import serial
    from esptool.reset import HardReset

    original = workflow.require_usb_port(args.port)
    workflow.require((original["vid"], original["pid"]) == (0x303A, 0x1001),
                     "This capture tool requires ESP32 USB JTAG/serial VID=303a PID=1001")
    workflow.require(1 <= args.seconds <= 300, "Capture duration must be 1..300 seconds")
    passive = getattr(args, "no_reset", False)
    directory = workflow.BACKUPS / (datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ") + ("-serial" if passive else "-boot"))
    workflow.private_directory(directory)
    stream = None
    chunks = []
    total = 0
    reconnects = 0
    limit_hit = False
    error = None
    started = time.monotonic()
    try:
        stream = open_stream(args.port, args.baud)
        if not passive:
            stream.reset_input_buffer()
            # USBJTAGSerialReset enters download mode. HardReset is the upstream
            # esptool strategy for booting the installed application from ROM.
            HardReset(stream, uses_usb=True)()
        deadline = time.monotonic() + args.seconds
        with (directory / "serial.log").open("wb") as raw:
            while time.monotonic() < deadline:
                try:
                    block = stream.read(min(max(stream.in_waiting, 1), 65536))
                    if block:
                        raw.write(block)
                        raw.flush()
                        chunks.append(block)
                        total += len(block)
                        if total >= 4 * 1024 * 1024:
                            limit_hit = True
                            break
                except (serial.SerialException, OSError):
                    stream.close()
                    # USB Serial/JTAG may re-enumerate on reset. Follow only
                    # this USB identity, and never reset again on reconnect.
                    while time.monotonic() < deadline:
                        matches = [record for record in workflow.list_ports() if same_device(record, original)]
                        workflow.require(len(matches) <= 1, "USB identity became ambiguous during capture")
                        if matches:
                            try:
                                stream = open_stream(matches[0]["device"], args.baud)
                                reconnects += 1
                                break
                            except (serial.SerialException, OSError):
                                pass
                        time.sleep(0.2)
                    else:
                        error = "USB serial did not reconnect before capture deadline"
                        break
    except Exception as exc:
        # Keep the private failure text while public output remains bounded.
        error = f"{type(exc).__name__}: {exc}"
    finally:
        if stream is not None:
            stream.close()

    data = b"".join(chunks)
    # Diagnostics remain private and include original lines for ELF decoding.
    lines = data.decode("utf-8", "replace").splitlines()
    interesting = []
    for index, line in enumerate(lines):
        if re.search(r"(?:^|\x1b\[[0-9;]*m)E \(|Guru Meditation|Backtrace:|panic'ed|abort\(\)|assert failed:|Core\s+[0-9]+ register dump|PC\s*:|EXCCAUSE|EXCVADDR|watchdog|Brownout", line, re.I):
            interesting.append({"line": index + 1, "text": line})
    workflow.save_json(directory / "diagnostics.json", {"error": error, "lines": interesting})
    summary = {"schema": 1, "created_utc": workflow.utc_now(), "port": original["device"],
               "reset_strategy": "passive_no_reset" if passive else "esptool.reset.HardReset(uses_usb=True)",
               "seconds": round(time.monotonic() - started, 3), "bytes": total,
               "reconnects": reconnects, "limit_hit": limit_hit,
               "capture_error": error is not None, **summarize(data)}
    workflow.save_json(directory / "summary.json", summary)
    print(json.dumps({"private_directory": str(directory), "summary": summary}, ensure_ascii=False, indent=2))
    return 2 if error or limit_hit else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--seconds", type=float, default=30)
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--no-reset", action="store_true", help="Read live output without resetting or discarding buffered data")
    args = parser.parse_args()
    workflow.require(0 < args.baud <= 2000000, "Invalid baud rate")
    return capture(args)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (workflow.WorkflowError, OSError, ValueError) as exc:
        print(f"Stopped: {exc}", file=sys.stderr)
        sys.exit(2)
