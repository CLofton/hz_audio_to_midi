#include "hz/microphone_permission.hpp"

#include <iostream>
#include <string>

namespace {

bool check(bool condition, char const* message) {
   if (!condition) std::cerr << message << '\n';
   return condition;
}

} // namespace

int main() {
   using hz::MicrophonePermission;
   bool ok = check(hz::microphone_permission_error(MicrophonePermission::authorized).empty(),
      "authorized microphone permission produced an error");
   auto const denied = hz::microphone_permission_error(MicrophonePermission::denied);
   ok = check(denied.find("System Settings") != std::string::npos
      && denied.find("Microphone") != std::string::npos,
      "denied permission did not explain how to grant access") && ok;
   ok = check(hz::microphone_permission_error(MicrophonePermission::restricted).find("restricted")
      != std::string::npos, "restricted permission was not actionable") && ok;
   ok = check(hz::microphone_permission_error(MicrophonePermission::timeout).find("timed out")
      != std::string::npos, "permission timeout was not reported") && ok;
   ok = check(hz::microphone_permission_error(MicrophonePermission::unavailable).find("HzLive.app")
      != std::string::npos, "missing bundle usage description did not identify the app fix") && ok;
   if (ok) std::cout << "microphone permission diagnostics passed\n";
   return ok ? 0 : 1;
}
