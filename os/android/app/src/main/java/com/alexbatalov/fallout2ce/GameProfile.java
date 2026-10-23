package com.alexbatalov.fallout2ce;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;

/** The native engine and Android save UI must use the same profile. */
final class GameProfile {
    static String activeName(File root) {
        if (root == null) throw new IllegalStateException("Game storage is unavailable");
        File marker = new File(root, "active-profile.txt");
        if (!marker.exists()) return "";
        try {
            if (marker.length() > 66) throw new IOException("profile name is too long");
            String name = new String(GameSaves.readAll(marker), StandardCharsets.UTF_8).replaceFirst("\\r?\\n$", "");
            if (!name.matches("[A-Za-z0-9_-]{1,64}")) throw new IOException("invalid profile name");
            File selected = new File(root, "profiles/" + name);
            if (!selected.isDirectory() || !new File(selected, "fallout2.cfg").isFile()) {
                throw new IOException("profile is missing: " + name);
            }
            return name;
        } catch (IOException e) {
            throw new IllegalStateException("Cannot open game profile: " + e.getMessage(), e);
        }
    }

    static File gameDirectory(File root) {
        String name = activeName(root);
        return name.isEmpty() ? root : new File(root, "profiles/" + name);
    }

    static File privateDirectory(File root, File internal) {
        String name = activeName(root);
        File data = name.isEmpty() ? internal : new File(internal, "profiles/" + name);
        if (!data.isDirectory() && !data.mkdirs()) throw new IllegalStateException("Cannot create profile metadata");
        return data;
    }
}
