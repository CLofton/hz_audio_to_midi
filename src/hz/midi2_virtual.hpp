#pragma once

#include <string>

namespace hz {

// Software-only CoreMIDI protocol-2 loopback. No external MIDI endpoint is used.
bool run_midi2_self_test(std::string& report);
bool run_midi2_sine_test(std::string& report);

} // namespace hz
