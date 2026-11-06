package flutter.soloud.flutter_soloud;

/**
 * Decides whether an engine lifecycle hook may call into the native library,
 * and loads it from Java only when doing so is a refcount bump.
 *
 * <p>The hooks that need the library run on the platform thread during engine
 * teardown and hot restart. There are only two cases:
 *
 * <ul>
 *   <li>Something in the process has already loaded the library (Dart's first
 *       {@code @Native} call into SoLoud does). Loading it again from Java
 *       only bumps the linker's refcount, and the hook must then make its
 *       native call synchronously, exactly as before.
 *   <li>Nothing has. Then no Dart code in this process has ever called into
 *       SoLoud, so the native engine holds no lifecycle claim and no Dart
 *       callback registration, and both JNI entry points are guaranteed no-ops
 *       that return false. Loading the library would be a real multi-megabyte
 *       load on the platform thread only to learn that, so it is skipped.
 * </ul>
 *
 * <p>Skipping reports "not loaded" without caching it: Dart can still load the
 * library later (before the next hook), and the next hook probes again.
 * Loading successfully is cached for good: ART keeps every library it loads
 * for the lifetime of the class loader.
 *
 * <p>Pure Java, free of Android and Flutter types, so the decision can be unit
 * tested on the JVM with a fake {@link Probe} and {@link Loader}.
 */
final class NativeLibraryGate {
    /** Answers "is the library already in this process?" without loading it. */
    interface Probe {
        /**
         * True when the library is already loaded in this process, or when
         * that cannot be ruled out. Must be cheap enough for the platform
         * thread and must never load the library itself.
         *
         * <p>A false answer is what allows a hook to be skipped, so it must
         * mean "definitely not loaded".
         */
        boolean mayBeLoaded();
    }

    /** Loads the library, e.g. {@code System.loadLibrary}. */
    interface Loader {
        /** @throws UnsatisfiedLinkError when the library cannot be loaded. */
        void load();
    }

    private final Probe probe;
    private final Loader loader;

    /** Guarded by this. */
    private boolean loaded = false;

    NativeLibraryGate(Probe probe, Loader loader) {
        this.probe = probe;
        this.loader = loader;
    }

    /**
     * Returns true when the library is loaded and its JNI entry points may be
     * called. Loads it from Java only when the probe says something in the
     * process already has, so this never performs a real load.
     *
     * <p>Returns false, without loading anything, when the library is
     * definitely not in the process -- the caller then has nothing to do --
     * or when the load fails. Neither is cached, so a later hook tries again.
     * Never throws.
     */
    synchronized boolean loadIfAlreadyInProcess() {
        if (loaded) {
            return true;
        }

        boolean mayBeLoaded;
        try {
            mayBeLoaded = probe.mayBeLoaded();
        } catch (Throwable error) {
            // A probe that cannot answer must not turn into "not loaded": that
            // is the answer that skips a teardown. Fall back to loading, which
            // is what the plugin always did before it had a probe.
            mayBeLoaded = true;
        }

        if (!mayBeLoaded) {
            return false;
        }

        try {
            loader.load();
            loaded = true;
            return true;
        } catch (UnsatisfiedLinkError error) {
            // Never fail engine teardown over this. The FFI layer opens the
            // same library from Dart and surfaces the error there, where it is
            // catchable and actionable.
            return false;
        }
    }
}
