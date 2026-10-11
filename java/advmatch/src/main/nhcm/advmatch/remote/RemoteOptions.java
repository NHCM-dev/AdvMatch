package nhcm.advmatch.remote;

import java.util.Objects;

public record RemoteOptions(String host, int port, String instanceId, String token, boolean tls,
                            boolean allowPlaintextNetwork, int connectTimeoutMillis, int requestTimeoutMillis,
                            int maxOutstanding, int maxFrameBytes, String expectedExchangeId)
{
    public RemoteOptions(String host, int port, String instanceId, String token)
    {
        this(host, port, instanceId, token, false, false, 5000, 30000, 256, 4194304, "");
    }

    public RemoteOptions
    {
        Objects.requireNonNull(host);
        Objects.requireNonNull(instanceId);
        Objects.requireNonNull(token);
        Objects.requireNonNull(expectedExchangeId);
        if (host.isBlank() || port < 1 || port > 65535 || !instanceId.matches("[a-zA-Z0-9_-]{1,64}")
            || token.length() < 32 || connectTimeoutMillis < 100 || requestTimeoutMillis < 100
            || maxOutstanding < 1 || maxOutstanding > 4096 || maxFrameBytes < 1024 || maxFrameBytes > 16777216)
        {
            throw new IllegalArgumentException("Invalid remote connection configuration");
        }
    }

    @Override
    public String toString()
    {
        return "RemoteOptions[host=" + host + ", port=" + port + ", instanceId=" + instanceId + ", tls=" + tls + "]";
    }
}
