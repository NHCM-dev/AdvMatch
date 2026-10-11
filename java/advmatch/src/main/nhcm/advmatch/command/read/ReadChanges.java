package nhcm.advmatch.command.read;

import nhcm.advmatch.command.Command;
import nhcm.advmatch.internal.ResponseCodec;
import nhcm.advmatch.internal.Wire;
import nhcm.advmatch.internal.WireWriter;
import nhcm.advmatch.model.ObservationBatch;
import nhcm.advmatch.model.ObservationQuery;
import nhcm.advmatch.model.ReadStamp;

import java.util.List;

/** Bounded conditional snapshots; omitted queries retain their previous image. */
public record ReadChanges(List<ObservationQuery> queries, List<ReadStamp> known) implements Command<ObservationBatch>
{
    public ReadChanges
    {
        queries = List.copyOf(queries);
        known = List.copyOf(known);
        if (queries.isEmpty() || queries.size() != known.size())
        {
            throw new IllegalArgumentException("Mismatched conditional observation batch");
        }
    }

    @Override
    public byte[] encode()
    {
        var writer = new WireWriter();
        encodeTo(writer);
        return writer.bytes();
    }

    @Override
    public void encodeTo(WireWriter writer)
    {
        writer.requestHeader(11);
        writer.u64(queries.size());
        for (int index = 0; index < queries.size(); index++)
        {
            Wire.writeObservationQuery(writer, queries.get(index));
            Wire.writeReadStamp(writer, known.get(index));
        }
    }

    @Override
    public ObservationBatch decode(byte[] response)
    {
        return ResponseCodec.batch(response);
    }

    @Override
    public ObservationBatch decode(byte[] response, int offset, int size)
    {
        return ResponseCodec.batch(response, offset, size);
    }
}
