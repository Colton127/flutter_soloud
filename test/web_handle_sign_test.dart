// SoLoud voice handles are unsigned 32-bit: (voice + 1) | (playIndex << 12),
// with a 20-bit play index, so from the 524288th play on the top bit is set.
// The FFI bindings read them as unsigned. The web bindings must return the
// same value, not a negative one. The WASM module is replaced by a stand-in
// whose play export returns such a handle.
@TestOn('browser')
library;

import 'dart:js_interop';
import 'dart:js_interop_unsafe';
import 'dart:typed_data';

import 'package:flutter_soloud/src/bindings/bindings_player_web.dart';
import 'package:flutter_soloud/src/enums.dart';
import 'package:flutter_soloud/src/sound_hash.dart';
import 'package:test/test.dart';
import 'package:web/web.dart' as web;

import 'web_capture_module.dart';

void main() {
  test('a handle with the top bit set comes back unsigned', () {
    const handle = (1 | (0x80000 << 12)) & 0xffffffff; // play index 2^19
    expect(handle, 0x80000001);

    final heap = ByteData(1 << 12);
    var top = 8;
    final module = JSObject()
      ..['_malloc'] = ((int size) {
        final ptr = top;
        top += (size + 7) & ~7;
        return ptr;
      }).toJS
      ..['_free'] = ((int ptr) {}).toJS
      ..['getValue'] = ((int ptr, String type) {
        switch (type) {
          case 'i32':
            return heap.getInt32(ptr, Endian.little);
          case '*':
          case 'u32':
            return heap.getUint32(ptr, Endian.little);
        }
        throw ArgumentError(type);
      }).toJS
      ..['_play'] =
          ((
                int hash,
                int bus,
                double volume,
                double pan,
                bool paused,
                bool looping,
                double loopStart,
                double loopEnd,
                int loopStartOffset,
                int loopEndOffset,
                double scale,
                int handlePtr,
              ) {
                heap.setUint32(handlePtr, handle, Endian.little);
                return 0;
              })
              .toJS;
    globalContext['Module_soloud'] = module;

    final r = FlutterSoLoudWeb().play(SoundHash.fromValue(1));
    expect(r.error, PlayerErrors.noError);
    expect(r.newHandle.id, handle);
  });
  test('voice-ended events use the same unsigned id as play', () async {
    final module = WebCaptureModule();
    final bindings = FlutterSoLoudWeb();
    await bindings.setDartEventCallbacks();
    final ended = <int>[];
    final sub = bindings.voiceEndedEventController.stream.listen(ended.add);
    module.worker.callMethod<JSAny?>(
      'onmessage'.toJS,
      web.MessageEvent(
        'message',
        web.MessageEventInit(
          data: {'message': 'voiceEndedCallback', 'value': -2147483647}.jsify(),
        ),
      ),
    );
    await Future<void>.delayed(const Duration(milliseconds: 20));
    expect(ended, [0x80000001]);
    await sub.cancel();
  });
}
