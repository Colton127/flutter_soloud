@TestOn('browser')
library;

import 'dart:typed_data';

import 'package:flutter_soloud/src/bindings/bindings_player_web.dart';
import 'package:flutter_soloud/src/mixer_output_stream_manager.dart';
import 'package:test/test.dart';

import 'web_capture_module.dart';

void main() {
  test('stop emits untaken chunks once, followed by the ring tail', () async {
    final module = WebCaptureModule();
    final bindings = FlutterSoLoudWeb();
    await bindings.setDartEventCallbacks();
    final manager = MixerOutputStreamManager(
      bindings: bindings,
      isReady: () => true,
    );
    final received = <int>[];
    final sub = manager.start(chunkPCMFrames: 2048).listen(received.addAll);
    final chunk = Uint8List.fromList(List.generate(2048 * 8, (i) => i & 255));
    final token = module.queue(chunk);
    final tail = Uint8List.fromList([9, 8, 7, 6, 5, 4, 3, 2]);
    module.setTail(tail);
    manager.stop();
    await Future<void>.delayed(Duration.zero);
    expect(received, orderedEquals([...chunk, ...tail]));
    expect(module.packets.containsKey(token), isFalse);
    module.deliver(token, chunk.length);
    await Future<void>.delayed(const Duration(milliseconds: 20));
    expect(received, orderedEquals([...chunk, ...tail]));
    await sub.cancel();
  });
}
