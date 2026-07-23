package flutter.soloud.flutter_soloud

import io.flutter.embedding.engine.plugins.FlutterPlugin

class FlutterSoloudPlugin : FlutterPlugin {
    companion object {
        init {
            System.loadLibrary("flutter_soloud_plugin")
        }

        @JvmStatic
        private external fun nativeClearDartCallbackRegistrations()
    }

    override fun onAttachedToEngine(binding: FlutterPlugin.FlutterPluginBinding) {
        // Dart continues using FFI; no platform channel is required.
    }

    override fun onDetachedFromEngine(binding: FlutterPlugin.FlutterPluginBinding) {
        nativeClearDartCallbackRegistrations()
    }
}
