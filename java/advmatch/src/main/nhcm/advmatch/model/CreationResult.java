package nhcm.advmatch.model;

import java.util.Objects;

// The handle is an entity ID within its Exchange, never a native address.
// Rejected creates retain their receipt and return ID zero.
public record CreationResult(CommandReceipt receipt) implements Receipt
{
    public CreationResult
    {
        Objects.requireNonNull(receipt);
    }
}
