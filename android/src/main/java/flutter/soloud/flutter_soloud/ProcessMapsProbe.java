package flutter.soloud.flutter_soloud;

import android.os.StrictMode;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;

/**
 * Tells whether libflutter_soloud_plugin.so is already loaded in this process,
 * without loading it, by looking for the marker it leaves in /proc/self/maps.
 *
 * <p>When the library is loaded -- by Dart's first {@code @Native} call, or by
 * {@code System.loadLibrary} -- a constructor in it maps one PROT_NONE page and
 * names it with {@code prctl(PR_SET_VMA_ANON_NAME)}, which shows up here as
 * {@code [anon:flutter_soloud_plugin]}. That runs inside dlopen(), before any
 * exported function can run, so the marker exists before Dart can have created
 * any native state that a teardown would act on.
 *
 * <p>Why a marker and not the library's own mapping: with the default
 * packaging (uncompressed libraries, {@code extractNativeLibs=false}) the
 * linker maps the library straight out of the APK, so its segments are listed
 * under base.apk (or a split APK), not under the library's name.
 *
 * <p>This reads an in-memory kernel file, not storage: one pass over the
 * process's mappings (a few hundred KB for a Flutter app), stopping at the
 * first match. The search runs in {@link String#indexOf}, which is compiled
 * into the boot image, rather than in a byte loop of this class, which may
 * still be interpreted when a hook first runs. The plugin stops probing once
 * the library is known to be loaded.
 */
final class ProcessMapsProbe implements NativeLibraryGate.Probe {
    /** Must match the name the constructor in src/bindings.cpp gives its page. */
    static final String MARKER_NAME = "flutter_soloud_plugin";

    enum Result {
        /** The marker is mapped: the library is loaded. */
        PRESENT,
        /** The kernel names anonymous mappings, and the marker is not among them. */
        ABSENT,
        /**
         * No anonymous mapping is named at all, so this kernel cannot name
         * them and the marker's absence proves nothing. Every kernel Android
         * ships names them -- bionic and ART name their own regions -- so this
         * is a guard, not an expected path.
         */
        UNKNOWN,
    }

    /** The name field of any named anonymous mapping starts like this. */
    private static final String NAMED_ANON = " [anon:";

    /** The whole name field of the marker, which ends its line. */
    private static final String MARKER = NAMED_ANON + MARKER_NAME + "]";
    private static final String MARKER_LINE_END = MARKER + "\n";

    private static final int BUFFER_SIZE = 64 * 1024;

    @Override
    public boolean mayBeLoaded() {
        // procfs is generated from memory, but StrictMode cannot tell it from
        // storage and would flag (or, under penaltyDeath, crash) the read.
        final StrictMode.ThreadPolicy policy = StrictMode.allowThreadDiskReads();
        try (InputStream maps = new FileInputStream("/proc/self/maps")) {
            return scan(maps) != Result.ABSENT;
        } catch (IOException | RuntimeException error) {
            // Unreadable maps prove nothing. Only a definite ABSENT may skip.
            return true;
        } finally {
            StrictMode.setThreadPolicy(policy);
        }
    }

    /** Scans the contents of a /proc/self/maps file. Package-private for tests. */
    static Result scan(InputStream maps) throws IOException {
        final byte[] buffer = new byte[BUFFER_SIZE];
        // The end of the previous read, prepended to the next one so that a
        // pattern split across two reads is still found. Long enough to hold
        // all but the last character of either pattern.
        String carry = "";
        boolean sawNamedAnon = false;

        int read;
        while ((read = maps.read(buffer)) != -1) {
            // ISO-8859-1 maps every byte to one char, so nothing is lost and
            // the ASCII patterns match byte for byte.
            final String window =
                carry + new String(buffer, 0, read, StandardCharsets.ISO_8859_1);
            if (window.contains(MARKER_LINE_END)) {
                return Result.PRESENT;
            }
            if (!sawNamedAnon && window.contains(NAMED_ANON)) {
                sawNamedAnon = true;
            }
            carry = window.substring(Math.max(0, window.length() - MARKER.length()));
        }

        // The kernel ends every line with '\n'; this only matters for input
        // that does not.
        if (carry.endsWith(MARKER)) {
            return Result.PRESENT;
        }
        return sawNamedAnon ? Result.ABSENT : Result.UNKNOWN;
    }
}
