package com.alexbatalov.fallout2ce;


import org.junit.Rule;
import org.junit.Test;
import org.junit.Assume;
import org.junit.rules.TemporaryFolder;

import java.io.BufferedReader;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.util.Arrays;
import java.util.Collections;
import java.util.Locale;
import java.util.concurrent.atomic.AtomicReference;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

import static org.junit.Assert.*;

public class HdPacksTest {
    @Rule public TemporaryFolder tmp = new TemporaryFolder();
    private static final HdPacks.Progress NOOP = (p, phase, n, total) -> { };

    private static String sha(byte[] bytes) throws Exception {
        byte[] digest = MessageDigest.getInstance("SHA-256").digest(bytes);
        StringBuilder s = new StringBuilder();
        for (byte b : digest) s.append(String.format("%02x", b & 255));
        return s.toString();
    }

    private static byte[] zip(String... names) throws Exception {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        try (ZipOutputStream z = new ZipOutputStream(out)) {
            for (String name : names) {
                z.putNextEntry(new ZipEntry(name));
                z.write(("content for " + name).getBytes(StandardCharsets.UTF_8));
                z.closeEntry();
            }
        }
        return out.toByteArray();
    }

    private static String manifest(byte[] payload, String url) throws Exception {
        return "{\"packs\":[{\"name\":\"arroyo-hd\",\"file\":\"arroyo-hd.zip\","
            + "\"covers\":\"Arroyo village\",\"frames\":497,\"size\":" + payload.length
            + ",\"sha256\":\"" + sha(payload) + "\",\"url\":\"" + url + "\"}]}";
    }

    private static HdPacks.Pack pack(byte[] payload, String url) throws Exception {
        return HdPacks.parseManifest(manifest(payload, url)).get(0);
    }

    private File source(byte[] payload) throws Exception {
        File file = File.createTempFile("hd-pack-", ".zip", tmp.getRoot());
        Files.write(file.toPath(), payload);
        return file;
    }

    private static void expectBad(String json) throws Exception {
        try {
            HdPacks.parseManifest(json);
            fail("invalid manifest accepted");
        } catch (IOException expected) { /* expected */ }
    }

    @Test public void parsePublishedManifestAndRejectInvalidEntries() throws Exception {
        byte[] payload = zip("hd/art/tiles/a.png");
        String valid = manifest(payload, "https://github.com/Linutesto/wasteland-hd-assets/releases/download/a/arroyo-hd.zip");
        HdPacks.Pack pack = HdPacks.parseManifest(valid).get(0);
        assertEquals("Arroyo village", pack.covers);
        assertEquals(payload.length, pack.size);
        assertEquals(64, pack.sha256.length());
        expectBad(valid.replace("arroyo-hd.zip", "../bad.zip"));
        expectBad(valid.replace("\"size\":" + payload.length, "\"size\":-1"));
        expectBad(valid.replace("\"sha256\":\"" + sha(payload), "\"sha256\":\"bad"));
        expectBad("{\"packs\":[" + valid.substring(valid.indexOf('[') + 1, valid.lastIndexOf(']')) + ","
            + valid.substring(valid.indexOf('[') + 1, valid.lastIndexOf(']')) + "]}");
        expectBad("{\"packs\":[{\"name\":\"../escape\"}]}");
    }

    @Test public void installedStateDetectsNewChangedAndCurrentPacks() throws Exception {
        File game = tmp.newFolder("game");
        byte[] payload = zip("hd/art/tiles/a.png");
        HdPacks.Pack pack = pack(payload, source(payload).toURI().toURL().toString());
        assertEquals(1, HdPacks.pending(Collections.singletonList(pack), game).size());
        HdPacks.install(game, pack, NOOP);
        assertTrue(HdPacks.pending(Collections.singletonList(pack), game).isEmpty());
        assertEquals(pack.sha256, HdPacks.installed(game).get(pack.name));
        byte[] different = zip("hd/art/tiles/b.png");
        assertEquals(1, HdPacks.pending(Collections.singletonList(
            pack(different, source(different).toURI().toURL().toString())), game).size());
    }

    @Test public void resumesWithHttpRange() throws Exception {
        byte[] payload = zip("hd/art/tiles/a.png");
        File game = tmp.newFolder("resume");
        AtomicReference<String> range = new AtomicReference<>();
        // A one-request HTTP server on a plain socket: Android unit tests
        // compile against android.jar, which has no com.sun.net.httpserver.
        ServerSocket server;
        try {
            server = new ServerSocket(0, 1, InetAddress.getByName("127.0.0.1"));
        } catch (IOException blocked) {
            // Some CI sandboxes disallow even loopback sockets.
            Assume.assumeNoException("loopback sockets unavailable", blocked);
            return;
        }
        Thread responder = new Thread(() -> {
            try (Socket socket = server.accept()) {
                BufferedReader in = new BufferedReader(new InputStreamReader(socket.getInputStream(), StandardCharsets.ISO_8859_1));
                String line;
                while ((line = in.readLine()) != null && !line.isEmpty())
                    if (line.toLowerCase(Locale.ROOT).startsWith("range:")) range.set(line.substring(6).trim());
                String requested = range.get();
                int offset = requested == null ? 0 : Integer.parseInt(requested.replace("bytes=", "").replace("-", ""));
                StringBuilder head = new StringBuilder(offset != 0 ? "HTTP/1.1 206 Partial Content\r\n" : "HTTP/1.1 200 OK\r\n");
                if (offset != 0) head.append("Content-Range: bytes ").append(offset).append('-').append(payload.length - 1).append('/').append(payload.length).append("\r\n");
                head.append("Content-Length: ").append(payload.length - offset).append("\r\nConnection: close\r\n\r\n");
                OutputStream out = socket.getOutputStream();
                out.write(head.toString().getBytes(StandardCharsets.ISO_8859_1));
                out.write(payload, offset, payload.length - offset);
                out.flush();
            } catch (IOException ignored) {
                // The assertions below report what went wrong.
            }
        });
        responder.start();
        try {
            HdPacks.Pack pack = pack(payload, "http://127.0.0.1:" + server.getLocalPort() + "/pack");
            File partial = new File(game, ".hd-" + pack.name + "-" + pack.sha256 + ".part");
            Files.write(partial.toPath(), Arrays.copyOf(payload, payload.length / 2));
            HdPacks.install(game, pack, NOOP);
            assertEquals("bytes=" + payload.length / 2 + "-", range.get());
            assertTrue(new File(game, "data/hd/art/tiles/a.png").isFile());
            assertFalse(partial.exists());
        } finally {
            server.close();
            responder.join(5000);
        }
    }

    @Test public void localFileResumeKeepsAlreadyDownloadedBytes() throws Exception {
        byte[] payload = zip("hd/art/tiles/a.png");
        File game = tmp.newFolder("local-resume");
        HdPacks.Pack pack = pack(payload, source(payload).toURI().toURL().toString());
        File partial = new File(game, ".hd-" + pack.name + "-" + pack.sha256 + ".part");
        int initial = payload.length / 2;
        Files.write(partial.toPath(), Arrays.copyOf(payload, initial));
        AtomicReference<Long> firstOffset = new AtomicReference<>();
        HdPacks.install(game, pack, (p, phase, current, total) -> {
            if (phase.equals("download") && firstOffset.get() == null) firstOffset.set(current);
        });
        assertEquals(Long.valueOf(initial), firstOffset.get());
        assertTrue(new File(game, "data/hd/art/tiles/a.png").isFile());
    }

    @Test public void badChecksumDoesNotTouchData() throws Exception {
        File game = tmp.newFolder("checksum");
        byte[] bytes = zip("hd/art/tiles/a.png");
        File file = source(bytes);
        HdPacks.Pack original = pack(bytes, file.toURI().toURL().toString());
        HdPacks.Pack bad = new HdPacks.Pack(original.name, original.file, original.covers,
            original.url, original.size, new String(new char[64]).replace('\0', '0'));
        try { HdPacks.install(game, bad, NOOP); fail("accepted damaged payload"); }
        catch (IOException expected) { assertTrue(expected.getMessage().contains("SHA-256")); }
        assertFalse(new File(game, "data").exists());
    }

    @Test public void zipSlipAndOutsideHdAreRejected() throws Exception {
        for (String path : new String[]{"hd/../other.png", "hd/art/../../other.png",
                "/hd/art/tiles/a.png", "art/tiles/a.png", "hd\\art\\a.png"}) {
            File game = tmp.newFolder("blocked" + Math.abs(path.hashCode()));
            byte[] bytes = zip(path);
            HdPacks.Pack p = pack(bytes, source(bytes).toURI().toURL().toString());
            try { HdPacks.install(game, p, NOOP); fail("accepted " + path); }
            catch (IOException expected) { /* expected */ }
            assertFalse(new File(game, "data").exists());
        }
    }

    @Test public void rejectsUnixSymlinkEntries() throws Exception {
        byte[] bytes = zip("hd/art/tiles/link.png");
        // Change the central directory external Unix mode to S_IFLNK.
        for (int i = 0; i < bytes.length - 46; i++) {
            if ((bytes[i] & 255) == 0x50 && (bytes[i + 1] & 255) == 0x4b
                    && (bytes[i + 2] & 255) == 1 && (bytes[i + 3] & 255) == 2) {
                int mode = 0120777;
                bytes[i + 40] = (byte) (mode & 255);
                bytes[i + 41] = (byte) (mode >>> 8);
                break;
            }
        }
        File game = tmp.newFolder("symlink");
        HdPacks.Pack p = pack(bytes, source(bytes).toURI().toURL().toString());
        try { HdPacks.install(game, p, NOOP); fail("accepted link"); }
        catch (IOException expected) { assertTrue(expected.getMessage().contains("symlink")); }
        assertFalse(new File(game, "data").exists());
    }

    @Test public void onlyHdFilesArePublishedAndUnlistedArtIsPreserved() throws Exception {
        File game = tmp.newFolder("keep");
        File unlisted = new File(game, "data/hd/art/tiles/keep.png");
        assertTrue(unlisted.getParentFile().mkdirs());
        Files.write(unlisted.toPath(), new byte[]{1, 2, 3});
        byte[] payload = zip("hd/art/tiles/a.png", "hd/art/critters/b.png");
        HdPacks.install(game, pack(payload, source(payload).toURI().toURL().toString()), NOOP);
        assertArrayEquals(new byte[]{1, 2, 3}, Files.readAllBytes(unlisted.toPath()));
        assertTrue(new File(game, "data/hd/art/tiles/a.png").isFile());
        assertTrue(new File(game, "data/hd/art/critters/b.png").isFile());
        assertEquals(1, HdPacks.installed(game).size());
    }
}
