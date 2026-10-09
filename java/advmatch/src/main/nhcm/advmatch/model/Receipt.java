package nhcm.advmatch.model;

public interface Receipt
{
    CommandReceipt receipt();

    default long id()
    {
        return this.result().entityId();
    }

    default long handle()
    {
        return this.id();
    }

    default CommandResult result()
    {
        return this.receipt().result();
    }

    default long sequence()
    {
        return this.receipt().sequence();
    }

    default long durableThrough()
    {
        return this.receipt().durableThrough();
    }

    default boolean duplicate()
    {
        return this.receipt().duplicate();
    }

    default boolean durable()
    {
        return this.receipt().durable();
    }
}
