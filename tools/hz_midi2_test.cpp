#include "hz/midi2_virtual.hpp"

#include <iostream>
#include <string>

namespace {

void usage() {
   std::cerr << "usage: hz_midi2_test --self-test | --sine-test\n";
}

} // namespace

int main(int argc, char** argv) {
   if (argc == 2 && std::string(argv[1]) == "--self-test") {
      std::string report;
      bool const passed = hz::run_midi2_self_test(report);
      (passed ? std::cout : std::cerr) << report << '\n';
      return passed ? 0 : 1;
   }
   if (argc == 2 && std::string(argv[1]) == "--sine-test") {
      std::string report;
      bool const passed = hz::run_midi2_sine_test(report);
      (passed ? std::cout : std::cerr) << report << '\n';
      return passed ? 0 : 1;
   }
   usage();
   return 2;
}
