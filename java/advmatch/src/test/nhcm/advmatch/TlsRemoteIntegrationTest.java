package nhcm.advmatch;

import nhcm.advmatch.command.read.ReadAssets;
import nhcm.advmatch.model.ReadState;
import nhcm.advmatch.model.ExchangeConfig;
import nhcm.advmatch.model.PersistenceConfig;
import nhcm.advmatch.remote.*;

import javax.net.ssl.*;
import java.nio.file.*;
import java.security.KeyStore;
import java.util.UUID;
import java.util.concurrent.*;

public class TlsRemoteIntegrationTest
{
    public static void main(String[] args) throws Exception
    {
        var store = KeyStore.getInstance("PKCS12");
        try (var input = Files.newInputStream(Path.of(args[0])))
        {
            store.load(input, "integration-password".toCharArray());
        }
        var keys = KeyManagerFactory.getInstance(KeyManagerFactory.getDefaultAlgorithm());
        keys.init(store, "integration-password".toCharArray());
        var trusts = TrustManagerFactory.getInstance(TrustManagerFactory.getDefaultAlgorithm());
        trusts.init(store);
        var tls = SSLContext.getInstance("TLS");
        tls.init(keys.getKeyManagers(), trusts.getTrustManagers(), null);
        String token = "tls-test-012345678901234567890123456789";
        var directory = Files.createTempDirectory("advmatch-tls-");
        var config = new ExchangeConfig().withPersistence(new PersistenceConfig().withEnabled(true).withDirectory(directory.toString()));
        try (var owner = new Exchange(config);
             var server = new ExchangeServer(owner, new ExchangeServer.Options("127.0.0.1", 0, token, UUID.randomUUID().toString(), false, 8, 64, 4194304), tls);
             var channel = new RemoteChannel(new RemoteOptions("localhost", server.port(), "tls-gateway", token,
                     true, false, 5000, 10000, 64, 4194304, ""), tls.getSocketFactory());
             var client = new Exchange(channel, ForkJoinPool.commonPool()))
        {
            owner.assets().create("tls", "TLS", 0).join();
            var assets = client.submit(new ReadAssets()).get(5, TimeUnit.SECONDS);
            if (assets.page().state() != ReadState.Success || assets.values().size() != 1) throw new AssertionError("TLS read failed");
            var untrusted = SSLContext.getInstance("TLS");
            untrusted.init(null, null, null);
            try (var rejected = new RemoteChannel(new RemoteOptions("localhost", server.port(), "untrusted", token,
                    true, false, 5000, 5000, 64, 4194304, ""), untrusted.getSocketFactory()))
            {
                boolean failed = false;
                try { rejected.request(new ReadAssets().encode()).get(5, TimeUnit.SECONDS); }
                catch (ExecutionException expected) { failed = true; }
                if (!failed) throw new AssertionError("Untrusted TLS certificate was accepted");
            }
        }
        System.out.println("Remote TLS: encrypted connection, trusted certificate and rejection of untrusted certificates passed");
    }
}
