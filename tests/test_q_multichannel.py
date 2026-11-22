#!/usr/bin/env python3
import array
import pathlib
import subprocess
import sys
import tempfile
import wave

NAMES = ["1a-Low-E.wav", "2a-A.wav", "3a-D.wav", "4a-G.wav", "5a-B.wav", "6a-High-E.wav"]
EXPECTED = list(enumerate([40, 45, 50, 55, 59, 64], start=1))


def read_vlq(data: bytes, index: int) -> tuple[int, int]:
    value = 0
    while True:
        byte = data[index]
        index += 1
        value = (value << 7) | (byte & 0x7F)
        if not byte & 0x80:
            return value, index


def note_ons(path: pathlib.Path) -> list[tuple[int, int]]:
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
        data1 = data[index]
        data2 = data[index + 1] if data_bytes == 2 else 0
        index += data_bytes
        if status & 0xF0 == 0x90 and data2 > 0:
            notes.append((status & 0x0F, data1))
    return notes


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} PATH_TO_HZ_CONVERT_Q Q_CHECKOUT", file=sys.stderr)
        return 2
    executable = sys.argv[1]
    audio = pathlib.Path(sys.argv[2]) / "test" / "audio_files"
    rate = 44100
    frames = rate * 2
    tracks = []
    for name in NAMES:
        with wave.open(str(audio / name), "rb") as source:
            if source.getnchannels() != 1 or source.getsampwidth() != 2 or source.getframerate() != rate:
                print(f"unexpected WAV format: {name}", file=sys.stderr)
                return 1
            track = array.array("h")
            track.frombytes(source.readframes(frames))
        if len(track) != frames:
            print(f"short WAV: {name}", file=sys.stderr)
            return 1
        tracks.append(track)

    with tempfile.TemporaryDirectory() as tmp:
        wav_path = pathlib.Path(tmp) / "six-open-strings.wav"
        midi_path = pathlib.Path(tmp) / "six-open-strings.mid"
        interleaved = array.array("h", (tracks[c][f] for f in range(frames) for c in range(6)))
        with wave.open(str(wav_path), "wb") as output:
            output.setnchannels(6)
            output.setsampwidth(2)
            output.setframerate(rate)
            output.writeframes(interleaved.tobytes())
        result = subprocess.run(
            [executable, "--wav", str(wav_path), "--out", str(midi_path)],
            capture_output=True,
            text=True,
            timeout=60,
            check=False,
        )
        notes = sorted(note_ons(midi_path)) if midi_path.exists() else []
    if result.returncode != 0 or notes != EXPECTED:
        print(
            f"FAIL six-channel take: expected {EXPECTED}, got {notes}; "
            f"returncode={result.returncode}; stderr={result.stderr.strip()!r}",
            file=sys.stderr,
        )
        return 1
    print("six-channel real-audio take produced the expected note on each MPE member")
    return 0


if __name__ == "__main__":
    sys.exit(main())
