package flutter.soloud.flutter_soloud;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import io.flutter.embedding.engine.FlutterEngine;
import io.flutter.embedding.engine.plugins.FlutterPlugin;

public final class FlutterSoloudPlugin implements FlutterPlugin {
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
     */
    private static boolean nativeLibraryLoadAttempted = false;
    private static boolean nativeLibraryLoaded = false;

    private static native boolean
        nativeClearDartCallbackRegistrationsForEngine(long engineId);

    @Nullable private FlutterEngine flutterEngine;
    @Nullable private Long engineId;
    @Nullable private FlutterEngine.EngineLifecycleListener lifecycleListener;

    private static synchronized boolean ensureNativeLibraryLoaded() {
        if (!nativeLibraryLoadAttempted) {
            nativeLibraryLoadAttempted = true;
            try {
                System.loadLibrary("flutter_soloud_plugin");
                nativeLibraryLoaded = true;
            } catch (UnsatisfiedLinkError error) {
                // Never fail plugin registration over this. The FFI layer opens
                // the same library from Dart and surfaces the error there, where
                // it is catchable and actionable.
                nativeLibraryLoaded = false;
            }
        }
        return nativeLibraryLoaded;
    }

    @SuppressWarnings("deprecation")
    @Override
    public void onAttachedToEngine(
        @NonNull FlutterPluginBinding binding
    ) {
        // Deliberately does no native work. This runs during app launch for
        // every app that depends on the plugin, whether or not it ever uses
        // SoLoud, so it must stay pure Java bookkeeping: read the engine id and
        // register a listener.
        final FlutterEngine engine = binding.getFlutterEngine();
        flutterEngine = engine;
        engineId = engine.getEngineId();

        lifecycleListener = new FlutterEngine.EngineLifecycleListener() {
            @Override
            public void onPreEngineRestart() {
                // Hot restart replaces the Dart isolate but does not detach
                // plugins, and the engine id is unchanged -- so without this the
                // registered NativeCallables silently go stale. Only the bridges
                // are cleared: the new isolate's init() finds the native engine
                // still initialized and deinits it itself.
                clearDartCallbackRegistrations();
            }

            @Override
            public void onEngineWillDestroy() {
                // Fires just before the plugin registry is destroyed, while the
                // engine is still valid.
                clearDartCallbackRegistrations();
            }
        };
        engine.addEngineLifecycleListener(lifecycleListener);
    }

    @Override
    public void onDetachedFromEngine(
        @NonNull FlutterPluginBinding binding
    ) {
        final FlutterEngine engine = flutterEngine;
        final FlutterEngine.EngineLifecycleListener listener = lifecycleListener;

        if (engine != null && listener != null) {
            engine.removeEngineLifecycleListener(listener);
        }

        clearDartCallbackRegistrations();

        flutterEngine = null;
        engineId = null;
        lifecycleListener = null;
    }

    private void clearDartCallbackRegistrations() {
        final Long id = engineId;
        if (id == null || !ensureNativeLibraryLoaded()) {
            return;
        }
        nativeClearDartCallbackRegistrationsForEngine(id);
    }
}
