package org.advmatch;

public final class AdvMatch {
    static { System.loadLibrary("advmatch_jni"); }

    private AdvMatch() {}

    public static native int abiVersion();

    public static void main(String[] args) {
        System.out.println("AdvMatch ABI " + abiVersion());
    }
}
