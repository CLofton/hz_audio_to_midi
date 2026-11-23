#pragma once

#include "hz/live.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace hz {

struct VirtualMidiDelivery {
   std::vector<std::uint32_t> words;
   std::uint64_t note_ons = 0;
   std::uint64_t note_offs = 0;
};

// Persistent software-only protocol-1 destination. Sends never wait or clear
// delivery history; callers wait once after their asynchronous producer drains.
class VirtualMidiOutput {
public:
   VirtualMidiOutput();
   ~VirtualMidiOutput();
   VirtualMidiOutput(VirtualMidiOutput const&) = delete;
   VirtualMidiOutput& operator=(VirtualMidiOutput const&) = delete;

   bool destination_is_enumerated() const;
   void send(std::vector<MidiEvent> const& events);
   VirtualMidiDelivery wait_for_delivery(
      std::size_t expected_words, std::chrono::milliseconds timeout);

private:
   struct Impl;
   std::unique_ptr<Impl> impl_;
};

} // namespace hz
