import 'dart:io';

import 'package:flutter/services.dart';
import 'package:logging/logging.dart';

/// Hands the iOS plugin the engine id it cannot obtain for itself.
///
/// On Android the plugin reads `FlutterEngine.getEngineId()` directly. iOS
/// exposes no such thing to a plugin, so the id has to travel from the isolate,
/// which gets it from `PlatformDispatcher.instance.engineId`. Once the plugin
/// knows it, `detachFromEngineForRegistrar:` can retire that engine's callables
/// and ask native code to tear down the engine it owns.
///
/// The native claim is taken by the *plugin*, inside the channel handler,
/// before it replies. Storing the id and letting Dart claim afterwards would
/// leave a window where the engine can be deallocated with no claim to tear
/// down — which is the case this exists to cover.
class IosEngineLifecycle {
  /// Creates a bridge over the plugin's engine-local lifecycle channel.
  const IosEngineLifecycle();

  static final Logger _log = Logger('flutter_soloud.IosEngineLifecycle');

  static const MethodChannel _channel = MethodChannel(
    'flutter_soloud/engine_lifecycle',
  );

  /// Whether this platform has the iOS lifecycle plugin at all.
  static bool get _isSupported => Platform.isIOS;

  /// Asks the plugin to adopt [engineId] and take the native lifecycle claim.
  ///
  /// Returns whether it did. `false` means the caller must take the claim
  /// itself through FFI: either this is not iOS, or the channel could not be
  /// used — most often because the app called `SoLoud.init()` without
  /// `WidgetsFlutterBinding.ensureInitialized()`, which has never been a
  /// requirement of this package and does not become one here.
  ///
  /// The cost of falling back is only that automatic teardown on FlutterEngine
  /// destruction is not armed; initialization itself is unaffected.
  Future<bool> prepareEngineInit(int engineId) async {
    if (!_isSupported) return false;

    try {
      final claimed = await _channel.invokeMethod<bool>(
        'prepareEngineInit',
        engineId,
      );
      if (claimed ?? false) return true;

      _log.warning(
        'The iOS lifecycle plugin declined to claim engine $engineId. '
        'Automatic native teardown on FlutterEngine destruction is not armed; '
        'initialization continues normally.',
      );
      return false;
    } on Object catch (error, stackTrace) {
      // Deliberately broad: a MissingPluginException (registrant not run), a
      // binding that was never initialized, and a platform-thread failure all
      // mean the same thing here, and none of them may be allowed to fail an
      // initialization that would otherwise have worked.
      _log.warning(
        'Unable to arm automatic iOS FlutterEngine lifecycle teardown; '
        'falling back to direct native initialization. The native engine will '
        'still be torn down by an explicit deinit(), and a later init() '
        'recovers a stale engine, but it will not be released automatically '
        'when the FlutterEngine is destroyed.',
        error,
        stackTrace,
      );
      return false;
    }
  }
}
