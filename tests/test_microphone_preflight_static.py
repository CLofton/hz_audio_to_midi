#!/usr/bin/env python3
from pathlib import Path
import sys

root = Path(__file__).resolve().parent.parent
live = (root / "src/hz/live.cpp").read_text()
run_start = live.index("bool run_live(")
self_test_start = live.index("bool run_midi_self_test(")
run = live[run_start:self_test_start]
errors = []
if run.count("ensure_microphone_permission") != 1:
    errors.append("run_live must perform exactly one microphone permission preflight")
if "std::chrono::seconds(30)" not in run:
    errors.append("microphone permission wait must be bounded at 30 seconds")
if "ensure_microphone_permission" in run and "kAudioDevicePropertyBufferFrameSizeRange" in run:
    if run.index("ensure_microphone_permission") > run.index("kAudioDevicePropertyBufferFrameSizeRange"):
        errors.append("permission preflight occurs after AUHAL/device setup")
if live.count("ensure_microphone_permission") != run.count("ensure_microphone_permission"):
    errors.append("a non-run mode references microphone permission preflight")

implementation = root / "src/hz/microphone_permission_darwin.mm"
if not implementation.exists():
    errors.append("AVFoundation microphone permission implementation is missing")
else:
    objective_c = implementation.read_text()
    for required in (
        "authorizationStatusForMediaType", "requestAccessForMediaType",
        "AVMediaTypeAudio", "NSMicrophoneUsageDescription", "NSRunLoop",
    ):
        if required not in objective_c:
            errors.append(f"permission implementation is missing {required}")

if errors:
    for error in errors:
        print(error, file=sys.stderr)
    raise SystemExit(1)
print("microphone preflight is run-only, bounded, and ordered before AUHAL setup")
