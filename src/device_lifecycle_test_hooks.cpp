#include "device_lifecycle_test_hooks.h"

#if defined(SOLOUD_LIFECYCLE_TEST_HOOKS)

#include <atomic>
#include <condition_variable>
#include <mutex>

namespace soloud_test
{
  namespace
  {
    struct Barrier
    {
      bool armed = false;
      bool reached = false;
    };

    std::mutex gMutex;
    std::condition_variable gCv;
    Barrier gBarriers[static_cast<int>(DeviceBarrier::barrierCount)];
    int gForcedStartFailures = 0;
    int gBackendDeviceStarts = 0;
    // Atomics rather than gMutex: the stall flag is read on the audio
    // callback path, which must never take a lock, test build or not.
    std::atomic<bool> gDeviceCallbackStalled{false};
    std::atomic<long long> gDeviceStallThresholdMs{0};

    Barrier &slot(DeviceBarrier barrier)
    {
      return gBarriers[static_cast<int>(barrier)];
    }
  } // namespace

  void armBarrier(DeviceBarrier barrier)
  {
    std::lock_guard<std::mutex> lock(gMutex);
    Barrier &b = slot(barrier);
    b.armed = true;
    b.reached = false;
  }

  void waitBarrierReached(DeviceBarrier barrier)
  {
    std::unique_lock<std::mutex> lock(gMutex);
    gCv.wait(lock, [&] { return slot(barrier).reached; });
  }

  void releaseBarrier(DeviceBarrier barrier)
  {
    {
      std::lock_guard<std::mutex> lock(gMutex);
      slot(barrier).armed = false;
    }
    gCv.notify_all();
  }

  void hitBarrier(DeviceBarrier barrier)
  {
    std::unique_lock<std::mutex> lock(gMutex);
    Barrier &b = slot(barrier);
    // One-shot: only the *first* thread to arrive parks. Later arrivals pass
    // straight through. Without this a barrier catches every thread reaching
    // the point, including ones the test itself set in motion -- a teardown
    // parking on the "device stopped" notification it just caused, for
    // instance, which makes the test pass no matter what the code under test
    // does.
    if (!b.armed || b.reached)
      return;

    b.reached = true;
    gCv.notify_all();
    gCv.wait(lock, [&] { return !slot(barrier).armed; });
  }

  void recordBackendDeviceStart()
  {
    std::lock_guard<std::mutex> lock(gMutex);
    ++gBackendDeviceStarts;
  }

  int backendDeviceStartCount()
  {
    std::lock_guard<std::mutex> lock(gMutex);
    return gBackendDeviceStarts;
  }

  void resetBackendDeviceStartCount()
  {
    std::lock_guard<std::mutex> lock(gMutex);
    gBackendDeviceStarts = 0;
  }

  void failNextDeviceStarts(int count)
  {
    std::lock_guard<std::mutex> lock(gMutex);
    gForcedStartFailures = count;
  }

  int pendingForcedDeviceStartFailures()
  {
    std::lock_guard<std::mutex> lock(gMutex);
    return gForcedStartFailures;
  }

  /// Consumes one forced failure, if any are pending. Used by the backend
  /// start path so a test can drive the rebuild/retry logic deterministically
  /// rather than by unplugging hardware.
  bool consumeForcedDeviceStartFailure()
  {
    std::lock_guard<std::mutex> lock(gMutex);
    if (gForcedStartFailures <= 0)
      return false;
    --gForcedStartFailures;
    return true;
  }

  void setDeviceCallbackStalled(bool stalled)
  {
    gDeviceCallbackStalled.store(stalled, std::memory_order_relaxed);
  }

  bool deviceCallbackStalled()
  {
    return gDeviceCallbackStalled.load(std::memory_order_relaxed);
  }

  void setDeviceStallThresholdMs(long long thresholdMs)
  {
    gDeviceStallThresholdMs.store(thresholdMs, std::memory_order_relaxed);
  }

  long long deviceStallThresholdOverrideMs()
  {
    return gDeviceStallThresholdMs.load(std::memory_order_relaxed);
  }
} // namespace soloud_test

#else

// Without the hooks this file has nothing in it, and an empty translation unit
// is ill-formed under -Wpedantic. Nothing references this.
namespace soloud_test
{
  namespace
  {
    [[maybe_unused]] const int kHooksCompiledOut = 0;
  }
} // namespace soloud_test

#endif // SOLOUD_LIFECYCLE_TEST_HOOKS
