package nhcm.advmatch;

import nhcm.advmatch.command.Command;

/** Opaque requests use exactly the same validated binding protocol as typed commands. */
public record RawCommand(byte[] packet) implements Command<byte[]>
{
    public RawCommand
    {
        packet = packet.clone();
    }

    @Override
    public byte[] encode()
    {
        return packet.clone();
    }

    @Override
    public byte[] decode(byte[] response)
    {
        return response.clone();
    }
}
