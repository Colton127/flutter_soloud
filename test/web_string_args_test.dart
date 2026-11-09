// The web bindings pass Dart strings to native exports that read them as
// NUL-terminated UTF-8, as the FFI bindings do with toNativeUtf8(). The WASM
// module is replaced by a stand-in whose heap starts dirty, like a block the
// allocator hands out again, and which records the C string each export
// would read.
@TestOn('browser')
library;

import 'dart:convert';
import 'dart:js_interop';
import 'dart:js_interop_unsafe';
import 'dart:typed_data';

import 'package:flutter_soloud/src/bindings/bindings_player_web.dart';
import 'package:flutter_soloud/src/enums.dart';
import 'package:test/test.dart';

void main() {
  late ByteData heap;
  late Map<String, String> received;

  String readCString(int ptr) {
    var end = ptr;
    while (end < heap.lengthInBytes && heap.getUint8(end) != 0) {
      end++;
    }
    return utf8.decode(
      heap.buffer.asUint8List(ptr, end - ptr),
      allowMalformed: true,
    );
  }

  setUp(() {
    heap = ByteData(1 << 16);
    for (var i = 0; i < heap.lengthInBytes; i++) {
      heap.setUint8(i, 0xAA);
    }
    received = {};
    var top = 8;
    final module = JSObject()
      ..['HEAPU8'] = heap.buffer.asUint8List().toJS
      ..['_malloc'] = ((int size) {
        final ptr = top;
        top += (size + 7) & ~7;
        return ptr;
      }).toJS
      ..['_free'] = ((int ptr) {}).toJS
      ..['setValue'] = ((int ptr, int value, String type) {
        if (type != 'i8') throw ArgumentError(type);
        heap.setInt8(ptr, value.toSigned(8));
      }).toJS
      ..['getValue'] = ((int ptr, String type) {
        return heap.getInt32(ptr, Endian.little);
      }).toJS
      ..['_loadMem'] = ((int name, int mem, int length, int mode, int hash) {
        received['loadMem'] = readCString(name);
        heap.setInt32(hash, 1, Endian.little);
        return 0;
      }).toJS
      ..['_joinTwoSources'] =
          ((int name, int mem1, int mem2, int len1, int len2, int hash) {
            received['joinTwoSources'] = readCString(name);
            heap.setInt32(hash, 1, Endian.little);
            return 0;
          }).toJS
      ..['_speechText'] = ((int text, int handle) {
        received['speechText'] = readCString(text);
        heap.setInt32(handle, 1, Endian.little);
        return 0;
      }).toJS;
    globalContext['Module_soloud'] = module;
  });

  test('loadMem passes its unique name', () {
    final r = FlutterSoLoudWeb().loadMem(
      'sound.mp3',
      Uint8List(16),
      LoadMode.memory,
    );
    expect(r.error, PlayerErrors.noError);
    expect(received['loadMem'], 'sound.mp3');
  });

  test('loadMem passes a non-ASCII unique name as UTF-8', () {
    FlutterSoLoudWeb().loadMem('señal.mp3', Uint8List(16), LoadMode.memory);
    expect(received['loadMem'], 'señal.mp3');
  });

  test('joinTwoSources passes its unique name', () {
    FlutterSoLoudWeb().joinTwoSources('joined', Uint8List(8), Uint8List(8));
    expect(received['joinTwoSources'], 'joined');
  });

  test('speechText passes its text', () {
    FlutterSoLoudWeb().speechText('hello world');
    expect(received['speechText'], 'hello world');
  });
}
