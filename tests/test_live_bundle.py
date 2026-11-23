#!/usr/bin/env python3
from pathlib import Path
import plistlib
import subprocess
import sys

normal = Path(sys.argv[1])
app = Path(sys.argv[2])
plist_path = app / "Contents/Info.plist"
executable = app / "Contents/MacOS/HzLive"
if not normal.is_file():
    raise SystemExit(f"normal hz_live binary is missing: {normal}")
if not executable.is_file():
    raise SystemExit(f"HzLive app executable is missing: {executable}")
if not plist_path.is_file():
    raise SystemExit(f"HzLive Info.plist is missing: {plist_path}")
with plist_path.open("rb") as stream:
    plist = plistlib.load(stream)
if plist.get("CFBundleIdentifier") != "com.clofton.hzlive":
    raise SystemExit("HzLive has the wrong or missing CFBundleIdentifier")
if plist.get("CFBundleExecutable") != "HzLive":
    raise SystemExit("HzLive CFBundleExecutable does not match its binary")
usage = plist.get("NSMicrophoneUsageDescription", "")
for phrase in ("bounded", "memory", "not recorded"):
    if phrase not in usage:
        raise SystemExit(f"microphone usage description is not truthful/complete: missing {phrase}")
for binary in (normal, executable):
    linked = subprocess.run(
        ["otool", "-L", str(binary)], text=True, capture_output=True, timeout=10)
    if linked.returncode != 0:
        raise SystemExit(linked.stderr)
    for framework in ("AVFoundation.framework", "Foundation.framework"):
        if framework not in linked.stdout:
            raise SystemExit(f"{binary} does not link {framework}")
verified = subprocess.run(
    ["codesign", "--verify", "--strict", "--verbose=2", str(app)],
    text=True, capture_output=True, timeout=15)
if verified.returncode != 0:
    raise SystemExit(f"HzLive ad-hoc signature verification failed: {verified.stderr}")
details = subprocess.run(
    ["codesign", "-dv", "--verbose=2", str(app)],
    text=True, capture_output=True, timeout=15)
identity = details.stdout + details.stderr
if "Identifier=com.clofton.hzlive" not in identity:
    raise SystemExit("signed HzLive identity does not match CFBundleIdentifier")
print("HzLive.app bundle metadata, AVFoundation linkage, and ad-hoc signature passed")
