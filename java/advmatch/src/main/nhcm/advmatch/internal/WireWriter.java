package nhcm.advmatch.internal;

import java.util.Arrays;
import java.nio.charset.StandardCharsets;
import java.nio.ByteOrder;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.VarHandle;

public final class WireWriter
{
    private static final VarHandle SHORTS = MethodHandles.byteArrayViewVarHandle(short[].class, ByteOrder.LITTLE_ENDIAN);
    private static final VarHandle INTS = MethodHandles.byteArrayViewVarHandle(int[].class, ByteOrder.LITTLE_ENDIAN);
    private static final VarHandle LONGS = MethodHandles.byteArrayViewVarHandle(long[].class, ByteOrder.LITTLE_ENDIAN);

    private byte[] output;
    private int offset;

    public WireWriter()
    {
        this(128);
    }

    public WireWriter(int capacity)
    {
        if (capacity < 1)
        {
            throw new IllegalArgumentException("Writer capacity must be positive");
        }

        this.output = new byte[capacity];
    }

    private void reserve(int count)
    {
        var needed = Math.addExact(this.offset, count);
        if (needed > this.output.length)
        {
            this.output = Arrays.copyOf(this.output, Math.max(needed, Math.multiplyExact(this.output.length, 2)));
        }
    }

    public void writeBytes(byte[] bytes)
    {
        this.reserve(bytes.length);
        System.arraycopy(bytes, 0, this.output, this.offset, bytes.length);
        this.offset += bytes.length;
    }

    public static WireWriter request(int operation)
    {
        var writer = new WireWriter();
        writer.requestHeader(operation);
        return writer;
    }

    public void requestHeader(int operation)
    {
        this.u16(1);
        this.u8(operation);
    }

    public int beginFrame()
    {
        var start = this.offset;
        this.u32(0);
        return start;
    }

    public void endFrame(int start)
    {
        if (start < 0 || start > this.offset - 4)
        {
            throw new IllegalArgumentException("Invalid frame offset");
        }

        INTS.set(this.output, start, this.offset - start - 4);
    }

    public void u8(int value)
    {
        if (value < 0 || value > 255)
        {
            throw new IllegalArgumentException("Value exceeds uint8");
        }

        this.reserve(1);
        this.output[this.offset++] = (byte)value;
    }

    public void u16(int value)
    {
        if (value < 0 || value > 65535)
        {
            throw new IllegalArgumentException("Value exceeds uint16");
        }

        this.reserve(2);
        SHORTS.set(this.output, this.offset, (short)value);
        this.offset += 2;
    }

    public void u32(long value)
    {
        if (value < 0 || value > 0xffffffffL)
        {
            throw new IllegalArgumentException("Value exceeds uint32");
        }

        this.i32((int)value);
    }

    public void i32(int value)
    {
        this.reserve(4);
        INTS.set(this.output, this.offset, value);
        this.offset += 4;
    }

    public void u64(long value)
    {
        this.i64(value);
    }

    public void i64(long value)
    {
        this.reserve(8);
        LONGS.set(this.output, this.offset, value);
        this.offset += 8;
    }

    public void bool(boolean value)
    {
        this.u8(value ? 1 : 0);
    }

    public void string(String value)
    {
        var bytes = value.getBytes(StandardCharsets.UTF_8);
        this.u32(bytes.length);
        this.writeBytes(bytes);
    }

    public byte[] bytes()
    {
        return Arrays.copyOf(this.output, this.offset);
    }
}
