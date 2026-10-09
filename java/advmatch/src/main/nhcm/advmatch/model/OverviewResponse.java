package nhcm.advmatch.model;

// value is null when the source is faulted or the query failed.
public record OverviewResponse(ObservationQuery query, ReadPage page, OverviewView value)
{
}
