@TestOn('browser')
library;

import 'package:flutter_soloud/src/bindings/bindings_player_web.dart';
import 'package:flutter_soloud/src/mixer_output_stream_manager.dart';
import 'package:test/test.dart';

import 'web_capture_module.dart';

void main() {
  test('a stopped worker capture is finalized before restarting', () async {
    final module = WebCaptureModule();
    final bindings = FlutterSoLoudWeb();
    await bindings.setDartEventCallbacks();
    final manager = MixerOutputStreamManager(
      bindings: bindings,
      isReady: () => true,
    );
    var firstDone = false;
    final first = manager.start(chunkPCMFrames: 2048);
    final firstSub = first.listen((_) {}, onDone: () => firstDone = true);
    final oldId = module.captureId;
    // A native engine teardown in another isolate stops capture without
    // clearing this worker's local stream or capture id.
    module.running = false;
    final second = manager.start(chunkPCMFrames: 2048);
    final secondSub = second.listen((_) {});
    await Future<void>.delayed(Duration.zero);
    expect(firstDone, isTrue);
    expect(identical(first, second), isFalse);
    expect(module.running, isTrue);
    expect(module.captureId, oldId + 1);
    manager.stop();
    await firstSub.cancel();
    await secondSub.cancel();
  });
}
