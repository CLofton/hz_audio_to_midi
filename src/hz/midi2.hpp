#pragma once

#include "hz/converter.hpp"

#include <q/midi/translate.hpp>

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace hz {

// Optional Q-backed translation from the converter's MIDI 1.0-shaped events
// to native MIDI 2.0 channel voice UMPs. Q owns packet layout and scaling.
class Midi2EventTranslator {
public:
   Midi2EventTranslator() = default;
   Midi2EventTranslator(Midi2EventTranslator const&) = delete;
   Midi2EventTranslator& operator=(Midi2EventTranslator const&) = delete;
   Midi2EventTranslator(Midi2EventTranslator&&) = delete;
   Midi2EventTranslator& operator=(Midi2EventTranslator&&) = delete;

   std::vector<cycfi::q::midi_2_0::packet> translate(
      std::vector<MidiEvent> const& events
   ) {
      namespace midi1 = cycfi::q::midi_1_0;
      for (auto const& event : events) {
         if (event.channel < 0 || event.channel > 15
            || event.data1 < 0 || event.data1 > 127
            || event.data2 < 0 || event.data2 > 127) {
            throw std::invalid_argument(
               "MIDI 2.0 translator event channel must be 0..15 and data must be 0..127");
         }
         if (event.status != 0x80 && event.status != 0x90
            && event.status != 0xE0 && event.status != 0xB0) {
            throw std::invalid_argument(
               "MIDI 2.0 translator supports note-on, note-off, control-change, and pitch-bend only");
         }
      }
      sink_.packets.clear();
      for (auto const& event : events) {
         if (event.status == 0x80) {
            chain_(midi1::note_off{
               static_cast<std::uint8_t>(event.channel),
               static_cast<std::uint8_t>(event.data1),
               static_cast<std::uint8_t>(event.data2)}, 0);
         } else if (event.status == 0x90) {
            chain_(midi1::note_on{
               static_cast<std::uint8_t>(event.channel),
               static_cast<std::uint8_t>(event.data1),
               static_cast<std::uint8_t>(event.data2)}, 0);
         } else if (event.status == 0xE0) {
            chain_(midi1::pitch_bend{
               static_cast<std::uint8_t>(event.channel),
               static_cast<std::uint8_t>(event.data1),
               static_cast<std::uint8_t>(event.data2)}, 0);
         } else if (event.status == 0xB0) {
            chain_(midi1::control_change{
               static_cast<std::uint8_t>(event.channel),
               midi1::cc::controller(static_cast<std::uint8_t>(event.data1)),
               static_cast<std::uint8_t>(event.data2)}, 0);
         } else {
            throw std::invalid_argument(
               "MIDI 2.0 translator supports note-on, note-off, control-change, and pitch-bend only");
         }
      }
      return sink_.packets;
   }

private:
   struct PacketSink : cycfi::q::midi_2_0::processor {
      using cycfi::q::midi_2_0::processor::operator();

      void operator()(cycfi::q::midi_2_0::note_on message, std::size_t) {
         packets.emplace_back(message.word(0), message.word(1));
      }

      void operator()(cycfi::q::midi_2_0::note_off message, std::size_t) {
         packets.emplace_back(message.word(0), message.word(1));
      }

      void operator()(cycfi::q::midi_2_0::pitch_bend message, std::size_t) {
         packets.emplace_back(message.word(0), message.word(1));
      }

      void operator()(cycfi::q::midi_2_0::registered_controller message, std::size_t) {
         packets.emplace_back(message.word(0), message.word(1));
      }

      void operator()(cycfi::q::midi_2_0::assignable_controller message, std::size_t) {
         packets.emplace_back(message.word(0), message.word(1));
      }

      void operator()(cycfi::q::midi_2_0::control_change message, std::size_t) {
         packets.emplace_back(message.word(0), message.word(1));
      }

      std::vector<cycfi::q::midi_2_0::packet> packets;
   };

   PacketSink sink_;
   cycfi::q::midi_2_0::to_midi2<PacketSink&> chain_{sink_};
};

} // namespace hz
