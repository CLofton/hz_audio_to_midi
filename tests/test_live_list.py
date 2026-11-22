#!/usr/bin/env python3
import subprocess
import sys

binary = sys.argv[1]
result = subprocess.run([binary, "--list"], text=True, capture_output=True, timeout=10)
if result.returncode != 0:
    print(result.stdout, end="")
    print(result.stderr, end="", file=sys.stderr)
    raise SystemExit("device enumeration failed")
if "MIDI destinations:" not in result.stdout:
    raise SystemExit("MIDI destination enumeration is missing")
if "Audio input devices:" not in result.stdout:
    raise SystemExit("CoreAudio input-device enumeration is missing")
print(result.stdout, end="")

for arguments in (
    [], ["--unknown"], ["--list", "--self-test"], ["--self-test", "extra"],
    ["--run"],
    ["--run", "--input", "UID"],
    ["--run", "--input", "UID", "--midi-index", "0"],
    ["--run", "--input", "UID", "--midi-index", "-1", "--seconds", "5"],
    ["--run", "--input", "UID", "--midi-index", "0", "--seconds", "0"],
    ["--run", "--input", "UID", "--midi-index", "0", "--seconds", "3601"],
    ["--run", "--input", "UID", "--midi-index", "0", "--seconds", "nan"],
    ["--run", "--midi-index", "0", "--input", "UID", "--seconds", "5"],
):
    rejected = subprocess.run(
        [binary, *arguments], text=True, capture_output=True, timeout=10)
    if rejected.returncode != 2 or "usage:" not in rejected.stderr:
        raise SystemExit(f"invalid mode selection was not rejected: {arguments}")
print("invalid and ambiguous CLI modes rejected")
