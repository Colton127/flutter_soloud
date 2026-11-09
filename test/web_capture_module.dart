import 'dart:js_interop';
import 'dart:js_interop_unsafe';
import 'dart:typed_data';

import 'package:flutter_soloud/src/enums.dart';

import 'package:web/web.dart' as web;

/// Models the native packet contract: stop/start preserves untaken tokens;
/// replacing the callback retires them. It does not model the Dart consumer.
class WebCaptureModule {
  WebCaptureModule() {
    module
      ..['HEAPU8'] = heap.buffer.asUint8List().toJS
      ..['HEAPF32'] = heap.buffer.asFloat32List().toJS
      ..['wasmWorker'] = worker
      ..['_malloc'] = allocate.toJS
      ..['_free'] = free.toJS
      ..['getValue'] = ((int ptr, String type) {
        return heap.getInt32(ptr, Endian.little);
      }).toJS
      ..['_isInited'] = (() => 1).toJS
      ..['_createWorkerInWasm'] = (() => 1).toJS
      ..['_setMixerOutputCallback'] = ((int pointer) {
        packets.clear();
      }).toJS
      ..['_startMixerCapture'] =
          ((
                int format,
                int rate,
                int channels,
                int size,
                int threshold,
                int chunkFrames,
              ) {
                if (running) return PlayerErrors.playerAlreadyInited.index;
                running = true;
                captureId++;
                return 0;
              })
              .toJS
      ..['_stopMixerCapture'] = (() {
        running = false;
      }).toJS
      ..['_stopMixerCaptureAndDrain'] = ((int id, int lengthPtr) {
        if (id == captureId) running = false;
        final tokens = packets.keys.where((t) => packets[t]!.id == id).toList()
          ..sort();
        final bytes = Uint8List.fromList([
          for (final token in tokens) ...packets.remove(token)!.bytes,
          if (id == captureId) ...tail,
        ]);
        if (id == captureId) tail = Uint8List(0);
        heap.setInt32(lengthPtr, bytes.length, Endian.little);
        if (bytes.isEmpty) return 0;
        final ptr = allocate(bytes.length);
        heap.buffer.asUint8List().setAll(ptr, bytes);
        return ptr;
      }).toJS
      ..['_getMixerCaptureId'] = (() => captureId).toJS
      ..['_isMixerCaptureRunning'] = (() => running ? 1 : 0).toJS
      ..['_getMixerCaptureAvailableBytes'] = (() => tail.length).toJS
      ..['_getMixerCaptureBufferPointer'] = (() => tailPtr).toJS
      ..['_getMixerCaptureBufferSize'] = (() => 1 << 20).toJS
      ..['_getMixerCaptureReadOffset'] = (() => 0).toJS
      ..['_drainMixerCapturePackets'] = ((int id, int lengthPtr) {
        final tokens = packets.keys.where((t) => packets[t]!.id == id).toList()
          ..sort();
        final bytes = Uint8List.fromList([
          for (final token in tokens) ...packets.remove(token)!.bytes,
        ]);
        heap.setInt32(lengthPtr, bytes.length, Endian.little);
        if (bytes.isEmpty) return 0;
        final ptr = allocate(bytes.length);
        heap.buffer.asUint8List().setAll(ptr, bytes);
        return ptr;
      }).toJS
      ..['_takeMixerOutputPacket'] = ((int token, int idPtr) {
        final packet = packets.remove(token);
        if (packet == null) return 0;
        heap.setInt32(idPtr, packet.id, Endian.little);
        final ptr = allocate(packet.bytes.length);
        heap.buffer.asUint8List().setAll(ptr, packet.bytes);
        return ptr;
      }).toJS;
    globalContext['Module_soloud'] = module;
  }

  final module = JSObject();
  final worker = JSObject();
  final heap = ByteData(1 << 20);
  final allocations = <int, int>{};
  final packets = <int, ({int id, Uint8List bytes})>{};
  Uint8List tail = Uint8List(0);
  int tailPtr = 0;
  void setTail(Uint8List bytes) {
    tail = bytes;
    tailPtr = allocate(bytes.length);
    heap.buffer.asUint8List().setAll(tailPtr, bytes);
  }

  int captureId = 0;
  bool running = false;
  var _top = 8;
  var _nextToken = 0;

  int allocate(int size) {
    final ptr = _top;
    _top += (size + 7) & ~7;
    allocations[ptr] = size;
    return ptr;
  }

  void free(int ptr) {
    allocations.remove(ptr);
  }

  int queue(Uint8List bytes) {
    final token = ++_nextToken;
    packets[token] = (id: captureId, bytes: Uint8List.fromList(bytes));
    return token;
  }

  void deliver(int token, int length) {
    final message = {
      'message': 'mixerOutputData',
      'offset': token,
      'length': length,
    }.jsify();
    worker.callMethod<JSAny?>(
      'onmessage'.toJS,
      web.MessageEvent('message', web.MessageEventInit(data: message)),
    );
  }
}
