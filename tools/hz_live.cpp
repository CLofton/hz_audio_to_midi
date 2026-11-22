#include "hz/live.hpp"

#include <iostream>
#include <string>

namespace {

void usage() {
   std::cerr
      << "usage:\n"
      << "  hz_live --list\n"
      << "  hz_live --self-test\n"
      << "  hz_live --sine-test\n";
}

} // namespace

int main(int argc, char** argv) {
   if (argc == 2 && std::string(argv[1]) == "--list") {
      try {
         std::cout << "MIDI destinations:\n";
         for (auto const& destination : hz::enumerate_midi_destinations()) {
            std::cout << "  [" << destination.index << "] name=\"" << destination.name
                      << "\" uniqueID=" << destination.unique_id << '\n';
         }
         std::cout << "Audio input devices:\n";
         for (auto const& device : hz::enumerate_audio_input_devices()) {
            std::cout << "  deviceID=" << device.id << " name=\"" << device.name
                      << "\" uid=\"" << device.uid << "\" rate=" << device.sample_rate
                      << " channels=" << device.input_channels
                      << " default=" << (device.is_default ? "yes" : "no") << '\n';
         }
         return 0;
      } catch (std::exception const& error) {
         std::cerr << "enumeration failed: " << error.what() << '\n';
         return 1;
      }
   }
   if (argc == 2 && std::string(argv[1]) == "--self-test") {
      std::string report;
      bool const passed = hz::run_midi_self_test(report);
      (passed ? std::cout : std::cerr) << report << '\n';
      return passed ? 0 : 1;
   }
   if (argc == 2 && std::string(argv[1]) == "--sine-test") {
      std::string report;
      bool const passed = hz::run_midi_sine_test(report);
      (passed ? std::cout : std::cerr) << report << '\n';
      return passed ? 0 : 1;
   }
   usage();
   return 2;
}
