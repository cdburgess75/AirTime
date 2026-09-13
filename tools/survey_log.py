#!/usr/bin/env python3
"""Timestamped serial logger for the AirTime RDS station survey.

Every line arriving from the device is prefixed with the Mac's wall clock
(UNIX seconds, millisecond resolution). That timestamp IS the measurement:
the survey firmware prints each clock-time group the instant it leaves the
radio's FIFO, so line-arrival time minus the asserted minute is the station's
offset from truth (tools/survey_report.py does the arithmetic). Keep the Mac
NTP-synced while this runs — System Settings > General > Date & Time >
"Set time and date automatically".

Usage:
    python3 tools/survey_log.py /dev/cu.usbmodemXXXX [outfile]

Stop with Ctrl-C. Default outfile: survey_<start-time>.log in the current
directory. Uses pyserial, which the esptool install already provided.
"""

import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial missing — run: python3 -m pip install pyserial")


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    port = sys.argv[1]
    out_path = sys.argv[2] if len(sys.argv) > 2 else f"survey_{int(time.time())}.log"

    ser = serial.Serial(port, 115200, timeout=1)
    ser.reset_input_buffer()
    print(f"logging {port} -> {out_path}  (Ctrl-C to stop)")

    with open(out_path, "a") as out:
        try:
            while True:
                raw = ser.readline()
                t = time.time()  # stamp immediately, before any processing
                if not raw:
                    continue
                line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
                if not line:
                    continue
                stamped = f"{t:.3f} {line}"
                print(stamped)
                out.write(stamped + "\n")
                out.flush()
        except KeyboardInterrupt:
            print(f"\nstopped. log: {out_path}")
        finally:
            ser.close()


if __name__ == "__main__":
    main()
