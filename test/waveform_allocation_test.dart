// Waveform extraction (readSamplesFromMem / readSamplesFromFile) allocates
// native memory for the call: a copy of the encoded input, the sample buffer
// the native side fills, and the UTF-8 file name. None of it may outlive the
// call: repeated extraction must not grow the process.
//
// These tests call the real native exports through the @Native bindings (the
// package's build hook builds the library for the host), and measure the
// resident set across repeated calls. Each scenario is sized so a leak of its
// allocation adds several times the allowed growth.
@TestOn('vm')
library;

import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_soloud/src/bindings/bindings_player_ffi.dart';
import 'package:flutter_soloud/src/exceptions/exceptions.dart';
import 'package:test/test.dart';

const int _mb = 1024 * 1024;

/// A 16-bit mono 44.1 kHz PCM WAV of [frames] frames of a tone.
Uint8List _wav(int frames) {
  final bytes = Uint8List(44 + frames * 2);
  final header = ByteData.sublistView(bytes, 0, 44);
  void ascii(int at, String s) {
    for (var i = 0; i < s.length; i++) {
      header.setUint8(at + i, s.codeUnitAt(i));
    }
  }

  ascii(0, 'RIFF');
  header.setUint32(4, 36 + frames * 2, Endian.little);
  ascii(8, 'WAVEfmt ');
  header
    ..setUint32(16, 16, Endian.little)
    ..setUint16(20, 1, Endian.little)
    ..setUint16(22, 1, Endian.little)
    ..setUint32(24, 44100, Endian.little)
    ..setUint32(28, 44100 * 2, Endian.little)
    ..setUint16(32, 2, Endian.little)
    ..setUint16(34, 16, Endian.little);
  ascii(36, 'data');
  header.setUint32(40, frames * 2, Endian.little);
  final samples = Int16List.sublistView(bytes, 44);
  for (var i = 0; i < frames; i++) {
    // A triangle wave: cheap to build, and never silent.
    final phase = i % 200;
    samples[i] = (phase < 100 ? phase : 200 - phase) * 100 - 5000;
  }
  return bytes;
}

/// Resident-set growth over [calls] calls of [call], after one warm-up call.
int _rssGrowth(int calls, void Function() call) {
  call();
  final before = ProcessInfo.currentRss;
  for (var i = 0; i < calls; i++) {
    call();
  }
  return ProcessInfo.currentRss - before;
}

void main() {
  final bindings = FlutterSoLoudFfi();

  test('readSamplesFromMem frees its copy of a rejected input', () {
    // Not audio, so the native side rejects it: the error path.
    final garbage = Uint8List(16 * _mb)..fillRange(0, 16 * _mb, 0x5a);
    final growth = _rssGrowth(12, () {
      expect(
        () => bindings.readSamplesFromMem(garbage, 1024),
        throwsA(isA<SoLoudCppException>()),
      );
    });
    expect(
      growth,
      lessThan(64 * _mb),
      reason:
          '12 rejected 16 MB inputs grew the process by '
          '${growth ~/ _mb} MB: the native copy of the input leaks',
    );
  });

  test('readSamplesFromMem frees its copy of a decoded input', () {
    final wav = _wav(8 * _mb); // 16 MB of PCM
    final growth = _rssGrowth(12, () {
      expect(bindings.readSamplesFromMem(wav, 1024).length, 1024);
    });
    expect(
      growth,
      lessThan(64 * _mb),
      reason:
          '12 decoded 16 MB inputs grew the process by '
          '${growth ~/ _mb} MB: the native copy of the input leaks',
    );
  });

  test('readSamplesFromFile frees its sample buffer', () {
    final dir = Directory.systemTemp.createTempSync('waveform_allocation');
    addTearDown(() => dir.deleteSync(recursive: true));
    final file = File('${dir.path}/tone.wav')..writeAsBytesSync(_wav(44100));

    const samplesPerCall = 4 * _mb; // 16 MB of floats written per call
    final growth = _rssGrowth(20, () {
      expect(
        bindings.readSamplesFromFile(file.path, samplesPerCall).length,
        samplesPerCall,
      );
    });
    expect(
      growth,
      lessThan(160 * _mb),
      reason:
          '20 extractions of 16 MB of samples grew the process by '
          '${growth ~/ _mb} MB: the native sample buffer leaks',
    );
  });
}
