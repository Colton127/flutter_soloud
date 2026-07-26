package flutter.soloud.flutter_soloud;

import android.util.Log;
import androidx.annotation.NonNull;
import io.flutter.embedding.engine.plugins.FlutterPlugin;

public final class FlutterSoloudPlugin implements FlutterPlugin {
    private static final String TAG = "FlutterSoloudPlugin";

    /**
     * Guarded by the class monitor.
     *
     * <p>The native library is loaded lazily, at the first point one of the
     * hooks below actually needs to call into it -- never from a static
     * initializer and never from {@link #onAttachedToEngine}. Both of those run
     * at engine startup (GeneratedPluginRegistrant instantiates and attaches
     * every plugin), which would drag the whole multi-megabyte library onto the
     * main thread during app launch even for an app that never plays a sound.
     *
     * <p>By the time a hook fires, an app that uses SoLoud has already loaded
     * the same library from Dart via {@code DynamicLibrary.open}, so
     * {@code System.loadLibrary} is a refcount bump rather than a real load.
     * Successful loads are cached; failed loads can be retried by a later hook.
     */
    private static boolean nativeLibraryLoaded = false;

    private static native boolean
        nativeClearDartCallbackRegistrationsForEngine(long engineId);

    private Long engineId;

    private static synchronized boolean ensureNativeLibraryLoaded() {
        if (nativeLibraryLoaded) {
            return true;
        }

        try {
            System.loadLibrary("flutter_soloud_plugin");
            nativeLibraryLoaded = true;
            return true;
        } catch (UnsatisfiedLinkError error) {
            // Never fail engine teardown over this. The FFI layer opens the
            // same library from Dart and surfaces the error there, where it is
            // catchable and actionable.
            return false;
        }
    }

    @SuppressWarnings("deprecation")
    @Override
    public void onAttachedToEngine(
        @NonNull FlutterPluginBinding binding
    ) {
        // Deliberately does no native work. This runs during app launch for
        // every app that depends on the plugin, whether or not it ever uses
        // SoLoud, so it must stay pure Java bookkeeping.
        engineId = binding.getFlutterEngine().getEngineId();
    }

    @Override
    public void onDetachedFromEngine(
        @NonNull FlutterPluginBinding binding
    ) {
        final Long detachedEngineId = engineId;
        engineId = null;

        if (detachedEngineId == null || !ensureNativeLibraryLoaded()) {
            return;
        }

        try {
            nativeClearDartCallbackRegistrationsForEngine(detachedEngineId);
        } catch (UnsatisfiedLinkError error) {
            Log.w(
                TAG,
                "Unable to clear Dart callback registrations during engine teardown",
                error
            );
        }
    }
}
