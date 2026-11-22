#include "hz/converter.hpp"
#include "hz/q_pitch_estimator.hpp"

#include <cmath>
#include <iostream>
#include <numbers>
#include <vector>

namespace {

int g_failed = 0;

void expect(bool cond, char const* name) {
   if (cond) {
      std::cout << "  pass  " << name << "\n";
      return;
   }
   std::cout << "  FAIL  " << name << "\n";
   ++g_failed;
}

std::vector<float> tone(float hz, float sps, float seconds) {
   int n = int(sps * seconds);
   std::vector<float> x(n);
   for (int i = 0; i < n; ++i)
      x[i] = 0.5f * std::sin(2.f * std::numbers::pi_v<float> * hz * float(i) / sps);
   return x;
}

void test_q_detector_survives_short_live_blocks() {
   std::cout << "test_q_detector_survives_short_live_blocks\n";
   auto samples = tone(440.f, 48000.f, 0.5f);
   hz::Config cfg;
   hz::Converter conv(1, cfg, hz::make_q_pitch_estimator(1, cfg));
   std::vector<hz::MidiEvent> events;
   for (int pos = 0; pos < int(samples.size()); pos += 256) {
      int count = std::min(256, int(samples.size()) - pos);
      std::vector<float> block(samples.begin() + pos, samples.begin() + pos + count);
      auto ev = conv.feed(block, false);
      events.insert(events.end(), ev.begin(), ev.end());
   }
   auto final_events = conv.feed(nullptr, 0, 0, true);
   events.insert(events.end(), final_events.begin(), final_events.end());

   int ons = 0;
   int note = -1;
   bool final_off = false;
   for (auto const& e : events) {
      if (e.status == 0x90) {
         ++ons;
         note = e.data1;
      }
      if (e.status == 0x80 && e.data1 == 69 && e.sample == int(samples.size()))
         final_off = true;
   }
   expect(ons == 1, "Q produces one note-on across short blocks");
   expect(note == 69, "Q detects A4 as MIDI note 69");
   expect(final_off, "Q-backed take releases at the absolute stream end");
}

} // namespace

int main() {
   test_q_detector_survives_short_live_blocks();
   if (g_failed) {
      std::cerr << g_failed << " assertion(s) failed\n";
      return 1;
   }
   std::cout << "all Q tests passed\n";
   return 0;
}
