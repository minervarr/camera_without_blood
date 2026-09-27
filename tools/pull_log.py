#!/usr/bin/env python3
"""Pull the debug-only native app.log off a connected Android device.

Debug builds write native LOGI/LOGE/LOGD to
<externalDataPath>/app.log (and to logcat). Release builds do not create
the file. Default package is the :demo app.

    python3 tools/pull_log.py
    python3 tools/pull_log.py -o /tmp/app.log
    python3 tools/pull_log.py -p com.example.host -s SERIAL

Uses `adb exec-out` so Git Bash does not mangle /storage/... paths.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

DEFAULT_PACKAGE = "io.nava.camera.demo"
FALLBACK_REMOTE = "/sdcard/Download/app.log"


def adb(args: list[str], serial: str | None, *, capture: bool = True) -> subprocess.CompletedProcess:
    cmd = ["adb"]
    if serial:
        cmd += ["-s", serial]
    cmd += args
    env = os.environ.copy()
    env["MSYS_NO_PATHCONV"] = "1"
    return subprocess.run(
        cmd,
        env=env,
        capture_output=capture,
        check=False,
    )


def first_device() -> str | None:
    proc = adb(["devices"], serial=None)
    if proc.returncode != 0:
        sys.stderr.write(proc.stderr.decode("utf-8", errors="replace"))
        return None
    for line in proc.stdout.decode("utf-8", errors="replace").splitlines()[1:]:
        parts = line.split()
        if len(parts) >= 2 and parts[1] == "device":
            return parts[0]
    return None


def remote_exists(serial: str, path: str) -> bool:
    proc = adb(["shell", f"test -f '{path}' && echo yes"], serial)
    return proc.returncode == 0 and b"yes" in proc.stdout


def pull_exec_out(serial: str, remote: str, dest: Path) -> bool:
    proc = adb(["exec-out", f"cat '{remote}'"], serial)
    if proc.returncode != 0:
        err = proc.stderr.decode("utf-8", errors="replace").strip()
        sys.stderr.write(f"adb exec-out failed for {remote}: {err or proc.returncode}\n")
        return False
    if not proc.stdout:
        sys.stderr.write(f"{remote} is empty\n")
        return False
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_bytes(proc.stdout)
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "-p", "--package",
        default=DEFAULT_PACKAGE,
        help=f"host app package (default: {DEFAULT_PACKAGE})",
    )
    parser.add_argument(
        "-s", "--serial",
        default=None,
        help="adb device serial (default: first 'device' in adb devices)",
    )
    parser.add_argument(
        "-o", "--out",
        default="app.log",
        help="local destination (default: ./app.log)",
    )
    args = parser.parse_args()

    serial = args.serial or first_device()
    if not serial:
        sys.stderr.write("No adb device. Connect a phone with USB debugging on.\n")
        return 1

    dest = Path(args.out)
    candidates = [
        f"/storage/emulated/0/Android/data/{args.package}/files/app.log",
        FALLBACK_REMOTE,
    ]

    remote = next((p for p in candidates if remote_exists(serial, p)), None)
    if remote is None:
        sys.stderr.write(
            "No app.log on the device. Debug builds write it on launch; "
            "release builds do not. Looked at:\n"
        )
        for p in candidates:
            sys.stderr.write(f"  {p}\n")
        return 1

    if not pull_exec_out(serial, remote, dest):
        return 1

    print(f"{serial}: {remote} -> {dest.resolve()} ({dest.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
