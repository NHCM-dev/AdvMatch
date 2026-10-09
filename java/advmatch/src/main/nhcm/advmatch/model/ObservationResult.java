package nhcm.advmatch.model;

import java.util.List;

public record ObservationResult(ObservationQuery query, ReadPage page, Object data)
{
    public <T> ReadResponse<T> typed(Class<T> type)
    {
        if (this.data == null)
        {
            return new ReadResponse<>(this.query, this.page, List.of());
        }

        if (!(this.data instanceof List<?> values))
        {
            throw new IllegalArgumentException("Observation is not a row response");
        }

        return new ReadResponse<>(this.query, this.page, values.stream().map(type::cast).toList());
    }
}
