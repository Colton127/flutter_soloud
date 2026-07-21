import 'package:flutter/foundation.dart';
import 'package:flutter_soloud/flutter_soloud.dart';

/// Validates idle-timeout behavior for the audio device:
/// 1) set timeout to 500 ms, 2) init, 3) state is started,
/// 4) wait timeout, 5) state becomes stopped.
Future<StringBuffer> testAudioDeviceIdleTimeout() async {
  final strBuf = StringBuffer();
  const idleTimeout = Duration(milliseconds: 500);

  SoLoud.instance.setAudioDeviceIdleTimeout(idleTimeout);
  await SoLoud.instance.init();

  try {
    final startedState = SoLoud.instance.getAudioDeviceState();
    assert(
      startedState == AudioDeviceState.started,
      'Immediately after init(), expected AudioDeviceState.started '
      'but got $startedState.',
    );
    strBuf.writeln('State immediately after init(): $startedState');

    await Future<void>.delayed(idleTimeout);

    var stoppedState = SoLoud.instance.getAudioDeviceState();

    if (kIsWeb) {
      strBuf.writeln(
        'Web keeps the device running; skipping stopped-state assertion.',
      );
      return strBuf;
    }

    // Allow a short grace period for async stop transitions.
    final deadline = DateTime.now().add(const Duration(milliseconds: 1000));
    while (stoppedState != AudioDeviceState.stopped && DateTime.now().isBefore(deadline)) {
      await Future<void>.delayed(const Duration(milliseconds: 25));
      stoppedState = SoLoud.instance.getAudioDeviceState();
    }

    assert(
      stoppedState == AudioDeviceState.stopped,
      'After waiting ${idleTimeout.inMilliseconds} ms, expected '
      'AudioDeviceState.stopped but got $stoppedState.',
    );
    strBuf.writeln('State after idle timeout: $stoppedState');

    // 1) Create a basic waveform audio source.
    final waveform = await SoLoud.instance.loadWaveform(
      WaveForm.sin,
      false,
      1,
      0,
    );
    SoLoud.instance.setWaveformFreq(waveform, 440);

    // 2) Create a handle with playback initially disabled (paused).
    final handle = SoLoud.instance.play(waveform, paused: true, volume: 0.2);

    // 3) Validate device state remains stopped.
    final stateAfterPausedHandle = SoLoud.instance.getAudioDeviceState();
    assert(
      stateAfterPausedHandle == AudioDeviceState.stopped,
      'After creating a paused handle, expected AudioDeviceState.stopped '
      'but got $stateAfterPausedHandle.',
    );
    strBuf.writeln('State after paused handle creation: $stateAfterPausedHandle');

    // 4) Play the sound handle for a few seconds.
    SoLoud.instance.setPause(handle, false);
    await Future<void>.delayed(const Duration(seconds: 2));

    // 5) Validate device state is started.
    final stateWhilePlaying = SoLoud.instance.getAudioDeviceState();
    assert(
      stateWhilePlaying == AudioDeviceState.started,
      'While playing, expected AudioDeviceState.started '
      'but got $stateWhilePlaying.',
    );
    strBuf.writeln('State while playing: $stateWhilePlaying');

    // 6) Pause the sound handle.
    SoLoud.instance.setPause(handle, true);

    // 7) Wait idleTimeout again, then validate stopped.
    await Future<void>.delayed(idleTimeout);
    var stateAfterPauseIdle = SoLoud.instance.getAudioDeviceState();

    final pauseIdleDeadline = DateTime.now().add(const Duration(milliseconds: 1000));
    while (stateAfterPauseIdle != AudioDeviceState.stopped && DateTime.now().isBefore(pauseIdleDeadline)) {
      await Future<void>.delayed(const Duration(milliseconds: 25));
      stateAfterPauseIdle = SoLoud.instance.getAudioDeviceState();
    }

    assert(
      stateAfterPauseIdle == AudioDeviceState.stopped,
      'After pausing and waiting ${idleTimeout.inMilliseconds} ms, expected '
      'AudioDeviceState.stopped but got $stateAfterPauseIdle.',
    );
    strBuf.writeln('State after pause + idle timeout: $stateAfterPauseIdle');

    await SoLoud.instance.disposeSource(waveform);
  } finally {
    if (SoLoud.instance.isInitialized) {
      SoLoud.instance.deinit();
    }
  }

  return strBuf;
}
