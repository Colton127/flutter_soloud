package flutter.soloud.flutter_soloud;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import io.flutter.embedding.engine.FlutterEngine;
import io.flutter.embedding.engine.plugins.FlutterPlugin;

/**
 * Keeps flutter_soloud's process-global native state in step with the lifetime
 * of the FlutterEngine that owns it.
 *
 * <p>The native engine (the SoLoud player, its output device, its lifecycle
 * scheduler and the registered Dart callback pointers) lives for the whole
 * process, while the Dart isolate that drives it belongs to a single
 * FlutterEngine. When an engine goes away but the process keeps running --
 * routine for a foreground-service audio app, e.g. audio_service -- native code
 * would otherwise keep calling into NativeCallables whose isolate is gone
 * (undefined behaviour) and keep an output device running with nothing left to
 * control it. Only the embedder can observe that transition: Dart's
 * {@code detached} lifecycle state is not guaranteed to arrive first, and there
 * is no reliable root-isolate exit hook.
 */
public final class FlutterSoloudPlugin implements FlutterPlugin {
    /**
     * Guarded by the class monitor. The library is loaded on first attach
     * rather than from a static initializer: a static block would force the
     * whole native library to load on the main thread at app start even for
     * apps that never play audio, and would turn a load failure into an
     * unrecoverable plugin-registration crash.
     */
    private static boolean nativeLibraryLoadAttempted = false;
    private static boolean nativeLibraryLoaded = false;

    private static native boolean
        nativeClearDartCallbackRegistrationsForEngine(long engineId);

    private static native boolean
        nativeRequestEngineTeardownForEngine(long engineId);

    @Nullable private FlutterEngine flutterEngine;
    @Nullable private Long engineId;
    @Nullable private FlutterEngine.EngineLifecycleListener lifecycleListener;

    /**
     * onEngineWillDestroy() and onDetachedFromEngine() both fire on a real
     * engine destroy; the teardown must only be requested once.
     */
    private boolean teardownRequested = false;

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
        ensureNativeLibraryLoaded();

        final FlutterEngine engine = binding.getFlutterEngine();
        flutterEngine = engine;
        engineId = engine.getEngineId();
        teardownRequested = false;

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
                // Fires just before the plugin registry is destroyed. The engine
                // is still valid here, so this is the earliest safe point.
                requestEngineTeardown();
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

        // Requested here too: onEngineWillDestroy() is not reached on every
        // detach path, and requestEngineTeardown() is idempotent.
        requestEngineTeardown();

        flutterEngine = null;
        engineId = null;
        lifecycleListener = null;
    }

    private void clearDartCallbackRegistrations() {
        final Long id = engineId;
        if (id == null || !nativeLibraryLoaded) {
            return;
        }
        nativeClearDartCallbackRegistrationsForEngine(id);
    }

    /**
     * Drops the Dart bridges and asks native code to tear the engine down. The
     * blocking part of the teardown (stopping the device, joining the lifecycle
     * scheduler) runs on a native worker thread, so this returns promptly and
     * never blocks the platform thread.
     */
    private void requestEngineTeardown() {
        final Long id = engineId;
        if (id == null || teardownRequested || !nativeLibraryLoaded) {
            return;
        }
        teardownRequested = true;
        nativeRequestEngineTeardownForEngine(id);
    }
}
