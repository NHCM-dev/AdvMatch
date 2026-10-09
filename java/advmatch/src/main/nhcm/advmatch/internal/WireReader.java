package nhcm.advmatch.internal;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.function.Supplier;

public final class WireReader
{
    private final ByteBuffer input;

    public WireReader(byte[] bytes)
    {
        this(bytes, 0, bytes.length);
    }

    public WireReader(byte[] bytes, int offset, int size)
    {
        this.input = ByteBuffer.wrap(bytes, offset, size).slice().order(ByteOrder.LITTLE_ENDIAN);
    }

    public int offset()
    {
        return this.input.position();
    }

    public void skip(int size)
    {
        if (size < 0 || size > this.input.remaining())
        {
            throw new IllegalArgumentException("Truncated response payload");
        }

        this.input.position(this.input.position() + size);
    }

    public static WireReader response(byte[] bytes, int type)
    {
        return response(bytes, type, 0, bytes.length);
    }

    public static WireReader response(byte[] bytes, int type, int offset, int size)
    {
        var reader = new WireReader(bytes, offset, size);
        if (reader.u16() != 1 || reader.u8() != type)
        {
            throw new IllegalArgumentException("Unsupported binding response schema/type");
        }

        return reader;
    }

    public int u8()
    {
        return Byte.toUnsignedInt(this.input.get());
    }

    public int u16()
    {
        return Short.toUnsignedInt(this.input.getShort());
    }

    public long u32()
    {
        return Integer.toUnsignedLong(this.input.getInt());
    }

    public int i32()
    {
        return this.input.getInt();
    }

    public long u64()
    {
        return this.input.getLong();
    }

    public long i64()
    {
        return this.input.getLong();
    }

    public boolean bool()
    {
        var value = this.u8();
        if (value > 1)
        {
            throw new IllegalArgumentException("Invalid boolean");
        }

        return value != 0;
    }

    public String string()
    {
        var size = this.u32();
        if (size > this.input.remaining())
        {
            throw new IllegalArgumentException("Truncated UTF-8 string");
        }

        var bytes = new byte[(int)size];
        this.input.get(bytes);
        return new String(bytes, StandardCharsets.UTF_8);
    }

    public <T> List<T> list(Supplier<T> read)
    {
        var size = this.u64();
        if (size < 0 || size > this.input.remaining())
        {
            throw new IllegalArgumentException("Invalid collection size");
        }

        var values = new ArrayList<T>((int)size);
        for (long index = 0; index < size; ++index)
        {
            values.add(read.get());
        }

        return List.copyOf(values);
    }

    public byte[] bytes(int size)
    {
        if (size < 0 || size > this.input.remaining())
        {
            throw new IllegalArgumentException("Truncated response payload");
        }

        var bytes = new byte[size];
        this.input.get(bytes);
        return bytes;
    }

    public void finish()
    {
        if (this.input.hasRemaining())
        {
            throw new IllegalArgumentException("Unexpected trailing response bytes");
        }
    }
}
