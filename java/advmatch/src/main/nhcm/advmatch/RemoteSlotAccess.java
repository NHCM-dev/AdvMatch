package nhcm.advmatch;

import nhcm.advmatch.model.ObservationResult;

public interface RemoteSlotAccess
{
    ObservationResult load(long id);

    void release(long id);
}
