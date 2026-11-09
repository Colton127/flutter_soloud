// The web getFilterParamNames() reads the array of name pointers the native
// export writes, one 4-byte pointer per parameter on wasm32. The parametric
// EQ has 195 parameters. The WASM module is replaced by a stand-in that, like
// the native export, writes one strdup()ed name per parameter into the array
// it is given, and records whether the array was large enough.
@TestOn('browser')
library;

import 'dart:convert';
import 'dart:js_interop';
import 'dart:js_interop_unsafe';
import 'dart:typed_data';

import 'package:flutter_soloud/src/bindings/bindings_player_web.dart';
import 'package:flutter_soloud/src/enums.dart';
import 'package:flutter_soloud/src/filters/filters.dart';
import 'package:test/test.dart';

void main() {
  test('getFilterParamNames reads every name without overflowing', () {
    final heap = ByteData(1 << 20);
    final sizes = <int, int>{};
    var top = 8;
    var overflow = 0;
    final names = [
      'Wet',
      'Window Size',
      'Bands Count',
      for (var b = 0; b < 64; b++) ...[
        'Band $b Gain',
        'Band $b Freq',
        'Band $b Q',
      ],
    ];
    expect(names, hasLength(195));

    int malloc(int size) {
      final ptr = top;
      top += (size + 7) & ~7;
      sizes[ptr] = size;
      return ptr;
    }

    int strdup(String s) {
      final bytes = utf8.encode(s);
      final ptr = malloc(bytes.length + 1);
      for (var i = 0; i < bytes.length; i++) {
        heap.setUint8(ptr + i, bytes[i]);
      }
      heap.setUint8(ptr + bytes.length, 0);
      return ptr;
    }

    final module = JSObject()
      ..['_malloc'] = malloc.toJS
      // toJS needs the int parameter, which a tear-off of remove() lacks.
      // ignore: unnecessary_lambdas
      ..['_free'] = ((int ptr) => sizes.remove(ptr)).toJS
      ..['setValue'] = ((int ptr, int value, String type) {
        heap.setInt32(ptr, value, Endian.little);
      }).toJS
      ..['getValue'] = ((int ptr, String type) {
        if (type != 'i32') throw ArgumentError(type);
        return heap.getInt32(ptr, Endian.little);
      }).toJS
      // Lenient, like Emscripten's: a wrong pointer reads garbage, not an
      // exception.
      ..['UTF8ToString'] = ((int ptr) {
        if (ptr <= 0 || ptr >= heap.lengthInBytes) return '';
        var end = ptr;
        while (end < heap.lengthInBytes && heap.getUint8(end) != 0) {
          end++;
        }
        return utf8.decode(
          heap.buffer.asUint8List(ptr, end - ptr),
          allowMalformed: true,
        );
      }).toJS
      ..['_getFilterParamNames'] =
          ((int type, int countPtr, int namesPtr, [int? slots]) {
            // `slots` is null for a caller that passes no capacity.
            heap.setInt32(countPtr, names.length, Endian.little);
            if (namesPtr == 0) return 0;
            if (slots != null && slots < names.length) return 1;
            final capacity = sizes[namesPtr] ?? 0;
            for (var i = 0; i < names.length; i++) {
              if ((i + 1) * 4 > capacity) overflow++;
              heap.setInt32(namesPtr + i * 4, strdup(names[i]), Endian.little);
            }
            return 0;
          }).toJS;
    globalContext['Module_soloud'] = module;

    final r = FlutterSoLoudWeb().getFilterParamNames(FilterType.parametricEq);
    expect(r.error, PlayerErrors.noError);
    expect(overflow, 0, reason: 'pointers written past the array');
    expect(r.names, names);
  });
}
