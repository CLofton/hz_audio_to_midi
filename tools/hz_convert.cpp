#include "hz/converter.hpp"
#ifdef HZ_WITH_Q
#include "hz/q_pitch_estimator.hpp"
#endif

#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

void usage() {
   std::cerr
      << "usage:\n"
      << "  hz_convert --tone HZ [--seconds 0.5] [--sps 48000] --out FILE.mid\n"
      << "  hz_convert --wav FILE.wav --out FILE.mid\n"
      << "  detector options: [--low-hz 50] [--high-hz 2000]\n"
      << "\n"
      << "One monophonic tracker per input channel. Channel 1 is the MPE\n"
      << "manager; input N is written on MIDI channel N+1.\n";
}

bool arg_eq(std::string const& a, char const* b) { return a == b; }

bool parse_number(std::string const& text, float& value) {
   try {
      std::size_t used = 0;
      value = std::stof(text, &used);
      return used == text.size() && std::isfinite(value);
   } catch (...) {
      return false;
   }
}

std::vector<float> tone(float hz, float sps, float seconds, float amp = 0.5f) {
   int n = int(sps * seconds);
   std::vector<float> x(n);
   for (int i = 0; i < n; ++i)
      x[i] = amp * std::sin(2.f * float(M_PI) * hz * float(i) / sps);
   return x;
}

// Mono or interleaved multichannel WAV: 16-bit PCM or 32-bit IEEE float.
// Returns false on anything else.
bool read_wav(
   std::string const& path,
   std::vector<std::vector<float>>& channels,
   float& sps
) {
   std::ifstream in(path, std::ios::binary);
   if (!in) return false;
   std::vector<unsigned char> b(
      (std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
   if (b.size() < 44) return false;
   if (std::string(b.begin(), b.begin() + 4) != "RIFF") return false;
   if (std::string(b.begin() + 8, b.begin() + 12) != "WAVE") return false;

   auto u16 = [&](std::size_t i) -> unsigned {
      return b[i] | (unsigned(b[i + 1]) << 8);
   };
   auto u32 = [&](std::size_t i) -> unsigned {
      return b[i] | (unsigned(b[i + 1]) << 8) | (unsigned(b[i + 2]) << 16) | (unsigned(b[i + 3]) << 24);
   };

   unsigned format = 0, nch = 0, rate = 0, bits = 0;
   std::size_t data = 0, data_n = 0;
   for (std::size_t i = 12; i + 8 <= b.size();) {
      std::string id(b.begin() + std::ptrdiff_t(i), b.begin() + std::ptrdiff_t(i + 4));
      unsigned size = u32(i + 4);
      std::size_t body = i + 8;
      if (body + size > b.size()) return false;
      if (id == "fmt ") {
         if (size < 16) return false;
         format = u16(body);
         nch = u16(body + 2);
         rate = u32(body + 4);
         bits = u16(body + 14);
      } else if (id == "data") {
         data = body;
         data_n = size;
      }
      i = body + size + (size & 1);
   }
   bool pcm16 = format == 1 && bits == 16;
   bool float32 = format == 3 && bits == 32 && sizeof(float) == 4;
   if ((!pcm16 && !float32) || nch < 1 || nch > 8 || rate == 0 || data == 0)
      return false;

   unsigned bytes_per_sample = bits / 8;
   std::size_t frames = data_n / (nch * bytes_per_sample);
   channels.assign(nch, std::vector<float>(frames));
   sps = float(rate);
   for (std::size_t f = 0; f < frames; ++f) {
      for (unsigned c = 0; c < nch; ++c) {
         std::size_t o = data + (f * nch + c) * bytes_per_sample;
         if (pcm16) {
            int16_t s = int16_t(b[o] | (unsigned(b[o + 1]) << 8));
            channels[c][f] = float(s) / 32768.f;
         } else {
            std::uint32_t raw = std::uint32_t(b[o])
               | (std::uint32_t(b[o + 1]) << 8)
               | (std::uint32_t(b[o + 2]) << 16)
               | (std::uint32_t(b[o + 3]) << 24);
            float sample = 0.f;
            std::memcpy(&sample, &raw, sizeof(sample));
            channels[c][f] = sample;
         }
      }
   }
   return true;
}

} // namespace

int main(int argc, char** argv) {
   float tone_hz = 0.f;
   float seconds = 0.5f;
   float sps = 48000.f;
   float low_hz = 50.f;
   float high_hz = 2000.f;
   std::string wav, out;
   for (int i = 1; i < argc; ++i) {
      std::string a = argv[i];
      auto need = [&](char const* name) -> std::string {
         if (i + 1 >= argc) {
            std::cerr << "missing value for " << name << "\n";
            std::exit(2);
         }
         return argv[++i];
      };
      auto number = [&](char const* name, float& value) {
         std::string text = need(name);
         if (parse_number(text, value)) return true;
         std::cerr << "invalid numeric value for " << name << ": " << text << "\n";
         return false;
      };
      if (arg_eq(a, "--tone")) { if (!number("--tone", tone_hz)) return 2; }
      else if (arg_eq(a, "--seconds")) { if (!number("--seconds", seconds)) return 2; }
      else if (arg_eq(a, "--sps")) { if (!number("--sps", sps)) return 2; }
      else if (arg_eq(a, "--low-hz")) { if (!number("--low-hz", low_hz)) return 2; }
      else if (arg_eq(a, "--high-hz")) { if (!number("--high-hz", high_hz)) return 2; }
      else if (arg_eq(a, "--wav")) wav = need("--wav");
      else if (arg_eq(a, "--out")) out = need("--out");
      else if (arg_eq(a, "--help") || arg_eq(a, "-h")) { usage(); return 0; }
      else { std::cerr << "unknown arg " << a << "\n"; usage(); return 2; }
   }
   if (tone_hz < 0.f || tone_hz > 20000.f
      || seconds <= 0.f || seconds > 60.f
      || sps < 1000.f || sps > 192000.f
      || low_hz < 20.f || high_hz > 20000.f || high_hz <= low_hz) {
      std::cerr << "numeric argument out of range\n";
      return 2;
   }
   if (wav.empty() && double(sps) * double(seconds) > 10000000.0) {
      std::cerr << "tone is too long at the requested sample rate\n";
      return 2;
   }
   if (out.empty() || (wav.empty() && tone_hz <= 0.f)) { usage(); return 2; }

   std::vector<std::vector<float>> channels;
   if (!wav.empty()) {
      if (!read_wav(wav, channels, sps)) {
         std::cerr << "could not read 16-bit PCM or 32-bit float WAV: " << wav << "\n";
         return 1;
      }
   } else {
      channels.push_back(tone(tone_hz, sps, seconds));
   }

   hz::Config cfg;
   cfg.sps = sps;
   cfg.low_hz = low_hz;
   cfg.high_hz = high_hz;
#ifdef HZ_WITH_Q
   cfg.onset_hops = 3;
   auto estimator = hz::make_q_pitch_estimator(int(channels.size()), cfg);
   hz::Converter conv(int(channels.size()), cfg, estimator);
   char const* detector = "q";
#else
   hz::Converter conv(int(channels.size()), cfg);
   char const* detector = "autocorrelation";
#endif
   std::vector<float const*> ptrs(channels.size());
   for (std::size_t c = 0; c < channels.size(); ++c) ptrs[c] = channels[c].data();
   int frames = channels.empty() ? 0 : int(channels[0].size());
   auto events = conv.feed(ptrs.data(), int(ptrs.size()), frames);
   if (!hz::write_midi(out, events, sps, int(channels.size()), cfg.bend_semitones)) {
      std::cerr << "could not write " << out << "\n";
      return 1;
   }

   int ons = 0;
   for (auto const& e : events) if (e.status == 0x90) ++ons;
   std::cout << "wrote " << out << "  channels=" << channels.size()
             << "  frames=" << frames << "  note-ons=" << ons
             << "  detector=" << detector << "\n";
   return 0;
}
