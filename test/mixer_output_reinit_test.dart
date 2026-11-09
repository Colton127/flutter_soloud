// deinit() stops the native capture. A mixer output stream left open across
// deinit() must not be handed back by startMixerOutputStream() after the next
// init(): that stream has no capture behind it.
@TestOn('vm')
library;

import 'dart:io';

import 'package:flutter/services.dart';
import 'package:flutter_soloud/flutter_soloud.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  final temp = Directory.systemTemp.createTempSync('soloud_reinit');
  TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
      .setMockMethodCallHandler(
        const MethodChannel('plugins.flutter.io/path_provider'),
        (call) async => temp.path,
      );
  tearDownAll(() => temp.deleteSync(recursive: true));

  test('startMixerOutputStream after deinit and init captures again', () async {
    final soloud = SoLoud.instance;
    await soloud.init();
    final first = soloud.startMixerOutputStream();
    var firstDone = false;
    first.listen((_) {}, onDone: () => firstDone = true);
    expect(soloud.isMixerOutputStreamRunning, isTrue);

    soloud.deinit();
    await Future<void>.delayed(Duration.zero);
    expect(firstDone, isTrue, reason: 'deinit ends the open stream');

    await soloud.init();
    final second = soloud.startMixerOutputStream().listen((_) {});
    expect(soloud.isMixerOutputStreamRunning, isTrue);
    soloud
      ..stopMixerOutputStream()
      ..deinit();
    await second.cancel();
  });
}
