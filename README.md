# Multichannel Pitch-to-MIDI Converter

**This fork is a build, not only a prospectus.** The plan and the order of work are in [PLAN.md](PLAN.md).

```
make test
make demo
```

`make demo` writes `build/a440.mid`: a 440 Hz tone as MPE note 69, manager on MIDI channel 1, the note on channel 2. A 16-bit PCM WAV uses the same engine:

```
./build/hz_convert --wav take.wav --out take.mid
```

One monophonic tracker per input. A single mixed pickup cannot produce chord MIDI. The default binary uses bootstrap autocorrelation. The optional Q binary uses Cycfi Q's BACF `pitch_detector` behind the same tracker and MIDI path.

Keep Q as an external dependency (verified with v1.5.4, commit `efe26114f39e4a78bd67c1456a25487d356fb910`):

```sh
git clone --recurse-submodules https://github.com/cycfi/Q.git ../Q
git -C ../Q checkout efe26114f39e4a78bd67c1456a25487d356fb910
git -C ../Q submodule update --init --recursive
make q-test Q_PATH=../Q
make q-demo Q_PATH=../Q
```

`make q-demo` writes `build/a440_q.mid`. `make q-test` runs all 18 Q recordings (open, 12th, and 24th fret) plus a six-channel divided-audio take, checking every note and MPE member channel. WAV input accepts 16-bit PCM and 32-bit IEEE float. Use `--low-hz` and `--high-hz` to set the detector range; the High E 24th-fret case uses `263.7..1648 Hz`. The ordinary `make test` neither fetches nor includes Q.

## Native live-I/O slice

On macOS, `make live-test` builds the C++20/CoreMIDI/CoreAudio slice and runs software-only integration tests. It links AudioToolbox, CoreAudio, CoreMIDI, and CoreFoundation; Q is not required.

```sh
./build/hz_live --list
./build/hz_live --self-test
./build/hz_live --sine-test
# Only after selecting an input and destination from --list:
./build/hz_live --run --input UID --midi-index N --seconds 5
```

`--list` reads device properties only: CoreAudio input UID/name/rate/channel count/default marker and CoreMIDI destination index/name/unique ID. It does not create an audio unit or open the microphone. The self-test and sine-test send only to a temporary software virtual destination, never an external MIDI destination. They verify exact MPE setup and note UMP words; virtual send-to-receive latency is diagnostic, not a timing pass/fail gate. `make live-test` never executes `--run`.

Live mode is opt-in and requires an exact input UID, an external MIDI destination index, and a finite duration in `(0, 3600]` seconds. It requests a 512-frame device buffer, accepts only 48 kHz and 1–8 input channels, and rejects devices whose reported variable block range can exceed 1024 frames. AUHAL renders float32 packed noninterleaved audio into a fixed 16-slot SPSC ring; a worker performs conversion and sends UMP with CoreMIDI timestamp 0. Overflow, render errors, oversized blocks, and sample discontinuities are fatal. Shutdown stops capture, drains the worker, sends final note-offs, and joins before disposing the unit.

The run report includes actual device buffer size/rate, queue high-water and failures, converter time, and maximum capture-buffer-to-MIDI-send time. The latter is a pipeline diagnostic, **not acoustic onset latency**; hardware streaming and end-to-end acoustic latency still require a user-selected microphone and MIDI destination to verify.

![Hz Pitch to MIDI](https://github.com/cycfi/hz_audio_to_midi/blob/master/docs/images/MIDI_Guitar-16-9.jpg?raw=true)

This project aims to create an open-source multichannel pitch-to-MIDI converter with 8 audio inputs, one MIDI output, and one USB-C output, leveraging the Hz Pitch Detection system (part of the Q DSP library) for precise and efficient pitch tracking The device will convert audio signals from multiple sources into MIDI (MPE and MIDI-2) in real time with minimal latency, providing a high-quality and customizable solution for electric guitars, basses, and other instruments.

- [Pitch Perfect: Enhanced Pitch Detection Techniques Part 1](https://www.cycfi.com/2024/09/pitch-perfect-enhanced-pitch-detection-techniques-part-1/)
- [Pitch Perfect: Enhanced Pitch Detection Techniques Part 2](https://www.cycfi.com/2024/10/pitch-perfect-enhanced-pitch-detection-techniques-part-2/)
- [Q DSP Library](https://github.com/cycfi/q)

This is a nascent project, and the initial feasibility assessment will be gauged on engagement, so your involvement is crucial in these early stages. If this idea resonates with you, give the project a star:

<img width="164" alt="image" src="https://github.com/user-attachments/assets/b9cb3fbb-1a0e-4403-939c-368b0f9cd1cd">

[Join in and introduce yourself](https://github.com/cycfi/hz_audio_to_midi/discussions/2). Share your background, your interest in pitch-to-MIDI conversion, and whether you see yourself as a potential user of this product. This will help us gauge community interest and understand how many people might benefit from this project. I'll set a modest target of 500 interested people and let's grow it from there. I encourage you to stay informed by clicking the Watch button to receive notifications: 

<img width="146" alt="image" src="https://github.com/user-attachments/assets/46ed30ef-df34-4129-aafd-254a7c8be8b1">

**And by all means, spread the word!**

As Andreas Szalay, creator of the Fishman Triple Play, noted, it took six years to release the product, and many years later after release, software issues persist. I can lay the groundwork for a multichannel audio-to-MIDI solution, but fine-tuning for various applications, instruments, and sensors will need broader community involvement. Together, we can bring this project to life and adapt it for various applications and instruments.

## Features
- 8 audio inputs for multiple sources (e.g., divided pickups, piezo pickups, Roland GK3, Nu Multi)
- Real-time audio to MIDI (MPE and MIDI-2) conversion
- Minimal latency for accurate performance
- MIDI and USB outputs

## Community Collaboration
This project is open-source and relies on community interest and collaboration to succeed. If you're interested in contributing, discussing ideas, or joining the development, please be part of the [discussions](https://github.com/cycfi/hz_audio_to_midi/discussions). Even the very act of joining and keeping company will fuel enthusiasm not only for the developers but also for the community as a whole. Your presence and support, whether through active contribution or simply engaging in discussions, can have a huge impact. It helps build momentum, keeps everyone motivated, and fosters a sense of belonging and shared purpose. Every bit of participation, no matter how small, contributes to the overall success and growth of this project.


## License
This project is licensed under the GPL version 3 - see the LICENSE file for details.
