// The web readSamplesFromMem() copies its result out of the WASM heap before
// freeing it. The WASM module is replaced by a stand-in whose allocator, like
// the real one, hands a freed block to the next request of the same size.
@TestOn('browser')
library;

import 'dart:js_interop';
import 'dart:js_interop_unsafe';
import 'dart:typed_data';

import 'package:flutter_soloud/src/bindings/bindings_player_web.dart';
import 'package:test/test.dart';

void main() {
  test('readSamplesFromMem returns samples the heap cannot overwrite', () {
    final heap = ByteData(1 << 16).buffer;
    final f32 = Float32List.view(heap);
    final sizes = <int, int>{};
    final freed = <int, List<int>>{};
    var top = 8;
    var calls = 0;

    final module = JSObject()
      ..['HEAPU8'] = Uint8List.view(heap).toJS
      ..['HEAPF32'] = f32.toJS
      ..['_malloc'] = ((int size) {
        final reuse = freed[size];
        final int ptr;
        if (reuse != null && reuse.isNotEmpty) {
          ptr = reuse.removeLast();
        } else {
          ptr = top;
          top += (size + 7) & ~7;
        }
        sizes[ptr] = size;
        return ptr;
      }).toJS
      ..['_free'] = ((int ptr) {
        (freed[sizes.remove(ptr)!] ??= []).add(ptr);
      }).toJS
      ..['_readSamplesFromMem'] =
          ((
                int buffer,
                int length,
                double start,
                double end,
                int count,
                bool average,
                int out,
              ) {
                calls++;
                for (var i = 0; i < count; i++) {
                  f32[out ~/ 4 + i] = calls * 100.0 + i;
                }
                return 0;
              })
              .toJS;
    globalContext['Module_soloud'] = module;

    final web = FlutterSoLoudWeb();
    final input = Uint8List(64);
    final first = web.readSamplesFromMem(input, 8);
    final expected = Float32List.fromList(first);
    expect(expected[0], 100.0);

    final second = web.readSamplesFromMem(input, 8);
    expect(second[0], 200.0);
    expect(
      first,
      expected,
      reason: 'the second call must not change the first result',
    );
  });
}
