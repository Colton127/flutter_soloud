// Two isolates start a mixer output capture at the same moment. Both find no
// capture running and publish their output callbacks; the second publication
// wins. The first isolate's native start then succeeds and the second's is
// refused, and the refused isolate exits. The capture must deliver to the
// isolate that started it, not to the refused one's deleted callback.
//
// The bindings calls below are the steps of MixerOutputStreamManager.start(),
// interleaved in that order.
@TestOn('vm')
library;

import 'dart:io';
import 'dart:isolate';

import 'package:flutter/services.dart';
import 'package:flutter_soloud/flutter_soloud.dart';
import 'package:flutter_soloud/src/enums.dart' show PlayerErrors;
import 'package:flutter_test/flutter_test.dart';

String _publishAndLeave() {
  // The refused isolate: it passed the running check and published its
  // callback; its own native start (refused) does not change anything else.
  SoLoudIsolate.instance.bindings.registerMixerOutputCallback();
  return 'published';
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  final temp = Directory.systemTemp.createTempSync('soloud_capture_race');
  TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
      .setMockMethodCallHandler(
        const MethodChannel('plugins.flutter.io/path_provider'),
        (call) async => temp.path,
      );
  tearDownAll(() => temp.deleteSync(recursive: true));

  test('the winning start keeps its own callback', () async {
    final soloud = SoLoud.instance;
    await soloud.init();
    final bindings = SoLoudIsolate.instance.bindings;
    var chunks = 0;
    final sub = bindings.mixerOutputChunkEvents.listen((_) => chunks++);

    // Main isolate: running check passed, publishes its callback.
    expect(bindings.isMixerOutputCaptureRunning(), isFalse);
    bindings.registerMixerOutputCallback();
    // Worker: running check passed too, publishes its callback, is refused
    // and exits.
    expect(await Isolate.run(_publishAndLeave), 'published');
    // Main isolate: its native start wins.
    expect(
      bindings.startMixerOutputCapture(
        MixerOutputFormat.pcmF32le,
        -1,
        -1,
        1024 * 1024,
        4096,
        2048,
      ),
      PlayerErrors.noError,
    );
    for (var i = 0; i < 100 && chunks == 0; i++) {
      await Future<void>.delayed(const Duration(milliseconds: 20));
    }
    expect(chunks, greaterThan(0), reason: 'the winner receives its chunks');
    bindings.stopMixerOutputCapture();
    await sub.cancel();
    soloud.deinit();
  });
}
