package flutter.soloud.flutter_soloud;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

public class NativeLibraryGateTest {
    /** A probe whose answer the test controls, counting how often it is asked. */
    private static final class FakeProbe implements NativeLibraryGate.Probe {
        boolean loadedInProcess;
        RuntimeException failure;
        int calls;

        @Override
        public boolean mayBeLoaded() {
            calls++;
            if (failure != null) {
                throw failure;
            }
            return loadedInProcess;
        }
    }

    /** A loader that counts loads and can be made to fail. */
    private static final class FakeLoader implements NativeLibraryGate.Loader {
        boolean fail;
        int calls;

        @Override
        public void load() {
            calls++;
            if (fail) {
                throw new UnsatisfiedLinkError("dlopen failed");
            }
        }
    }

    private final FakeProbe probe = new FakeProbe();
    private final FakeLoader loader = new FakeLoader();
    private final NativeLibraryGate gate = new NativeLibraryGate(probe, loader);

    @Test
    public void notLoadedInProcessSkipsWithoutLoading() {
        probe.loadedInProcess = false;

        assertFalse(gate.loadIfAlreadyInProcess());
        assertEquals("must never do a real load", 0, loader.calls);
    }

    @Test
    public void loadedInProcessLoadsOnceAndCaches() {
        probe.loadedInProcess = true;

        assertTrue(gate.loadIfAlreadyInProcess());
        assertTrue(gate.loadIfAlreadyInProcess());
        assertTrue(gate.loadIfAlreadyInProcess());

        assertEquals(1, loader.calls);
        assertEquals("a loaded library is never probed again", 1, probe.calls);
    }

    @Test
    public void skipIsNotCachedSoALaterHookSeesDartsLoad() {
        // onEngineWillDestroy(): Dart has not opened the library yet.
        probe.loadedInProcess = false;
        assertFalse(gate.loadIfAlreadyInProcess());

        // Dart opens it before onDetachedFromEngine(), which must now act.
        probe.loadedInProcess = true;
        assertTrue(gate.loadIfAlreadyInProcess());

        assertEquals(2, probe.calls);
        assertEquals(1, loader.calls);
    }

    @Test
    public void repeatedSkipsNeverLoad() {
        probe.loadedInProcess = false;

        for (int i = 0; i < 5; i++) {
            assertFalse(gate.loadIfAlreadyInProcess());
        }

        assertEquals(5, probe.calls);
        assertEquals(0, loader.calls);
    }

    @Test
    public void failedLoadReturnsFalseAndIsRetried() {
        probe.loadedInProcess = true;
        loader.fail = true;

        assertFalse(gate.loadIfAlreadyInProcess());
        assertFalse(gate.loadIfAlreadyInProcess());
        assertEquals(2, loader.calls);

        loader.fail = false;
        assertTrue(gate.loadIfAlreadyInProcess());
        assertTrue(gate.loadIfAlreadyInProcess());
        assertEquals(3, loader.calls);
    }

    @Test
    public void probeThatThrowsFallsBackToLoading() {
        // "Cannot tell" must never become "not loaded": that answer skips a
        // teardown the engine may need.
        probe.failure = new IllegalStateException("maps unreadable");

        assertTrue(gate.loadIfAlreadyInProcess());
        assertEquals(1, loader.calls);
    }

    @Test
    public void probeThatThrowsAndFailedLoadNeverThrows() {
        probe.failure = new IllegalStateException("maps unreadable");
        loader.fail = true;

        assertFalse(gate.loadIfAlreadyInProcess());
    }
}
