#!/usr/bin/env python3
"""Read-only, timestamped capture of the MBB console.

Usage: tools/capture.py [DEVICE] [LOGFILE]
  DEVICE defaults to the first CP2102 found: /dev/cu.usbserial-* on macOS,
  /dev/ttyUSB* on Linux. With a DevKit on the same Mac that can be the
  DevKit's own bridge, so name the adapter's device when both are attached.
  LOGFILE defaults to logs/mbb-capture-DATE_TIME.log.

Nothing is sent to the bike. Every line is stamped with the local time at
its first byte, NULs and carriage returns are dropped, and a partial line
such as the prompt is written out after two seconds of silence. The file
is flushed per line, so it survives a pulled cable or a Ctrl-C. If the
adapter disappears the script waits for it to come back and notes the gap.
"""
import argparse
import datetime
import glob
import os
import signal
import sys
import time

import serial

BAUD = 115200
IDLE_FLUSH = 2.0


def find_device():
    for pattern in ("/dev/serial/by-id/*", "/dev/cu.usbserial-*", "/dev/ttyUSB*"):
        found = sorted(glob.glob(pattern))
        if found:
            return found[0]
    return None


def _stop(*_):
    raise KeyboardInterrupt


def stamp():
    return datetime.datetime.now().strftime("%Y-%m-%dT%H:%M:%S.%f")[:-3]


def main():
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser(description="Read-only, timestamped capture of the MBB console.")
    ap.add_argument("device", nargs="?", help="serial device (default: the first CP2102 found)")
    ap.add_argument("logfile", nargs="?", help="output file (default: logs/mbb-capture-DATE_TIME.log)")
    args = ap.parse_args()
    dev = args.device or find_device()
    if not dev:
        sys.exit("capture.py: no serial device found; pass one as the first argument")
    if not dev.startswith("/dev/"):
        sys.exit("capture.py: %r does not look like a serial device" % dev)
    if args.logfile:
        path = args.logfile
    else:
        os.makedirs(os.path.join(repo, "logs"), exist_ok=True)
        path = os.path.join(repo, "logs", "mbb-capture-%s.log"
                            % datetime.datetime.now().strftime("%Y-%m-%d_%H%M%S"))
    print("capture.py: %s, logging to %s (Ctrl-C to stop)" % (dev, path), file=sys.stderr)
    signal.signal(signal.SIGTERM, _stop)

    with open(path, "a", buffering=1, encoding="utf-8") as log:
        echo = [True]

        def emit(text):
            log.write(text + "\n")
            if echo[0]:
                try:
                    print(text, flush=True)
                except BrokenPipeError:   # stdout went away; the file is what matters
                    echo[0] = False

        emit("%s capture.py: start %s" % (stamp(), dev))
        buf = bytearray()
        started = None
        last = time.monotonic()
        waiting_since = None

        def flush_partial():
            if buf:
                emit("%s %s" % (started, buf.decode("ascii", "backslashreplace")))
                buf.clear()

        try:
            while True:
                try:
                    port = serial.Serial(dev, BAUD, timeout=0.2)
                except (serial.SerialException, OSError) as exc:
                    if waiting_since is None:
                        waiting_since = time.monotonic()
                        emit("%s capture.py: cannot open %s (%s), retrying every 5 s"
                             % (stamp(), dev, exc))
                    time.sleep(5)
                    continue
                with port:
                    if waiting_since is None:
                        emit("%s capture.py: open %s" % (stamp(), dev))
                    else:
                        emit("%s capture.py: open %s after %.0f s away"
                             % (stamp(), dev, time.monotonic() - waiting_since))
                        waiting_since = None
                    try:
                        while True:
                            # in_waiting raises a bare OSError when the USB device vanishes
                            data = port.read(port.in_waiting or 1)
                            if not data:
                                if buf and time.monotonic() - last > IDLE_FLUSH:
                                    flush_partial()
                                    started = None
                                continue
                            last = time.monotonic()
                            for b in data:
                                if b in (0, 13):
                                    continue
                                if started is None:
                                    started = stamp()
                                if b == 10:
                                    flush_partial()
                                    started = None
                                else:
                                    buf.append(b)
                    except (serial.SerialException, OSError) as exc:
                        flush_partial()   # what arrived before the loss, in order
                        started = None
                        emit("%s capture.py: lost %s (%s)" % (stamp(), dev, exc))
                        time.sleep(2)
        except KeyboardInterrupt:
            flush_partial()
            emit("%s capture.py: stop" % stamp())


if __name__ == "__main__":
    main()
