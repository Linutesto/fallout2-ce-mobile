package com.alexbatalov.fallout2ce;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.RandomAccessFile;
import java.net.HttpURLConnection;
import java.net.URL;
import java.net.URLConnection;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Enumeration;
import java.util.HashMap;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import java.util.zip.CRC32;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/** Pure Java HD-pack updater. All paths passed here are within the active game profile. */
final class HdPacks {
    static final String MANIFEST_URL =
        "https://raw.githubusercontent.com/Linutesto/wasteland-hd-assets/main/packs.json";
    private static final long MAX_PACK_BYTES = 16L * 1024 * 1024 * 1024;
    private static final int MAX_MANIFEST_BYTES = 2 * 1024 * 1024;
    private static final Pattern NAME = Pattern.compile("[A-Za-z0-9][A-Za-z0-9_.-]{0,63}");
    private static final Pattern FILE_NAME = Pattern.compile("[A-Za-z0-9][A-Za-z0-9_.-]*\\.zip");
    private static final Pattern HASH = Pattern.compile("[a-fA-F0-9]{64}");
    private static final Pattern CONTENT_RANGE = Pattern.compile("bytes (\\d+)-(\\d+)/(\\d+)");

    private HdPacks() { }

    static final class Pack {
        final String name, file, covers, url, sha256;
        final long size;
        Pack(String name, String file, String covers, String url, long size, String sha256) {
            this.name = name;
            this.file = file;
            this.covers = covers;
            this.url = url;
            this.size = size;
            this.sha256 = sha256.toLowerCase(java.util.Locale.ROOT);
        }
    }

    interface Progress {
        // phase = download, verify, extract or install; callbacks run on the worker thread.
        void update(Pack pack, String phase, long current, long total) throws IOException;
    }

    static List<Pack> fetchManifest() throws IOException {
        URLConnection conn = new URL(MANIFEST_URL).openConnection();
        conn.setConnectTimeout(4000);
        conn.setReadTimeout(4000);
        try (InputStream in = conn.getInputStream()) {
            return parseManifest(new String(readLimited(in, MAX_MANIFEST_BYTES), StandardCharsets.UTF_8));
        }
    }

    static List<Pack> parseManifest(String json) throws IOException {
        Map<String, Object> root = object(new Json(json).parse());
        Object packsValue = root.get("packs");
        if (!(packsValue instanceof List)) throw new IOException("missing packs array");
        List<Pack> packs = new ArrayList<>();
        Set<String> names = new HashSet<>();
        for (Object value : (List<?>) packsValue) {
            Map<String, Object> fields = object(value);
            String name = string(fields.get("name"));
            String file = string(fields.get("file"));
            String hash = string(fields.get("sha256"));
            String url = string(fields.get("url"));
            String covers = string(fields.get("covers"));
            Object sizeValue = fields.get("size");
            if (!NAME.matcher(name).matches() || name.equals(".") || name.equals("..")
                    || !FILE_NAME.matcher(file).matches() || !HASH.matcher(hash).matches()
                    || !(sizeValue instanceof Long) || (Long) sizeValue <= 0
                    || (Long) sizeValue >= MAX_PACK_BYTES || covers.length() > 300
                    || !names.add(name)) {
                throw new IOException("invalid or duplicate HD pack: " + name);
            }
            URL source = new URL(url);
            if (!(source.getProtocol().equals("https") || source.getProtocol().equals("http")
                    || source.getProtocol().equals("file"))) {
                throw new IOException("unsupported pack source");
            }
            packs.add(new Pack(name, file, covers, url, (Long) sizeValue, hash));
        }
        return packs;
    }

    static List<Pack> pending(List<Pack> packs, File game) throws IOException {
        Map<String, String> installed = installed(game);
        List<Pack> result = new ArrayList<>();
        for (Pack pack : packs) {
            if (!pack.sha256.equalsIgnoreCase(installed.get(pack.name))) result.add(pack);
        }
        return result;
    }

    static Map<String, String> installed(File game) throws IOException {
        File state = new File(game, "data/hd/.installed-packs.json");
        if (!state.isFile()) return new LinkedHashMap<>();
        if (state.length() > MAX_MANIFEST_BYTES) throw new IOException("HD state too large");
        Map<String, Object> raw;
        try (InputStream in = new FileInputStream(state)) {
            raw = object(new Json(new String(readLimited(in, MAX_MANIFEST_BYTES),
                StandardCharsets.UTF_8)).parse());
        }
        Map<String, String> result = new LinkedHashMap<>();
        for (Map.Entry<String, Object> pair : raw.entrySet()) {
            String value = string(pair.getValue());
            if (!NAME.matcher(pair.getKey()).matches() || !HASH.matcher(value).matches()) {
                throw new IOException("invalid HD installed state");
            }
            result.put(pair.getKey(), value);
        }
        return result;
    }

    static void install(File game, Pack pack, Progress progress) throws IOException {
        if (!game.isDirectory()) throw new IOException("game folder missing");
        File partial = new File(game, ".hd-" + pack.name + "-" + pack.sha256 + ".part");
        download(pack, partial, progress);
        try {
            progress.update(pack, "verify", 0, pack.size);
            if (!pack.sha256.equals(sha256(partial))) {
                if (!partial.delete()) partial.deleteOnExit();
                throw new IOException("HD download SHA-256 mismatch: " + pack.name);
            }
            extract(game, pack, partial, progress);
            Map<String, String> state = installed(game);
            state.put(pack.name, pack.sha256);
            writeInstalled(game, state);
            if (!partial.delete()) partial.deleteOnExit();
        } catch (IOException e) {
            // Keep complete verified downloads on installation errors, and partial downloads
            // on network errors; a damaged full download was removed above.
            throw e;
        }
    }

    static void download(Pack pack, File partial, Progress progress) throws IOException {
        long offset = partial.isFile() ? partial.length() : 0;
        if (offset > pack.size) {
            if (!partial.delete()) throw new IOException("cannot discard oversized partial");
            offset = 0;
        }
        if (offset == pack.size) {
            progress.update(pack, "download", offset, pack.size);
            return;
        }
        URLConnection conn = new URL(pack.url).openConnection();
        conn.setConnectTimeout(15000);
        conn.setReadTimeout(20000);
        if (offset > 0 && conn instanceof HttpURLConnection) {
            conn.setRequestProperty("Range", "bytes=" + offset + "-");
        }
        if (conn instanceof HttpURLConnection) {
            ((HttpURLConnection) conn).setInstanceFollowRedirects(true);
        }
        try {
            try (InputStream in = conn.getInputStream()) {
                boolean resume = false;
                if (offset > 0 && conn instanceof HttpURLConnection
                        && ((HttpURLConnection) conn).getResponseCode() == 206) {
                    Matcher m = CONTENT_RANGE.matcher(conn.getHeaderField("Content-Range") == null
                        ? "" : conn.getHeaderField("Content-Range"));
                    if (!m.matches() || Long.parseLong(m.group(1)) != offset
                            || Long.parseLong(m.group(3)) != pack.size
                            || Long.parseLong(m.group(2)) < offset) {
                        throw new IOException("invalid HTTP Content-Range");
                    }
                    resume = true;
                } else if (offset > 0 && conn instanceof HttpURLConnection
                        && ((HttpURLConnection) conn).getResponseCode() != 200) {
                    throw new IOException("server rejected HTTP resume");
                }
                // A local file source is useful for offline tests: seek by consuming
                // bytes. Actual remote HTTP resumes always use a verified 206 Range.
                if (offset > 0 && conn.getURL().getProtocol().equals("file")) {
                    long skipped = 0;
                    while (skipped < offset) {
                        long n = in.skip(offset - skipped);
                        if (n <= 0) throw new IOException("local resume source too short");
                        skipped += n;
                    }
                    resume = true;
                }
                // Servers ignoring Range download from byte zero.
                if (!resume) offset = 0;
                try (OutputStream out = new FileOutputStream(partial, resume)) {
                    byte[] buffer = new byte[65536];
                    int n;
                    long written = offset;
                    progress.update(pack, "download", written, pack.size);
                    while ((n = in.read(buffer)) != -1) {
                        written += n;
                        if (written > pack.size) throw new IOException("HD download larger than manifest");
                        out.write(buffer, 0, n);
                        progress.update(pack, "download", written, pack.size);
                    }
                    if (written != pack.size) throw new IOException("HD download incomplete");
                }
            }
        } finally {
            if (conn instanceof HttpURLConnection) ((HttpURLConnection) conn).disconnect();
        }
    }

    private static void extract(File game, Pack pack, File archive, Progress progress) throws IOException {
        File stage = File.createTempFile(".hd-stage-", "", game);
        if (!stage.delete() || !stage.mkdir()) throw new IOException("cannot create staging directory");
        try (ZipFile zip = new ZipFile(archive)) {
            List<? extends ZipEntry> entries = Collections.list(zip.entries());
            Map<String, Integer> unixTypes = centralDirectoryTypes(archive);
            Set<String> seen = new HashSet<>();
            long expanded = 0;
            long maxExpanded = Math.max(128L * 1024 * 1024, pack.size * 100);
            if (entries.size() != unixTypes.size()) throw new IOException("ZIP directory mismatch");
            for (ZipEntry entry : entries) {
                String name = entry.getName();
                String[] parts = name.replaceFirst("/$", "").split("/", -1);
                if (parts.length < 2 || !parts[0].equals("hd") || name.contains("\\")
                        || !seen.add(name) || entry.getSize() < 0
                        || entry.getSize() > maxExpanded - expanded) {
                    throw new IOException("unsafe HD ZIP entry: " + name);
                }
                for (String p : parts) {
                    if (p.isEmpty() || p.equals(".") || p.equals("..")) {
                        throw new IOException("unsafe HD ZIP path: " + name);
                    }
                }
                Integer type = unixTypes.get(name);
                if (type == null || !(type == 0 || type == (entry.isDirectory() ? 0040000 : 0100000))) {
                    throw new IOException("unsupported HD ZIP entry (possibly symlink): " + name);
                }
                expanded += entry.getSize();
            }
            // No game-file changes until the entire verified archive is safely staged.
            long extracted = 0;
            for (ZipEntry entry : entries) {
                File destination = new File(stage, entry.getName());
                if (entry.isDirectory()) {
                    if (!destination.isDirectory() && !destination.mkdirs()) throw new IOException("mkdir stage failed");
                    continue;
                }
                if (!destination.getParentFile().isDirectory() && !destination.getParentFile().mkdirs()) {
                    throw new IOException("mkdir stage parent failed");
                }
                CRC32 crc = new CRC32();
                long copied = 0;
                try (InputStream in = zip.getInputStream(entry);
                        OutputStream out = new FileOutputStream(destination)) {
                    byte[] buf = new byte[65536];
                    int n;
                    while ((n = in.read(buf)) != -1) {
                        copied += n;
                        if (copied > entry.getSize()) throw new IOException("HD ZIP size mismatch");
                        out.write(buf, 0, n);
                        crc.update(buf, 0, n);
                        progress.update(pack, "extract", extracted + copied, expanded);
                    }
                }
                if (copied != entry.getSize() || crc.getValue() != entry.getCrc()) {
                    throw new IOException("HD ZIP CRC or size mismatch: " + entry.getName());
                }
                extracted += copied;
            }
            progress.update(pack, "install", 0, expanded);
            File data = new File(game, "data");
            for (ZipEntry entry : entries) {
                if (entry.isDirectory()) continue;
                File destination = new File(data, entry.getName());
                File parent = destination.getParentFile();
                // Guard even against an existing symlink in the installed tree.
                // Check before mkdirs, which might itself follow a hostile symlink.
                if (!destination.getCanonicalPath().startsWith(data.getCanonicalPath() + File.separator)) {
                    throw new IOException("HD destination escapes data: " + entry.getName());
                }
                if (!parent.isDirectory() && !parent.mkdirs()) throw new IOException("mkdir data failed");
                if (!new File(stage, entry.getName()).renameTo(destination)) {
                    throw new IOException("atomic HD file rename failed: " + entry.getName());
                }
            }
        } finally {
            removeTree(stage);
        }
    }

    // ZipFile hides external_attr. Read the classic central directory to reject UNIX
    // symlinks/special files before extraction; ZIP64 is rejected instead of guessed.
    private static Map<String, Integer> centralDirectoryTypes(File archive) throws IOException {
        Map<String, Integer> types = new HashMap<>();
        try (RandomAccessFile raf = new RandomAccessFile(archive, "r")) {
            long length = raf.length();
            int tail = (int) Math.min(length, 65557);
            byte[] end = new byte[tail];
            raf.seek(length - tail);
            raf.readFully(end);
            int eocd = -1;
            for (int i = tail - 22; i >= 0; i--) {
                if (u32(end, i) == 0x06054b50L && i + 22 + u16(end, i + 20) == tail) {
                    eocd = i;
                    break;
                }
            }
            if (eocd < 0 || u16(end, eocd + 4) != 0 || u16(end, eocd + 6) != 0
                    || u16(end, eocd + 8) != u16(end, eocd + 10)) {
                throw new IOException("unsupported HD ZIP directory");
            }
            int count = u16(end, eocd + 10);
            long dirSize = u32(end, eocd + 12);
            long dirOffset = u32(end, eocd + 16);
            if (count == 65535 || dirSize == 0xffffffffL || dirOffset == 0xffffffffL
                    || dirOffset + dirSize > length - tail + eocd) {
                throw new IOException("unsupported HD ZIP64 or invalid directory");
            }
            raf.seek(dirOffset);
            for (int i = 0; i < count; i++) {
                byte[] header = new byte[46];
                raf.readFully(header);
                if (u32(header, 0) != 0x02014b50L) throw new IOException("bad HD ZIP central header");
                int nameLength = u16(header, 28);
                int extraLength = u16(header, 30);
                int commentLength = u16(header, 32);
                if (u16(header, 8) != 0 && (u16(header, 8) & 1) != 0) {
                    throw new IOException("encrypted HD ZIP not supported");
                }
                byte[] nameBytes = new byte[nameLength];
                raf.readFully(nameBytes);
                String name = new String(nameBytes, StandardCharsets.UTF_8);
                int kind = (int) ((u32(header, 38) >>> 16) & 0170000);
                if (types.put(name, kind) != null) throw new IOException("duplicate ZIP entry");
                raf.seek(raf.getFilePointer() + extraLength + commentLength);
            }
        }
        return types;
    }

    private static int u16(byte[] b, int i) {
        return (b[i] & 255) | ((b[i + 1] & 255) << 8);
    }

    private static long u32(byte[] b, int i) {
        return ((long) u16(b, i) | ((long) u16(b, i + 2) << 16)) & 0xffffffffL;
    }

    private static void writeInstalled(File game, Map<String, String> entries) throws IOException {
        File dir = new File(game, "data/hd");
        if (!dir.isDirectory() && !dir.mkdirs()) throw new IOException("cannot create HD state folder");
        File state = new File(dir, ".installed-packs.json");
        File temp = File.createTempFile(".installed-", ".tmp", dir);
        try {
            StringBuilder json = new StringBuilder("{\n");
            for (Map.Entry<String, String> pair : entries.entrySet()) {
                if (json.length() > 2) json.append(",\n");
                json.append("  \"").append(pair.getKey()).append("\": \"")
                    .append(pair.getValue()).append('"');
            }
            json.append("\n}\n");
            try (OutputStream out = new FileOutputStream(temp)) {
                out.write(json.toString().getBytes(StandardCharsets.UTF_8));
                out.flush();
                ((FileOutputStream) out).getFD().sync();
            }
            if (!temp.renameTo(state)) throw new IOException("atomic HD state rename failed");
        } finally {
            if (temp.exists()) temp.delete();
        }
    }

    private static String sha256(File file) throws IOException {
        try {
            MessageDigest md = MessageDigest.getInstance("SHA-256");
            try (InputStream in = new FileInputStream(file)) {
                byte[] buf = new byte[65536];
                int n;
                while ((n = in.read(buf)) != -1) md.update(buf, 0, n);
            }
            StringBuilder hex = new StringBuilder();
            for (byte b : md.digest()) hex.append(String.format(java.util.Locale.ROOT, "%02x", b & 255));
            return hex.toString();
        } catch (NoSuchAlgorithmException e) {
            throw new IOException("SHA-256 unavailable", e);
        }
    }

    private static byte[] readLimited(InputStream in, int limit) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] buf = new byte[8192];
        int n;
        while ((n = in.read(buf)) != -1) {
            if (out.size() + n > limit) throw new IOException("JSON too large");
            out.write(buf, 0, n);
        }
        return out.toByteArray();
    }

    private static void removeTree(File path) {
        File[] children = path.listFiles();
        if (children != null) for (File child : children) removeTree(child);
        path.delete();
    }

    @SuppressWarnings("unchecked")
    private static Map<String, Object> object(Object value) throws IOException {
        if (!(value instanceof Map)) throw new IOException("expected JSON object");
        return (Map<String, Object>) value;
    }

    private static String string(Object value) throws IOException {
        if (!(value instanceof String)) throw new IOException("expected JSON string");
        return (String) value;
    }

    /** Small strict JSON reader so the updater's tests need no Android runtime or dependencies. */
    private static final class Json {
        final String text;
        int pos;
        Json(String text) { this.text = text; }

        Object parse() throws IOException {
            Object value = value();
            whitespace();
            if (pos != text.length()) throw new IOException("trailing JSON");
            return value;
        }

        private Object value() throws IOException {
            whitespace();
            if (pos >= text.length()) throw new IOException("truncated JSON");
            char c = text.charAt(pos);
            if (c == '{') {
                pos++;
                Map<String, Object> map = new LinkedHashMap<>();
                whitespace();
                if (take('}')) return map;
                do {
                    whitespace();
                    String key = quoted();
                    if (!take(':')) throw new IOException("expected colon");
                    Object item = value();
                    if (map.containsKey(key)) throw new IOException("duplicate JSON key");
                    map.put(key, item);
                    whitespace();
                    if (take('}')) return map;
                    if (!take(',')) throw new IOException("expected comma");
                } while (true);
            }
            if (c == '[') {
                pos++;
                List<Object> list = new ArrayList<>();
                whitespace();
                if (take(']')) return list;
                do {
                    list.add(value());
                    whitespace();
                    if (take(']')) return list;
                    if (!take(',')) throw new IOException("expected comma");
                } while (true);
            }
            if (c == '"') return quoted();
            if (c == '-' || (c >= '0' && c <= '9')) {
                int start = pos;
                if (c == '-') pos++;
                if (pos >= text.length() || !Character.isDigit(text.charAt(pos))) throw new IOException("bad number");
                if (text.charAt(pos) == '0') pos++;
                else while (pos < text.length() && text.charAt(pos) >= '0' && text.charAt(pos) <= '9') pos++;
                try { return Long.parseLong(text.substring(start, pos)); }
                catch (NumberFormatException e) { throw new IOException("bad integer", e); }
            }
            for (String literal : new String[]{"true", "false", "null"}) {
                if (text.startsWith(literal, pos)) {
                    pos += literal.length();
                    return literal.equals("null") ? null : Boolean.valueOf(literal);
                }
            }
            throw new IOException("invalid JSON value");
        }

        private String quoted() throws IOException {
            whitespace();
            if (!take('"')) throw new IOException("expected string");
            StringBuilder out = new StringBuilder();
            while (pos < text.length()) {
                char c = text.charAt(pos++);
                if (c == '"') return out.toString();
                if (c < 32) throw new IOException("control character in JSON");
                if (c != '\\') { out.append(c); continue; }
                if (pos >= text.length()) throw new IOException("bad JSON escape");
                char esc = text.charAt(pos++);
                switch (esc) {
                    case '"': case '\\': case '/': out.append(esc); break;
                    case 'b': out.append('\b'); break;
                    case 'f': out.append('\f'); break;
                    case 'n': out.append('\n'); break;
                    case 'r': out.append('\r'); break;
                    case 't': out.append('\t'); break;
                    case 'u':
                        if (pos + 4 > text.length()) throw new IOException("bad JSON unicode");
                        try { out.append((char) Integer.parseInt(text.substring(pos, pos + 4), 16)); }
                        catch (NumberFormatException e) { throw new IOException("bad JSON unicode", e); }
                        pos += 4;
                        break;
                    default: throw new IOException("bad JSON escape");
                }
            }
            throw new IOException("unterminated JSON string");
        }

        private void whitespace() {
            while (pos < text.length() && (text.charAt(pos) == ' ' || text.charAt(pos) == '\n'
                    || text.charAt(pos) == '\r' || text.charAt(pos) == '\t')) pos++;
        }

        private boolean take(char c) {
            whitespace();
            if (pos < text.length() && text.charAt(pos) == c) { pos++; return true; }
            return false;
        }
    }
}
