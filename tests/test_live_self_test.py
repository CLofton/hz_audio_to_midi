#!/usr/bin/env python3
import subprocess
import sys

binary = sys.argv[1]
result = subprocess.run([binary, "--self-test"], text=True, capture_output=True, timeout=10)
if result.returncode != 0:
    print(result.stdout, end="")
    print(result.stderr, end="", file=sys.stderr)
    raise SystemExit("virtual CoreMIDI self-test failed")
if "self-test passed" not in result.stdout:
    raise SystemExit("self-test did not report exact virtual-loopback verification")
if "virtual destination enumerated" not in result.stdout:
    raise SystemExit("self-test did not discover its own virtual MIDI destination")
print(result.stdout, end="")

result = subprocess.run([binary, "--sine-test"], text=True, capture_output=True, timeout=15)
if result.returncode != 0:
    print(result.stdout, end="")
    print(result.stderr, end="", file=sys.stderr)
    raise SystemExit("synthetic streaming CoreMIDI sine-test failed")
if "sine-test passed" not in result.stdout:
    raise SystemExit("sine-test did not report note/event verification")
print(result.stdout, end="")
