// getFilterParamNames() asks the native side for the names of a filter's
// parameters. The native export writes one `char *` per parameter into the
// array it is given, so that array must hold as many pointers as the filter
// has parameters: the parametric EQ has 195.
//
// This calls the real native export through the @Native bindings (the
// package's build hook builds the library for the host) on an initialized
// engine (miniaudio falls back to its null backend without a device). An
// array too small for the names is a heap overflow: run it under Guard
// Malloc to make one fault where it happens:
//
//   DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib MallocNanoZone=0 \
//     flutter test test/filter_param_names_test.dart
@TestOn('vm')
library;

import 'package:flutter_soloud/src/bindings/bindings_player_ffi.dart';
import 'package:flutter_soloud/src/bindings/flutter_soloud_ffigen.dart'
    as native;
import 'package:flutter_soloud/src/enums.dart';
import 'package:flutter_soloud/src/filters/filters.dart';
import 'package:test/test.dart';

void main() {
  final bindings = FlutterSoLoudFfi();

  setUpAll(() {
    native.prepareEngineInit(1);
    final result = native.initEngine(-1, 44100, 2048, 2, 0, 0, 0);
    expect(result, native.PlayerErrors.noError, reason: 'engine init');
  });

  tearDownAll(native.dispose);

  test('every filter reports all of its parameter names', () {
    // Repeated so a corrupted heap shows up even without Guard Malloc.
    for (var round = 0; round < 50; round++) {
      final eq = bindings.getFilterParamNames(FilterType.parametricEq);
      expect(eq.error, PlayerErrors.noError);
      expect(eq.names, hasLength(195));
      expect(eq.names.take(3), ['Wet', 'Window Size', 'Bands Count']);
      expect(eq.names[3], 'Band 0 Gain');

      for (final type in FilterType.values) {
        final r = bindings.getFilterParamNames(type);
        expect(r.error, PlayerErrors.noError, reason: '$type');
        expect(r.names, isNotEmpty, reason: '$type');
        expect(r.names.every((n) => n.isNotEmpty), isTrue, reason: '$type');
      }
    }
  });
}
