// Only one mixer output capture runs at a time. A worker isolate that tries to
// start one while the main isolate's capture runs is refused; the refusal must
// leave the running capture's output callback alone, so the main stream keeps
// receiving chunks.
@TestOn('vm')
library;

import 'dart:io';
import 'dart:isolate';

import 'package:flutter/services.dart';
import 'package:flutter_soloud/flutter_soloud.dart';
import 'package:flutter_test/flutter_test.dart';

String _startInWorker() {
  try {
    SoLoudIsolate.instance.startMixerOutputStream(chunkPCMFrames: 2048);
    return 'started';
  } on SoLoudCppException {
    return 'refused';
  }
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  final temp = Directory.systemTemp.createTempSync('soloud_capture_refusal');
  TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
      .setMockMethodCallHandler(
        const MethodChannel('plugins.flutter.io/path_provider'),
        (call) async => temp.path,
      );
  tearDownAll(() => temp.deleteSync(recursive: true));

  Future<void> waitForChunks(int Function() count, int above) async {
    for (var i = 0; i < 150 && count() <= above; i++) {
      await Future<void>.delayed(const Duration(milliseconds: 20));
    }
  }

  test('a refused worker start leaves the running capture alone', () async {
    final soloud = SoLoud.instance;
    await soloud.init();
    var chunks = 0;
    final sub = soloud
        .startMixerOutputStream(chunkPCMFrames: 2048)
        .listen((_) => chunks++);
    await waitForChunks(() => chunks, 0);
    expect(chunks, greaterThan(0), reason: 'the main capture delivers chunks');

    expect(await Isolate.run(_startInWorker), 'refused');

    final before = chunks;
    await waitForChunks(() => chunks, before);
    expect(
      chunks,
      greaterThan(before),
      reason: 'the main stream keeps receiving chunks after the refusal',
    );
    soloud
      ..stopMixerOutputStream()
      ..deinit();
    await sub.cancel();
  });
}
