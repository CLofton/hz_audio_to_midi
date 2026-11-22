#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace hz {

struct MidiEvent {
   int sample = 0;
   int channel = 0;   // 0-based MIDI channel. Channel 0 is the MPE manager.
   int status = 0;    // 0x90 note on, 0x80 note off, 0xE0 bend, 0xB0 cc
   int data1 = 0;
   int data2 = 0;
};

struct PitchEstimate {
   float hz = 0.f;
   float periodicity = 0.f;
   bool valid = false;
};

// Normalized autocorrelation. Prefers the shortest lag near the best score
// so a clean tone reports its fundamental, not a multiple.
inline PitchEstimate estimate_pitch(
   float const* x, int n, float sps, float low_hz, float high_hz
) {
   PitchEstimate e;
   if (n < 64 || sps <= 0.f) return e;

   int min_lag = std::max(2, int(std::floor(sps / high_hz)));
   int max_lag = std::min(n / 2, int(std::ceil(sps / low_hz)));
   if (max_lag <= min_lag + 2) return e;

   std::vector<float> y(static_cast<std::size_t>(n));
   double mean = 0.0;
   for (int i = 0; i < n; ++i) mean += x[i];
   mean /= n;
   for (int i = 0; i < n; ++i) y[static_cast<std::size_t>(i)] = x[i] - float(mean);

   std::vector<double> acf(static_cast<std::size_t>(max_lag + 1), 0.0);
   double best = -1.0;
   int best_lag = min_lag;
   for (int lag = min_lag; lag <= max_lag; ++lag) {
      double sum = 0.0, e0 = 0.0, e1 = 0.0;
      int m = n - lag;
      for (int i = 0; i < m; ++i) {
         float a = y[static_cast<std::size_t>(i)];
         float b = y[static_cast<std::size_t>(i + lag)];
         sum += double(a) * double(b);
         e0 += double(a) * double(a);
         e1 += double(b) * double(b);
      }
      double score = sum / (std::sqrt(e0 * e1) + 1e-12);
      acf[static_cast<std::size_t>(lag)] = score;
      if (score > best) {
         best = score;
         best_lag = lag;
      }
   }

   // The autocorrelation of a tone is high at small lags too. The period is
   // the first local maximum, not the first high value and not a later
   // multiple of the period.
   int lag = -1;
   double peak_floor = std::max(0.55, best * 0.8);
   for (int L = min_lag + 1; L < max_lag; ++L) {
      double a = acf[static_cast<std::size_t>(L - 1)];
      double b = acf[static_cast<std::size_t>(L)];
      double c = acf[static_cast<std::size_t>(L + 1)];
      if (b >= a && b >= c && b >= peak_floor) {
         lag = L;
         break;
      }
   }
   if (lag < 0) lag = best_lag;

   double delta = 0.0;
   if (lag > min_lag && lag < max_lag) {
      double a = acf[static_cast<std::size_t>(lag - 1)];
      double b = acf[static_cast<std::size_t>(lag)];
      double c = acf[static_cast<std::size_t>(lag + 1)];
      double den = a - 2.0 * b + c;
      if (std::abs(den) > 1e-12) delta = 0.5 * (a - c) / den;
      if (delta > 1.0) delta = 1.0;
      if (delta < -1.0) delta = -1.0;
   }

   e.hz = float(sps / (double(lag) + delta));
   e.periodicity = float(std::clamp(best, 0.0, 1.0));
   e.valid = e.periodicity >= 0.5f && e.hz >= low_hz * 0.9f && e.hz <= high_hz * 1.1f;
   return e;
}

inline float rms_of(float const* x, int n) {
   if (n <= 0) return 0.f;
   double s = 0.0;
   for (int i = 0; i < n; ++i) s += double(x[i]) * double(x[i]);
   return float(std::sqrt(s / n));
}

inline int midi_note_of(float hz) {
   if (hz <= 0.f) return -1;
   float note = 69.f + 12.f * std::log2(hz / 440.f);
   return std::clamp(int(std::lround(note)), 0, 127);
}

inline float hz_of_note(int note) {
   return 440.f * std::pow(2.f, (note - 69) / 12.f);
}

// MPE bend: 8192 is center, range_semitones is the full ± span.
inline int bend_of(float hz, int note, int range_semitones) {
   float base = hz_of_note(note);
   if (base <= 0.f || hz <= 0.f || range_semitones < 1) return 8192;
   float cents = 1200.f * std::log2(hz / base);
   float span = float(range_semitones) * 100.f;
   int v = int(std::lround(8192.f + (cents / span) * 8192.f));
   return std::clamp(v, 0, 16383);
}

inline void push_bend(std::vector<MidiEvent>& out, int sample, int channel, int bend) {
   MidiEvent e;
   e.sample = sample;
   e.channel = channel;
   e.status = 0xE0;
   e.data1 = bend & 0x7F;
   e.data2 = (bend >> 7) & 0x7F;
   out.push_back(e);
}

struct Config {
   float sps = 48000.f;
   float low_hz = 50.f;
   float high_hz = 2000.f;
   int window = 2048;
   int hop = 1024;
   int release_hops = 2;
   int retrigger_hops = 2;
   int bend_semitones = 48;
   float min_periodicity = 0.72f;
   float min_rms = 0.015f;
};

// One monophonic tracker per input. Channel 0 is reserved for the MPE
// manager; input N speaks on MIDI channel N+1.
class Converter {
public:
   explicit Converter(int channels, Config cfg = {})
      : channels_(std::clamp(channels, 1, 15))
      , cfg_(cfg)
      , held_(static_cast<std::size_t>(channels_), -1)
      , quiet_(static_cast<std::size_t>(channels_), 0)
      , pending_(static_cast<std::size_t>(channels_), -1)
      , pending_n_(static_cast<std::size_t>(channels_), 0)
      , last_bend_(static_cast<std::size_t>(channels_), -1)
      , buffered_(static_cast<std::size_t>(channels_)) {}

   std::vector<MidiEvent> feed(std::vector<float> const& mono, bool end_of_take = true) {
      float const* p = mono.data();
      return feed(&p, 1, int(mono.size()), end_of_take);
   }

   // Ordinary blocks must carry exactly the configured channel count. A
   // variable-channel block is rejected without changing stream state. The
   // sole exception is feed(nullptr, 0, 0, true), which ends the take and
   // releases every held note. Other end_of_take calls release held notes too.
   std::vector<MidiEvent> feed(float const* const* ch, int nch, int frames, bool end_of_take = true) {
      std::vector<MidiEvent> out;
      bool final_empty = end_of_take && nch == 0 && frames == 0;
      if (!final_empty && nch != channels_) return out;
      int use = std::min(nch, channels_);
      if (frames < 0 || cfg_.window < 1 || cfg_.hop < 1) return out;
      for (int c = 0; c < use; ++c) {
         auto& buffer = buffered_[static_cast<std::size_t>(c)];
         if (frames > 0) buffer.insert(buffer.end(), ch[c], ch[c] + frames);
      }

      int pos = 0;
      int buffered_frames = int(buffered_[0].size());
      for (; use > 0 && pos + cfg_.window <= buffered_frames; pos += cfg_.hop) {
         int sample = stream_pos_ + pos;
         for (int c = 0; c < use; ++c) {
            float const* window = buffered_[static_cast<std::size_t>(c)].data() + pos;
            auto est = estimate_pitch(
               window, cfg_.window, cfg_.sps, cfg_.low_hz, cfg_.high_hz);
            float rms = rms_of(window, cfg_.window);
            bool gated = est.valid
               && est.periodicity >= cfg_.min_periodicity
               && rms >= cfg_.min_rms;
            auto& held = held_[static_cast<std::size_t>(c)];
            auto& quiet = quiet_[static_cast<std::size_t>(c)];
            auto& pending = pending_[static_cast<std::size_t>(c)];
            auto& pending_n = pending_n_[static_cast<std::size_t>(c)];

            if (!gated) {
               pending = -1;
               pending_n = 0;
               if (held < 0) continue;
               if (++quiet < cfg_.release_hops) continue;
               MidiEvent off;
               off.sample = sample;
               off.channel = c + 1;
               off.status = 0x80;
               off.data1 = held;
               off.data2 = 0;
               out.push_back(off);
               held = -1;
               quiet = 0;
               last_bend_[static_cast<std::size_t>(c)] = -1;
               continue;
            }

            quiet = 0;
            int note = midi_note_of(est.hz);
            int vel = std::clamp(int(std::lround(rms * 2.f * 127.f)), 1, 127);
            int bend = bend_of(est.hz, note, cfg_.bend_semitones);
            auto& last_bend = last_bend_[static_cast<std::size_t>(c)];

            auto emit_bend = [&]() {
               if (bend == last_bend) return;
               push_bend(out, sample, c + 1, bend);
               last_bend = bend;
            };

            if (held >= 0 && note != held) {
               if (pending != note) {
                  pending = note;
                  pending_n = 1;
               } else {
                  ++pending_n;
               }
               if (pending_n < cfg_.retrigger_hops) continue;
               MidiEvent off;
               off.sample = sample;
               off.channel = c + 1;
               off.status = 0x80;
               off.data1 = held;
               off.data2 = 0;
               out.push_back(off);
               MidiEvent on;
               on.sample = sample;
               on.channel = c + 1;
               on.status = 0x90;
               on.data1 = note;
               on.data2 = vel;
               out.push_back(on);
               held = note;
               pending = -1;
               pending_n = 0;
               last_bend = -1;
               emit_bend();
               continue;
            }

            pending = -1;
            pending_n = 0;
            if (held >= 0) {
               emit_bend();
               continue;
            }

            MidiEvent on;
            on.sample = sample;
            on.channel = c + 1;
            on.status = 0x90;
            on.data1 = note;
            on.data2 = vel;
            out.push_back(on);
            held = note;
            emit_bend();
         }
      }
      int consumed = std::min(pos, buffered_frames);
      for (int c = 0; c < use; ++c) {
         auto& buffer = buffered_[static_cast<std::size_t>(c)];
         buffer.erase(buffer.begin(), buffer.begin() + consumed);
      }
      stream_pos_ += consumed;
      int end_sample = stream_pos_ + int(buffered_[0].size());
      for (int c = 0; c < channels_ && end_of_take; ++c) {
         auto& held = held_[static_cast<std::size_t>(c)];
         if (held >= 0) {
            MidiEvent off;
            off.sample = end_sample;
            off.channel = c + 1;
            off.status = 0x80;
            off.data1 = held;
            off.data2 = 0;
            out.push_back(off);
            held = -1;
         }
         quiet_[static_cast<std::size_t>(c)] = 0;
         last_bend_[static_cast<std::size_t>(c)] = -1;
         buffered_[static_cast<std::size_t>(c)].clear();
      }
      if (end_of_take) stream_pos_ = end_sample;
      return out;
   }

   Config const& config() const { return cfg_; }

private:
   int channels_;
   Config cfg_;
   std::vector<int> held_;
   std::vector<int> quiet_;
   std::vector<int> pending_;
   std::vector<int> pending_n_;
   std::vector<int> last_bend_;
   std::vector<std::vector<float>> buffered_;
   int stream_pos_ = 0;
};

inline void put_u16(std::string& o, unsigned v) {
   o.push_back(static_cast<char>((v >> 8) & 0xFF));
   o.push_back(static_cast<char>(v & 0xFF));
}

inline void put_u32(std::string& o, unsigned v) {
   o.push_back(static_cast<char>((v >> 24) & 0xFF));
   o.push_back(static_cast<char>((v >> 16) & 0xFF));
   o.push_back(static_cast<char>((v >> 8) & 0xFF));
   o.push_back(static_cast<char>(v & 0xFF));
}

inline void put_vlq(std::string& o, unsigned v) {
   unsigned buffer = v & 0x7F;
   while ((v >>= 7) != 0) {
      buffer <<= 8;
      buffer |= (v & 0x7F) | 0x80;
   }
   while (true) {
      o.push_back(static_cast<char>(buffer & 0xFF));
      if (buffer & 0x80) buffer >>= 8;
      else break;
   }
}

inline bool write_midi(
   std::string const& path,
   std::vector<MidiEvent> const& events,
   float sps,
   int channels,
   int bend_semitones
) {
   if (sps <= 0.f) return false;
   channels = std::clamp(channels, 1, 15);
   bend_semitones = std::clamp(bend_semitones, 1, 96);

   struct Row { int tick; int order; int channel; int status; int d1; int d2; };
   std::vector<Row> rows;
   int order = 0;
   auto add = [&](int tick, int channel, int status, int d1, int d2) {
      rows.push_back(Row{tick, order++, channel, status, d1, d2});
   };

   // Manager channel 0, members on 1..N. RPN 6 sets the zone member count.
   add(0, 0, 0xB0, 101, 0);
   add(0, 0, 0xB0, 100, 6);
   add(0, 0, 0xB0, 6, channels);
   add(0, 0, 0xB0, 38, 0);
   for (int c = 1; c <= channels; ++c) {
      add(0, c, 0xB0, 101, 0);
      add(0, c, 0xB0, 100, 0);
      add(0, c, 0xB0, 6, bend_semitones);
      add(0, c, 0xB0, 38, 0);
   }
   for (auto const& e : events) {
      int tick = int(std::llround(double(e.sample) * 960.0 / double(sps)));
      if (tick < 0) tick = 0;
      add(tick, e.channel, e.status, e.data1, e.data2);
   }
   std::stable_sort(rows.begin(), rows.end(), [](Row const& a, Row const& b) {
      if (a.tick != b.tick) return a.tick < b.tick;
      return a.order < b.order;
   });

   std::string track;
   put_vlq(track, 0);
   track.push_back(static_cast<char>(0xFF));
   track.push_back(static_cast<char>(0x51));
   track.push_back(static_cast<char>(0x03));
   track.push_back(static_cast<char>(0x07));
   track.push_back(static_cast<char>(0xA1));
   track.push_back(static_cast<char>(0x20)); // 120 bpm

   int last = 0;
   for (auto const& row : rows) {
      put_vlq(track, static_cast<unsigned>(std::max(0, row.tick - last)));
      last = row.tick;
      track.push_back(static_cast<char>((row.status & 0xF0) | (row.channel & 0x0F)));
      track.push_back(static_cast<char>(row.d1 & 0x7F));
      track.push_back(static_cast<char>(row.d2 & 0x7F));
   }
   put_vlq(track, 0);
   track.push_back(static_cast<char>(0xFF));
   track.push_back(static_cast<char>(0x2F));
   track.push_back(static_cast<char>(0x00));

   std::string file;
   file.append("MThd");
   put_u32(file, 6);
   put_u16(file, 0);
   put_u16(file, 1);
   put_u16(file, 480);
   file.append("MTrk");
   put_u32(file, static_cast<unsigned>(track.size()));
   file.append(track);

   std::ofstream out(path, std::ios::binary);
   if (!out) return false;
   out.write(file.data(), static_cast<std::streamsize>(file.size()));
   return out.good();
}

} // namespace hz
