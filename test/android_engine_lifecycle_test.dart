import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';
import 'package:flutter_soloud/src/bindings/android_engine_lifecycle.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:logging/logging.dart';

/// The Android plugin skips its engine-teardown hooks unless something says
/// the native library is in use. This signal is the reliable one, the
/// library's own load marker the fallback, so it has to hold two properties:
/// a delivered signal is remembered for the isolate, and a signal that cannot
/// be delivered never fails initialization or gets remembered as delivered.
void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  const bridge = AndroidEngineLifecycle();

  late List<MethodCall> calls;
  late List<LogRecord> warnings;
  late StreamSubscription<LogRecord> logSubscription;

  setUp(() {
    calls = <MethodCall>[];
    warnings = <LogRecord>[];
    Logger.root.level = Level.ALL;
    logSubscription = Logger.root.onRecord
        .where((record) => record.level >= Level.WARNING)
        .listen(warnings.add);
    debugDefaultTargetPlatformOverride = TargetPlatform.android;
    AndroidEngineLifecycle.debugReset();
  });

  tearDown(() async {
    await logSubscription.cancel();
    debugDefaultTargetPlatformOverride = null;
    AndroidEngineLifecycle.debugReset();
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(
          const MethodChannel(AndroidEngineLifecycle.channelName),
          null,
        );
  });

  void handleWith(Future<Object?>? Function(MethodCall call) handler) {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(
          const MethodChannel(AndroidEngineLifecycle.channelName),
          (call) {
            calls.add(call);
            return handler(call);
          },
        );
  }

  test('uses the same engine-local channel as iOS and macOS', () {
    expect(
      AndroidEngineLifecycle.channelName,
      'flutter_soloud/engine_lifecycle',
    );
  });

  test('an acknowledged signal is sent once per isolate', () async {
    handleWith((call) async => true);

    expect(AndroidEngineLifecycle.isPending, isTrue);
    expect(await bridge.markNativeLibraryInUse(), isTrue);

    expect(calls.single.method, AndroidEngineLifecycle.methodName);
    expect(calls.single.method, 'nativeLibraryInUse');
    expect(calls.single.arguments, isNull);
    expect(warnings, isEmpty);

    // The plugin's record is process-wide and permanent, so later inits on
    // this isolate neither suspend nor send.
    expect(AndroidEngineLifecycle.isPending, isFalse);
    expect(await bridge.markNativeLibraryInUse(), isTrue);
    expect(calls, hasLength(1));
  });

  test('an unregistered plugin is logged and not remembered', () async {
    // No handler at all: what an engine without the plugin looks like.
    expect(await bridge.markNativeLibraryInUse(), isFalse);

    expect(AndroidEngineLifecycle.isPending, isTrue);
    expect(warnings.single.message, contains('not registered'));
    expect(warnings.single.error, isA<MissingPluginException>());
  });

  test('a MissingPluginException reply is logged and not remembered', () async {
    handleWith((call) async => throw MissingPluginException());

    expect(await bridge.markNativeLibraryInUse(), isFalse);

    expect(calls, hasLength(1));
    expect(AndroidEngineLifecycle.isPending, isTrue);
    expect(warnings.single.error, isA<MissingPluginException>());
  });

  test('an error reply never throws and the next init retries', () async {
    handleWith((call) async => throw PlatformException(code: 'boom'));

    expect(await bridge.markNativeLibraryInUse(), isFalse);
    expect(AndroidEngineLifecycle.isPending, isTrue);
    expect(warnings.single.error, isA<PlatformException>());

    // A failure is not cached: the next initialization tries again, and
    // success then sticks.
    handleWith((call) async => true);
    expect(await bridge.markNativeLibraryInUse(), isTrue);
    expect(calls, hasLength(2));
    expect(AndroidEngineLifecycle.isPending, isFalse);
  });

  test('an unknown failure never throws', () async {
    handleWith((call) async => throw StateError('the reply went missing'));

    expect(await bridge.markNativeLibraryInUse(), isFalse);
    expect(AndroidEngineLifecycle.isPending, isTrue);
    expect(warnings, hasLength(1));
  });

  test('a reply that is not true is not an acknowledgement', () async {
    for (final reply in <Object?>[false, null]) {
      calls.clear();
      warnings.clear();
      handleWith((call) async => reply);

      expect(await bridge.markNativeLibraryInUse(), isFalse);
      expect(calls, hasLength(1));
      expect(
        AndroidEngineLifecycle.isPending,
        isTrue,
        reason: '$reply must not be remembered as delivered',
      );
      expect(warnings, hasLength(1));
    }
  });

  test('platforms without the Android plugin never send anything', () async {
    for (final platform in <TargetPlatform>[
      TargetPlatform.iOS,
      TargetPlatform.macOS,
      TargetPlatform.linux,
      TargetPlatform.windows,
      TargetPlatform.fuchsia,
    ]) {
      calls.clear();
      debugDefaultTargetPlatformOverride = platform;
      handleWith((call) async => true);

      expect(
        AndroidEngineLifecycle.isPending,
        isFalse,
        reason: '$platform must not make init suspend for this',
      );
      expect(await bridge.markNativeLibraryInUse(), isFalse);
      expect(calls, isEmpty, reason: '$platform must not send anything');
    }
    expect(warnings, isEmpty);
  });
}
