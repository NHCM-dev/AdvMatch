package nhcm.advmatch.internal;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Locale;

final class NativeLoader
{
    private NativeLoader()
    {
    }

    static void load()
    {
        var directory = System.getProperty("advmatch.nativeDir", System.getenv("ADVMATCH_NATIVE_DIR"));
        if (directory != null && !directory.isBlank())
        {
            loadDirectory(Path.of(directory).toAbsolutePath());
            return;
        }

        var osName = System.getProperty("os.name").toLowerCase(Locale.ROOT);
        var os = osName.contains("win") ? "windows" : osName.contains("mac") ? "macos" : "linux";
        var archName = System.getProperty("os.arch").toLowerCase(Locale.ROOT);
        var arch = archName.equals("x86_64") ? "amd64" : archName.equals("aarch64") ? "arm64" : archName;
        var prefix = "/native/" + os + "-" + arch + "/";
        try
        {
            var extracted = Files.createTempDirectory("advmatch-native-");
            extracted.toFile().deleteOnExit();
            for (var library : new String[]{"advmatch", "advmatch_jni"})
            {
                var filename = System.mapLibraryName(library);
                try (var input = NativeLoader.class.getResourceAsStream(prefix + filename))
                {
                    if (input == null)
                    {
                        throw new UnsatisfiedLinkError("No native library for " + os + "-" + arch + "; set advmatch.nativeDir or ADVMATCH_NATIVE_DIR");
                    }

                    var file = extracted.resolve(filename);
                    Files.copy(input, file);
                    file.toFile().deleteOnExit();
                }
            }

            loadDirectory(extracted);
        }
        catch (IOException error)
        {
            throw new UnsatisfiedLinkError("Cannot extract AdvMatch native libraries: " + error.getMessage());
        }
    }

    private static void loadDirectory(Path directory)
    {
        // Load C ABI first so the JNI DLL's dependency is already resolved.
        System.load(directory.resolve(System.mapLibraryName("advmatch")).toString());
        System.load(directory.resolve(System.mapLibraryName("advmatch_jni")).toString());
    }
}
