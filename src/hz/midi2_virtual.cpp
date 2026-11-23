#include "hz/midi2_virtual.hpp"

#include "hz/midi2.hpp"

#include <CoreMIDI/CoreMIDI.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace hz {
namespace {

using Packet = cycfi::q::midi_2_0::packet;

struct CallbackState {
   std::mutex mutex;
   std::condition_variable received;
   std::vector<std::uint32_t> words;
};

void require_success(OSStatus status, char const* operation) {
   if (status == noErr) return;
   std::ostringstream message;
   message << operation << " failed with OSStatus " << status;
   throw std::runtime_error(message.str());
}

std::string words_hex(std::vector<std::uint32_t> const& words) {
   std::ostringstream out;
   out << std::hex << std::setfill('0');
   for (std::size_t index = 0; index < words.size(); ++index) {
      if (index != 0) out << ' ';
      out << "0x" << std::setw(8) << words[index];
   }
   return out.str();
}

class VirtualMidi2Loopback {
public:
   VirtualMidi2Loopback() : state_(std::make_shared<CallbackState>()) {
      try {
         require_success(
            MIDIClientCreateWithBlock(
               CFSTR("hz_midi2 test client"), &client_,
               ^(MIDINotification const*) {}),
            "MIDIClientCreateWithBlock(midi2)");
         auto state = state_;
         require_success(
            MIDIDestinationCreateWithProtocol(
               client_, CFSTR("hz_midi2 protocol 2 virtual test destination"),
               kMIDIProtocol_2_0, &destination_,
               ^(MIDIEventList const* event_list, void*) {
                  {
                     std::lock_guard lock(state->mutex);
                     auto const* packet = &event_list->packet[0];
                     for (UInt32 packet_index = 0;
                          packet_index < event_list->numPackets; ++packet_index) {
                        for (UInt32 index = 0; index < packet->wordCount; ++index) {
                           state->words.push_back(packet->words[index]);
                        }
                        packet = MIDIEventPacketNext(packet);
                     }
                  }
                  state->received.notify_all();
               }),
            "MIDIDestinationCreateWithProtocol(kMIDIProtocol_2_0)");
         require_success(
            MIDIOutputPortCreate(client_, CFSTR("hz_midi2 test output"), &output_),
            "MIDIOutputPortCreate(midi2)");
      } catch (...) {
         cleanup();
         throw;
      }
   }

   VirtualMidi2Loopback(VirtualMidi2Loopback const&) = delete;
   VirtualMidi2Loopback& operator=(VirtualMidi2Loopback const&) = delete;
   ~VirtualMidi2Loopback() { cleanup(); }

   bool destination_is_enumerated() const {
      SInt32 unique_id = 0;
      require_success(
         MIDIObjectGetIntegerProperty(destination_, kMIDIPropertyUniqueID, &unique_id),
         "MIDIObjectGetIntegerProperty(midi2 virtual uniqueID)");
      ItemCount const count = MIDIGetNumberOfDestinations();
      for (ItemCount index = 0; index < count; ++index) {
         SInt32 candidate_id = 0;
         require_success(
            MIDIObjectGetIntegerProperty(
               MIDIGetDestination(index), kMIDIPropertyUniqueID, &candidate_id),
            "MIDIObjectGetIntegerProperty(enumerated uniqueID)");
         if (candidate_id == unique_id) return true;
      }
      return false;
   }

   std::vector<std::uint32_t> send(std::vector<Packet> const& packets) {
      alignas(UInt32) std::array<std::byte, 8192> storage{};
      auto* event_list = reinterpret_cast<MIDIEventList*>(storage.data());
      MIDIEventPacket* event_packet = MIDIEventListInit(event_list, kMIDIProtocol_2_0);
      for (auto const& packet : packets) {
         if (packet.words() != 2) {
            throw std::invalid_argument(
               "midi2 virtual output accepts only 64-bit MIDI 2.0 voice packets");
         }
         UInt32 const words[2] = {packet.word(0), packet.word(1)};
         event_packet = MIDIEventListAdd(
            event_list, static_cast<UInt32>(storage.size()), event_packet, 0, 2, words);
         if (event_packet == nullptr) {
            throw std::runtime_error("CoreMIDI protocol-2 event list capacity exceeded");
         }
      }

      std::size_t const expected_words = packets.size() * 2;
      {
         std::lock_guard lock(state_->mutex);
         state_->words.clear();
      }
      require_success(
         MIDISendEventList(output_, destination_, event_list),
         "MIDISendEventList(midi2 protocol 2 virtual)");

      std::unique_lock lock(state_->mutex);
      bool const delivered = state_->received.wait_for(
         lock, std::chrono::seconds(2), [&] { return state_->words.size() >= expected_words; });
      if (!delivered) {
         std::ostringstream message;
         message << "protocol-2 virtual destination delivered " << state_->words.size()
                 << " of " << expected_words << " words";
         throw std::runtime_error(message.str());
      }
      return state_->words;
   }

private:
   void cleanup() {
      if (destination_ != 0) MIDIEndpointDispose(destination_);
      if (output_ != 0) MIDIPortDispose(output_);
      if (client_ != 0) MIDIClientDispose(client_);
      destination_ = 0;
      output_ = 0;
      client_ = 0;
   }

   std::shared_ptr<CallbackState> state_;
   MIDIClientRef client_ = 0;
   MIDIPortRef output_ = 0;
   MIDIEndpointRef destination_ = 0;
};

} // namespace

bool run_midi2_self_test(std::string& report) {
   try {
      std::vector<MidiEvent> events = make_mpe_setup_events(1, 48);
      events.push_back(MidiEvent{0, 1, 0x90, 69, 127});
      events.push_back(MidiEvent{1, 1, 0x80, 69, 64});
      Midi2EventTranslator translator;
      auto const packets = translator.translate(events);

      VirtualMidi2Loopback loopback;
      if (!loopback.destination_is_enumerated()) {
         report = "midi2 self-test failed: protocol-2 virtual destination was not enumerated";
         return false;
      }
      auto const actual = loopback.send(packets);
      std::vector<std::uint32_t> const expected = {
         0x40200006u, 0x02000000u,
         0x40210000u, 0x60000000u,
         0x40914500u, 0xFFFF0000u,
         0x40814500u, 0x80000000u,
      };
      if (actual != expected) {
         report = "midi2 self-test failed\nexpected: " + words_hex(expected)
            + "\nactual:   " + words_hex(actual);
         return false;
      }
      report = "midi2 self-test passed: protocol 2 virtual destination enumerated; "
         "8 exact words for shared MPE setup, A4 note-on, and note-off";
      return true;
   } catch (std::exception const& error) {
      report = std::string("midi2 self-test failed: ") + error.what();
      return false;
   }
}

bool run_midi2_sine_test(std::string& report) {
   try {
      constexpr float sps = 48000.0f;
      constexpr int total_frames = 12000;
      constexpr int block_frames = 256;
      Config config;
      config.sps = sps;
      Converter converter(1, config);
      std::vector<MidiEvent> converted;
      for (int position = 0; position < total_frames; position += block_frames) {
         int const frames = std::min(block_frames, total_frames - position);
         std::vector<float> block(static_cast<std::size_t>(frames));
         for (int index = 0; index < frames; ++index) {
            double const phase = 2.0 * 3.14159265358979323846 * 440.0
               * double(position + index) / double(sps);
            block[static_cast<std::size_t>(index)] = 0.5f * float(std::sin(phase));
         }
         auto events = converter.feed(block, false);
         converted.insert(converted.end(), events.begin(), events.end());
      }
      auto final_events = converter.feed(nullptr, 0, 0, true);
      converted.insert(converted.end(), final_events.begin(), final_events.end());

      int note_ons = 0;
      int note_offs = 0;
      for (auto const& event : converted) {
         if (event.status == 0x90) ++note_ons;
         if (event.status == 0x80) ++note_offs;
      }
      bool const final_off = !converted.empty() && converted.back().status == 0x80
         && converted.back().channel == 1 && converted.back().data1 == 69
         && converted.back().sample == total_frames;
      if (note_ons != 1 || note_offs != 1 || !final_off) {
         std::ostringstream message;
         message << "midi2 sine-test failed: note-ons=" << note_ons
                 << " note-offs=" << note_offs << " final-off=" << final_off;
         report = message.str();
         return false;
      }

      std::vector<MidiEvent> transmitted = make_mpe_setup_events(1, config.bend_semitones);
      transmitted.insert(transmitted.end(), converted.begin(), converted.end());
      Midi2EventTranslator translator;
      auto const packets = translator.translate(transmitted);
      VirtualMidi2Loopback loopback;
      auto const actual = loopback.send(packets);
      std::vector<std::uint32_t> const expected = {
         0x40200006u, 0x02000000u,
         0x40210000u, 0x60000000u,
         0x40914500u, 0xB4D30000u,
         0x40E10000u, 0x80000000u,
         0x40814500u, 0x00000000u,
      };
      if (actual != expected) {
         report = "midi2 sine-test failed: protocol-2 virtual words differ\nexpected: "
            + words_hex(expected) + "\nactual:   " + words_hex(actual);
         return false;
      }
      report = "midi2 sine-test passed: converted synthetic A4 after 47 streaming blocks; "
         "10 exact words including center bend and final note-off";
      return true;
   } catch (std::exception const& error) {
      report = std::string("midi2 sine-test failed: ") + error.what();
      return false;
   }
}

} // namespace hz