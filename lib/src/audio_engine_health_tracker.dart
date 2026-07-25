import 'package:flutter_soloud/src/enums.dart';
import 'package:meta/meta.dart';

/// Decides an [AudioEngineHealth] from successive samples of the output
/// device's state and the rendered-frame heartbeat.
///
/// Split out from `SoLoud.monitorEngineHealth` so the decision rules can be
/// tested without a running engine. The rules are deliberately conservative
/// about reporting [AudioEngineHealth.stalled]: on platforms that stop the
/// device aggressively while idle (iOS, where a silent stream breaks the
/// Control Center transport controls), a false stall would be worse than no
/// monitoring at all.
@internal
class AudioEngineHealthTracker {
  /// Creates a tracker that reports a stall once the device has claimed to be
  /// started for [stallThreshold] without rendering a single frame.
  AudioEngineHealthTracker({required this.stallThreshold});

  /// How long a started device may render nothing before it is a stall.
  final Duration stallThreshold;

  int? _lastFrames;
  DateTime? _lastProgressAt;

  /// Folds one sample into the tracker and returns the resulting health.
  ///
  /// [framesRendered] is the monotonic heartbeat, or a negative value on
  /// platforms that do not provide one.
  AudioEngineHealth evaluate({
    required AudioDeviceState state,
    required int framesRendered,
    required DateTime now,
  }) {
    // No heartbeat available: report the device's own claim rather than
    // inventing a stall that cannot actually be observed.
    if (framesRendered < 0) {
      return state == AudioDeviceState.started
          ? AudioEngineHealth.healthy
          : AudioEngineHealth.idle;
    }

    if (framesRendered != _lastFrames) {
      _lastFrames = framesRendered;
      _lastProgressAt = now;
    }

    if (state != AudioDeviceState.started) {
      // Stopped on purpose — idle timeout, explicit stop, OS interruption, or
      // not initialized. Reset the clock so time spent legitimately stopped
      // cannot be counted against the device once it starts again.
      _lastProgressAt = now;
      return AudioEngineHealth.idle;
    }

    // First sample of a started device: treat it as the start of the window
    // rather than as time already spent stalled.
    final since = _lastProgressAt ??= now;

    return now.difference(since) >= stallThreshold
        ? AudioEngineHealth.stalled
        : AudioEngineHealth.healthy;
  }
}
