#pragma once

#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace hz {

inline constexpr int capture_ring_slots = 16;
inline constexpr int capture_max_channels = 8;
inline constexpr int capture_max_frames = 1024;

struct CaptureConfig {
   double sample_rate = 48000.0;
   int channels = 8;
   int requested_frames = 512;
   int maximum_frames = capture_max_frames;
};

inline int capture_maximum_frames_for_range(double minimum, double maximum) noexcept {
   if (!std::isfinite(minimum) || !std::isfinite(maximum)
      || minimum < 1.0 || maximum < minimum
      || minimum > 512.0 || maximum < 512.0) {
      return 0;
   }
   return capture_max_frames;
}

inline char const* capture_config_error(CaptureConfig const& config) noexcept {
   if (config.sample_rate != 48000.0) return "capture sample rate must be exactly 48000 Hz";
   if (config.channels < 1 || config.channels > capture_max_channels) {
      return "capture channel count must be in 1..8";
   }
   if (config.requested_frames != 512) return "capture device buffer request must be 512 frames";
   if (config.maximum_frames < config.requested_frames
      || config.maximum_frames > capture_max_frames) {
      return "capture maximum block size must be in 512..1024 frames";
   }
   return nullptr;
}

struct LiveRunOptions {
   std::string input_uid;
   int midi_index = -1;
   double seconds = 0.0;
   bool virtual_output = false;
};

inline bool parse_live_run_arguments(
   std::vector<std::string> const& arguments, LiveRunOptions& options, std::string& error
) {
   bool const common = arguments.size() >= 4 && arguments[0] == "--run"
      && arguments[1] == "--input" && !arguments[2].empty();
   bool const indexed = arguments.size() == 7 && arguments[3] == "--midi-index"
      && arguments[5] == "--seconds";
   bool const virtual_output = arguments.size() == 6 && arguments[3] == "--virtual-out"
      && arguments[4] == "--seconds";
   if (!common || (!indexed && !virtual_output)) {
      error = "live mode requires exactly one output: --midi-index N or --virtual-out";
      return false;
   }
   LiveRunOptions parsed;
   parsed.input_uid = arguments[2];
   parsed.virtual_output = virtual_output;
   if (indexed) {
      auto const& midi_text = arguments[4];
      auto const midi_result = std::from_chars(
         midi_text.data(), midi_text.data() + midi_text.size(), parsed.midi_index);
      if (midi_text.empty() || midi_result.ec != std::errc{}
         || midi_result.ptr != midi_text.data() + midi_text.size() || parsed.midi_index < 0) {
         error = "MIDI destination index must be a non-negative integer";
         return false;
      }
   }
   std::string const& seconds_text = arguments[virtual_output ? 5 : 6];
   char* end = nullptr;
   errno = 0;
   parsed.seconds = std::strtod(seconds_text.c_str(), &end);
   if (seconds_text.empty() || end != seconds_text.c_str() + seconds_text.size()
      || errno == ERANGE || !std::isfinite(parsed.seconds)
      || parsed.seconds <= 0.0 || parsed.seconds > 3600.0) {
      error = "live duration must be finite and in (0, 3600] seconds";
      return false;
   }
   options = std::move(parsed);
   error.clear();
   return true;
}

struct CaptureBlock {
   std::uint64_t start_sample = 0;
   std::uint64_t capture_host_time = 0;
   std::uint32_t frames = 0;
   std::uint32_t channels = 0;
   std::array<std::array<float, capture_max_frames>, capture_max_channels> samples{};
};

// Fixed single-producer/single-consumer storage owned for the entire capture run.
// Monotonic indices provide acquire/release publication without callback locks.
class CaptureRing {
public:
   CaptureBlock* acquire_write() noexcept {
      auto const head = head_.load(std::memory_order_relaxed);
      auto const tail = tail_.load(std::memory_order_acquire);
      if (head - tail >= std::uint32_t(capture_ring_slots)) {
         overflows_.fetch_add(1, std::memory_order_relaxed);
         return nullptr;
      }
      return &blocks_[head % std::uint32_t(capture_ring_slots)];
   }

   void commit_write(
      std::uint32_t frames, std::uint32_t channels,
      std::uint64_t start_sample, std::uint64_t capture_host_time
   ) noexcept {
      auto const head = head_.load(std::memory_order_relaxed);
      auto& block = blocks_[head % std::uint32_t(capture_ring_slots)];
      block.frames = frames;
      block.channels = channels;
      block.start_sample = start_sample;
      block.capture_host_time = capture_host_time;
      auto const depth = head - tail_.load(std::memory_order_acquire) + 1;
      auto high_water = high_water_.load(std::memory_order_relaxed);
      while (high_water < depth && !high_water_.compare_exchange_weak(
         high_water, depth, std::memory_order_relaxed)) {}
      head_.store(head + 1, std::memory_order_release);
   }

   CaptureBlock const* acquire_read() noexcept {
      auto const tail = tail_.load(std::memory_order_relaxed);
      if (tail == head_.load(std::memory_order_acquire)) return nullptr;
      return &blocks_[tail % std::uint32_t(capture_ring_slots)];
   }

   void release_read() noexcept {
      auto const tail = tail_.load(std::memory_order_relaxed);
      tail_.store(tail + 1, std::memory_order_release);
   }

   std::uint32_t queue_high_water() const noexcept {
      return high_water_.load(std::memory_order_relaxed);
   }

   std::uint32_t overflow_count() const noexcept {
      return overflows_.load(std::memory_order_relaxed);
   }

private:
   std::array<CaptureBlock, capture_ring_slots> blocks_{};
   std::atomic<std::uint32_t> head_{0};
   std::atomic<std::uint32_t> tail_{0};
   std::atomic<std::uint32_t> high_water_{0};
   std::atomic<std::uint32_t> overflows_{0};
};

enum class CaptureTimelineResult {
   accepted,
   invalid_timestamp,
   discontinuity
};

class CaptureTimeline {
public:
   explicit CaptureTimeline(
      std::atomic<std::uint32_t>* callback_discontinuities = nullptr
   ) noexcept : callback_discontinuities_(callback_discontinuities) {}

   CaptureTimelineResult accept(
      bool sample_time_valid, double device_sample, std::uint32_t frames,
      std::uint64_t& stream_sample
   ) noexcept {
      constexpr double maximum_exact_integer = 9007199254740991.0; // 2^53 - 1
      if (!sample_time_valid || !std::isfinite(device_sample) || device_sample < 0.0
         || device_sample > maximum_exact_integer || std::trunc(device_sample) != device_sample) {
         return CaptureTimelineResult::invalid_timestamp;
      }
      if (!started_) {
         expected_device_sample_ = device_sample;
         started_ = true;
      }
      if (device_sample != expected_device_sample_) {
         if (callback_discontinuities_ != nullptr) {
            callback_discontinuities_->fetch_add(1, std::memory_order_relaxed);
         }
         return CaptureTimelineResult::discontinuity;
      }
      stream_sample = next_stream_sample_;
      expected_device_sample_ = device_sample + frames;
      next_stream_sample_ += frames;
      return CaptureTimelineResult::accepted;
   }

private:
   std::atomic<std::uint32_t>* callback_discontinuities_;
   bool started_ = false;
   double expected_device_sample_ = 0.0;
   std::uint64_t next_stream_sample_ = 0;
};

class CaptureSequence {
public:
   bool accept(std::uint64_t start_sample, std::uint32_t frames) noexcept {
      if (start_sample != next_sample_) {
         ++discontinuities_;
         return false;
      }
      next_sample_ += frames;
      return true;
   }

   std::uint32_t discontinuity_count() const noexcept { return discontinuities_; }

private:
   std::uint64_t next_sample_ = 0;
   std::uint32_t discontinuities_ = 0;
};

} // namespace hz
