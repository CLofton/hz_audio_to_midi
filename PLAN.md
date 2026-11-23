# Build plan

The upstream repository is a product statement: eight audio inputs, one MIDI out, one USB-C out, MPE and MIDI 2.0, built on Cycfi Q's Hz pitch detector. It has no code. This fork starts the engine.

## Decision

Build the conversion core on the Mac first. The same tracker later sits on a microcontroller. Do not draw the analog front end until a divided-pickup recording survives this tracker.

One monophonic tracker per input. Chords require a divided source (hex pickup, Roland GK, Nu Multi, per-string piezo). A single mixed output cannot do this. That is the difference from `pico_guitar_to_midi`, which reads one ADC pin.

Q stays a dependency. It is MIT; this repository is GPL-3. Do not copy Q in.

## What is running now

`src/hz/converter.hpp` is the engine. `make test` is the contract.

- Hopping normalized autocorrelation, first local peak, so a clean tone reports its fundamental.
- Per channel: onset, release, retrigger on a stable pitch jump, MPE pitch bend at ±48 semitones.
- MIDI channel 1 is the MPE manager. Input N speaks on MIDI channel N+1.
- A complete take releases any note still held. A later live stream can pass `end_of_take=false`.
- `write_midi` emits a Type 0 SMF: tempo, manager RPN 6 zone sizing, per-member bend-range RPN, then the notes.
- `hz_convert` turns a sine, a 16-bit PCM WAV, or a 32-bit IEEE-float WAV into that file with bootstrap autocorrelation.
- `hz_convert_q` uses Q's `pitch_detector` behind the same tracker. `make q-test Q_PATH=/path/to/Q` and `make q-demo Q_PATH=/path/to/Q` build the optional path; Q stays external and is not needed by `make test`. `--low-hz` and `--high-hz` set the detector range.

- `hz_live --list` performs native CoreAudio/CoreMIDI property enumeration without creating AUHAL or opening the microphone. `--self-test` and `--sine-test` use only a temporary software virtual MIDI destination and verify exact MPE/note UMP delivery; virtual latency is reported diagnostically.
- Opt-in `hz_live --run` implements AUHAL capture into a fixed SPSC ring, worker conversion, external CoreMIDI output, CoreAudio timestamp-gap detection, and drained final note-offs. It requires an explicit input UID, destination index, and bounded duration. No hardware stream has been run.
- `Midi2EventTranslator` uses Q's `to_midi2` and UMP packet words. `make midi2-test Q_PATH=/path/to/Q` verifies golden MIDI 2.0 note/bend/controller words and protocol-2 virtual delivery of shared MPE setup, a streamed A4, and final note-off. No external receiver has been tested.
- `write_midi` and live output share `make_mpe_setup_events`, so manager/member RPN configuration cannot diverge.

The bootstrap detector remains the default test double. Q v1.5.4 is verified on clean tones, 256-sample live blocks with carried overlap, all 18 open/12th/24th-fret recordings, and a six-channel divided-audio take with the expected note on each MPE member. The Q CLI uses three-hop onset confirmation to reject startup transients; the High E 24th-fret case uses Q's per-string `263.7..1648 Hz` range.

## Next, in order

1. **Hardware acceptance.** Select a microphone and MIDI destination explicitly, validate AUHAL timestamps and shutdown, then measure real audio-to-MIDI latency. Test a MIDI 2.0 receiver before relying on protocol negotiation.
2. **The box.** Eight inputs, MIDI DIN, USB-C device. The firmware target is whatever already runs Q (STM32 with an FPU is the documented one; a Pico is the wrong host for eight channels of this detector). Schematic after the live Mac path is boring.

## Out of scope until the tracker is boring

Enclosure, battery, a custom ASIC, a DAW plugin. A plugin is a nice host for the same core later. It is not the product in the README.
