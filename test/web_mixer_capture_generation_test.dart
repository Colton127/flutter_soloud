@TestOn('browser')
library;

import 'dart:typed_data';

import 'package:flutter_soloud/src/bindings/bindings_player_web.dart';
import 'package:flutter_soloud/src/enums.dart';
import 'package:test/test.dart';

import 'web_capture_module.dart';

void main() {
  test(
    'a delayed packet from the previous capture is freed, not emitted',
    () async {
      final module = WebCaptureModule();
      final bindings = FlutterSoLoudWeb();
      await bindings.setDartEventCallbacks();
      final received = <Uint8List>[];
      final subscription = bindings.mixerOutputChunkEvents.listen(received.add);
      expect(
        bindings.startMixerOutputCapture(
          MixerOutputFormat.pcmF32le,
          44100,
          2,
          1 << 20,
          4096,
          2048,
        ),
        PlayerErrors.noError,
      );
      final old = module.queue(Uint8List.fromList([1, 2, 3, 4]));
      bindings
        ..stopMixerOutputCapture()
        ..registerMixerOutputCallback();
      expect(
        bindings.startMixerOutputCapture(
          MixerOutputFormat.pcmF32le,
          44100,
          2,
          1 << 20,
          4096,
          2048,
        ),
        PlayerErrors.noError,
      );
      module.deliver(old, 4);
      await Future<void>.delayed(const Duration(milliseconds: 20));
      expect(received, isEmpty);
      expect(module.allocations, isEmpty);
      final current = module.queue(Uint8List.fromList([5, 6, 7, 8]));
      module.deliver(current, 4);
      await Future<void>.delayed(const Duration(milliseconds: 20));
      expect(received, [
        orderedEquals([5, 6, 7, 8]),
      ]);
      expect(module.allocations, isEmpty);
      bindings.stopMixerOutputCapture();
      await subscription.cancel();
    },
  );
}
