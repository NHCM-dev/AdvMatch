package nhcm.advmatch.model;

import java.util.List;

public record ReadResponse<T>(ObservationQuery query, ReadPage page, List<T> values)
{
    public ReadResponse
    {
        values = List.copyOf(values);
    }
}
