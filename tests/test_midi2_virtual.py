#!/usr/bin/env python3
import subprocess
import sys

binary = sys.argv[1]
result = subprocess.run([binary, "--self-test"], text=True, capture_output=True, timeout=10)
if result.returncode != 0:
    print(result.stdout, end="")
    print(result.stderr, end="", file=sys.stderr)
    raise SystemExit("MIDI 2.0 protocol-2 virtual self-test failed")
if "midi2 self-test passed" not in result.stdout:
    raise SystemExit("MIDI 2.0 self-test did not report exact virtual-loopback verification")
if "protocol 2 virtual destination enumerated" not in result.stdout:
    raise SystemExit("MIDI 2.0 self-test did not discover its protocol-2 virtual destination")
if "8 exact words" not in result.stdout:
    raise SystemExit("MIDI 2.0 self-test did not verify the exact golden word count")
print(result.stdout, end="")

result = subprocess.run([binary, "--sine-test"], text=True, capture_output=True, timeout=15)
if result.returncode != 0:
    print(result.stdout, end="")
    print(result.stderr, end="", file=sys.stderr)
    raise SystemExit("MIDI 2.0 converted-sine virtual test failed")
if "midi2 sine-test passed" not in result.stdout:
    raise SystemExit("MIDI 2.0 sine-test did not report exact converted-event verification")
if "10 exact words" not in result.stdout or "final note-off" not in result.stdout:
    raise SystemExit("MIDI 2.0 sine-test did not verify A4, bend, and final note-off goldens")
print(result.stdout, end="")
