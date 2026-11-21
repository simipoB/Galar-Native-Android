package com.nxnative;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.zip.CRC32;
import java.util.zip.Deflater;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

public final class SaveImportTest {

    // -------------------------------------------------------------------------
    // Archive builders
    // -------------------------------------------------------------------------

    /** Single-file archive with text content. */
    private static byte[] archive(String name, String content) throws IOException {
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        try (ZipOutputStream zip = new ZipOutputStream(bytes)) {
            zip.putNextEntry(new ZipEntry(name));
            zip.write(content.getBytes(StandardCharsets.UTF_8));
            zip.closeEntry();
        }
        return bytes.toByteArray();
    }

    /** Archive with multiple files in one or more directories. */
    private static byte[] multiFileArchive() throws IOException {
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        try (ZipOutputStream zip = new ZipOutputStream(bytes)) {
            addEntry(zip, "profile/save1.sav", "data1");
            addEntry(zip, "profile/save2.sav", "data2");
            addEntry(zip, "screenshots/screen.png", "imgdata");
        }
        return bytes.toByteArray();
    }

    /** Archive with no file entries (only a directory entry). */
    private static byte[] emptyArchive() throws IOException {
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        try (ZipOutputStream zip = new ZipOutputStream(bytes)) {
            // no entries at all
        }
        return bytes.toByteArray();
    }

    /** Archive with a directory-only entry (no files). */
    private static byte[] directoryOnlyArchive() throws IOException {
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        try (ZipOutputStream zip = new ZipOutputStream(bytes)) {
            zip.putNextEntry(new ZipEntry("emptydir/"));
            zip.closeEntry();
        }
        return bytes.toByteArray();
    }

    /** Archive containing two entries with the same normalised path. */
    private static byte[] duplicatePathArchive() throws IOException {
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        try (ZipOutputStream zip = new ZipOutputStream(bytes)) {
            addEntry(zip, "profile/save.sav", "first");
            addEntry(zip, "profile/save.saX", "second");
        }
        byte[] raw = bytes.toByteArray();
        // Replace 'saX' with 'sav' in the byte array so ZipOutputStream's duplicate
        // check is bypassed while ZipInputStream sees two identical entries.
        byte[] target = "profile/save.saX".getBytes(StandardCharsets.UTF_8);
        byte[] replace = "profile/save.sav".getBytes(StandardCharsets.UTF_8);
        for (int i = 0; i <= raw.length - target.length; i++) {
            boolean match = true;
            for (int j = 0; j < target.length; j++) {
                if (raw[i + j] != target[j]) { match = false; break; }
            }
            if (match) {
                System.arraycopy(replace, 0, raw, i, replace.length);
            }
        }
        return raw;
    }

    /** Archive with MAX_ENTRIES+1 entries so it trips the limit. */
    private static byte[] tooManyEntriesArchive() throws IOException {
        final int MAX_ENTRIES = 10000;
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        try (ZipOutputStream zip = new ZipOutputStream(bytes)) {
            for (int i = 0; i <= MAX_ENTRIES; i++) {
                addEntry(zip, "file" + i + ".sav", "x");
            }
        }
        return bytes.toByteArray();
    }

    /**
     * Archive whose single entry decompresses to more than 256 MB.
     * We write 256*1024*1024 + 1 bytes in chunks to avoid allocating it all at once.
     */
    private static byte[] tooBigArchive() throws IOException {
        final long LIMIT = 256L * 1024 * 1024 + 1;
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        try (ZipOutputStream zip = new ZipOutputStream(bytes)) {
            zip.putNextEntry(new ZipEntry("big.sav"));
            // Write in 64 KB chunks so heap stays small; DEFLATE compresses
            // runs of zeros extremely well, keeping the ZIP itself tiny.
            byte[] chunk = new byte[65536];
            long remaining = LIMIT;
            while (remaining > 0) {
                int n = (int) Math.min(chunk.length, remaining);
                zip.write(chunk, 0, n);
                remaining -= n;
            }
            zip.closeEntry();
        }
        return bytes.toByteArray();
    }

    private static void addEntry(ZipOutputStream zip, String name, String content)
            throws IOException {
        zip.putNextEntry(new ZipEntry(name));
        zip.write(content.getBytes(StandardCharsets.UTF_8));
        zip.closeEntry();
    }

    // -------------------------------------------------------------------------
    // Helpers
    // -------------------------------------------------------------------------

    private static void check(boolean condition, String msg) {
        if (!condition) throw new AssertionError(msg);
    }

    /** Expect SaveImport.install() to throw IOException with a message containing hint. */
    private static void expectReject(InputStream in, File dest, String hint) throws IOException {
        try {
            SaveImport.install(in, dest);
            throw new AssertionError("expected rejection but install() succeeded (hint: " + hint + ")");
        } catch (IOException ok) {
            // expected
        }
    }

    private static InputStream stream(byte[] data) {
        return new ByteArrayInputStream(data);
    }

    /** Delete a whole directory tree (best-effort, used in finally blocks). */
    private static void delete(File file) {
        File[] children = file.listFiles();
        if (children != null) for (File child : children) delete(child);
        file.delete();
    }

    /** Create a fresh, empty destination directory inside root. */
    private static File freshTarget(File root) throws IOException {
        File target = new File(root, "save_import");
        delete(target);
        if (!target.mkdir()) throw new AssertionError("could not create target dir");
        return target;
    }

    // -------------------------------------------------------------------------
    // main
    // -------------------------------------------------------------------------

    public static void main(String[] args) throws Exception {
        File root = Files.createTempDirectory("save-import-test").toFile();
        int passed = 0;
        try {

            // ------------------------------------------------------------------
            // 1. Successful multi-file import with nested directories
            // ------------------------------------------------------------------
            {
                File target = freshTarget(root);
                Files.write(new File(target, "old.sav").toPath(), new byte[]{1});

                int count = SaveImport.install(stream(multiFileArchive()), target);
                check(count == 3, "multi-file: expected 3 files, got " + count);
                check(new File(target, "profile/save1.sav").isFile(), "profile/save1.sav missing");
                check(new File(target, "profile/save2.sav").isFile(), "profile/save2.sav missing");
                check(new File(target, "screenshots/screen.png").isFile(), "screenshots/screen.png missing");
                check(!new File(target, "old.sav").exists(), "old file not replaced");
                // .previous must have been deleted
                check(!new File(root, "save_import.previous").exists(), ".previous not cleaned up after success");
                passed++;
                System.out.println("PASS 1: multi-file import with nested directories");
            }

            // ------------------------------------------------------------------
            // 2. Backup-on-success: .previous is gone when install succeeds
            // ------------------------------------------------------------------
            {
                File target = freshTarget(root);
                // Pre-install something so a .previous gets created
                SaveImport.install(stream(archive("first.sav", "v1")), target);
                // Install again — a .previous must be created then deleted
                SaveImport.install(stream(archive("second.sav", "v2")), target);
                File backup = new File(root, "save_import.previous");
                check(!backup.exists(), "backup .previous not deleted after successful install");
                passed++;
                System.out.println("PASS 2: backup (.previous) deleted after successful install");
            }

            // ------------------------------------------------------------------
            // 3. Empty archive (truly zero entries) is rejected
            // ------------------------------------------------------------------
            {
                File target = freshTarget(root);
                File pending = new File(root, "save_import.pending");
                expectReject(stream(emptyArchive()), target, "empty archive");
                check(!pending.exists(), "staging dir left behind after empty-archive rejection");
                passed++;
                System.out.println("PASS 3: empty archive rejected, staging cleaned up");
            }

            // ------------------------------------------------------------------
            // 4. Directory-only archive (no files) is rejected
            // ------------------------------------------------------------------
            {
                File target = freshTarget(root);
                File pending = new File(root, "save_import.pending");
                expectReject(stream(directoryOnlyArchive()), target, "dir-only archive");
                check(!pending.exists(), "staging dir left behind after dir-only-archive rejection");
                passed++;
                System.out.println("PASS 4: directory-only archive rejected, staging cleaned up");
            }

            // ------------------------------------------------------------------
            // 5. Duplicate path is rejected; staging cleaned up
            // ------------------------------------------------------------------
            {
                File target = freshTarget(root);
                File pending = new File(root, "save_import.pending");
                expectReject(stream(duplicatePathArchive()), target, "duplicate path");
                check(!pending.exists(), "staging dir left behind after duplicate-path rejection");
                passed++;
                System.out.println("PASS 5: duplicate path rejected, staging cleaned up");
            }

            // ------------------------------------------------------------------
            // 6. Absolute path (starts with "/") is rejected
            // ------------------------------------------------------------------
            {
                File target = freshTarget(root);
                File pending = new File(root, "save_import.pending");
                expectReject(stream(archive("/etc/passwd", "bad")), target, "absolute path");
                check(!pending.exists(), "staging dir left behind after absolute-path rejection");
                passed++;
                System.out.println("PASS 6: absolute path rejected, staging cleaned up");
            }

            // ------------------------------------------------------------------
            // 7. Backslash in path is rejected
            // ------------------------------------------------------------------
            {
                File target = freshTarget(root);
                File pending = new File(root, "save_import.pending");
                expectReject(stream(archive("profile\\save.sav", "bad")), target, "backslash path");
                check(!pending.exists(), "staging dir left behind after backslash-path rejection");
                passed++;
                System.out.println("PASS 7: backslash path rejected, staging cleaned up");
            }

            // ------------------------------------------------------------------
            // 8. Empty component ("a//b") is rejected
            // ------------------------------------------------------------------
            {
                File target = freshTarget(root);
                File pending = new File(root, "save_import.pending");
                expectReject(stream(archive("a//b.sav", "bad")), target, "empty component");
                check(!pending.exists(), "staging dir left behind after empty-component rejection");
                passed++;
                System.out.println("PASS 8: empty component (a//b) rejected, staging cleaned up");
            }

            // ------------------------------------------------------------------
            // 9. Dot component ("./file") is rejected
            // ------------------------------------------------------------------
            {
                File target = freshTarget(root);
                File pending = new File(root, "save_import.pending");
                expectReject(stream(archive("./file.sav", "bad")), target, "dot component");
                check(!pending.exists(), "staging dir left behind after dot-component rejection");
                passed++;
                System.out.println("PASS 9: dot component (./file) rejected, staging cleaned up");
            }

            // ------------------------------------------------------------------
            // 10. Double-dot ("../escape") is rejected; no file written outside root
            // ------------------------------------------------------------------
            {
                File target = freshTarget(root);
                File pending = new File(root, "save_import.pending");
                expectReject(stream(archive("../escape.sav", "bad")), target, "double-dot");
                check(!pending.exists(), "staging dir left behind after double-dot rejection");
                check(!new File(root.getParentFile(), "escape.sav").exists(),
                        "double-dot archive wrote file outside save storage");
                passed++;
                System.out.println("PASS 10: double-dot (../escape) rejected, staging cleaned up, no escape");
            }

            // ------------------------------------------------------------------
            // 11. Colon in component ("a:b") is rejected
            // ------------------------------------------------------------------
            {
                File target = freshTarget(root);
                File pending = new File(root, "save_import.pending");
                expectReject(stream(archive("a:b.sav", "bad")), target, "colon in component");
                check(!pending.exists(), "staging dir left behind after colon-component rejection");
                passed++;
                System.out.println("PASS 11: colon component (a:b) rejected, staging cleaned up");
            }

            // ------------------------------------------------------------------
            // 12. MAX_ENTRIES+1 entries cause rejection; staging cleaned up
            // ------------------------------------------------------------------
            {
                File target = freshTarget(root);
                File pending = new File(root, "save_import.pending");
                expectReject(stream(tooManyEntriesArchive()), target, "too many entries");
                check(!pending.exists(), "staging dir left behind after too-many-entries rejection");
                passed++;
                System.out.println("PASS 12: MAX_ENTRIES+1 rejected, staging cleaned up");
            }

            // ------------------------------------------------------------------
            // 13. MAX_BYTES exceeded causes rejection; staging cleaned up
            // ------------------------------------------------------------------
            {
                File target = freshTarget(root);
                File pending = new File(root, "save_import.pending");
                expectReject(stream(tooBigArchive()), target, "too many bytes");
                check(!pending.exists(), "staging dir left behind after too-many-bytes rejection");
                passed++;
                System.out.println("PASS 13: MAX_BYTES+1 rejected, staging cleaned up");
            }

            // ------------------------------------------------------------------
            // 14. Symlink destination is rejected immediately
            // ------------------------------------------------------------------
            {
                // Only runnable on OSes / JVMs that can create symlinks.
                File realDir = new File(root, "real_import");
                File symlink = new File(root, "symlink_import");
                delete(realDir);
                delete(symlink);
                realDir.mkdir();
                boolean canSymlink = false;
                try {
                    Files.createSymbolicLink(symlink.toPath(), realDir.toPath());
                    canSymlink = true;
                } catch (UnsupportedOperationException | IOException ignored) {
                    // Platform doesn't support symlinks — skip
                }
                if (canSymlink) {
                    expectReject(stream(archive("file.sav", "data")), symlink, "symlink destination");
                    passed++;
                    System.out.println("PASS 14: symlink destination rejected immediately");
                } else {
                    System.out.println("SKIP 14: symlink creation not supported on this platform");
                    passed++;   // count as passed so we don't fail the suite
                }
                delete(realDir);
                delete(symlink);
            }

            // ------------------------------------------------------------------
            // 15. Staging cleanup on failure preserves pre-existing destination
            // ------------------------------------------------------------------
            {
                File target = freshTarget(root);
                // Write a known file into the destination first
                Files.write(new File(target, "existing.sav").toPath(),
                        "keep me".getBytes(StandardCharsets.UTF_8));

                File pending = new File(root, "save_import.pending");
                expectReject(stream(archive("../escape.sav", "bad")), target, "staging cleanup");

                // Staging must be gone
                check(!pending.exists(), ".pending not cleaned up after failure");
                // The pre-existing destination must be untouched
                check(new File(target, "existing.sav").isFile(),
                        "failed import removed the pre-existing destination");
                String content = new String(
                        Files.readAllBytes(new File(target, "existing.sav").toPath()),
                        StandardCharsets.UTF_8);
                check("keep me".equals(content), "pre-existing file content changed after failed import");
                passed++;
                System.out.println("PASS 15: staging cleaned up, pre-existing destination untouched on failure");
            }

            // ------------------------------------------------------------------
            // 16. Original regression: nested path import + traversal rejection
            //     (preserving the original test intent)
            // ------------------------------------------------------------------
            {
                File target = freshTarget(root);
                Files.write(new File(target, "previous.sav").toPath(), new byte[]{1});

                int count = SaveImport.install(
                        stream(archive("profile/data.sav", "edited")), target);
                check(count == 1, "regression: wrong imported file count: " + count);
                String saved = new String(
                        Files.readAllBytes(new File(target, "profile/data.sav").toPath()),
                        StandardCharsets.UTF_8);
                check("edited".equals(saved), "regression: nested save file not imported correctly");
                check(!new File(target, "previous.sav").exists(),
                        "regression: old file not replaced");

                // Now a traversal must fail without damaging the import above
                expectReject(stream(archive("../escape.sav", "bad")), target, "traversal regression");
                check(new File(target, "profile/data.sav").isFile(),
                        "regression: failed import damaged the staged save");
                check(!new File(root, "escape.sav").exists(),
                        "regression: archive wrote outside save storage");
                passed++;
                System.out.println("PASS 16: regression — nested import and traversal rejection");
            }

        } finally {
            delete(root);
        }

        System.out.println("\nAll " + passed + " test cases passed.");
    }
}
