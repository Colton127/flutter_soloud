// A worker isolate that is terminated while its mixer output capture runs
// (Isolate.run returning, Isolate.exit, Isolate.kill) takes its output
// callback with it. The capture must not keep calling that callback.
@TestOn('vm')
library;

import 'dart:io';
import 'dart:isolate';

import 'package:flutter/services.dart';
import 'package:flutter_soloud/flutter_soloud.dart';
import 'package:flutter_test/flutter_test.dart';

String _startAndLeave() {
  SoLoudIsolate.instance.startMixerOutputStream(chunkPCMFrames: 2048);
  return 'started';
}

void _startUntilTerminated(List<Object> args) {
  final reply = args[0] as SendPort;
  final exitNow = args[1] as bool;
  SoLoudIsolate.instance.startMixerOutputStream(chunkPCMFrames: 2048);
  if (exitNow) Isolate.exit(reply, 'started');
  reply.send('started');
  ReceivePort().listen((_) {});
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  final temp = Directory.systemTemp.createTempSync('soloud_capture_exit');
  TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
      .setMockMethodCallHandler(
        const MethodChannel('plugins.flutter.io/path_provider'),
        (call) async => temp.path,
      );
  tearDownAll(() => temp.deleteSync(recursive: true));

  test('a worker that exits with its capture running', () async {
    final soloud = SoLoud.instance;
    await soloud.init();
    expect(await Isolate.run(_startAndLeave), 'started');
    // Long enough for the capture to publish several chunks.
    await Future<void>.delayed(const Duration(milliseconds: 500));
    soloud.deinit();
  });
  for (final exitNow in [true, false]) {
    test(
      exitNow
          ? 'explicit exit with capture running'
          : 'kill with capture running',
      () async {
        final soloud = SoLoud.instance;
        await soloud.init();
        final ready = ReceivePort();
        final exited = ReceivePort();
        final worker = await Isolate.spawn(_startUntilTerminated, <Object>[
          ready.sendPort,
          exitNow,
        ], onExit: exited.sendPort);
        expect(await ready.first, 'started');
        if (!exitNow) worker.kill(priority: Isolate.immediate);
        await exited.first;
        await Future<void>.delayed(const Duration(milliseconds: 500));
        ready.close();
        exited.close();
        soloud.deinit();
      },
    );
  }

  test('an exited worker does not block the next capture', () async {
    final soloud = SoLoud.instance;
    await soloud.init();
    expect(await Isolate.run(_startAndLeave), 'started');
    var chunks = 0;
    final stream = soloud.startMixerOutputStream(chunkPCMFrames: 2048);
    final subscription = stream.listen((_) => chunks++);
    for (var i = 0; i < 100 && chunks == 0; ++i) {
      await Future<void>.delayed(const Duration(milliseconds: 20));
    }
    expect(soloud.isMixerOutputStreamRunning, isTrue);
    expect(chunks, greaterThan(0));
    soloud.stopMixerOutputStream();
    await subscription.cancel();
    soloud.deinit();
  });
}
