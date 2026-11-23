#include "hz/live_virtual.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

bool check(bool condition, char const* message) {
   if (!condition) std::cerr << message << '\n';
   return condition;
}

std::vector<std::uint32_t> words_of(std::vector<hz::MidiEvent> const& events) {
   std::vector<std::uint32_t> words;
   for (auto const& event : events) words.push_back(hz::midi1_channel_voice_ump(event));
   return words;
}

} // namespace

int main() {
   try {
      hz::VirtualMidiOutput output;
      bool ok = check(output.destination_is_enumerated(),
         "persistent protocol-1 virtual destination was not enumerated");

      auto setup = hz::make_mpe_setup_events(1, 48);
      std::vector<hz::MidiEvent> notes{
         {0, 1, 0x90, 69, 100},
         {1, 1, 0x80, 69, 0}
      };
      output.send(setup);
      output.send(notes);
      auto expected_events = setup;
      expected_events.insert(expected_events.end(), notes.begin(), notes.end());
      auto expected_words = words_of(expected_events);
      auto delivered = output.wait_for_delivery(expected_words.size(), std::chrono::seconds(2));
      ok = check(delivered.words == expected_words,
         "asynchronous setup/note delivery differed or was cleared between sends") && ok;
      ok = check(delivered.note_ons == 1 && delivered.note_offs == 1,
         "virtual note-on/off counts did not match the first delivery") && ok;

      std::vector<hz::MidiEvent> const second_notes{
         {2, 1, 0x90, 72, 90},
         {3, 1, 0x80, 72, 0}
      };
      output.send(second_notes);
      expected_events.insert(expected_events.end(), second_notes.begin(), second_notes.end());
      expected_words = words_of(expected_events);
      delivered = output.wait_for_delivery(expected_words.size(), std::chrono::seconds(2));
      ok = check(delivered.words == expected_words,
         "persistent virtual stream did not preserve the full cumulative words") && ok;
      ok = check(delivered.note_ons == 2 && delivered.note_offs == 2,
         "persistent virtual note counts did not accumulate") && ok;
      if (ok) {
         std::cout << "persistent protocol-1 virtual delivery passed: "
            << delivered.words.size() << " words, " << delivered.note_ons
            << " note-ons, " << delivered.note_offs << " note-offs\n";
      }
      return ok ? 0 : 1;
   } catch (std::exception const& error) {
      std::cerr << "persistent virtual delivery failed: " << error.what() << '\n';
      return 1;
   }
}
