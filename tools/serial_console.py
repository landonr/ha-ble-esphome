#!/usr/bin/env python3
"""Talk to a device running the serial_console dev component over USB.

The logger's UART is bidirectional: logs stream out, and short command
lines written in are handed to the YAML on_command trigger. One port
handle does both (macOS serial devices are exclusive-open, so this can't
run alongside `esphome logs`).

Usage:
  serial_console.py --port /dev/cu.SLAB_USBtoUART                # tail logs
  serial_console.py --port ... --cmd "lamp toggle"               # send, watch 5 s
  serial_console.py --port ... --cmd "vol 40" --watch 10
  serial_console.py --port ... --watch 30 --grep "Connection params"
"""

import argparse
import os
import re
import sys
import time

import serial

ANSI = re.compile(r"\x1b\[[0-9;]*m")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", default="/dev/cu.SLAB_USBtoUART")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--cmd", action="append", default=[], help="command line to send (repeatable)")
    ap.add_argument("--watch", type=float, default=None,
                    help="seconds of log output to show (default: 5 with --cmd, forever without)")
    ap.add_argument("--grep", default=None, help="only print lines matching this regex")
    ap.add_argument("--fifo", default=None,
                    help="daemon mode: hold the port open forever and forward command lines "
                         "written to this FIFO (macOS CP210x resets the ESP32 on every port "
                         "open, so one long-lived open beats one open per command)")
    args = ap.parse_args()

    watch = args.watch if args.watch is not None else (5.0 if args.cmd else None)
    pattern = re.compile(args.grep) if args.grep else None

    # Open with DTR/RTS deasserted: on ESP32 dev boards those lines drive
    # EN/GPIO0 (auto-reset wiring), and pyserial asserts both on open, which
    # resets the chip or traps it in the bootloader.
    port = serial.Serial(None, args.baud, timeout=0.2)
    port.port = args.port
    port.dtr = False
    port.rts = False
    port.open()
    fifo_fd = None
    fifo_buf = b""
    if args.fifo:
        if not os.path.exists(args.fifo):
            os.mkfifo(args.fifo)
        fifo_fd = os.open(args.fifo, os.O_RDONLY | os.O_NONBLOCK)
        watch = None  # daemon runs until killed

    with port:
        port.reset_input_buffer()
        for cmd in args.cmd:
            port.write(cmd.encode() + b"\n")
            port.flush()
            time.sleep(0.05)  # device RX buffer is ~129 bytes; don't burst

        deadline = time.monotonic() + watch if watch is not None else None
        buf = b""
        try:
            while deadline is None or time.monotonic() < deadline:
                if fifo_fd is not None:
                    try:
                        fifo_buf += os.read(fifo_fd, 1024)
                    except BlockingIOError:
                        pass
                    while b"\n" in fifo_buf:
                        cmd_line, fifo_buf = fifo_buf.split(b"\n", 1)
                        if cmd_line.strip():
                            port.write(cmd_line.strip() + b"\n")
                            port.flush()
                            print(f">>> sent: {cmd_line.strip().decode(errors='replace')}", flush=True)
                buf += port.read(4096)
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    text = ANSI.sub("", line.decode(errors="replace")).rstrip()
                    if text and (pattern is None or pattern.search(text)):
                        print(text, flush=True)
        except KeyboardInterrupt:
            pass
        finally:
            if fifo_fd is not None:
                os.close(fifo_fd)
    return 0


if __name__ == "__main__":
    sys.exit(main())
