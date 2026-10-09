package nhcm.advmatch.command;

// Owned request bytes and response DTOs only; native manager references never
// escape. Compose arbitrary application logic with CompletableFuture chains.
public interface Command<T>
{
    byte[] encode();

    // Generated commands write directly into the owning batch buffer. Custom
    // commands keep their existing byte[] implementation through this fallback.
    default void encodeTo(nhcm.advmatch.internal.WireWriter writer)
    {
        writer.writeBytes(this.encode());
    }

    T decode(byte[] response);

    // Generated commands decode the immutable batch in place. Custom commands
    // remain compatible through an owning copy unless they override this.
    default T decode(byte[] response, int offset, int size)
    {
        return this.decode(java.util.Arrays.copyOfRange(response, offset, Math.addExact(offset, size)));
    }
}
