package nhcm.advmatch.internal;

import nhcm.advmatch.BusinessException;
import nhcm.advmatch.model.*;

public final class ApiResults
{
    private ApiResults()
    {
    }

    public static CommandReceipt accepted(Receipt result)
    {
        if (!result.result().accepted())
        {
            throw new BusinessException(result.receipt());
        }

        return result.receipt();
    }

    public static long created(CreationResult result)
    {
        accepted(result);
        if (result.id() == 0)
        {
            throw new IllegalStateException("Accepted creation has no ID");
        }

        return result.id();
    }

    public static <T> T one(ReadResponse<T> response)
    {
        if (response.page().state() != ReadState.Success || response.values().size() != 1)
        {
            throw new BusinessException(response.page());
        }

        return response.values().getFirst();
    }
}
