package com.alexbatalov.fallout2ce;

import org.junit.Test;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import static org.junit.Assert.*;

public class GameProfileTest {
    @Test public void profilesSeparateGameAndMetadata() throws Exception {
        File root = Files.createTempDirectory("profile-game").toFile();
        File internal = Files.createTempDirectory("profile-private").toFile();
        assertEquals(root, GameProfile.gameDirectory(root));
        assertEquals(internal, GameProfile.privateDirectory(root, internal));
        for (String name : new String[]{"rpu-plus", "ettu"}) {
            File game = new File(root, "profiles/" + name);
            assertTrue(game.mkdirs());
            assertTrue(new File(game, "fallout2.cfg").createNewFile());
            Files.write(new File(root, "active-profile.txt").toPath(), (name + "\n").getBytes(StandardCharsets.UTF_8));
            assertEquals(game, GameProfile.gameDirectory(root));
            assertEquals(new File(internal, "profiles/" + name), GameProfile.privateDirectory(root, internal));
        }
    }

    @Test public void invalidOrMissingSelectionNeverFallsBackToVanilla() throws Exception {
        File root = Files.createTempDirectory("profile-invalid").toFile();
        for (String name : new String[]{"../vanilla", "missing", "", " padded ", "bad\nextra", "a".repeat(65)}) {
            Files.write(new File(root, "active-profile.txt").toPath(), name.getBytes(StandardCharsets.UTF_8));
            try {
                GameProfile.gameDirectory(root);
                fail("accepted " + name);
            } catch (IllegalStateException expected) { }
        }
    }
}
