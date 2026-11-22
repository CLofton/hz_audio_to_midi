#include "hz/converter.hpp"
#include "hz/live.hpp"

#include <cstdint>
#include <iostream>

int main() {
   hz::MidiEvent event{0, 1, 0x90, 69, 100};
   std::uint32_t const expected =
      0x20000000u | (std::uint32_t(0x91) << 16)
      | (std::uint32_t(69) << 8) | std::uint32_t(100);
   if (hz::midi1_channel_voice_ump(event) != expected) {
      std::cerr << "MIDI 1 channel-voice UMP word did not match the protocol layout\n";
      return 1;
   }
   std::cout << "MIDI 1 channel-voice UMP encoding passed\n";
   return 0;
}
