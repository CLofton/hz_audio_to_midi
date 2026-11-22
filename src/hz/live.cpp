#include "hz/live.hpp"

#include <CoreAudio/CoreAudio.h>
#include <CoreMIDI/CoreMIDI.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace hz {
namespace {

using Clock = std::chrono::steady_clock;

struct CallbackState {
   std::mutex mutex;
   std::condition_variable received;
   std::vector<std::uint32_t> words;
   Clock::time_point send_started;
   double max_latency_ms = 0.0;
   bool sending = false;
};

void require_success(OSStatus status, char const* operation) {
   if (status == noErr) return;
   std::ostringstream message;
   message << operation << " failed with OSStatus " << status;
   throw std::runtime_error(message.str());
}

AudioObjectPropertyAddress audio_address(
   AudioObjectPropertySelector selector, AudioObjectPropertyScope scope) {
   return AudioObjectPropertyAddress{selector, scope, kAudioObjectPropertyElementMain};
}

void get_audio_property(
   AudioObjectID object, AudioObjectPropertyAddress const& address,
   UInt32 size, void* data, char const* operation) {
   require_success(
      AudioObjectGetPropertyData(object, &address, 0, nullptr, &size, data), operation);
}

class VirtualMidiLoopback {
public:
   VirtualMidiLoopback() : state_(std::make_shared<CallbackState>()) {
      try {
         require_success(
            MIDIClientCreateWithBlock(
               CFSTR("hz_live test client"), &client_,
               ^(MIDINotification const*) {}),
            "MIDIClientCreateWithBlock");

         auto state = state_;
         require_success(
            MIDIDestinationCreateWithProtocol(
               client_, CFSTR("hz_live virtual test destination"),
               kMIDIProtocol_1_0, &destination_,
               ^(MIDIEventList const* event_list, void*) {
                  auto now = Clock::now();
                  {
                     std::lock_guard lock(state->mutex);
                     if (state->sending) {
                        double const elapsed = std::chrono::duration<double, std::milli>(
                           now - state->send_started).count();
                        state->max_latency_ms = std::max(state->max_latency_ms, elapsed);
                     }
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
            "MIDIDestinationCreateWithProtocol");
         require_success(
            MIDIOutputPortCreate(client_, CFSTR("hz_live test output"), &output_),
            "MIDIOutputPortCreate");
      } catch (...) {
         cleanup();
         throw;
      }
   }

   VirtualMidiLoopback(VirtualMidiLoopback const&) = delete;
   VirtualMidiLoopback& operator=(VirtualMidiLoopback const&) = delete;

   ~VirtualMidiLoopback() { cleanup(); }

   std::pair<std::vector<std::uint32_t>, double> send(
      std::vector<MidiEvent> const& events) {
      alignas(UInt32) std::array<std::byte, 8192> storage{};
      auto* event_list = reinterpret_cast<MIDIEventList*>(storage.data());
      MIDIEventPacket* packet = MIDIEventListInit(event_list, kMIDIProtocol_1_0);
      for (auto const& event : events) {
         UInt32 word = midi1_channel_voice_ump(event);
         packet = MIDIEventListAdd(
            event_list, static_cast<UInt32>(storage.size()), packet, 0, 1, &word);
         if (packet == nullptr) throw std::runtime_error("CoreMIDI event list capacity exceeded");
      }

      {
         std::lock_guard lock(state_->mutex);
         state_->words.clear();
         state_->max_latency_ms = 0.0;
         state_->send_started = Clock::now();
         state_->sending = true;
      }
      OSStatus const status = MIDISendEventList(output_, destination_, event_list);
      if (status != noErr) {
         std::lock_guard lock(state_->mutex);
         state_->sending = false;
         require_success(status, "MIDISendEventList");
      }

      std::unique_lock lock(state_->mutex);
      bool const delivered = state_->received.wait_for(
         lock, std::chrono::seconds(2), [&] { return state_->words.size() >= events.size(); });
      state_->sending = false;
      if (!delivered) {
         std::ostringstream message;
         message << "virtual destination delivered " << state_->words.size()
                 << " of " << events.size() << " words";
         throw std::runtime_error(message.str());
      }
      return {state_->words, state_->max_latency_ms};
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

std::string copy_cf_string(CFStringRef value) {
   if (value == nullptr) return {};
   CFIndex const maximum = CFStringGetMaximumSizeForEncoding(
      CFStringGetLength(value), kCFStringEncodingUTF8);
   std::string text(static_cast<std::size_t>(maximum + 1), '\0');
   if (!CFStringGetCString(value, text.data(), maximum + 1, kCFStringEncodingUTF8)) return {};
   text.resize(std::char_traits<char>::length(text.c_str()));
   return text;
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

} // namespace

std::vector<MidiDestinationInfo> enumerate_midi_destinations() {
   ItemCount const count = MIDIGetNumberOfDestinations();
   std::vector<MidiDestinationInfo> destinations;
   destinations.reserve(count);
   for (ItemCount index = 0; index < count; ++index) {
      MIDIEndpointRef endpoint = MIDIGetDestination(index);
      CFStringRef name = nullptr;
      OSStatus status = MIDIObjectGetStringProperty(endpoint, kMIDIPropertyDisplayName, &name);
      if (status != noErr) {
         status = MIDIObjectGetStringProperty(endpoint, kMIDIPropertyName, &name);
      }
      require_success(status, "MIDIObjectGetStringProperty(name)");
      std::string copied_name = copy_cf_string(name);
      if (name != nullptr) CFRelease(name);

      SInt32 unique_id = 0;
      require_success(
         MIDIObjectGetIntegerProperty(endpoint, kMIDIPropertyUniqueID, &unique_id),
         "MIDIObjectGetIntegerProperty(uniqueID)");
      destinations.push_back(MidiDestinationInfo{
         static_cast<int>(index), std::move(copied_name), unique_id});
   }
   return destinations;
}

std::vector<AudioInputDeviceInfo> enumerate_audio_input_devices() {
   auto const devices_address = audio_address(
      kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal);
   UInt32 devices_bytes = 0;
   require_success(
      AudioObjectGetPropertyDataSize(
         kAudioObjectSystemObject, &devices_address, 0, nullptr, &devices_bytes),
      "AudioObjectGetPropertyDataSize(devices)");
   std::vector<AudioDeviceID> devices(devices_bytes / sizeof(AudioDeviceID));
   if (!devices.empty()) {
      get_audio_property(
         kAudioObjectSystemObject, devices_address, devices_bytes, devices.data(),
         "AudioObjectGetPropertyData(devices)");
   }

   auto const default_address = audio_address(
      kAudioHardwarePropertyDefaultInputDevice, kAudioObjectPropertyScopeGlobal);
   AudioDeviceID default_input = kAudioObjectUnknown;
   get_audio_property(
      kAudioObjectSystemObject, default_address, sizeof(default_input), &default_input,
      "AudioObjectGetPropertyData(default input)");

   std::vector<AudioInputDeviceInfo> inputs;
   for (AudioDeviceID device : devices) {
      auto const configuration_address = audio_address(
         kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeInput);
      UInt32 configuration_bytes = 0;
      OSStatus const size_status = AudioObjectGetPropertyDataSize(
         device, &configuration_address, 0, nullptr, &configuration_bytes);
      if (size_status == kAudioHardwareUnknownPropertyError) continue;
      require_success(size_status, "AudioObjectGetPropertyDataSize(input stream configuration)");
      if (configuration_bytes < sizeof(AudioBufferList)) continue;

      std::vector<std::uint64_t> configuration_storage(
         (configuration_bytes + sizeof(std::uint64_t) - 1) / sizeof(std::uint64_t));
      auto* buffers = reinterpret_cast<AudioBufferList*>(configuration_storage.data());
      get_audio_property(
         device, configuration_address, configuration_bytes, buffers,
         "AudioObjectGetPropertyData(input stream configuration)");
      int input_channels = 0;
      for (UInt32 index = 0; index < buffers->mNumberBuffers; ++index) {
         input_channels += static_cast<int>(buffers->mBuffers[index].mNumberChannels);
      }
      if (input_channels == 0) continue;

      auto const uid_address = audio_address(
         kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal);
      CFStringRef uid_reference = nullptr;
      get_audio_property(
         device, uid_address, sizeof(uid_reference), &uid_reference,
         "AudioObjectGetPropertyData(device UID)");
      std::string uid = copy_cf_string(uid_reference);
      if (uid_reference != nullptr) CFRelease(uid_reference);

      auto const name_address = audio_address(
         kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal);
      CFStringRef name_reference = nullptr;
      get_audio_property(
         device, name_address, sizeof(name_reference), &name_reference,
         "AudioObjectGetPropertyData(device name)");
      std::string name = copy_cf_string(name_reference);
      if (name_reference != nullptr) CFRelease(name_reference);

      auto const rate_address = audio_address(
         kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal);
      Float64 sample_rate = 0.0;
      get_audio_property(
         device, rate_address, sizeof(sample_rate), &sample_rate,
         "AudioObjectGetPropertyData(nominal sample rate)");
      inputs.push_back(AudioInputDeviceInfo{
         device, std::move(uid), std::move(name), sample_rate, input_channels,
         device == default_input});
   }
   return inputs;
}

bool run_midi_self_test(std::string& report) {
   try {
      std::vector<MidiEvent> events = make_mpe_setup_events(1, 48);
      events.push_back(MidiEvent{0, 1, 0x90, 69, 100});
      events.push_back(MidiEvent{1, 1, 0x80, 69, 0});

      VirtualMidiLoopback loopback;
      auto const destinations = enumerate_midi_destinations();
      auto const virtual_destination = std::find_if(
         destinations.begin(), destinations.end(), [](MidiDestinationInfo const& destination) {
            return destination.name == "hz_live virtual test destination";
         });
      if (virtual_destination == destinations.end()) {
         report = "self-test failed: virtual destination was not enumerated";
         return false;
      }
      auto [actual, latency_ms] = loopback.send(events);
      std::vector<std::uint32_t> expected;
      for (auto const& event : events) expected.push_back(midi1_channel_voice_ump(event));
      if (actual != expected) {
         report = "self-test failed\nexpected: " + words_hex(expected)
            + "\nactual:   " + words_hex(actual);
         return false;
      }
      std::ostringstream message;
      message << "self-test passed: virtual destination enumerated; " << actual.size()
              << " exact MPE setup/note-on/note-off words; max virtual send-to-receive latency "
              << std::fixed << std::setprecision(3) << latency_ms << " ms";
      report = message.str();
      return true;
   } catch (std::exception const& error) {
      report = std::string("self-test failed: ") + error.what();
      return false;
   }
}

bool run_midi_sine_test(std::string& report) {
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
      bool member_note_69 = true;
      for (auto const& event : converted) {
         if (event.status == 0x90) {
            ++note_ons;
            member_note_69 = member_note_69 && event.channel == 1 && event.data1 == 69;
         } else if (event.status == 0x80) {
            ++note_offs;
            member_note_69 = member_note_69 && event.channel == 1 && event.data1 == 69;
         }
      }
      bool const final_off = !converted.empty() && converted.back().status == 0x80
         && converted.back().channel == 1 && converted.back().data1 == 69;
      if (note_ons != 1 || note_offs != 1 || !member_note_69 || !final_off) {
         std::ostringstream message;
         message << "sine-test failed: note-ons=" << note_ons << " note-offs=" << note_offs
                 << " member-note69=" << member_note_69 << " final-off=" << final_off;
         report = message.str();
         return false;
      }

      std::vector<MidiEvent> transmitted = make_mpe_setup_events(1, config.bend_semitones);
      transmitted.insert(transmitted.end(), converted.begin(), converted.end());
      VirtualMidiLoopback loopback;
      auto [actual, latency_ms] = loopback.send(transmitted);
      std::vector<std::uint32_t> expected;
      for (auto const& event : transmitted) expected.push_back(midi1_channel_voice_ump(event));
      if (actual != expected) {
         report = "sine-test failed: virtual words differ\nexpected: " + words_hex(expected)
            + "\nactual:   " + words_hex(actual);
         return false;
      }
      std::ostringstream message;
      message << "sine-test passed: one member-channel note 69 plus final note-off after "
              << (total_frames + block_frames - 1) / block_frames << " streaming blocks; " << actual.size()
              << " exact words; max virtual send-to-receive latency "
              << std::fixed << std::setprecision(3) << latency_ms << " ms";
      report = message.str();
      return true;
   } catch (std::exception const& error) {
      report = std::string("sine-test failed: ") + error.what();
      return false;
   }
}

} // namespace hz
