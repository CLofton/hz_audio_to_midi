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
- `hz_convert` turns a sine or a 16-bit PCM WAV into that file.

This detector is the bootstrap, not the product detector. It is accurate on sines and good enough to lock the tracker and the MIDI path. It will miss what Q's BACF is for: missing fundamentals, octave jumps on a decaying guitar note, sub-cent period from zero-crossing edges, and a cost low enough for eight channels on a small MCU.

## Next, in order

1. **Q's `pitch_detector` behind the same tracker.** Needs CMake (not on this machine) and Q's headers. Keep the autocorrelation path as the test double so `make test` does not depend on Q. Swap only the estimate. Do not rewrite the note logic.
2. **Real divided-pickup recordings.** Six or eight channels, one string each, standard tuning through a two-octave range. The pass bar is the note Q's own guitar suite already uses, not another sine.
3. **Live I/O.** CoreAudio in, CoreMIDI out, on the Mac. Block size at or under the hop (1024 samples, about 21 ms at 48 kHz). Report end-to-end latency before touching hardware.
4. **MIDI 2.0 UMP.** Q already has UMP, per-note controllers, and MIDI-CI, and has been checked against the MIDI Association workbench. Translate the events we already emit. Do not write a second MIDI stack.
5. **The box.** Eight inputs, MIDI DIN, USB-C device. The firmware target is whatever already runs Q (STM32 with an FPU is the documented one; a Pico is the wrong host for eight channels of this detector). Schematic after the live Mac path is boring.

## Out of scope until the tracker is boring

Enclosure, battery, a custom ASIC, a DAW plugin. A plugin is a nice host for the same core later. It is not the product in the README.
