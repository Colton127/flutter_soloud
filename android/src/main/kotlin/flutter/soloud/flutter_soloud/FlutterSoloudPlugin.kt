package flutter.soloud.flutter_soloud

import io.flutter.embedding.engine.plugins.FlutterPlugin

class FlutterSoloudPlugin : FlutterPlugin {
    companion object {
        init {
            System.loadLibrary("flutter_soloud_plugin")
        }

        @JvmStatic
        private external fun nativeClearDartCallbackRegistrationsForEngine(
            engineId: Long,
        ): Boolean
    }

    private var engineId: Long? = null

    @Suppress("DEPRECATION")
    override fun onAttachedToEngine(binding: FlutterPlugin.FlutterPluginBinding) {
        // Dart continues using FFI; no platform channel is required.
        engineId = binding.flutterEngine.engineId
    }

    override fun onDetachedFromEngine(binding: FlutterPlugin.FlutterPluginBinding) {
        val detachedEngineId = engineId
        engineId = null

        if (detachedEngineId != null) {
            nativeClearDartCallbackRegistrationsForEngine(detachedEngineId)
        }
    }
}
