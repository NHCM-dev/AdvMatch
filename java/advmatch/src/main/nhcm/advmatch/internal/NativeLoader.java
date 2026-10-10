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

        var platform = platform(System.getProperty("os.name"), System.getProperty("os.arch"));
        var prefix = "/native/" + platform + "/";
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
                        throw new UnsatisfiedLinkError("No native library for " + platform + "; set advmatch.nativeDir or ADVMATCH_NATIVE_DIR");
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

    static String platform(String osName, String archName)
    {
        var os = osName.toLowerCase(Locale.ROOT);
        String system;
        if (os.startsWith("windows"))
        {
            system = "windows";
        }
        else if (os.equals("linux"))
        {
            system = "linux";
        }
        else if (os.contains("mac") || os.equals("darwin"))
        {
            system = "macos";
        }
        else
        {
            throw new UnsatisfiedLinkError("Unsupported AdvMatch operating system: " + osName);
        }
        var arch = switch (archName.toLowerCase(Locale.ROOT))
        {
            case "amd64", "x86_64", "x64" -> "amd64";
            case "aarch64", "arm64" -> "arm64";
            default -> throw new UnsatisfiedLinkError("Unsupported AdvMatch architecture: " + archName);
        };
        return system + "-" + arch;
    }

    private static void loadDirectory(Path directory)
    {
        // Load the C ABI first; Linux JNI also resolves sibling dependencies through $ORIGIN.
        System.load(directory.resolve(System.mapLibraryName("advmatch")).toString());
        System.load(directory.resolve(System.mapLibraryName("advmatch_jni")).toString());
    }
}
