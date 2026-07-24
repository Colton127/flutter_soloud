package flutter.soloud.flutter_soloud;

import androidx.annotation.NonNull;
import io.flutter.embedding.engine.plugins.FlutterPlugin;

public final class FlutterSoloudPlugin implements FlutterPlugin {
    static {
        System.loadLibrary("flutter_soloud_plugin");
    }

    private static native boolean
        nativeClearDartCallbackRegistrationsForEngine(long engineId);

    private Long engineId;

    @SuppressWarnings("deprecation")
    @Override
    public void onAttachedToEngine(
        @NonNull FlutterPluginBinding binding
    ) {
        engineId = binding.getFlutterEngine().getEngineId();
    }

    @Override
    public void onDetachedFromEngine(
        @NonNull FlutterPluginBinding binding
    ) {
        final Long detachedEngineId = engineId;
        engineId = null;

        if (detachedEngineId != null) {
            nativeClearDartCallbackRegistrationsForEngine(
                detachedEngineId
            );
        }
    }
}
