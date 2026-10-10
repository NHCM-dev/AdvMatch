package nhcm.advmatch.internal;

public class NativeLoaderTest
{
    public static void main(String[] args)
    {
        check("Windows 11", "amd64", "windows-amd64");
        check("Linux", "x86_64", "linux-amd64");
        check("Linux", "x64", "linux-amd64");
        check("Linux", "aarch64", "linux-arm64");
        check("Darwin", "arm64", "macos-arm64");
        check("Mac OS X", "x86_64", "macos-amd64");
        rejected("FreeBSD", "amd64");
        rejected("Linux", "x86");
        System.out.println("Native platform detection passed");
    }

    private static void check(String os, String arch, String expected)
    {
        if (!NativeLoader.platform(os, arch).equals(expected))
        {
            throw new AssertionError("Wrong native platform: " + os + "/" + arch);
        }
    }

    private static void rejected(String os, String arch)
    {
        try
        {
            NativeLoader.platform(os, arch);
            throw new AssertionError("Unsupported platform was accepted");
        }
        catch (UnsatisfiedLinkError expected)
        {
        }
    }
}
