#include "hz/midi2.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

using Midi2Translator = hz::Midi2EventTranslator;
static_assert(!std::is_copy_constructible_v<Midi2Translator>,
   "MIDI 2.0 translator copy construction would alias its packet sink");
static_assert(!std::is_copy_assignable_v<Midi2Translator>,
   "MIDI 2.0 translator copy assignment would alias its packet sink");
static_assert(!std::is_move_constructible_v<Midi2Translator>,
   "MIDI 2.0 translator move construction would alias its packet sink");
static_assert(!std::is_move_assignable_v<Midi2Translator>,
   "MIDI 2.0 translator move assignment would alias its packet sink");

namespace {

int g_failed = 0;

void expect(bool condition, char const* name) {
   if (condition) {
      std::cout << "  pass  " << name << "\n";
      return;
   }
   std::cout << "  FAIL  " << name << "\n";
   ++g_failed;
}

void test_note_on_uses_q_midi2_scaling() {
   std::cout << "test_note_on_uses_q_midi2_scaling\n";
   hz::Midi2EventTranslator translator;
   auto const packets = translator.translate({hz::MidiEvent{0, 1, 0x90, 69, 127}});
   expect(packets.size() == 1, "one MIDI 2.0 packet");
   if (packets.size() == 1) {
      expect(packets[0].words() == 2, "MIDI 2.0 voice packet is two words");
      expect(packets[0].word(0) == 0x40914500u, "exact A4 member-channel note-on word 0");
      expect(packets[0].word(1) == 0xFFFF0000u, "exact full-velocity note-on word 1");
   }
}

void test_note_off_uses_q_midi2_scaling() {
   std::cout << "test_note_off_uses_q_midi2_scaling\n";
   hz::Midi2EventTranslator translator;
   auto const packets = translator.translate({hz::MidiEvent{1, 1, 0x80, 69, 64}});
   expect(packets.size() == 1, "one MIDI 2.0 packet");
   if (packets.size() == 1) {
      expect(packets[0].word(0) == 0x40814500u, "exact A4 member-channel note-off word 0");
      expect(packets[0].word(1) == 0x80000000u, "exact release-velocity note-off word 1");
   }
}

void test_pitch_bend_center_is_exact() {
   std::cout << "test_pitch_bend_center_is_exact\n";
   hz::Midi2EventTranslator translator;
   auto const packets = translator.translate({hz::MidiEvent{0, 1, 0xE0, 0, 64}});
   expect(packets.size() == 1, "one MIDI 2.0 packet");
   if (packets.size() == 1) {
      expect(packets[0].word(0) == 0x40E10000u, "exact member-channel pitch-bend word 0");
      expect(packets[0].word(1) == 0x80000000u, "14-bit center scales to 32-bit center");
   }
}

void test_shared_mpe_setup_becomes_registered_controllers() {
   std::cout << "test_shared_mpe_setup_becomes_registered_controllers\n";
   hz::Midi2EventTranslator translator;
   auto const packets = translator.translate(hz::make_mpe_setup_events(1, 48));
   expect(packets.size() == 2, "eight shared RPN bytes become two MIDI 2.0 packets");
   if (packets.size() == 2) {
      expect(packets[0].word(0) == 0x40200006u, "manager RPN 6 packet word 0");
      expect(packets[0].word(1) == 0x02000000u, "one-member zone uses zero-extended value");
      expect(packets[1].word(0) == 0x40210000u, "member RPN 0 packet word 0");
      expect(packets[1].word(1) == 0x60000000u, "48-semitone bend range uses zero-extended value");
   }
}

void test_unsupported_status_has_clear_capability_error() {
   std::cout << "test_unsupported_status_has_clear_capability_error\n";
   hz::Midi2EventTranslator translator;
   try {
      (void)translator.translate({hz::MidiEvent{0, 1, 0xA0, 69, 100}});
      expect(false, "unsupported poly-pressure is rejected");
   } catch (std::invalid_argument const& error) {
      expect(std::string(error.what()) ==
         "MIDI 2.0 translator supports note-on, note-off, control-change, and pitch-bend only",
         "error names the supported event capabilities");
   }
}

void test_out_of_range_fields_have_clear_error() {
   std::cout << "test_out_of_range_fields_have_clear_error\n";
   std::vector<hz::MidiEvent> const invalid = {
      {0, 16, 0x90, 69, 100},
      {0, 1, 0x90, 128, 100},
      {0, 1, 0x90, 69, -1},
   };
   for (auto const& event : invalid) {
      hz::Midi2EventTranslator translator;
      try {
         (void)translator.translate({event});
         expect(false, "out-of-range channel/data is rejected");
      } catch (std::invalid_argument const& error) {
         expect(std::string(error.what()) ==
            "MIDI 2.0 translator event channel must be 0..15 and data must be 0..127",
            "error states the valid channel and data ranges");
      }
   }
}

void test_ordinary_control_change_is_not_dropped() {
   std::cout << "test_ordinary_control_change_is_not_dropped\n";
   hz::Midi2EventTranslator translator;
   auto const packets = translator.translate({hz::MidiEvent{0, 0, 0xB0, 7, 70}});
   expect(packets.size() == 1, "one ordinary control-change packet");
   if (packets.size() == 1) {
      expect(packets[0].word(0) == 0x40B00700u, "exact control-change word 0");
      expect(packets[0].word(1) == 0x8C30C30Cu, "Q scales controller value to 32 bits");
   }
}

void test_assignable_controller_is_not_dropped() {
   std::cout << "test_assignable_controller_is_not_dropped\n";
   hz::Midi2EventTranslator translator;
   auto const packets = translator.translate({
      hz::MidiEvent{0, 0, 0xB0, 99, 0x21},
      hz::MidiEvent{0, 0, 0xB0, 98, 0x09},
      hz::MidiEvent{0, 0, 0xB0, 6, 127},
      hz::MidiEvent{0, 0, 0xB0, 38, 127},
   });
   expect(packets.size() == 1, "NRPN sequence becomes one assignable-controller packet");
   if (packets.size() == 1) {
      expect(packets[0].word(0) == 0x40302109u, "exact assignable-controller word 0");
      expect(packets[0].word(1) == 0xFFFFFFFFu, "assignable-controller maximum scales exactly");
   }
}

void test_rejected_batch_does_not_mutate_rpn_state() {
   std::cout << "test_rejected_batch_does_not_mutate_rpn_state\n";
   hz::Midi2EventTranslator translator;
   try {
      (void)translator.translate({
         hz::MidiEvent{0, 0, 0xB0, 101, 0},
         hz::MidiEvent{0, 0, 0xB0, 100, 0},
         hz::MidiEvent{0, 0, 0xB0, 6, 12},
         hz::MidiEvent{0, 0, 0xA0, 69, 100},
      });
      expect(false, "batch with an unsupported status is rejected");
   } catch (std::invalid_argument const&) {
      auto const packets = translator.translate({hz::MidiEvent{0, 0, 0xB0, 38, 0}});
      expect(packets.empty(), "rejected batch leaves no pending registered controller");
   }
}

} // namespace

int main() {
   test_note_on_uses_q_midi2_scaling();
   test_note_off_uses_q_midi2_scaling();
   test_pitch_bend_center_is_exact();
   test_shared_mpe_setup_becomes_registered_controllers();
   test_ordinary_control_change_is_not_dropped();
   test_assignable_controller_is_not_dropped();
   test_unsupported_status_has_clear_capability_error();
   test_out_of_range_fields_have_clear_error();
   test_rejected_batch_does_not_mutate_rpn_state();
   if (g_failed != 0) {
      std::cerr << g_failed << " assertion(s) failed\n";
      return 1;
   }
   std::cout << "all MIDI 2.0 unit tests passed\n";
   return 0;
}
