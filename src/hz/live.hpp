#pragma once

#include "hz/converter.hpp"
#include "hz/live_stream.hpp"

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

struct LiveRunReport {
   std::uint32_t device_buffer_frames = 0;
   std::uint32_t maximum_frames = 0;
   double sample_rate = 0.0;
   int channels = 0;
   std::uint64_t captured_blocks = 0;
   std::uint64_t captured_frames = 0;
   std::uint32_t queue_high_water = 0;
   std::uint32_t overflow_count = 0;
   std::uint32_t render_error_count = 0;
   std::uint32_t discontinuity_count = 0;
   double converter_total_ms = 0.0;
   double max_capture_to_send_ms = 0.0;
   std::uint64_t virtual_words = 0;
   std::uint64_t virtual_note_ons = 0;
   std::uint64_t virtual_note_offs = 0;
   bool virtual_setup_verified = false;
};

// Opens the selected input only after explicit validation. Output is either the
// selected external index or a private software virtual destination, never both.
bool run_live(LiveRunOptions const& options, LiveRunReport& report, std::string& message);

} // namespace hz
