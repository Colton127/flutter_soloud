// The web reflection of metadata_ffi.h reads C strings out of the WASM heap.
// The native side stores UTF-8, so a tag outside ASCII must decode as UTF-8.
// The WASM module is replaced by a stand-in heap with Emscripten's getValue().
@TestOn('browser')
library;

import 'dart:convert';
import 'dart:js_interop';
import 'dart:js_interop_unsafe';
import 'dart:typed_data';

import 'package:flutter_soloud/src/bindings/native_metadata_web.dart';
import 'package:test/test.dart';

void main() {
  test('metadata strings decode as UTF-8', () {
    final heap = ByteData(1 << 17);
    final module = JSObject()
      ..['getValue'] = ((int ptr, String type) {
        switch (type) {
          case 'i8':
            return heap.getInt8(ptr);
          case 'i32':
            return heap.getInt32(ptr, Endian.little);
        }
        throw ArgumentError(type);
      }).toJS;
    globalContext['Module_soloud'] = module;

    void putString(int at, String s) {
      final bytes = utf8.encode(s);
      for (var i = 0; i < bytes.length; i++) {
        heap.setUint8(at + i, bytes[i]);
      }
      heap.setUint8(at + bytes.length, 0);
    }

    const base = 64;
    // An Mp3MetadataFFI: title at 0, artist at 1024.
    putString(base, 'Café del Mar');
    putString(base + 1024, 'Sigur Rós');
    final mp3 = NativeMp3Metadata(base);
    expect(mp3.title, 'Café del Mar');
    expect(mp3.artist, 'Sigur Rós');
  });
}
