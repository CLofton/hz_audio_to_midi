#!/usr/bin/env python3
import pathlib
import subprocess
import sys
import tempfile


def test_invalid_numeric_arguments(executable: str) -> int:
    cases = [
        ["--tone", "nan"],
        ["--tone", "inf"],
        ["--tone", "-inf"],
        ["--tone", "-440"],
        ["--tone", "20001"],
        ["--tone", "not-a-number"],
        ["--tone", "440junk"],
        ["--tone", "440", "--seconds", "nan"],
        ["--tone", "440", "--seconds", "inf"],
        ["--tone", "440", "--seconds", "-1"],
        ["--tone", "440", "--seconds", "0"],
        ["--tone", "440", "--seconds", "1e30"],
        ["--tone", "440", "--sps", "nan"],
        ["--tone", "440", "--sps", "inf"],
        ["--tone", "440", "--sps", "-48000"],
        ["--tone", "440", "--sps", "0"],
        ["--tone", "440", "--sps", "1e30"],
        ["--tone", "440", "--seconds", "60", "--sps", "192000"],
        ["--tone", "440", "--seconds", "0.01", "--low-hz", "0.000001"],
        ["--tone", "440", "--low-hz", "19.999"],
        ["--tone", "440", "--low-hz", "0"],
        ["--tone", "440", "--low-hz", "-1"],
        ["--tone", "440", "--low-hz", "nan"],
        ["--tone", "440", "--low-hz", "inf"],
        ["--tone", "440", "--low-hz", "1e30"],
        ["--tone", "440", "--high-hz", "0"],
        ["--tone", "440", "--high-hz", "-1"],
        ["--tone", "440", "--high-hz", "nan"],
        ["--tone", "440", "--high-hz", "inf"],
        ["--tone", "440", "--high-hz", "20001"],
        ["--tone", "440", "--high-hz", "1e30"],
        ["--tone", "440", "--low-hz", "440", "--high-hz", "440"],
        ["--tone", "440", "--low-hz", "2000", "--high-hz", "50"],
    ]
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        for index, args in enumerate(cases):
            output = pathlib.Path(tmp) / f"invalid-{index}.mid"
            try:
                result = subprocess.run(
                    [executable, *args, "--out", str(output)],
                    capture_output=True,
                    text=True,
                    timeout=2,
                    check=False,
                )
            except subprocess.TimeoutExpired:
                failures += 1
                print(f"FAIL {' '.join(args)}: timed out", file=sys.stderr)
                continue
            if result.returncode != 2 or not result.stderr.strip() or output.exists():
                failures += 1
                print(
                    f"FAIL {' '.join(args)}: returncode={result.returncode}, "
                    f"stderr={result.stderr.strip()!r}, output_created={output.exists()}",
                    file=sys.stderr,
                )
    if failures == 0:
        print(f"all {len(cases)} invalid numeric CLI cases rejected cleanly")
    return failures


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} PATH_TO_HZ_CONVERT", file=sys.stderr)
        sys.exit(2)
    sys.exit(1 if test_invalid_numeric_arguments(sys.argv[1]) else 0)
