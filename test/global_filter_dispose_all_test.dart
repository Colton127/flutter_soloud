// disposeAllSound() detaches the global filters from the engine. The global
// filter list must forget them too, or the filter is reported active while
// nothing applies it, and adding it again is refused.
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

  test('disposing all sounds leaves no stale global filter', () {
    expect(bindings.addFilter(FilterType.echoFilter), PlayerErrors.noError);
    expect(bindings.isFilterActive(FilterType.echoFilter).index, 0);

    bindings.disposeAllSound();

    expect(bindings.isFilterActive(FilterType.echoFilter).index, -1);
    expect(bindings.addFilter(FilterType.echoFilter), PlayerErrors.noError);
    expect(bindings.isFilterActive(FilterType.echoFilter).index, 0);
    expect(bindings.removeFilter(FilterType.echoFilter), PlayerErrors.noError);
  });
}
