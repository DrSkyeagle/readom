import java.io.File;
import java.io.BufferedReader;
import java.io.FileReader;
import java.io.RandomAccessFile;
import java.math.BigInteger;
import java.nio.MappedByteBuffer;
import java.nio.channels.FileChannel;
import java.security.MessageDigest;
import java.util.Arrays;
import java.util.HashSet;
import java.util.Set;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicReference;
import java.util.zip.Deflater;
import java.util.zip.Inflater;

/** Core-library/GC runtime test. No Android services, accounts or network. */
public final class ArtSmoke {
    private static int checks;
    private static volatile long sink;

    private static void check(boolean ok, String name) {
        if (!ok) throw new AssertionError(name);
        checks++;
        System.out.println("ART_CHECK " + name + " PASS");
    }

    public static void main(String[] args) throws Exception {
        check(args.length == 1, "explicit-private-output-directory");
        check("Dalvik".equals(System.getProperty("java.vm.name")), "android-runtime");
        System.out.println("ART_VM_VERSION " + System.getProperty("java.vm.version"));
        Set<String> mappedImages = new HashSet<String>();
        BufferedReader maps = new BufferedReader(new FileReader("/proc/self/maps"));
        String line;
        while ((line = maps.readLine()) != null) {
            int slash = line.indexOf('/');
            if (slash < 0) continue;
            String path = line.substring(slash);
            if (path.endsWith(".art") && (path.contains("/boot") || path.contains("@boot")))
                mappedImages.add(path);
        }
        maps.close();
        int expectedImages = 0;
        File[] images = new File("/system/framework/arm").listFiles();
        if (images != null) {
            for (File image : images)
                if (image.getName().startsWith("boot") && image.getName().endsWith(".art"))
                    expectedImages++;
        }
        check(expectedImages > 0 && mappedImages.size() == expectedImages, "boot-images-mapped");
        String text = "阅读 \ud83d\udcd6";
        check(text.equals(new String(text.getBytes("UTF-8"), "UTF-8")), "unicode-roundtrip");
        byte[] hash = MessageDigest.getInstance("SHA-256").digest("abc".getBytes("UTF-8"));
        StringBuilder hex = new StringBuilder();
        for (byte b : hash) hex.append(String.format("%02x", b & 255));
        check(hex.toString().equals("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "sha256-known-answer");
        check(BigInteger.valueOf(2).pow(100).toString().equals("1267650600228229401496703205376"), "big-integer");

        byte[] original = new byte[16384];
        for (int i = 0; i < original.length; i++) original[i] = (byte)(i * 17);
        Deflater deflater = new Deflater();
        deflater.setInput(original);
        deflater.finish();
        byte[] compressed = new byte[original.length + 1024];
        int length = deflater.deflate(compressed);
        check(deflater.finished(), "native-deflate");
        deflater.end();
        Inflater inflater = new Inflater();
        inflater.setInput(compressed, 0, length);
        byte[] restored = new byte[original.length];
        int inflated = inflater.inflate(restored);
        check(inflated == original.length && inflater.finished() && Arrays.equals(original, restored), "native-inflate");
        inflater.end();

        File file = new File(args[0], "mapped-file");
        check(file.createNewFile(), "create-private-file");
        RandomAccessFile stream = new RandomAccessFile(file, "rw");
        stream.setLength(4096);
        MappedByteBuffer mapping = stream.getChannel().map(FileChannel.MapMode.READ_WRITE, 0, 4096);
        mapping.putInt(0, 0x13572468);
        mapping.force();
        stream.seek(0);
        check(stream.readInt() == 0x13572468, "shared-file-map-and-force");
        stream.close();
        check(file.delete(), "delete-private-file");

        final CountDownLatch ready = new CountDownLatch(4);
        final CountDownLatch go = new CountDownLatch(1);
        final CountDownLatch done = new CountDownLatch(4);
        final AtomicReference<Throwable> failure = new AtomicReference<Throwable>();
        for (int t = 0; t < 4; t++) {
            new Thread(new Runnable() {
                public void run() {
                    ready.countDown();
                    try {
                        go.await();
                        long total = 0;
                        for (int i = 0; i < 12000; i++) {
                            byte[] block = new byte[2048];
                            block[i & 2047] = (byte)i;
                            total += block[i & 2047];
                            if (i % 3000 == 0) System.gc();
                        }
                        sink = total;
                    } catch (Throwable error) {
                        failure.compareAndSet(null, error);
                    } finally {
                        done.countDown();
                    }
                }
            }, "art-smoke-" + t).start();
        }
        ready.await();
        go.countDown();
        done.await();
        if (failure.get() != null) throw new AssertionError(failure.get());
        check(true, "four-thread-allocation-and-gc");
        System.out.println("PW3_AOSP_ART_SMOKE_PASS checks=" + checks);
    }
}
