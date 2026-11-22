#!/usr/bin/env python3
import pathlib
import subprocess
import sys
import tempfile


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} PATH_TO_HZ_CONVERT_Q", file=sys.stderr)
        return 2
    executable = sys.argv[1]
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        output = pathlib.Path(tmp) / "a440-q.mid"
        result = subprocess.run(
            [executable, "--tone", "440", "--seconds", "0.5", "--out", str(output)],
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )
        if result.returncode != 0 or "detector=q" not in result.stdout:
            failures += 1
            print(
                f"FAIL Q CLI: returncode={result.returncode}, "
                f"stdout={result.stdout.strip()!r}, stderr={result.stderr.strip()!r}",
                file=sys.stderr,
            )
        data = output.read_bytes() if output.exists() else b""
        note69 = any(
            (data[i] & 0xF0) == 0x90 and data[i + 1] == 69 and data[i + 2] > 0
            for i in range(max(0, len(data) - 2))
        )
        if not data.startswith(b"MThd") or not note69:
            failures += 1
            print("FAIL Q CLI did not write MIDI note 69", file=sys.stderr)
    if not failures:
        print("Q CLI wrote A4 with detector=q")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
