package dev.datalux.testing;

import java.lang.reflect.Method;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.Arrays;
import java.util.Random;

/** Runs against the actual app DEX and native library, without credentials or camera input. */
public final class NativeRuntimeSmoke {
    private static Object call(String owner, String name, Object... args) throws Exception {
        for (Method method : Class.forName(owner).getDeclaredMethods()) {
            if (method.getName().equals(name) && method.getParameterCount() == args.length) {
                return method.invoke(null, args);
            }
        }
        throw new NoSuchMethodException(owner + "." + name);
    }

    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }

    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        String codec = "dev.fileflow.secure.NativeCodec";
        String tx = "dev.fileflow.tx.NativeTransmitter";
        String rx = "dev.fileflow.rx.NativeReceiver";
        int rounds = 0;
        int frames = 0;
        int[] sizes = {0, 1, 4095, 4096, 4097, 16383, 16384, 16385, 65537};
        for (int size : sizes) {
            for (int pattern = 0; pattern < 2; pattern++) {
                byte[] data = new byte[size];
                if (pattern == 0) Arrays.fill(data, (byte) 65);
                else new Random(7201L + size).nextBytes(data);
                for (int level : new int[]{1, 9}) {
                    byte[] compressed = (byte[]) call(codec, "zstdCompress", data, level, false);
                    check(compressed != null, "compression failed at " + size);
                    // Exercise a non-zero JNI slice offset as well as page-boundary allocations.
                    byte[] wrapped = new byte[compressed.length + 10];
                    System.arraycopy(compressed, 0, wrapped, 5, compressed.length);
                    byte[] restored = (byte[]) call(codec, "zstdDecompress", wrapped, 5, compressed.length, size);
                    check(Arrays.equals(data, restored), "round trip differs at " + size);
                    check(call(codec, "zstdDecompress", compressed, 0, compressed.length, size + 1) == null,
                            "wrong output length accepted");
                    check(call(codec, "zstdDecompress", compressed, -1, compressed.length, size) == null,
                            "negative input offset accepted");
                    rounds++;
                }
            }
        }
        byte[] data = new byte[16385];
        new Random(173).nextBytes(data);
        byte[] longCompressed = (byte[]) call(codec, "zstdCompress", data, 3, true);
        check(longCompressed != null && Arrays.equals(data,
                (byte[]) call(codec, "zstdDecompress", longCompressed, 0, longCompressed.length, data.length)),
                "long-window compression round trip failed");
        rounds++;
        byte[] digest = MessageDigest.getInstance("SHA-256").digest(data);
        StringBuilder expectedHash = new StringBuilder();
        for (byte b : digest) expectedHash.append(String.format("%02x", b & 255));
        for (int colours : new int[]{0, 4, 8}) {
            long handle = (long) call(tx, "openFile", 120, 260, 32, data,
                    "synthetic-runtime.bin".getBytes(StandardCharsets.UTF_8), colours, 0L, 0, 0);
            check(handle != 0, "transmitter not opened");
            try {
                check((long) call(tx, "fileSize", handle) == data.length, "payload size differs");
                check(expectedHash.toString().equals(call(tx, "payloadSha256", handle)), "payload hash differs");
                check((int) call(tx, "symbolBytes", handle) > 0, "invalid symbol size");
                check((int) call(tx, "payloadCells", handle) > 0, "invalid payload cells");
                check((int) call(tx, "nextFrame", handle, ByteBuffer.allocateDirect(1)) != 0,
                        "undersized direct buffer accepted");
                check((int) call(tx, "nextFrame", handle, ByteBuffer.allocate(120 * 260)) != 0,
                        "heap buffer accepted as native direct memory");
                ByteBuffer cells = ByteBuffer.allocateDirect(120 * 260);
                for (int i = 0; i < 32; i++) {
                    check((int) call(tx, "nextFrame", handle, cells) == 0, "frame generation failed");
                    boolean black = false;
                    boolean white = false;
                    for (int j = 0; j < cells.capacity(); j++) {
                        int value = cells.get(j) & 255;
                        black |= value == 0;
                        white |= value == 255;
                        // Structural/header cells retain M0's 0/255 luminance values;
                        // colour payload cells encode RGB in their low three bits.
                        boolean valid = value == 0 || value == 255;
                        if (colours == 4) valid |= value == 3 || value == 5 || value == 6;
                        if (colours == 8) valid |= value <= 7;
                        check(valid, "invalid cell value");
                    }
                    check(black && white, "frame contains no black/white structure");
                    frames++;
                }
            } finally {
                call(tx, "close", handle);
            }
        }
        long receiver = (long) call(rx, "open", 120, 260, 1_000_000L, 1, 0.0);
        check(receiver != 0, "receiver not opened");
        try {
            int slots = (int) call(rx, "slotCount");
            double[] status = new double[slots];
            Arrays.fill(status, Double.NaN);
            ByteBuffer blank = ByteBuffer.allocateDirect(640 * 480);
            call(rx, "ingest", receiver, blank, 640, 480, 640, null, null, 0, 0, 1L, 0, status);
            check(Double.isFinite(status[0]) && status[0] >= 0,
                    "receiver rejected valid blank-frame marshaling");
            check(call(rx, "fileBytes", receiver) == null, "blank frame produced a file");
        } finally {
            call(rx, "close", receiver);
        }
        System.out.println("DATALUX_NATIVE_SMOKE_PASS compression_roundtrips=" + rounds + " generated_frames=" + frames);
    }
}
