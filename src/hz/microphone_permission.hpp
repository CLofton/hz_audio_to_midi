#pragma once

#include <chrono>
#include <string>

namespace hz {

enum class MicrophonePermission {
   authorized,
   denied,
   restricted,
   timeout,
   unavailable
};

inline std::string microphone_permission_error(MicrophonePermission status) {
   switch (status) {
      case MicrophonePermission::authorized:
         return {};
      case MicrophonePermission::denied:
         return "microphone access denied; enable HzLive in System Settings > Privacy & Security > Microphone, then retry";
      case MicrophonePermission::restricted:
         return "microphone access is restricted by system policy or device management; ask the device administrator to permit HzLive";
      case MicrophonePermission::timeout:
         return "microphone permission request timed out; respond to the macOS prompt or enable HzLive in System Settings > Privacy & Security > Microphone, then retry";
      case MicrophonePermission::unavailable:
         return "microphone permission metadata is unavailable; run build/HzLive.app/Contents/MacOS/HzLive so macOS can read CFBundleIdentifier and NSMicrophoneUsageDescription";
   }
   return "microphone permission is unavailable";
}

// Checks/requests audio capture authorization without creating an audio unit.
// Only run_live calls this; listing and virtual self-tests never request access.
bool ensure_microphone_permission(std::chrono::milliseconds timeout, std::string& error);

} // namespace hz
