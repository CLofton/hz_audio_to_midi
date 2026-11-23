#include "hz/live.hpp"
#include "hz/live_virtual.hpp"
#include "hz/microphone_permission.hpp"

#include <AudioToolbox/AudioUnit.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreMIDI/CoreMIDI.h>
#include <mach/mach_time.h>

#include <algorithm>
#include <array>
#include <atomic>
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
#include <thread>
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

void set_audio_property(
   AudioObjectID object, AudioObjectPropertyAddress const& address,
   UInt32 size, void const* data, char const* operation) {
   require_success(
      AudioObjectSetPropertyData(object, &address, 0, nullptr, size, data), operation);
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

class CoreMidiOutput {
public:
   explicit CoreMidiOutput(int destination_index) {
      try {
         require_success(
            MIDIClientCreateWithBlock(
               CFSTR("hz_live capture client"), &client_, ^(MIDINotification const*) {}),
            "MIDIClientCreateWithBlock(live)");
         require_success(
            MIDIOutputPortCreate(client_, CFSTR("hz_live capture output"), &output_),
            "MIDIOutputPortCreate(live)");
         destination_ = MIDIGetDestination(static_cast<ItemCount>(destination_index));
         if (destination_ == 0) throw std::runtime_error("selected MIDI destination is unavailable");
      } catch (...) {
         cleanup();
         throw;
      }
   }

   CoreMidiOutput(CoreMidiOutput const&) = delete;
   CoreMidiOutput& operator=(CoreMidiOutput const&) = delete;
   ~CoreMidiOutput() { cleanup(); }

   void send(std::vector<MidiEvent> const& events) {
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
      require_success(MIDISendEventList(output_, destination_, event_list), "MIDISendEventList(live)");
   }

private:
   void cleanup() {
      if (output_ != 0) MIDIPortDispose(output_);
      if (client_ != 0) MIDIClientDispose(client_);
      output_ = 0;
      client_ = 0;
      destination_ = 0;
   }

   MIDIClientRef client_ = 0;
   MIDIPortRef output_ = 0;
   MIDIEndpointRef destination_ = 0;
};

enum CallbackFailure {
   callback_failure_none = 0,
   callback_failure_invalid_buffer = 1,
   callback_failure_too_many_frames = 2,
   callback_failure_overflow = 3,
   callback_failure_render = 4,
   callback_failure_invalid_timestamp = 5,
   callback_failure_discontinuity = 6
};

struct LiveCallbackState {
   explicit LiveCallbackState(int channel_count)
      : channels(static_cast<UInt32>(channel_count)), timeline(&callback_discontinuities) {}

   AudioUnit unit = nullptr;
   UInt32 channels;
   CaptureRing ring;
   std::atomic<bool> stop{false};
   std::atomic<bool> fatal{false};
   std::atomic<int> callback_failure{callback_failure_none};
   std::atomic<OSStatus> render_status{noErr};
   std::atomic<std::uint32_t> render_errors{0};
   std::mutex error_mutex;
   std::string worker_error;
   std::atomic<std::uint32_t> callback_discontinuities{0};
   CaptureTimeline timeline;
   alignas(AudioBufferList) std::array<std::byte,
      sizeof(AudioBufferList) + (capture_max_channels - 1) * sizeof(AudioBuffer)> abl_storage{};
};

void callback_fatal(LiveCallbackState& state, int failure) noexcept {
   int expected = callback_failure_none;
   state.callback_failure.compare_exchange_strong(expected, failure, std::memory_order_relaxed);
   state.fatal.store(true, std::memory_order_release);
}

OSStatus capture_input_callback(
   void* reference, AudioUnitRenderActionFlags* action_flags, AudioTimeStamp const* timestamp,
   UInt32, UInt32 frames, AudioBufferList* io_data
) noexcept {
   auto& state = *static_cast<LiveCallbackState*>(reference);
   if (state.fatal.load(std::memory_order_acquire)) return kAudio_ParamError;
   if (io_data != nullptr || timestamp == nullptr) {
      callback_fatal(state, callback_failure_invalid_buffer);
      return kAudio_ParamError;
   }
   if (frames > std::uint32_t(capture_max_frames)) {
      callback_fatal(state, callback_failure_too_many_frames);
      return kAudio_ParamError;
   }
   std::uint64_t start_sample = 0;
   auto const timeline_result = state.timeline.accept(
      (timestamp->mFlags & kAudioTimeStampSampleTimeValid) != 0,
      timestamp->mSampleTime, frames, start_sample);
   if (timeline_result == CaptureTimelineResult::invalid_timestamp) {
      callback_fatal(state, callback_failure_invalid_timestamp);
      return kAudio_ParamError;
   }
   if (timeline_result == CaptureTimelineResult::discontinuity) {
      callback_fatal(state, callback_failure_discontinuity);
      return kAudio_ParamError;
   }
   if (frames == 0) return noErr;
   CaptureBlock* block = state.ring.acquire_write();
   if (block == nullptr) {
      callback_fatal(state, callback_failure_overflow);
      return kAudio_ParamError;
   }

   auto* buffers = reinterpret_cast<AudioBufferList*>(state.abl_storage.data());
   buffers->mNumberBuffers = state.channels;
   for (UInt32 channel = 0; channel < state.channels; ++channel) {
      buffers->mBuffers[channel].mNumberChannels = 1;
      buffers->mBuffers[channel].mDataByteSize = frames * UInt32(sizeof(float));
      buffers->mBuffers[channel].mData = block->samples[channel].data();
   }
   OSStatus const status = AudioUnitRender(
      state.unit, action_flags, timestamp, 1, frames, buffers);
   if (status != noErr) {
      state.render_status.store(status, std::memory_order_relaxed);
      state.render_errors.fetch_add(1, std::memory_order_relaxed);
      callback_fatal(state, callback_failure_render);
      return status;
   }
   for (UInt32 channel = 0; channel < state.channels; ++channel) {
      if (buffers->mBuffers[channel].mDataByteSize != frames * UInt32(sizeof(float))) {
         state.render_status.store(kAudio_ParamError, std::memory_order_relaxed);
         state.render_errors.fetch_add(1, std::memory_order_relaxed);
         callback_fatal(state, callback_failure_render);
         return kAudio_ParamError;
      }
   }
   std::uint64_t const host_time = (timestamp->mFlags & kAudioTimeStampHostTimeValid) != 0
      ? timestamp->mHostTime : AudioGetCurrentHostTime();
   state.ring.commit_write(frames, state.channels, start_sample, host_time);
   return noErr;
}

void worker_fatal(LiveCallbackState& state, std::string const& error) {
   {
      std::lock_guard lock(state.error_mutex);
      if (state.worker_error.empty()) state.worker_error = error;
   }
   state.fatal.store(true, std::memory_order_release);
}

std::string callback_error_message(LiveCallbackState const& state) {
   switch (state.callback_failure.load(std::memory_order_relaxed)) {
      case callback_failure_invalid_buffer:
         return "AUHAL input callback received an invalid buffer contract";
      case callback_failure_too_many_frames:
         return "AUHAL input callback exceeded the 1024-frame preallocated block";
      case callback_failure_overflow:
         return "capture SPSC ring overflowed";
      case callback_failure_render: {
         std::ostringstream message;
         message << "AudioUnitRender failed with OSStatus "
                 << state.render_status.load(std::memory_order_relaxed);
         return message.str();
      }
      case callback_failure_invalid_timestamp:
         return "AUHAL input callback received a missing or invalid device sample timestamp";
      case callback_failure_discontinuity:
         return "AUHAL device sample timestamp discontinuity detected";
      default:
         return "live capture failed";
   }
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

bool run_live(LiveRunOptions const& options, LiveRunReport& report, std::string& message) {
   report = LiveRunReport{};
   message.clear();
   if (options.input_uid.empty()
      || (options.virtual_output == (options.midi_index >= 0))
      || !std::isfinite(options.seconds) || options.seconds <= 0.0 || options.seconds > 3600.0) {
      message = "live run requires an input UID, exactly one output selector, and duration in (0, 3600]";
      return false;
   }

   AudioUnit unit = nullptr;
   bool initialized = false;
   bool started = false;
   std::thread worker;
   auto state = std::make_shared<LiveCallbackState>(1);
   std::uint32_t worker_discontinuities = 0;
   OSStatus stop_status = noErr;
   auto shutdown = [&] {
      if (started) {
         OSStatus const status = AudioOutputUnitStop(unit);
         if (stop_status == noErr) stop_status = status;
         started = false;
      }
      if (worker.joinable()) {
         state->stop.store(true, std::memory_order_release);
         worker.join();
      }
      if (initialized) {
         AudioUnitUninitialize(unit);
         initialized = false;
      }
      if (unit != nullptr) {
         AudioComponentInstanceDispose(unit);
         unit = nullptr;
      }
   };

   try {
      auto const inputs = enumerate_audio_input_devices();
      auto const input = std::find_if(inputs.begin(), inputs.end(), [&](auto const& candidate) {
         return candidate.uid == options.input_uid;
      });
      if (input == inputs.end()) throw std::runtime_error("selected audio input UID was not found");
      if (!options.virtual_output
         && options.midi_index >= static_cast<int>(MIDIGetNumberOfDestinations())) {
         throw std::runtime_error("selected MIDI destination index is out of range");
      }
      std::string permission_error;
      if (!ensure_microphone_permission(std::chrono::seconds(30), permission_error)) {
         message = permission_error;
         return false;
      }

      auto const range_address = audio_address(
         kAudioDevicePropertyBufferFrameSizeRange, kAudioObjectPropertyScopeGlobal);
      AudioValueRange frame_range{};
      get_audio_property(input->id, range_address, sizeof(frame_range), &frame_range,
         "AudioObjectGetPropertyData(buffer frame size range)");
      CaptureConfig capture_config;
      capture_config.sample_rate = input->sample_rate;
      capture_config.channels = input->input_channels;
      capture_config.maximum_frames = capture_maximum_frames_for_range(
         frame_range.mMinimum, frame_range.mMaximum);
      if (char const* error = capture_config_error(capture_config)) {
         throw std::runtime_error(error);
      }
      report.sample_rate = input->sample_rate;
      report.channels = input->input_channels;
      report.maximum_frames = static_cast<std::uint32_t>(capture_config.maximum_frames);

      auto const buffer_address = audio_address(
         kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal);
      UInt32 requested_buffer = 512;
      set_audio_property(input->id, buffer_address, sizeof(requested_buffer), &requested_buffer,
         "AudioObjectSetPropertyData(buffer frame size 512)");
      UInt32 actual_buffer = 0;
      get_audio_property(input->id, buffer_address, sizeof(actual_buffer), &actual_buffer,
         "AudioObjectGetPropertyData(buffer frame size readback)");
      if (actual_buffer == 0 || actual_buffer > std::uint32_t(capture_max_frames)) {
         throw std::runtime_error("device buffer size readback exceeds preallocated capture blocks");
      }
      report.device_buffer_frames = actual_buffer;

      AudioComponentDescription description{};
      description.componentType = kAudioUnitType_Output;
      description.componentSubType = kAudioUnitSubType_HALOutput;
      description.componentManufacturer = kAudioUnitManufacturer_Apple;
      AudioComponent component = AudioComponentFindNext(nullptr, &description);
      if (component == nullptr) throw std::runtime_error("AUHAL component was not found");
      require_success(AudioComponentInstanceNew(component, &unit), "AudioComponentInstanceNew(AUHAL)");
      state = std::make_shared<LiveCallbackState>(input->input_channels);
      state->unit = unit;

      UInt32 disable_output = 0;
      require_success(AudioUnitSetProperty(
         unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0,
         &disable_output, sizeof(disable_output)), "AUHAL disable output element 0");
      UInt32 enable_input = 1;
      require_success(AudioUnitSetProperty(
         unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1,
         &enable_input, sizeof(enable_input)), "AUHAL enable input element 1");
      AudioDeviceID device = input->id;
      require_success(AudioUnitSetProperty(
         unit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0,
         &device, sizeof(device)), "AUHAL select current input device");
      AURenderCallbackStruct callback{&capture_input_callback, state.get()};
      require_success(AudioUnitSetProperty(
         unit, kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global, 0,
         &callback, sizeof(callback)), "AUHAL set input callback");

      AudioStreamBasicDescription format{};
      format.mSampleRate = 48000.0;
      format.mFormatID = kAudioFormatLinearPCM;
      format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked
         | kAudioFormatFlagIsNonInterleaved;
      format.mBytesPerPacket = sizeof(float);
      format.mFramesPerPacket = 1;
      format.mBytesPerFrame = sizeof(float);
      format.mChannelsPerFrame = static_cast<UInt32>(input->input_channels);
      format.mBitsPerChannel = 8 * sizeof(float);
      require_success(AudioUnitSetProperty(
         unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1,
         &format, sizeof(format)), "AUHAL set float32 packed noninterleaved input format");
      UInt32 maximum_frames = static_cast<UInt32>(capture_config.maximum_frames);
      require_success(AudioUnitSetProperty(
         unit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0,
         &maximum_frames, sizeof(maximum_frames)), "AUHAL set MaximumFramesPerSlice");
      require_success(AudioUnitInitialize(unit), "AudioUnitInitialize(AUHAL)");
      initialized = true;

      Config converter_config;
      converter_config.sps = 48000.0f;
      Converter converter(input->input_channels, converter_config);
      std::unique_ptr<CoreMidiOutput> indexed_output;
      std::unique_ptr<VirtualMidiOutput> virtual_output;
      if (options.virtual_output) {
         virtual_output = std::make_unique<VirtualMidiOutput>();
      } else {
         indexed_output = std::make_unique<CoreMidiOutput>(options.midi_index);
      }
      std::uint64_t transmitted_words = 0;
      auto transmit = [&](std::vector<MidiEvent> const& events) {
         if (options.virtual_output) virtual_output->send(events);
         else indexed_output->send(events);
         transmitted_words += events.size();
      };
      auto const mpe_setup = make_mpe_setup_events(
         input->input_channels, converter_config.bend_semitones);
      transmit(mpe_setup);

      mach_timebase_info_data_t timebase{};
      if (mach_timebase_info(&timebase) != KERN_SUCCESS || timebase.denom == 0) {
         throw std::runtime_error("mach_timebase_info failed");
      }
      worker = std::thread([&, converter = std::move(converter)]() mutable {
         CaptureSequence sequence;
         std::array<float const*, capture_max_channels> channel_data{};
         try {
            for (;;) {
               while (CaptureBlock const* block = state->ring.acquire_read()) {
                  if (!sequence.accept(block->start_sample, block->frames)) {
                     ++worker_discontinuities;
                     worker_fatal(*state, "capture sample discontinuity detected");
                     state->ring.release_read();
                     continue;
                  }
                  for (UInt32 channel = 0; channel < state->channels; ++channel) {
                     channel_data[channel] = block->samples[channel].data();
                  }
                  auto const converter_started = Clock::now();
                  auto events = converter.feed(
                     channel_data.data(), static_cast<int>(state->channels),
                     static_cast<int>(block->frames), false);
                  report.converter_total_ms += std::chrono::duration<double, std::milli>(
                     Clock::now() - converter_started).count();
                  transmit(events);
                  auto const sent_host_time = AudioGetCurrentHostTime();
                  if (sent_host_time >= block->capture_host_time) {
                     long double const ticks = sent_host_time - block->capture_host_time;
                     double const elapsed_ms = static_cast<double>(
                        ticks * timebase.numer / timebase.denom / 1000000.0L);
                     report.max_capture_to_send_ms = std::max(
                        report.max_capture_to_send_ms, elapsed_ms);
                  }
                  ++report.captured_blocks;
                  report.captured_frames += block->frames;
                  state->ring.release_read();
               }
               if (state->stop.load(std::memory_order_acquire)) break;
               std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
         } catch (std::exception const& error) {
            worker_fatal(*state, std::string("live worker failed: ") + error.what());
         }
         try {
            auto const final_started = Clock::now();
            auto final_events = converter.feed(nullptr, 0, 0, true);
            report.converter_total_ms += std::chrono::duration<double, std::milli>(
               Clock::now() - final_started).count();
            transmit(final_events);
         } catch (std::exception const& error) {
            worker_fatal(*state, std::string("live final note-off flush failed: ") + error.what());
         }
      });

      require_success(AudioOutputUnitStart(unit), "AudioOutputUnitStart(AUHAL)");
      started = true;
      auto const deadline = Clock::now() + std::chrono::duration<double>(options.seconds);
      while (Clock::now() < deadline && !state->fatal.load(std::memory_order_acquire)) {
         std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      shutdown();

      report.queue_high_water = state->ring.queue_high_water();
      report.overflow_count = state->ring.overflow_count();
      report.render_error_count = state->render_errors.load(std::memory_order_relaxed);
      report.discontinuity_count = worker_discontinuities
         + state->callback_discontinuities.load(std::memory_order_relaxed);
      if (stop_status != noErr) require_success(stop_status, "AudioOutputUnitStop(AUHAL)");
      if (state->fatal.load(std::memory_order_acquire)) {
         std::string worker_error;
         {
            std::lock_guard lock(state->error_mutex);
            worker_error = state->worker_error;
         }
         message = !worker_error.empty() ? worker_error : callback_error_message(*state);
         return false;
      }
      if (options.virtual_output) {
         auto const delivery = virtual_output->wait_for_delivery(
            static_cast<std::size_t>(transmitted_words), std::chrono::seconds(2));
         report.virtual_words = delivery.words.size();
         report.virtual_note_ons = delivery.note_ons;
         report.virtual_note_offs = delivery.note_offs;
         std::vector<std::uint32_t> expected_setup;
         for (auto const& event : mpe_setup) {
            expected_setup.push_back(midi1_channel_voice_ump(event));
         }
         bool const setup_prefix = delivery.words.size() >= expected_setup.size()
            && std::equal(expected_setup.begin(), expected_setup.end(), delivery.words.begin());
         report.virtual_setup_verified = setup_prefix;
         if (delivery.words.size() != transmitted_words) {
            std::ostringstream error;
            error << "virtual delivery word count mismatch: delivered " << delivery.words.size()
                  << " of " << transmitted_words << " expected words";
            message = error.str();
            return false;
         }
         if (!setup_prefix) {
            message = "virtual delivery did not preserve the shared MPE setup prefix";
            return false;
         }
      }
      std::ostringstream success;
      success << "live run complete: bounded duration elapsed; worker drained and final note-offs sent";
      if (options.virtual_output) {
         success << "; virtual setup prefix and " << transmitted_words << " words verified";
      }
      message = success.str();
      return true;
   } catch (std::exception const& error) {
      shutdown();
      report.queue_high_water = state->ring.queue_high_water();
      report.overflow_count = state->ring.overflow_count();
      report.render_error_count = state->render_errors.load(std::memory_order_relaxed);
      report.discontinuity_count = worker_discontinuities
         + state->callback_discontinuities.load(std::memory_order_relaxed);
      message = std::string("live run failed: ") + error.what();
      return false;
   }
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
