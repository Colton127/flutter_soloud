@TestOn('browser')
library;

import 'dart:typed_data';

import 'package:flutter_soloud/src/bindings/bindings_player_web.dart';
import 'package:flutter_soloud/src/bindings/js_extension.dart'
    show wasmGetMixerCaptureId;
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

  test(
    'a capture keeps its start id when another capture replaces it',
    () async {
      final module = WebCaptureModule();
      final bindings = FlutterSoLoudWeb();
      await bindings.setDartEventCallbacks();
      module.replaceCaptureOnIdRead = true;
      expect(
        bindings.startMixerOutputCapture(
          MixerOutputFormat.pcmF32le,
          44100,
          2,
          65536,
          4096,
          2048,
        ),
        PlayerErrors.noError,
      );
      // Old bindings have already queried here and adopted the replacement id.
      // New bindings read the out slot; this unchanged getter then places the
      // same replacement after their start. Both paths exercise a running B.
      wasmGetMixerCaptureId();
      expect(module.replacements, 1);
      expect(module.captureId, 2);
      final token = module.queue(Uint8List.fromList([7, 7, 7, 7]));
      bindings.stopMixerOutputCapture();
      expect(module.running, isTrue, reason: 'stopping A must spare B');
      expect(module.packets.containsKey(token), isTrue);
      expect(module.globalStops, 0);
    },
  );
}
