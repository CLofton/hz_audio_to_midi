#include "hz/live_virtual.hpp"

#include <CoreMIDI/CoreMIDI.h>

#include <array>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>

namespace hz {
namespace {

struct VirtualDeliveryState {
   std::mutex mutex;
   std::condition_variable received;
   std::vector<std::uint32_t> words;
   std::uint64_t note_ons = 0;
   std::uint64_t note_offs = 0;
};

void require_virtual_success(OSStatus status, char const* operation) {
   if (status == noErr) return;
   std::ostringstream message;
   message << operation << " failed with OSStatus " << status;
   throw std::runtime_error(message.str());
}

} // namespace

struct VirtualMidiOutput::Impl {
   Impl() : state(std::make_shared<VirtualDeliveryState>()) {
      try {
         require_virtual_success(
            MIDIClientCreateWithBlock(
               CFSTR("hz_live persistent virtual client"), &client,
               ^(MIDINotification const*) {}),
            "MIDIClientCreateWithBlock(persistent virtual)");
         auto shared_state = state;
         require_virtual_success(
            MIDIDestinationCreateWithProtocol(
               client, CFSTR("hz_live persistent protocol 1 virtual destination"),
               kMIDIProtocol_1_0, &destination,
               ^(MIDIEventList const* event_list, void*) {
                  {
                     std::lock_guard lock(shared_state->mutex);
                     auto const* packet = &event_list->packet[0];
                     for (UInt32 packet_index = 0;
                          packet_index < event_list->numPackets; ++packet_index) {
                        for (UInt32 index = 0; index < packet->wordCount; ++index) {
                           std::uint32_t const word = packet->words[index];
                           shared_state->words.push_back(word);
                           std::uint32_t const status = (word >> 16) & 0xF0u;
                           std::uint32_t const velocity = word & 0x7Fu;
                           if (status == 0x90u && velocity != 0) ++shared_state->note_ons;
                           if (status == 0x80u || (status == 0x90u && velocity == 0)) {
                              ++shared_state->note_offs;
                           }
                        }
                        packet = MIDIEventPacketNext(packet);
                     }
                  }
                  shared_state->received.notify_all();
               }),
            "MIDIDestinationCreateWithProtocol(persistent protocol 1)");
         require_virtual_success(
            MIDIOutputPortCreate(client, CFSTR("hz_live persistent virtual output"), &output),
            "MIDIOutputPortCreate(persistent virtual)");
      } catch (...) {
         cleanup();
         throw;
      }
   }

   ~Impl() { cleanup(); }

   void cleanup() {
      if (destination != 0) MIDIEndpointDispose(destination);
      if (output != 0) MIDIPortDispose(output);
      if (client != 0) MIDIClientDispose(client);
      destination = 0;
      output = 0;
      client = 0;
   }

   std::shared_ptr<VirtualDeliveryState> state;
   MIDIClientRef client = 0;
   MIDIPortRef output = 0;
   MIDIEndpointRef destination = 0;
};

VirtualMidiOutput::VirtualMidiOutput() : impl_(std::make_unique<Impl>()) {}
VirtualMidiOutput::~VirtualMidiOutput() = default;

bool VirtualMidiOutput::destination_is_enumerated() const {
   SInt32 unique_id = 0;
   require_virtual_success(
      MIDIObjectGetIntegerProperty(impl_->destination, kMIDIPropertyUniqueID, &unique_id),
      "MIDIObjectGetIntegerProperty(persistent virtual uniqueID)");
   ItemCount const count = MIDIGetNumberOfDestinations();
   for (ItemCount index = 0; index < count; ++index) {
      SInt32 candidate_id = 0;
      require_virtual_success(
         MIDIObjectGetIntegerProperty(
            MIDIGetDestination(index), kMIDIPropertyUniqueID, &candidate_id),
         "MIDIObjectGetIntegerProperty(enumerated uniqueID)");
      if (candidate_id == unique_id) return true;
   }
   return false;
}

void VirtualMidiOutput::send(std::vector<MidiEvent> const& events) {
   if (events.empty()) return;
   alignas(UInt32) std::array<std::byte, 8192> storage{};
   auto* event_list = reinterpret_cast<MIDIEventList*>(storage.data());
   MIDIEventPacket* packet = MIDIEventListInit(event_list, kMIDIProtocol_1_0);
   for (auto const& event : events) {
      UInt32 word = midi1_channel_voice_ump(event);
      packet = MIDIEventListAdd(
         event_list, static_cast<UInt32>(storage.size()), packet, 0, 1, &word);
      if (packet == nullptr) throw std::runtime_error("CoreMIDI event list capacity exceeded");
   }
   require_virtual_success(
      MIDISendEventList(impl_->output, impl_->destination, event_list),
      "MIDISendEventList(persistent protocol 1 virtual)");
}

VirtualMidiDelivery VirtualMidiOutput::wait_for_delivery(
   std::size_t expected_words, std::chrono::milliseconds timeout
) {
   std::unique_lock lock(impl_->state->mutex);
   impl_->state->received.wait_for(lock, timeout, [&] {
      return impl_->state->words.size() >= expected_words;
   });
   return VirtualMidiDelivery{
      impl_->state->words, impl_->state->note_ons, impl_->state->note_offs};
}

} // namespace hz
