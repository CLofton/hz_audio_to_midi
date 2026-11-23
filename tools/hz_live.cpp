#include "hz/live.hpp"

#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {

void usage() {
   std::cerr
      << "usage:\n"
      << "  hz_live --list\n"
      << "  hz_live --self-test\n"
      << "  hz_live --sine-test\n"
      << "  hz_live --run --input UID --midi-index N --seconds 5\n"
      << "  hz_live --run --input UID --virtual-out --seconds 5\n";
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
   std::vector<std::string> arguments;
   for (int index = 1; index < argc; ++index) arguments.emplace_back(argv[index]);
   if (!arguments.empty() && arguments[0] == "--run") {
      hz::LiveRunOptions options;
      std::string message;
      if (!hz::parse_live_run_arguments(arguments, options, message)) {
         std::cerr << message << '\n';
         usage();
         return 2;
      }
      hz::LiveRunReport report;
      bool const passed = hz::run_live(options, report, message);
      (passed ? std::cout : std::cerr) << message << '\n';
      std::cout << std::fixed << std::setprecision(3)
         << "capture: rate=" << report.sample_rate << " Hz channels=" << report.channels
         << " device-buffer=" << report.device_buffer_frames
         << " frames maximum-block=" << report.maximum_frames << '\n'
         << "queue: high-water=" << report.queue_high_water
         << " overflows=" << report.overflow_count
         << " render-errors=" << report.render_error_count
         << " discontinuities=" << report.discontinuity_count << '\n'
         << "processed: blocks=" << report.captured_blocks
         << " frames=" << report.captured_frames
         << " converter-total=" << report.converter_total_ms << " ms\n"
         << "diagnostic capture-buffer-to-MIDI-send-max="
         << report.max_capture_to_send_ms
         << " ms (not acoustic onset latency)\n";
      if (options.virtual_output) {
         std::cout << "virtual: words=" << report.virtual_words
            << " note-ons=" << report.virtual_note_ons
            << " note-offs=" << report.virtual_note_offs
            << " setup-prefix=" << (report.virtual_setup_verified ? "verified" : "failed") << '\n';
      }
      return passed ? 0 : 1;
   }
   usage();
   return 2;
}
