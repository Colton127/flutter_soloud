#pragma once

// Named barriers for forcing the device-lifecycle interleavings that matter.
//
// The races these cover are between a direct device operation (an explicit
// start/stop, a device change, a teardown) and lifecycle intent posted
// concurrently by ordinary playback. Every one of them turns on a window of a
// few instructions between observing state and acting on it, so a timing-based
// test can only make the collision likely. A barrier makes it certain.
//
// The whole facility compiles to nothing unless SOLOUD_LIFECYCLE_TEST_HOOKS is
// defined, which no shipping build does. `SOLOUD_TEST_BARRIER()` expands to a
// void expression, so the production call sites cost nothing and cannot drift
// out of sync with the tests.

#if defined(SOLOUD_LIFECYCLE_TEST_HOOKS)

namespace soloud_test
{
  /// Points a test can park production code at. Each names the instant
  /// *after* a decision has been made but *before* it has been acted on --
  /// that is the window every one of these races lives in.
  enum class DeviceBarrier
  {
    /// Inside the native changeDevice() export, after the global `player` has
    /// been pinned but before Player::changeDevice() runs. Parks a change
    /// worker against a concurrent teardown, or against the calls that must
    /// not wait for it.
    changeDeviceEntered = 0,
    /// In Player::changeDevice(), after `shouldStartReplacement` has been
    /// decided from the active-voice count and device state.
    changeDeviceStartDecided,
    /// In Player::stopAudioDevice(), after the conditional form has observed
    /// the active-voice count as zero.
    stopAudioDeviceVoiceCountObserved,
    /// In Player::startAudioDevice(), after the stale-interruption latch has
    /// been cleared and superseded work cancelled.
    startAudioDeviceLatchCleared,
    /// In Player::performAudioDeviceStart(), before the backend start runs.
    performAudioDeviceStartEntered,
    /// In MixerOutput::onAudioData(), after the callback has been admitted to
    /// the active capture session but before it touches any capture state.
    mixerCaptureCallbackAdmitted,
    /// In the miniaudio notification callback, after it has been admitted and
    /// has pinned the current engine, but before it dereferences the Soloud or
    /// the interruption callback's Player* context.
    deviceNotificationAdmitted,
    /// At the top of Player::dispose(), before it stops accepting lifecycle
    /// requests. Parking here gives the scheduler an opportunity to act on
    /// anything teardown queued behind it, which is what makes "teardown
    /// performed no device start" a deterministic assertion instead of a race.
    playerDisposeEntered,
    /// In the deferred idle-timeout worker, after it has applied a published
    /// policy but before it decides whether to go idle. This is the window a
    /// naive "clear the queued flag after applying" scheme loses a write in.
    idleTimeoutWorkerApplied,
    /// At the top of Player::decodeFile(). A load parked here is mid-decode,
    /// so anything it still held would block the calls a test makes meanwhile.
    soundDecodeEntered,
    /// After a retirement has retired the Dart callback registration and
    /// released the gate, before it stops the mixer capture and the
    /// visualizations. A newer registration can be claimed in this window.
    retiredCaptureStopEntered,
    /// Inside the native startAudioDevice() export, after the global `player`
    /// has been pinned but before Player::startAudioDevice() runs.
    startAudioDeviceEntered,
    /// Inside the native stopAudioDevice() export, after the global `player`
    /// has been pinned but before Player::stopAudioDevice() runs.
    stopAudioDeviceEntered,
    /// In the device-callback stall watchdog, each time it wakes, before it
    /// reads the clock and samples the device. Parking it here is a watchdog
    /// frozen by a process suspension: when released, its wake-up is far
    /// later than scheduled.
    stallWatchdogWoke,
    /// In every Player::play* variant, right after the sound has been looked
    /// up by hash and before it is used. A disposeSound()/disposeAllSound()
    /// started while a play is parked here must neither free the sound under
    /// it nor leave the voice it goes on to create alive.
    playSoundLookedUp,
    /// In a BufferStream's background decode thread (started by
    /// setDataIsEnded() while the decoder still holds data), after a chunk has
    /// been decoded into the buffer and before checkBuffering() runs for it.
    /// That checkBuffering() can pause or unpause voices automatically, from
    /// a thread holding check_buffer_mutex.
    bufferStreamBackgroundDecoded,
    /// At the top of Player::resumeEngine() and
    /// Player::evaluateAudioDeviceIdle() when they actually post (a request
    /// deferred to a SoundsLock release does not get here), before
    /// mInterruptionMutex is taken. Only threads that set
    /// tObserveLifecyclePosts reach it, so a test can tell its own thread's
    /// posts from the mixer's and the scheduler's.
    lifecyclePostEntered,
    /// In Player::addPullBufferDataStream(), right after the stream has been
    /// looked up by hash and before data is added to it. A disposal started
    /// while a feeder is parked here must not destroy the stream under it.
    pullDataSoundLookedUp,
    /// At the end of a PullBufferStream seek out of the buffered range, in
    /// PullBufferStreamInstance::seek(): the buffer has been discarded and the
    /// seek stays pending until data at its target has been decoded. SoLoud
    /// applies seeks with its audio mutex held, usually on the mixer thread.
    pullSeekPending,
    /// In PullBufferStream::addAudioData(), after the delivered bytes have
    /// been queued for decoding and before the delivering thread decodes
    /// them. While a feeder is parked here the mixer can decode them itself
    /// (PullBufferStream::tryDecodePendingData()).
    pullDataQueued,
    /// In PullBufferStream::applyPendingSeek(), right after data decoded at
    /// the target has completed a pending seek, before the buffering check a
    /// delivering thread runs next.
    pullSeekCompleted,
    /// At the top of PullBufferStream::checkBuffering(), which runs only on a
    /// thread delivering data, holding sounds_mutex, and never on the mixer.
    pullBufferingCheckEntered,
    /// In PullBufferStreamInstance::seek(), for a target outside the buffered
    /// range at a known encoded offset, and in rewind(): right before the
    /// buffered audio and the queued bytes are discarded. SoLoud applies seeks
    /// (and rewinds a looping voice) with its audio mutex held, usually on the
    /// mixer thread.
    pullSeekDiscarding,
    /// In PullBufferStream::decodePendingDataLocked(), for raw PCM, after the
    /// queued bytes have been converted and before they are written to the
    /// buffer and dropped from the queue. The decoding thread holds the decode
    /// mutex here: a delivery, or the mixer (tryDecodePendingData()).
    pullDecoding,
    /// In Player::handleAudioInterruption(), right after mInterruptionMutex
    /// has been taken. An OS interruption's notification, or
    /// debugTriggerAudioInterruption(), parked here holds that lock.
    audioInterruptionLocked,
    /// In Player::handleAudioInterruption(), for an interruption that ended
    /// with a finite idle timeout, right before it counts the active voices,
    /// which waits for SoLoud's audio mutex.
    audioInterruptionCounting,
    /// Right after that count found active voices, before the handler takes
    /// mInterruptionMutex again to ask for the device to start.
    audioInterruptionCounted,
    /// In Player::useSoundByHash() and useSoundByHandle(), right after the
    /// sound has been looked up and before it is used (by the filter calls,
    /// for one). A disposal started while a caller is parked here must not
    /// destroy the sound under it.
    soundUseLookedUp,
    /// In the PlayerPin constructor, right after an operation pin has been
    /// taken and player_pin_mutex released, before the export holding it
    /// checks or uses the Player. A teardown started while an export is
    /// parked here must not dispose, destroy or replace that Player.
    playerOperationPinned,
    /// After the mixer removed pull-stream samples from the circular buffer,
    /// before it accounts for them. Reset must not split these two updates.
    pullSamplesRead,
    /// After native capture packet allocation, before the ring-copy lock.
    /// Parking here must never prevent the device from writing another block.
    mixerPacketStorageAllocated,
    /// After the stopped ring was selected, before copying its tail.
    mixerCaptureTailSelected,
    barrierCount
  };

  /// Set on a thread a test drives, so that thread's lifecycle posts hit the
  /// lifecyclePostEntered barrier (see SOLOUD_TEST_LIFECYCLE_POST()).
  inline thread_local bool tObserveLifecyclePosts = false;

  /// Arm [barrier]. The *next* thread reaching it parks until released; any
  /// thread arriving after that passes straight through, so a barrier cannot
  /// accidentally catch work the test itself triggered.
  void armBarrier(DeviceBarrier barrier);

  /// Block until a thread has parked on [barrier].
  void waitBarrierReached(DeviceBarrier barrier);

  /// Release [barrier] and disarm it.
  void releaseBarrier(DeviceBarrier barrier);

  /// Called from production code. A no-op unless [barrier] is armed.
  void hitBarrier(DeviceBarrier barrier);

  /// Force the next [count] backend device starts to fail, so a test can drive
  /// the rebuild/retry path and the failure reporting behind it.
  void failNextDeviceStarts(int count);

  /// How many forced failures are still pending.
  int pendingForcedDeviceStartFailures();

  /// Counts real backend device starts -- the points where ma_device_start()
  /// actually runs, not merely where the engine considered starting. A start
  /// request that finds the device already running costs nothing and is
  /// deliberately not counted, which is what lets a test assert "teardown
  /// performed no device start" without tripping over a start that was already
  /// queued and harmlessly no-ops.
  void recordBackendDeviceStart();
  int backendDeviceStartCount();
  void resetBackendDeviceStartCount();

  /// Consume one forced failure. Called by the backend start path so a test
  /// can drive rebuild/retry deterministically rather than by unplugging
  /// hardware. Returns true when this start must be failed.
  bool consumeForcedDeviceStartFailure();

  /// Make the device data callback return silence without reaching the
  /// engine, while the device itself keeps running and reporting started:
  /// nothing is mixed, voices do not advance, and the callback heartbeat the
  /// stall watchdog reads stops moving. That is the field failure the
  /// watchdog exists to report, without needing an OS that misbehaves.
  void setDeviceCallbackStalled(bool stalled);

  /// Called on the audio callback path, so lock-free: a relaxed atomic load.
  bool deviceCallbackStalled();

  /// Override the stall watchdog threshold picked up by the next
  /// Player::init(), so a test runs in seconds. 0 restores the computed
  /// production threshold.
  void setDeviceStallThresholdMs(long long thresholdMs);
  long long deviceStallThresholdOverrideMs();
} // namespace soloud_test

#define SOLOUD_TEST_BARRIER(name)                                              \
  ::soloud_test::hitBarrier(::soloud_test::DeviceBarrier::name)

/// A lifecycle request is about to be posted on this thread.
#define SOLOUD_TEST_LIFECYCLE_POST()                                           \
  do                                                                           \
  {                                                                            \
    if (::soloud_test::tObserveLifecyclePosts)                                 \
      SOLOUD_TEST_BARRIER(lifecyclePostEntered);                               \
  } while (0)

#else

#define SOLOUD_TEST_BARRIER(name) ((void)0)
#define SOLOUD_TEST_LIFECYCLE_POST() ((void)0)

#endif // SOLOUD_LIFECYCLE_TEST_HOOKS
