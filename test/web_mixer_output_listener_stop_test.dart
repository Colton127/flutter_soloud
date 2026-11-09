@TestOn('browser')
library;

import 'dart:async';
import 'dart:typed_data';

import 'package:flutter_soloud/src/bindings/bindings_player_web.dart';
import 'package:flutter_soloud/src/mixer_output_stream_manager.dart';
import 'package:test/test.dart';

import 'web_capture_module.dart';

void main() {
  test('a listener can stop the stream it is listening to', () async {
    final module = WebCaptureModule();
    final bindings = FlutterSoLoudWeb();
    await bindings.setDartEventCallbacks();
    final manager = MixerOutputStreamManager(
      bindings: bindings,
      isReady: () => true,
    );
    final received = <int>[];
    var done = false;
    manager.start(chunkPCMFrames: 2048).listen((chunk) {
      received.addAll(chunk);
      // Record one chunk, then stop from inside the listener.
      if (received.length == chunk.length) manager.stop();
    }, onDone: () => done = true);
    final chunk = Uint8List.fromList(List.generate(2048 * 8, (i) => i & 255));
    final tail = Uint8List.fromList([9, 8, 7, 6, 5, 4, 3, 2]);
    module.setTail(tail);
    module.deliver(module.queue(chunk), chunk.length);
    await Future<void>.delayed(const Duration(milliseconds: 20));
    expect(received, orderedEquals([...chunk, ...tail]));
    expect(done, isTrue);

    // The manager is reusable: a new start opens a new stream.
    final again = <int>[];
    final sub = manager.start(chunkPCMFrames: 2048).listen(again.addAll);
    final next = Uint8List.fromList(List.filled(2048 * 8, 7));
    module.deliver(module.queue(next), next.length);
    await Future<void>.delayed(const Duration(milliseconds: 20));
    expect(again, orderedEquals(next));
    await sub.cancel();
  });

  test('a start during reentrant stop never receives the old tail', () async {
    final module = WebCaptureModule();
    final bindings = FlutterSoLoudWeb();
    await bindings.setDartEventCallbacks();
    final manager = MixerOutputStreamManager(
      bindings: bindings,
      isReady: () => true,
    );
    final first = <int>[];
    final second = <int>[];
    var stopped = false;
    var firstDone = false;
    StreamSubscription<Uint8List>? secondSub;
    final firstSub = manager.start(chunkPCMFrames: 2048).listen((bytes) {
      first.addAll(bytes);
      if (!stopped) {
        stopped = true;
        manager.stop();
        secondSub = manager.start(chunkPCMFrames: 2048).listen(second.addAll);
      }
    }, onDone: () => firstDone = true);
    final chunk = Uint8List.fromList(List.filled(2048 * 8, 4));
    final tail = Uint8List.fromList([9, 8, 7, 6, 5, 4, 3, 2]);
    module.setTail(tail);
    module.deliver(module.queue(chunk), chunk.length);
    await Future<void>.delayed(const Duration(milliseconds: 20));
    expect(first, orderedEquals([...chunk, ...tail]));
    expect(firstDone, isTrue);
    expect(secondSub, isNotNull);
    expect(second, isEmpty);
    expect(module.running, isTrue);
    final next = Uint8List.fromList(List.filled(2048 * 8, 7));
    module.deliver(module.queue(next), next.length);
    await Future<void>.delayed(const Duration(milliseconds: 20));
    expect(second, orderedEquals(next));
    manager.stop();
    await firstSub.cancel();
    await secondSub?.cancel();
  });
}
