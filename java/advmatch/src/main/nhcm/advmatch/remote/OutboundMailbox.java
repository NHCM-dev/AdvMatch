package nhcm.advmatch.remote;

import java.util.ArrayDeque;
import java.util.LinkedHashMap;

/** Replies have reserved capacity; snapshots replace obsolete snapshots for the same subscription. */
class OutboundMailbox
{
    private final ArrayDeque<NetworkFrames.Frame> replies = new ArrayDeque<>();
    private final LinkedHashMap<Long, NetworkFrames.Frame> updates = new LinkedHashMap<>();
    private long replyBytes, updateBytes, coalesced;
    private int consecutiveReplies;
    private boolean closed;

    synchronized boolean offer(NetworkFrames.Frame frame)
    {
        if (closed) return false;
        long size = frame.payload().length + 13L;
        if (frame.type() == NetworkFrames.UPDATE)
        {
            var previous = updates.get(frame.id());
            long next = updateBytes + size - (previous == null ? 0 : previous.payload().length + 13L);
            if (next > 8 * 1024 * 1024 || previous == null && updates.size() >= 1024) return false;
            updates.put(frame.id(), frame);
            updateBytes = next;
            if (previous != null) coalesced++;
        }
        else
        {
            if (replies.size() >= 256 || replyBytes + size > 8 * 1024 * 1024) return false;
            replies.addLast(frame);
            replyBytes += size;
        }
        notifyAll();
        return true;
    }

    synchronized NetworkFrames.Frame poll(long waitMillis) throws InterruptedException
    {
        long deadline = System.nanoTime() + waitMillis * 1_000_000L;
        while (!closed && replies.isEmpty() && updates.isEmpty() && waitMillis > 0)
        {
            wait(waitMillis);
            waitMillis = Math.max(0, (deadline - System.nanoTime()) / 1_000_000L);
        }
        if (!replies.isEmpty() && (updates.isEmpty() || consecutiveReplies < 16))
        {
            consecutiveReplies++;
            var frame = replies.removeFirst();
            replyBytes -= frame.payload().length + 13L;
            return frame;
        }
        if (!updates.isEmpty())
        {
            consecutiveReplies = 0;
            var iterator = updates.values().iterator();
            var frame = iterator.next();
            iterator.remove();
            updateBytes -= frame.payload().length + 13L;
            return frame;
        }
        return null;
    }

    synchronized long coalesced()
    {
        return coalesced;
    }

    synchronized void close()
    {
        closed = true;
        replies.clear();
        updates.clear();
        replyBytes = updateBytes = 0;
        notifyAll();
    }
}
