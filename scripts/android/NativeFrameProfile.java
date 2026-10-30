package dev.datalux.testing;

import java.io.File;
import java.io.FileInputStream;
import java.lang.reflect.Method;
import java.nio.ByteBuffer;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.Map;

/**
 * Profiles one already-saved packed Y/U/V frame through the actual app DEX/native receiver.
 * Arguments: native library, frame, width, height, total iterations (1..300).
 * The first min(20, iterations) calls are warmup; no images, messages, names or payloads are printed.
 * This is a repeated-frame/single-lane diagnostic, not an optical throughput benchmark.
 * PipelineMs includes its five subphases; do not add that aggregate to those subphase means.
 */
public final class NativeFrameProfile {
    private static final long MAX_PIXELS = 16_000_000L;
    private static final long MAX_FRAME_BYTES = 24_000_000L;
    private static final String OWNER = "dev.fileflow.rx.NativeReceiver";
    private static final String[] STAGES = {
            "PipelineMs", "TrackingMs", "AlignmentMs", "LumaSamplingMs", "PhotometryMs",
            "ColorSamplingMs", "HeaderMs", "DemodMs", "FecMs", "CombineMs", "LockWaitMs", "CommitMs"
    };
    private static final String[] COUNTERS = {
            "Phase", "Frames", "TimedFrames", "HeaderFrames", "PayloadFrames", "FecFailedFrames",
            "CrcFailedFrames", "UsefulSymbols", "CombinedFrames", "DistinctStates", "BitsPerCell"
    };

    private static void require(boolean valid) {
        if (!valid) throw new IllegalArgumentException("invalid diagnostic input or status");
    }

    private static Method method(String name, Class<?>... types) throws Exception {
        Method method = Class.forName(OWNER).getDeclaredMethod(name, types);
        method.setAccessible(true);
        return method;
    }

    private static Map<String, Integer> slotIndices() throws Exception {
        Object[] constants = Class.forName("dev.fileflow.rx.LiveDecoder$Slot").getEnumConstants();
        require(constants != null);
        Map<String, Integer> indices = new LinkedHashMap<>();
        for (Object constant : constants) {
            Enum<?> slot = (Enum<?>) constant;
            require(indices.put(slot.name(), slot.ordinal()) == null);
        }
        return indices;
    }

    private static void readPlane(FileInputStream input, ByteBuffer plane, byte[] scratch) throws Exception {
        while (plane.hasRemaining()) {
            int count = input.read(scratch, 0, Math.min(scratch.length, plane.remaining()));
            require(count > 0);
            plane.put(scratch, 0, count);
        }
        plane.flip();
    }

    private static double slot(double[] status, Map<String, Integer> indices, String name) {
        Integer index = indices.get(name);
        require(index != null);
        double value = status[index];
        require(Double.isFinite(value));
        return value;
    }

    private static double delta(double[] status, double[] baseline, Map<String, Integer> indices, String name) {
        double difference = slot(status, indices, name) - slot(baseline, indices, name);
        require(difference >= 0);
        return difference;
    }

    public static void main(String[] args) {
        String stage = "arguments";
        try {
            require(args.length == 5);
            int width = Integer.parseInt(args[2]);
            int height = Integer.parseInt(args[3]);
            int iterations = Integer.parseInt(args[4]);
            require(width > 0 && height > 0 && width <= 4096 && height <= 4096);
            require(iterations >= 1 && iterations <= 300);
            long lumaBytes = (long) width * height;
            long chromaBytes = (long) (width / 2) * (height / 2);
            long frameBytes = lumaBytes + 2 * chromaBytes;
            require(lumaBytes <= MAX_PIXELS && frameBytes <= MAX_FRAME_BYTES);
            File frame = new File(args[1]);
            require(frame.isFile() && frame.length() == frameBytes);

            stage = "frame_read";
            ByteBuffer y = ByteBuffer.allocateDirect((int) lumaBytes);
            ByteBuffer u = ByteBuffer.allocateDirect((int) chromaBytes);
            ByteBuffer v = ByteBuffer.allocateDirect((int) chromaBytes);
            try (FileInputStream input = new FileInputStream(frame)) {
                byte[] scratch = new byte[64 * 1024];
                readPlane(input, y, scratch);
                readPlane(input, u, scratch);
                readPlane(input, v, scratch);
                require(input.read() == -1);
            }

            stage = "native_load";
            System.load(args[0]);
            Map<String, Integer> indices = slotIndices();
            int count = (Integer) method("slotCount").invoke(null);
            require(count == indices.size());
            for (String name : STAGES) require(indices.containsKey(name));
            for (String name : COUNTERS) require(indices.containsKey(name));
            Method open = method("open", int.class, int.class, long.class, int.class, double.class);
            Method close = method("close", long.class);
            Method ingest = method("ingest", long.class, ByteBuffer.class, int.class, int.class, int.class,
                    ByteBuffer.class, ByteBuffer.class, int.class, int.class, long.class, int.class, double[].class);
            double[] status = new double[count];
            double[] baseline = new double[count];
            long handle = (Long) open.invoke(null, 120, 260, 4_000_000L, 1, 0.0);
            require(handle != 0);
            int warmup = Math.min(20, iterations);
            long ingestWallNs = 0;
            stage = "ingest";
            try {
                for (int i = 0; i < iterations; i++) {
                    Arrays.fill(status, Double.NaN);
                    long started = System.nanoTime();
                    // Ignore the returned human-facing message; it may contain a transfer name.
                    ingest.invoke(null, handle, y, width, height, width, u, v,
                            width / 2, 1, 1L + i * 20_000_000L, 0, status);
                    long elapsed = System.nanoTime() - started;
                    require(slot(status, indices, "Phase") >= 0);
                    if (i + 1 == warmup) System.arraycopy(status, 0, baseline, 0, count);
                    if (i >= warmup) ingestWallNs += elapsed;
                }
            } finally {
                close.invoke(null, handle);
            }

            stage = "summary";
            double measuredFrames = delta(status, baseline, indices, "TimedFrames");
            int measuredIterations = iterations - warmup;
            StringBuilder report = new StringBuilder("DATALUX_NATIVE_FRAME_PROFILE {\"iterations\":")
                    .append(iterations).append(",\"warmupIterations\":").append(warmup)
                    .append(",\"measuredIterations\":").append(measuredIterations)
                    .append(",\"nativeTimedFrames\":").append(measuredFrames)
                    .append(",\"ingestWallTotalMs\":").append(ingestWallNs / 1e6)
                    .append(",\"ingestWallMeanMs\":")
                    .append(measuredIterations > 0 ? ingestWallNs / 1e6 / measuredIterations : 0.0)
                    .append(",\"stageMeanMsPerTimedFrame\":{");
            for (int i = 0; i < STAGES.length; i++) {
                if (i != 0) report.append(',');
                String name = STAGES[i];
                double difference = delta(status, baseline, indices, name);
                report.append('"').append(name).append("\":")
                        .append(measuredFrames > 0 ? difference / measuredFrames : 0.0);
            }
            report.append("},\"finalCounters\":{");
            for (int i = 0; i < COUNTERS.length; i++) {
                if (i != 0) report.append(',');
                String name = COUNTERS[i];
                double value = slot(status, indices, name);
                require(value >= 0);
                report.append('"').append(name).append("\":").append(value);
            }
            report.append("}}");
            System.out.println(report);
        } catch (Throwable error) {
            // Do not emit exception messages/stacks: file and native errors can embed input paths.
            System.err.println("DATALUX_NATIVE_FRAME_PROFILE_ERROR stage=" + stage +
                    " type=" + error.getClass().getSimpleName());
            if (error instanceof UnsatisfiedLinkError) {
                String message = error.getMessage();
                System.err.println("native_load_category=" +
                        (message != null && message.contains("Permission denied") ? "permission_denied" :
                         message != null && message.contains("not accessible") ? "namespace_denied" :
                         message != null && message.contains("writable") ? "writable_code_rejected" :
                         message != null && message.contains("not found") ? "library_not_found" : "other_link_error"));
            }
            System.exit(2);
        }
    }
}
