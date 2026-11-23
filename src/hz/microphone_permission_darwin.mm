#include "hz/microphone_permission.hpp"

#import <AVFoundation/AVFoundation.h>
#import <Foundation/Foundation.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>

namespace hz {
namespace {

struct PermissionWaitState {
   std::atomic<bool> finished{false};
   std::atomic<bool> granted{false};
};

} // namespace

bool ensure_microphone_permission(std::chrono::milliseconds timeout, std::string& error) {
   error.clear();
   if (timeout <= std::chrono::milliseconds::zero()) {
      error = microphone_permission_error(MicrophonePermission::timeout);
      return false;
   }

   @autoreleasepool {
      AVAuthorizationStatus const status =
         [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
      if (status == AVAuthorizationStatusAuthorized) return true;
      if (status == AVAuthorizationStatusDenied) {
         error = microphone_permission_error(MicrophonePermission::denied);
         return false;
      }
      if (status == AVAuthorizationStatusRestricted) {
         error = microphone_permission_error(MicrophonePermission::restricted);
         return false;
      }
      if (status != AVAuthorizationStatusNotDetermined) {
         error = microphone_permission_error(MicrophonePermission::unavailable);
         return false;
      }

      NSString* const bundle_identifier = [[NSBundle mainBundle] bundleIdentifier];
      NSString* const usage_description =
         [[NSBundle mainBundle] objectForInfoDictionaryKey:@"NSMicrophoneUsageDescription"];
      if (bundle_identifier == nil || bundle_identifier.length == 0
         || usage_description == nil || usage_description.length == 0) {
         error = microphone_permission_error(MicrophonePermission::unavailable);
         return false;
      }

      auto state = std::make_shared<PermissionWaitState>();
      @try {
         [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio
            completionHandler:^(BOOL granted) {
               state->granted.store(granted == YES, std::memory_order_release);
               state->finished.store(true, std::memory_order_release);
            }];
      } @catch (NSException* exception) {
         char const* reason = [[exception reason] UTF8String];
         error = "AVFoundation microphone authorization failed: "
            + std::string(reason == nullptr ? "unknown error" : reason);
         return false;
      }

      auto const deadline = std::chrono::steady_clock::now() + timeout;
      while (!state->finished.load(std::memory_order_acquire)
         && std::chrono::steady_clock::now() < deadline) {
         @autoreleasepool {
            NSDate* const next = [NSDate dateWithTimeIntervalSinceNow:0.05];
            [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode beforeDate:next];
         }
      }
      if (!state->finished.load(std::memory_order_acquire)) {
         error = microphone_permission_error(MicrophonePermission::timeout);
         return false;
      }
      if (state->granted.load(std::memory_order_acquire)) return true;

      AVAuthorizationStatus const final_status =
         [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
      error = microphone_permission_error(final_status == AVAuthorizationStatusRestricted
         ? MicrophonePermission::restricted : MicrophonePermission::denied);
      return false;
   }
}

} // namespace hz
