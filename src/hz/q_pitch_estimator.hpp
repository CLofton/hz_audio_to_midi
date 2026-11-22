#pragma once

#include "hz/converter.hpp"

#include <q/pitch/pitch_detector.hpp>

#include <algorithm>
#include <memory>
#include <vector>

namespace hz::detail {

class QPitchEstimatorState {
public:
   QPitchEstimatorState(int channels, Config const& cfg)
      : next_sample_(static_cast<std::size_t>(std::clamp(channels, 1, 15)), 0) {
      int count = int(next_sample_.size());
      detectors_.reserve(next_sample_.size());
      for (int c = 0; c < count; ++c) {
         detectors_.push_back(std::make_unique<cycfi::q::pitch_detector>(
            cycfi::q::frequency{cfg.low_hz},
            cycfi::q::frequency{cfg.high_hz},
            cfg.sps,
            cycfi::q::dB(-45.0)));
      }
   }

   PitchEstimate estimate(
      int channel, float const* samples, int n, Config const& cfg, int sample
   ) {
      auto& pd = *detectors_[static_cast<std::size_t>(channel)];
      auto& next = next_sample_[static_cast<std::size_t>(channel)];

      // Windows overlap, and a later feed() can begin with an old buffered
      // tail. Submit only samples this detector has never seen.
      int start = std::clamp(next - sample, 0, n);
      for (int i = start; i < n; ++i) (void)pd(samples[i]);
      next = sample + n;

      PitchEstimate out;
      out.periodicity = pd.periodicity();
      if (out.periodicity >= cfg.min_periodicity) {
         out.hz = pd.get_frequency();
         out.valid = out.hz > 0.f
            && out.hz >= cfg.low_hz * 0.9f
            && out.hz <= cfg.high_hz * 1.1f;
      }
      return out;
   }

private:
   std::vector<std::unique_ptr<cycfi::q::pitch_detector>> detectors_;
   std::vector<int> next_sample_;
};

} // namespace hz::detail

namespace hz {

// The shared state gives every input one persistent Q detector while the
// Converter remains copyable through this type-erased estimator callback.
inline PitchEstimator make_q_pitch_estimator(int channels, Config const& cfg) {
   auto state = std::make_shared<detail::QPitchEstimatorState>(channels, cfg);
   return [state](int channel, float const* samples, int n, Config const& config, int sample) {
      return state->estimate(channel, samples, n, config, sample);
   };
}

} // namespace hz
