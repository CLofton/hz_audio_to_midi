#pragma once

#include "hz/converter.hpp"

#include <cstdint>
#include <string>

namespace hz {

inline std::uint32_t midi1_channel_voice_ump(MidiEvent const& event) {
   return 0x20000000u
      | (std::uint32_t((event.status & 0xF0) | (event.channel & 0x0F)) << 16)
      | (std::uint32_t(event.data1 & 0x7F) << 8)
      | std::uint32_t(event.data2 & 0x7F);
}

bool run_midi_self_test(std::string& report);
bool run_midi_sine_test(std::string& report);

struct MidiDestinationInfo {
   int index = 0;
   std::string name;
   std::int32_t unique_id = 0;
};

std::vector<MidiDestinationInfo> enumerate_midi_destinations();

struct AudioInputDeviceInfo {
   std::uint32_t id = 0;
   std::string uid;
   std::string name;
   double sample_rate = 0.0;
   int input_channels = 0;
   bool is_default = false;
};

std::vector<AudioInputDeviceInfo> enumerate_audio_input_devices();

} // namespace hz
