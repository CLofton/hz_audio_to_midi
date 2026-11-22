#!/usr/bin/env python3
import pathlib
import subprocess
import sys
import tempfile

EXPECTED = {
    "1a-Low-E.wav": 40,
    "1b-Low-E-12th.wav": 52,
    "1c-Low-E-24th.wav": 64,
    "2a-A.wav": 45,
    "2b-A-12th.wav": 57,
    "2c-A-24th.wav": 69,
    "3a-D.wav": 50,
    "3b-D-12th.wav": 62,
    "3c-D-24th.wav": 74,
    "4a-G.wav": 55,
    "4b-G-12th.wav": 67,
    "4c-G-24th.wav": 79,
    "5a-B.wav": 59,
    "5b-B-12th.wav": 71,
    "5c-B-24th.wav": 83,
    "6a-High-E.wav": 64,
    "6b-High-E-12th.wav": 76,
    "6c-High-E-24th.wav": 88,
}


def read_vlq(data: bytes, index: int) -> tuple[int, int]:
    value = 0
    while True:
        byte = data[index]
        index += 1
        value = (value << 7) | (byte & 0x7F)
        if not byte & 0x80:
            return value, index


def note_ons(path: pathlib.Path) -> list[int]:
    data = path.read_bytes()
    index = data.index(b"MTrk") + 8
    running = 0
    notes = []
    while index < len(data):
        _, index = read_vlq(data, index)
        if index >= len(data):
            break
        status = data[index]
        if status < 0x80:
            status = running
        else:
            running = status
            index += 1
        if status == 0xFF:
            meta = data[index]
            index += 1
            length, index = read_vlq(data, index)
            index += length
            if meta == 0x2F:
                break
            continue
        data_bytes = 1 if status & 0xF0 in (0xC0, 0xD0) else 2
        if index + data_bytes > len(data):
            break
        data1 = data[index]
        data2 = data[index + 1] if data_bytes == 2 else 0
        index += data_bytes
        if status & 0xF0 == 0x90 and data2 > 0:
            notes.append(data1)
    return notes


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} PATH_TO_HZ_CONVERT_Q Q_CHECKOUT", file=sys.stderr)
        return 2
    executable = sys.argv[1]
    audio = pathlib.Path(sys.argv[2]) / "test" / "audio_files"
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        for name, expected in EXPECTED.items():
            source = audio / name
            output = pathlib.Path(tmp) / f"{name}.mid"
            arguments = [executable, "--wav", str(source), "--out", str(output)]
            if name == "6c-High-E-24th.wav":
                arguments += ["--low-hz", "263.7", "--high-hz", "1648"]
            result = subprocess.run(
                arguments,
                capture_output=True,
                text=True,
                timeout=60,
                check=False,
            )
            notes = note_ons(output) if output.exists() else []
            if result.returncode != 0 or notes != [expected]:
                failures += 1
                print(
                    f"FAIL {name}: expected [{expected}], got {notes}; "
                    f"returncode={result.returncode}; stderr={result.stderr.strip()!r}",
                    file=sys.stderr,
                )
    if not failures:
        print("all 18 Q recordings produced exactly the expected MIDI note")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
