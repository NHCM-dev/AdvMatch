package nhcm.advmatch;

import nhcm.advmatch.model.*;

// Raw commands return business results; the object facade rejects failed calls.
public final class BusinessException extends RuntimeException
{
    private final CommandReceipt receipt;
    private final ReadPage page;

    public BusinessException(CommandReceipt receipt)
    {
        super("Command rejected: " + receipt.result().type() + " state=" + receipt.result().state());
        this.receipt = receipt;
        this.page = null;
    }

    public BusinessException(ReadPage page)
    {
        super("Read failed: " + page.state());
        this.receipt = null;
        this.page = page;
    }

    public CommandReceipt receipt()
    {
        return this.receipt;
    }

    public ReadPage page()
    {
        return this.page;
    }
}
