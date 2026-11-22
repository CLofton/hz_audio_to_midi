#include "hz/converter.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <string>
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

std::vector<float> tone(float hz, float sps, float seconds, float amp = 0.5f) {
   int n = int(sps * seconds);
   std::vector<float> x(n);
   for (int i = 0; i < n; ++i)
      x[i] = amp * std::sin(2.f * float(M_PI) * hz * float(i) / sps);
   return x;
}

std::vector<hz::MidiEvent> note_ons(std::vector<hz::MidiEvent> const& ev) {
   std::vector<hz::MidiEvent> out;
   for (auto const& e : ev)
      if (e.status == 0x90) out.push_back(e);
   return out;
}

void test_a440_is_note_69() {
   std::cout << "test_a440_is_note_69\n";
   auto samples = tone(440.f, 48000.f, 0.5f);
   hz::Converter conv(1);
   auto ons = note_ons(conv.feed(samples));
   expect(ons.size() == 1, "one note-on");
   if (!ons.empty()) {
      expect(ons[0].data1 == 69, "MIDI note 69 (A4)");
      expect(ons[0].channel == 1, "member channel 1 (MIDI ch 2)");
      expect(ons[0].data2 >= 1 && ons[0].data2 <= 127, "velocity in 1..127");
   }
}

void test_pitch_jump_retriggers() {
   std::cout << "test_pitch_jump_retriggers\n";
   auto a = tone(440.f, 48000.f, 0.5f);
   auto c = tone(523.25f, 48000.f, 0.5f);
   a.insert(a.end(), c.begin(), c.end());
   hz::Converter conv(1);
   auto ons = note_ons(conv.feed(a));
   expect(ons.size() >= 2, "a jump produces a second note-on");
   if (ons.size() >= 2) {
      expect(ons.front().data1 == 69, "starts on A4");
      expect(ons.back().data1 == 72, "lands on C5");
      expect(ons.back().sample > ons.front().sample, "C5 is later than A4");
   }
}

void test_release_emits_note_off() {
   std::cout << "test_release_emits_note_off\n";
   auto note = tone(440.f, 48000.f, 0.4f);
   auto gap = tone(0.f, 48000.f, 0.4f, 0.f);
   note.insert(note.end(), gap.begin(), gap.end());
   hz::Converter conv(1);
   auto ev = conv.feed(note);
   hz::MidiEvent const* off = nullptr;
   for (auto const& e : ev) {
      if (e.status == 0x80 && e.data1 == 69) off = &e;
   }
   expect(off != nullptr, "note-off for 69");
   if (off) {
      expect(off->channel == 1, "note-off on the member channel");
      expect(off->sample > 16000, "note-off lands after the tone");
   }
}

void test_note_after_silence_is_still_detected() {
   std::cout << "test_note_after_silence_is_still_detected\n";
   auto gap = tone(0.f, 48000.f, 0.3f, 0.f);
   auto note = tone(440.f, 48000.f, 0.5f);
   gap.insert(gap.end(), note.begin(), note.end());
   hz::Converter conv(1);
   auto ons = note_ons(conv.feed(gap));
   expect(ons.size() == 1, "one note-on after the gap");
   if (!ons.empty()) {
      expect(ons[0].data1 == 69, "delayed A4 is still note 69");
      expect(ons[0].sample >= 12000, "note-on is not stamped at sample 0");
   }
}

void test_two_channels_stay_independent() {
   std::cout << "test_two_channels_stay_independent\n";
   auto a = tone(440.f, 48000.f, 0.4f);
   auto e = tone(329.63f, 48000.f, 0.4f);
   float const* ch[2] = { a.data(), e.data() };
   hz::Converter conv(2);
   auto ons = note_ons(conv.feed(ch, 2, int(a.size())));
   bool a4 = false, e4 = false;
   for (auto const& ev : ons) {
      if (ev.channel == 1 && ev.data1 == 69) a4 = true;
      if (ev.channel == 2 && ev.data1 == 64) e4 = true;
   }
   expect(a4, "channel 1 is A4");
   expect(e4, "channel 2 is E4");
}

void test_detune_is_pitch_bend() {
   std::cout << "test_detune_is_pitch_bend\n";
   auto samples = tone(445.f, 48000.f, 0.4f);
   hz::Converter conv(1);
   auto ev = conv.feed(samples);
   bool note = false, bent = false;
   for (auto const& e : ev) {
      if (e.status == 0x90 && e.data1 == 69) note = true;
      if (e.status == 0xE0 && e.channel == 1) {
         int bend = (e.data2 << 7) | e.data1;
         if (bend > 8192) bent = true;
      }
   }
   expect(note, "445 Hz still quantizes to A4");
   expect(bent, "residual is a sharp pitch bend");
}

void test_midi_file_contains_the_note() {
   std::cout << "test_midi_file_contains_the_note\n";
   auto samples = tone(440.f, 48000.f, 0.4f);
   hz::Converter conv(1);
   auto ev = conv.feed(samples);
   std::string path = "build/a440_test.mid";
   expect(hz::write_midi(path, ev, 48000.f, 1, 48), "writes a MIDI file");

   std::ifstream in(path, std::ios::binary);
   std::vector<unsigned char> bytes(
      (std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
   expect(bytes.size() > 22 && bytes[0] == 'M' && bytes[1] == 'T', "file is a MIDI header");

   bool note69 = false;
   std::vector<int> manager_cc;
   std::size_t i = 0;
   while (i + 4 <= bytes.size()) {
      if (bytes[i] == 'M' && bytes[i + 1] == 'T' && bytes[i + 2] == 'r' && bytes[i + 3] == 'k') {
         i += 8;
         int running = 0;
         while (i < bytes.size()) {
            unsigned value = 0;
            unsigned char b = 0;
            do {
               if (i >= bytes.size()) break;
               b = bytes[i++];
               value = (value << 7) | (b & 0x7F);
            } while (b & 0x80);
            if (i >= bytes.size()) break;
            unsigned char status = bytes[i];
            if (status < 0x80) {
               status = static_cast<unsigned char>(running);
            } else {
               running = status;
               ++i;
            }
            if (status == 0xFF) {
               if (i >= bytes.size()) break;
               unsigned char meta = bytes[i++];
               unsigned len = 0;
               do {
                  if (i >= bytes.size()) break;
                  b = bytes[i++];
                  len = (len << 7) | (b & 0x7F);
               } while (b & 0x80);
               i += len;
               if (meta == 0x2F) break;
               continue;
            }
            int data_bytes = (status & 0xF0) == 0xC0 || (status & 0xF0) == 0xD0 ? 1 : 2;
            if (i + data_bytes > bytes.size()) break;
            unsigned char d1 = bytes[i];
            unsigned char d2 = data_bytes == 2 ? bytes[i + 1] : 0;
            i += static_cast<std::size_t>(data_bytes);
            if ((status & 0xF0) == 0x90 && d1 == 69 && d2 > 0) note69 = true;
            if (status == 0xB0) {
               manager_cc.push_back(d1);
               manager_cc.push_back(d2);
            }
         }
         break;
      }
      ++i;
   }
   expect(note69, "file contains note-on 69");
   std::vector<int> const expected_mpe_setup = {101, 0, 100, 6, 6, 1, 38, 0};
   expect(manager_cc == expected_mpe_setup, "manager RPN 6 declares a 1-channel MPE zone");
}

void test_short_live_blocks_are_buffered() {
   std::cout << "test_short_live_blocks_are_buffered\n";
   hz::Config cfg;
   cfg.window = 256;
   cfg.hop = 128;
   auto samples = tone(440.f, 48000.f, 0.05f);
   hz::Converter conv(1, cfg);
   std::vector<hz::MidiEvent> events;
   for (int pos = 0; pos < int(samples.size());) {
      int count = std::min(100, int(samples.size()) - pos);
      std::vector<float> block(samples.begin() + pos, samples.begin() + pos + count);
      auto ev = conv.feed(block, false);
      events.insert(events.end(), ev.begin(), ev.end());
      pos += count;
   }
   auto ons = note_ons(events);
   expect(ons.size() == 1, "successive short blocks produce one note-on");
   if (!ons.empty()) expect(ons[0].data1 == 69, "buffered short blocks detect A4");
}

void test_stream_timestamps_are_absolute() {
   std::cout << "test_stream_timestamps_are_absolute\n";
   hz::Config cfg;
   cfg.window = 256;
   cfg.hop = 256;
   cfg.release_hops = 1;
   auto note = tone(440.f, 48000.f, 0.05f);
   note.resize(256);
   std::vector<float> silence(256, 0.f);
   hz::Converter conv(1, cfg);
   conv.feed(note, false);
   auto ev = conv.feed(silence, false);
   hz::MidiEvent const* off = nullptr;
   for (auto const& e : ev)
      if (e.status == 0x80 && e.data1 == 69) off = &e;
   expect(off != nullptr, "second live block releases the held note");
   if (off) expect(off->sample == 256, "note-off uses the absolute stream sample");
}

void test_final_empty_block_releases_every_channel() {
   std::cout << "test_final_empty_block_releases_every_channel\n";
   hz::Config cfg;
   cfg.window = 256;
   cfg.hop = 256;
   auto a = tone(440.f, 48000.f, 0.05f);
   auto e = tone(440.f, 48000.f, 0.05f);
   a.resize(256);
   e.resize(256);
   float const* ch[2] = { a.data(), e.data() };
   hz::Converter conv(2, cfg);
   conv.feed(ch, 2, 256, false);
   auto ev = conv.feed(nullptr, 0, 0, true);
   bool off1 = false, off2 = false;
   bool absolute = true;
   for (auto const& event : ev) {
      if (event.status != 0x80) continue;
      if (event.channel == 1 && event.data1 == 69) off1 = true;
      if (event.channel == 2 && event.data1 == 69) off2 = true;
      if (event.sample != 256) absolute = false;
   }
   expect(off1 && off2, "final empty block releases notes on all channels");
   expect(absolute, "final note-offs use the absolute stream end");
}

void test_variable_channel_live_block_does_not_mutate_state() {
   std::cout << "test_variable_channel_live_block_does_not_mutate_state\n";
   hz::Config cfg;
   cfg.window = 256;
   cfg.hop = 256;
   auto samples = tone(440.f, 48000.f, 0.05f);
   samples.resize(256);
   float const* both[2] = { samples.data(), samples.data() };
   hz::Converter conv(2, cfg);
   conv.feed(both, 2, 256, false);

   float const* omitted[1] = { samples.data() };
   auto rejected = conv.feed(omitted, 1, 64, false);
   expect(rejected.empty(), "variable-channel live block is rejected");

   conv.feed(both, 2, 256, false);
   auto ev = conv.feed(nullptr, 0, 0, true);
   int offs = 0;
   bool continuous = true;
   for (auto const& event : ev) {
      if (event.status != 0x80) continue;
      ++offs;
      if (event.sample != 512) continuous = false;
   }
   expect(offs == 2, "final empty feed still releases both channels");
   expect(continuous, "rejected block cannot corrupt buffered stream timestamps");
}

void test_converter_clamps_to_mpe_member_limit() {
   std::cout << "test_converter_clamps_to_mpe_member_limit\n";
   hz::Config cfg;
   cfg.window = 256;
   cfg.hop = 256;
   auto samples = tone(440.f, 48000.f, 0.05f);
   samples.resize(256);
   std::vector<float const*> ch(16, samples.data());
   hz::Converter conv(16, cfg);
   auto ons = note_ons(conv.feed(ch.data(), 15, 256));
   bool legal_channels = true;
   for (auto const& e : ons)
      if (e.channel < 1 || e.channel > 15) legal_channels = false;
   expect(ons.size() == 15, "converter clamps input count to 15 members");
   expect(legal_channels, "member events never exceed MIDI channel 15");
}

void test_initial_note_waits_for_stable_pitch() {
   std::cout << "test_initial_note_waits_for_stable_pitch\n";
   hz::Config cfg;
   cfg.window = 64;
   cfg.hop = 64;
   cfg.retrigger_hops = 2;
   cfg.onset_hops = 2;
   int call = 0;
   hz::PitchEstimator estimator = [&call](int, float const*, int, hz::Config const&, int) {
      int const notes[] = {55, 40, 40, 40};
      hz::PitchEstimate est;
      est.hz = hz::hz_of_note(notes[std::min(call, 3)]);
      est.periodicity = 1.f;
      est.valid = true;
      ++call;
      return est;
   };
   auto samples = tone(440.f, 48000.f, 256.f / 48000.f);
   hz::Converter conv(1, cfg, estimator);
   auto ons = note_ons(conv.feed(samples));
   expect(ons.size() == 1, "one-hop startup pitch does not emit a false note");
   if (!ons.empty()) expect(ons[0].data1 == 40, "first note-on is the stable pitch");
}

void test_take_end_resets_pending_onset() {
   std::cout << "test_take_end_resets_pending_onset\n";
   hz::Config cfg;
   cfg.window = 64;
   cfg.hop = 64;
   cfg.onset_hops = 3;
   hz::PitchEstimator estimator = [](int, float const*, int, hz::Config const&, int) {
      hz::PitchEstimate est;
      est.hz = 440.f;
      est.periodicity = 1.f;
      est.valid = true;
      return est;
   };
   auto samples = tone(440.f, 48000.f, 0.05f);
   samples.resize(192);
   hz::Converter conv(1, cfg, estimator);

   std::vector<float> first(samples.begin(), samples.begin() + 64);
   auto first_ons = note_ons(conv.feed(first, true));
   expect(first_ons.empty(), "one-window first take leaves no confirmed onset");

   std::vector<float> second(samples.begin(), samples.begin() + 128);
   auto second_ons = note_ons(conv.feed(second, true));
   expect(second_ons.empty(), "next take restarts the three-window onset confirmation");
}

void test_buffer_end_releases_the_note() {
   std::cout << "test_buffer_end_releases_the_note\n";
   auto samples = tone(440.f, 48000.f, 0.4f);
   hz::Converter conv(1);
   auto ev = conv.feed(samples);
   bool off = false;
   for (auto const& e : ev)
      if (e.status == 0x80 && e.data1 == 69) off = true;
   expect(off, "note-off even when the buffer ends on the tone");
}

} // namespace

int main() {
   test_a440_is_note_69();
   test_note_after_silence_is_still_detected();
   test_release_emits_note_off();
   test_pitch_jump_retriggers();
   test_two_channels_stay_independent();
   test_detune_is_pitch_bend();
   test_midi_file_contains_the_note();
   test_short_live_blocks_are_buffered();
   test_stream_timestamps_are_absolute();
   test_final_empty_block_releases_every_channel();
   test_variable_channel_live_block_does_not_mutate_state();
   test_converter_clamps_to_mpe_member_limit();
   test_initial_note_waits_for_stable_pitch();
   test_take_end_resets_pending_onset();
   test_buffer_end_releases_the_note();
   if (g_failed) {
      std::cerr << g_failed << " assertion(s) failed\n";
      return 1;
   }
   std::cout << "all passed\n";
   return 0;
}
