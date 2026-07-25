import 'package:flutter_soloud/src/audio_engine_health_tracker.dart';
import 'package:flutter_soloud/src/enums.dart';
import 'package:test/test.dart';

void main() {
  const threshold = Duration(seconds: 3);
  final t0 = DateTime.utc(2026);

  AudioEngineHealthTracker newTracker() =>
      AudioEngineHealthTracker(stallThreshold: threshold);

  group('AudioEngineHealthTracker', () {
    test('a started device rendering frames is healthy', () {
      final tracker = newTracker();
      var frames = 1000;

      for (var i = 0; i < 10; i++) {
        final health = tracker.evaluate(
          state: AudioDeviceState.started,
          framesRendered: frames += 512,
          now: t0.add(Duration(seconds: i)),
        );
        expect(health, AudioEngineHealth.healthy);
      }
    });

    test('a started device that stops rendering stalls after the threshold',
        () {
      final tracker = newTracker();

      expect(
        tracker.evaluate(
          state: AudioDeviceState.started,
          framesRendered: 1000,
          now: t0,
        ),
        AudioEngineHealth.healthy,
      );
      // Frozen counter, still short of the threshold.
      expect(
        tracker.evaluate(
          state: AudioDeviceState.started,
          framesRendered: 1000,
          now: t0.add(const Duration(milliseconds: 2999)),
        ),
        AudioEngineHealth.healthy,
      );

      // Threshold reached.
      expect(
        tracker.evaluate(
          state: AudioDeviceState.started,
          framesRendered: 1000,
          now: t0.add(threshold),
        ),
        AudioEngineHealth.stalled,
      );
    });

    test('recovers to healthy once frames advance again', () {
      final tracker = newTracker()
        ..evaluate(
          state: AudioDeviceState.started,
          framesRendered: 1000,
          now: t0,
        );
      expect(
        tracker.evaluate(
          state: AudioDeviceState.started,
          framesRendered: 1000,
          now: t0.add(const Duration(seconds: 5)),
        ),
        AudioEngineHealth.stalled,
      );
      expect(
        tracker.evaluate(
          state: AudioDeviceState.started,
          framesRendered: 1512,
          now: t0.add(const Duration(seconds: 6)),
        ),
        AudioEngineHealth.healthy,
      );
    });

    test('a stopped device is idle, never stalled, however long it stays so',
        () {
      final tracker = newTracker();

      for (var i = 0; i < 60; i++) {
        expect(
          tracker.evaluate(
            state: AudioDeviceState.stopped,
            framesRendered: 1000, // frozen: nothing is rendering
            now: t0.add(Duration(seconds: i)),
          ),
          AudioEngineHealth.idle,
          reason: 'a deliberately stopped device must never read as a stall',
        );
      }
    });

    test('transitional and uninitialized states are idle', () {
      for (final state in [
        AudioDeviceState.starting,
        AudioDeviceState.stopping,
        AudioDeviceState.uninitialized,
      ]) {
        expect(
          newTracker().evaluate(
            state: state,
            framesRendered: 1000,
            now: t0,
          ),
          AudioEngineHealth.idle,
          reason: '$state should not be reported as a fault',
        );
      }
    });

    test('a long idle period is not charged against the device on restart', () {
      final tracker = newTracker();

      // Device stopped for a minute (the iOS aggressive idle-stop pattern:
      // a silent stream breaks the Control Center transport, so the device is
      // stopped as soon as playback ends).
      for (var i = 0; i < 60; i++) {
        tracker.evaluate(
          state: AudioDeviceState.stopped,
          framesRendered: 1000,
          now: t0.add(Duration(seconds: i)),
        );
      }

      // It starts again. The very first sample after a minute of idle must not
      // read as a stall just because the clock moved on while it was stopped.
      expect(
        tracker.evaluate(
          state: AudioDeviceState.started,
          framesRendered: 1000,
          now: t0.add(const Duration(seconds: 60)),
        ),
        AudioEngineHealth.healthy,
      );
    });

    test('first sample of a started device starts the window, not a stall', () {
      // A tracker created long after the engine began running must not report
      // a stall on its very first observation.
      expect(
        newTracker().evaluate(
          state: AudioDeviceState.started,
          framesRendered: 999999,
          now: t0.add(const Duration(hours: 5)),
        ),
        AudioEngineHealth.healthy,
      );
    });

    test('no heartbeat available (web) never reports a stall', () {
      final tracker = newTracker();

      // Started with a frozen, negative counter for well past the threshold.
      for (var i = 0; i < 30; i++) {
        expect(
          tracker.evaluate(
            state: AudioDeviceState.started,
            framesRendered: -1,
            now: t0.add(Duration(seconds: i)),
          ),
          AudioEngineHealth.healthy,
        );
      }
      expect(
        tracker.evaluate(
          state: AudioDeviceState.stopped,
          framesRendered: -1,
          now: t0.add(const Duration(seconds: 31)),
        ),
        AudioEngineHealth.idle,
      );
    });

    test('a counter wrapping or resetting across deinit/init is not a stall',
        () {
      final tracker = newTracker()
        ..evaluate(
          state: AudioDeviceState.started,
          framesRendered: 5000,
          now: t0,
        );
      // Any change counts as progress, including a decrease.
      expect(
        tracker.evaluate(
          state: AudioDeviceState.started,
          framesRendered: 12,
          now: t0.add(const Duration(seconds: 4)),
        ),
        AudioEngineHealth.healthy,
      );
    });
  });
}
