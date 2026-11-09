// metadata_ffi.h stores tags as UTF-8 in fixed char arrays. The FFI reflection
// must decode them as UTF-8, whether `char` is signed (Apple, x86) or unsigned
// (Android ARM).
@TestOn('vm')
library;

import 'dart:convert';
import 'dart:ffi' as ffi;

import 'package:ffi/ffi.dart';
import 'package:flutter_soloud/src/bindings/native_metadata_ffi.dart';
import 'package:test/test.dart';

void main() {
  test('metadata strings decode as UTF-8', () {
    final meta = calloc<NativeAudioMetadata>();
    addTearDown(() => calloc.free(meta));

    void put(ffi.Array<ffi.Char> array, String s) {
      final bytes = utf8.encode(s);
      for (var i = 0; i < bytes.length; i++) {
        array[i] = bytes[i];
      }
      array[bytes.length] = 0;
    }

    put(meta.ref.mp3Metadata.title, 'Café del Mar');
    put(meta.ref.mp3Metadata.artist, 'Sigur Rós');
    put(meta.ref.oggMetadata.vendor, 'Lavf 61 – libopus');
    meta.ref.oggMetadata.commentsCount = 1;
    put(meta.ref.oggMetadata.comments[0].key, 'TITLE');
    put(meta.ref.oggMetadata.comments[0].value, 'Jóga');

    final result = meta.ref.toAudioMetadata();
    expect(result.mp3Metadata!.title, 'Café del Mar');
    expect(result.mp3Metadata!.artist, 'Sigur Rós');
    expect(result.oggMetadata!.vendor, 'Lavf 61 – libopus');
    expect(result.oggMetadata!.comments, {'TITLE': 'Jóga'});
  });

  test('metadata strings stay bounded and tolerate malformed UTF-8', () {
    final meta = calloc<NativeAudioMetadata>();
    addTearDown(() => calloc.free(meta));
    final title = utf8.encode('  Café  ');
    for (var i = 0; i < title.length; i++) {
      meta.ref.mp3Metadata.title[i] = title[i];
    }
    meta.ref.aacMetadata.title[0] = 0xc3;
    meta.ref.aacMetadata.title[1] = 0x28;
    // Exactly fill both 32-byte fields, without a NUL terminator.
    for (var i = 0; i < 32; i++) {
      final byte = i.isEven ? 0xc3 : 0xa9;
      meta.ref.aacMetadata.profile[i] = byte;
      meta.ref.m4aMetadata.codec[i] = byte;
    }
    final result = meta.ref.toAudioMetadata();
    expect(result.mp3Metadata!.title, 'Café');
    expect(result.aacMetadata!.title, '\uFFFD(');
    expect(result.aacMetadata!.profile, List.filled(16, 'é').join());
    expect(result.m4aMetadata!.codec, List.filled(16, 'é').join());
  });
}
