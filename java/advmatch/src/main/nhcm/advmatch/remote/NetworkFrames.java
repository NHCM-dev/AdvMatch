package nhcm.advmatch.remote;

import java.io.*;
import java.nio.charset.StandardCharsets;

public class NetworkFrames
{
    public static final int VERSION = 1;
    public static final int HELLO = 1, RESPONSE = 2, ERROR = 3, COMMAND = 4, EXTENSION = 5,
                            SUBSCRIBE = 6, UNSUBSCRIBE = 7, UPDATE = 8, RELEASE_SLOT = 9, PING = 10;

    public record Frame(int type, long id, byte[] payload) {}

    public static void write(DataOutputStream out, Frame frame, int limit) throws IOException
    {
        int length = Math.addExact(9, frame.payload().length);
        if (length > limit)
        {
            throw new IOException("Network frame limit exceeded");
        }
        out.writeInt(length);
        out.writeByte(frame.type());
        out.writeLong(frame.id());
        out.write(frame.payload());
        out.flush();
    }

    public static Frame read(DataInputStream in, int limit) throws IOException
    {
        int length = in.readInt();
        if (length < 9 || length > limit)
        {
            throw new IOException("Invalid network frame size");
        }
        int type = in.readUnsignedByte();
        long id = in.readLong();
        byte[] payload = in.readNBytes(length - 9);
        if (payload.length != length - 9)
        {
            throw new EOFException("Truncated network frame");
        }
        return new Frame(type, id, payload);
    }

    public static void string(DataOutputStream out, String text) throws IOException
    {
        byte[] bytes = text.getBytes(StandardCharsets.UTF_8);
        out.writeInt(bytes.length);
        out.write(bytes);
    }

    public static String string(DataInputStream in, int limit) throws IOException
    {
        int size = in.readInt();
        if (size < 0 || size > limit)
        {
            throw new IOException("Invalid string size");
        }
        byte[] bytes = in.readNBytes(size);
        if (bytes.length != size)
        {
            throw new EOFException("Truncated string");
        }
        return new String(bytes, StandardCharsets.UTF_8);
    }

    public interface Encoder { void write(DataOutputStream out) throws IOException; }

    public static byte[] encode(Encoder encoder)
    {
        try
        {
            var bytes = new ByteArrayOutputStream();
            encoder.write(new DataOutputStream(bytes));
            return bytes.toByteArray();
        }
        catch (IOException error)
        {
            throw new UncheckedIOException(error);
        }
    }

    public static DataInputStream input(byte[] bytes)
    {
        return new DataInputStream(new ByteArrayInputStream(bytes));
    }
}
