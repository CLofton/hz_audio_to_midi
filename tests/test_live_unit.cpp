#include "hz/converter.hpp"
#include "hz/live.hpp"
#include "hz/live_stream.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <string>

namespace {

bool check(bool condition, std::string const& message) {
   if (!condition) std::cerr << message << '\n';
   return condition;
}

bool midi_ump_layout() {
   hz::MidiEvent event{0, 1, 0x90, 69, 100};
   std::uint32_t const expected =
      0x20000000u | (std::uint32_t(0x91) << 16)
      | (std::uint32_t(69) << 8) | std::uint32_t(100);
   return check(hz::midi1_channel_voice_ump(event) == expected,
      "MIDI 1 channel-voice UMP word did not match the protocol layout");
}

bool ring_preserves_one_planar_block() {
   hz::CaptureRing ring;
   auto* writable = ring.acquire_write();
   bool ok = check(writable != nullptr, "empty capture ring did not provide a slot");
   if (writable == nullptr) return false;
   writable->samples[0][0] = 0.25f;
   writable->samples[0][511] = -0.5f;
   writable->samples[1][0] = 0.75f;
   ring.commit_write(512, 2, 4096, 123456);
   auto const* readable = ring.acquire_read();
   ok = check(readable != nullptr, "committed capture block was not readable") && ok;
   if (readable == nullptr) return false;
   ok = check(readable->frames == 512 && readable->channels == 2,
      "capture block dimensions changed") && ok;
   ok = check(readable->start_sample == 4096 && readable->capture_host_time == 123456,
      "capture block timing changed") && ok;
   ok = check(readable->samples[0][0] == 0.25f
      && readable->samples[0][511] == -0.5f
      && readable->samples[1][0] == 0.75f,
      "planar capture samples changed") && ok;
   ring.release_read();
   ok = check(ring.acquire_read() == nullptr, "released capture block remained queued") && ok;
   return ok;
}

bool ring_wraps_with_variable_blocks() {
   hz::CaptureRing ring;
   bool ok = true;
   for (int round = 0; round < 3; ++round) {
      for (int index = 0; index < hz::capture_ring_slots; ++index) {
         auto* writable = ring.acquire_write();
         ok = check(writable != nullptr, "capture ring wrapped too early") && ok;
         if (writable == nullptr) return false;
         std::uint32_t const frames = index % 3 == 0 ? 1u
            : (index % 3 == 1 ? 511u : 1024u);
         std::uint32_t const channels = std::uint32_t(index % hz::capture_max_channels + 1);
         int const serial = round * hz::capture_ring_slots + index;
         writable->samples[0][0] = float(serial) + 0.25f;
         writable->samples[channels - 1][frames - 1] = -float(serial) - 0.75f;
         ring.commit_write(frames, channels,
            std::uint64_t(serial * 1024), std::uint64_t(9000 + serial));
      }
      for (int index = 0; index < hz::capture_ring_slots; ++index) {
         auto const* readable = ring.acquire_read();
         ok = check(readable != nullptr, "wrapped capture block disappeared") && ok;
         if (readable == nullptr) return false;
         std::uint32_t const frames = index % 3 == 0 ? 1u
            : (index % 3 == 1 ? 511u : 1024u);
         std::uint32_t const channels = std::uint32_t(index % hz::capture_max_channels + 1);
         int const serial = round * hz::capture_ring_slots + index;
         bool const matches = readable->frames == frames && readable->channels == channels
            && readable->start_sample == std::uint64_t(serial * 1024)
            && readable->capture_host_time == std::uint64_t(9000 + serial)
            && ((frames == 1 && channels == 1)
               || readable->samples[0][0] == float(serial) + 0.25f)
            && readable->samples[channels - 1][frames - 1] == -float(serial) - 0.75f;
         if (!matches) {
            std::cerr << "round=" << round << " index=" << index
               << " frames=" << readable->frames << " channels=" << readable->channels
               << " start=" << readable->start_sample
               << " host=" << readable->capture_host_time
               << " first=" << readable->samples[0][0]
               << " last=" << readable->samples[channels - 1][frames - 1] << '\n';
         }
         ok = check(matches, "wrap or variable block corrupted capture data") && ok;
         ring.release_read();
      }
   }
   return ok;
}

bool ring_overflow_is_counted_and_never_overwrites() {
   hz::CaptureRing ring;
   bool ok = true;
   for (int index = 0; index < hz::capture_ring_slots; ++index) {
      auto* writable = ring.acquire_write();
      ok = check(writable != nullptr, "capture ring filled before its fixed capacity") && ok;
      if (writable == nullptr) return false;
      writable->samples[0][0] = float(index + 1);
      ring.commit_write(1, 1, std::uint64_t(index), std::uint64_t(index));
   }
   ok = check(ring.queue_high_water() == std::uint32_t(hz::capture_ring_slots),
      "capture queue high-water mark was not tracked") && ok;
   ok = check(ring.acquire_write() == nullptr,
      "full capture ring allowed a seventeenth writer") && ok;
   ok = check(ring.overflow_count() == 1, "capture overflow was not counted") && ok;
   auto const* first = ring.acquire_read();
   ok = check(first != nullptr && first->samples[0][0] == 1.0f,
      "capture overflow overwrote the oldest queued block") && ok;
   return ok;
}

bool capture_configuration_rejects_unsafe_formats() {
   hz::CaptureConfig valid;
   bool ok = check(hz::capture_config_error(valid) == nullptr,
      "48 kHz, 512-frame, eight-channel configuration was rejected");
   auto const rejects = [&](hz::CaptureConfig config, char const* message) {
      ok = check(hz::capture_config_error(config) != nullptr, message) && ok;
   };
   auto config = valid;
   config.sample_rate = 44100.0;
   rejects(config, "non-48-kHz capture was accepted");
   config = valid;
   config.channels = 0;
   rejects(config, "zero-channel capture was accepted");
   config = valid;
   config.channels = 9;
   rejects(config, "nine-channel capture was accepted");
   config = valid;
   config.requested_frames = 256;
   rejects(config, "non-512-frame request was accepted");
   config = valid;
   config.maximum_frames = 1025;
   rejects(config, "capture blocks larger than the preallocated slots were accepted");
   config = valid;
   config.maximum_frames = 511;
   rejects(config, "maximum block size below the requested block size was accepted");
   return ok;
}

bool capture_discontinuity_is_fatal_to_sequence_validation() {
   hz::CaptureSequence sequence;
   bool ok = check(sequence.accept(0, 512), "initial capture block was rejected");
   ok = check(sequence.accept(512, 1), "contiguous variable capture block was rejected") && ok;
   ok = check(!sequence.accept(514, 10), "capture sample gap was silently accepted") && ok;
   ok = check(sequence.discontinuity_count() == 1,
      "capture discontinuity was not counted") && ok;
   return ok;
}

bool capture_timeline_maps_epoch_and_variable_blocks() {
   hz::CaptureTimeline timeline;
   constexpr double epoch = 8589934592.0;
   std::uint64_t stream_sample = 999;
   bool ok = check(timeline.accept(true, epoch, 512, stream_sample)
      == hz::CaptureTimelineResult::accepted, "initial device epoch was rejected");
   ok = check(stream_sample == 0, "initial device epoch did not map to stream sample zero") && ok;
   ok = check(timeline.accept(true, epoch + 512.0, 1, stream_sample)
      == hz::CaptureTimelineResult::accepted, "continuous one-frame block was rejected") && ok;
   ok = check(stream_sample == 512, "one-frame block used the wrong stream position") && ok;
   ok = check(timeline.accept(true, epoch + 513.0, 511, stream_sample)
      == hz::CaptureTimelineResult::accepted, "continuous variable block was rejected") && ok;
   ok = check(stream_sample == 513, "variable blocks did not advance contiguous stream time") && ok;
   return ok;
}

bool capture_timeline_rejects_device_gap_before_publication() {
   std::atomic<std::uint32_t> callback_discontinuities{0};
   hz::CaptureTimeline timeline(&callback_discontinuities);
   hz::CaptureRing ring;
   auto publish = [&](double device_sample, std::uint32_t frames) {
      auto* block = ring.acquire_write();
      if (block == nullptr) return hz::CaptureTimelineResult::discontinuity;
      std::uint64_t stream_sample = 999;
      auto const result = timeline.accept(true, device_sample, frames, stream_sample);
      if (result == hz::CaptureTimelineResult::accepted) {
         ring.commit_write(frames, 1, stream_sample, 0);
      }
      return result;
   };
   bool ok = check(publish(40000.0, 512) == hz::CaptureTimelineResult::accepted,
      "initial timeline block was not published");
   auto const* first = ring.acquire_read();
   ok = check(first != nullptr && first->start_sample == 0,
      "initial timeline block had the wrong stream epoch") && ok;
   if (first != nullptr) ring.release_read();
   ok = check(callback_discontinuities.load(std::memory_order_relaxed) == 0,
      "continuous callback blocks were miscounted as discontinuities") && ok;
   ok = check(publish(40513.0, 1) == hz::CaptureTimelineResult::discontinuity,
      "real device sample gap was not rejected") && ok;
   ok = check(ring.acquire_read() == nullptr,
      "device sample gap reached ring publication") && ok;
   ok = check(callback_discontinuities.load(std::memory_order_relaxed) == 1,
      "rejected device gap did not increment shared callback accounting") && ok;
   return ok;
}

bool capture_timeline_rejects_missing_and_invalid_timestamps() {
   struct Case { bool valid; double sample; };
   std::array<Case, 6> const cases{{
      {false, 1000.0},
      {true, std::numeric_limits<double>::quiet_NaN()},
      {true, std::numeric_limits<double>::infinity()},
      {true, -std::numeric_limits<double>::infinity()},
      {true, -1.0},
      {true, 12.5}
   }};
   bool ok = true;
   for (auto const& test : cases) {
      hz::CaptureTimeline timeline;
      std::uint64_t stream_sample = 777;
      ok = check(timeline.accept(test.valid, test.sample, 512, stream_sample)
         == hz::CaptureTimelineResult::invalid_timestamp,
         "missing or invalid device timestamp was not rejected") && ok;
      ok = check(stream_sample == 777,
         "invalid device timestamp mutated the output stream position") && ok;
      ok = check(timeline.accept(true, 700.0, 8, stream_sample)
         == hz::CaptureTimelineResult::accepted && stream_sample == 0,
         "rejected timestamp improperly established or advanced the timeline") && ok;
   }
   return ok;
}

bool live_run_arguments_are_explicit_and_bounded() {
   hz::LiveRunOptions options;
   std::string error;
   bool ok = check(hz::parse_live_run_arguments(
      {"--run", "--input", "BuiltInMicrophoneDevice", "--midi-index", "2", "--seconds", "5"},
      options, error), "explicit valid live-run arguments were rejected: " + error);
   ok = check(options.input_uid == "BuiltInMicrophoneDevice"
      && options.midi_index == 2 && options.seconds == 5.0,
      "live-run arguments were parsed incorrectly") && ok;
   std::vector<std::vector<std::string>> const invalid{
      {}, {"--run"}, {"--run", "--input", "UID", "--midi-index", "0"},
      {"--run", "--input", "", "--midi-index", "0", "--seconds", "5"},
      {"--run", "--input", "UID", "--midi-index", "-1", "--seconds", "5"},
      {"--run", "--input", "UID", "--midi-index", "0", "--seconds", "0"},
      {"--run", "--input", "UID", "--midi-index", "0", "--seconds", "3601"},
      {"--run", "--input", "UID", "--midi-index", "0", "--seconds", "nan"},
      {"--run", "--input", "UID", "--midi-index", "zero", "--seconds", "5"},
      {"--run", "--midi-index", "0", "--input", "UID", "--seconds", "5"}
   };
   for (auto const& arguments : invalid) {
      ok = check(!hz::parse_live_run_arguments(arguments, options, error),
         "unsafe or ambiguous live-run arguments were accepted") && ok;
   }
   return ok;
}

} // namespace

int main() {
   int failures = 0;
   failures += !midi_ump_layout();
   failures += !ring_preserves_one_planar_block();
   failures += !ring_wraps_with_variable_blocks();
   failures += !ring_overflow_is_counted_and_never_overwrites();
   failures += !capture_configuration_rejects_unsafe_formats();
   failures += !capture_discontinuity_is_fatal_to_sequence_validation();
   failures += !capture_timeline_maps_epoch_and_variable_blocks();
   failures += !capture_timeline_rejects_device_gap_before_publication();
   failures += !capture_timeline_rejects_missing_and_invalid_timestamps();
   failures += !live_run_arguments_are_explicit_and_bounded();
   if (failures == 0) std::cout << "all live unit tests passed\n";
   return failures == 0 ? 0 : 1;
}
