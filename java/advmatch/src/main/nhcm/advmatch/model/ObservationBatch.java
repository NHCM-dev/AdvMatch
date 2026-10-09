package nhcm.advmatch.model;

import java.util.List;

public record ObservationBatch(ReadState state, ReadStamp stamp, List<ObservationResult> results)
{
    public ObservationBatch
    {
        results = List.copyOf(results);
    }
}
