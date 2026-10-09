package nhcm.advmatch.benchmark.support;

import java.util.LinkedHashMap;
import java.util.Map;
import java.util.Set;

public final class Options
{
    private final Map<String, String> values = new LinkedHashMap<>();

    public Options(String[] arguments, Set<String> allowed)
    {
        for (int index = 0; index < arguments.length; ++index)
        {
            var name = arguments[index];
            if (!allowed.contains(name))
            {
                throw new IllegalArgumentException("Unknown option: " + name);
            }

            if (name.equals("--force"))
            {
                this.values.put(name, "1");
                continue;
            }

            if (++index == arguments.length)
            {
                throw new IllegalArgumentException("Missing value for " + name);
            }

            if (this.values.put(name, arguments[index]) != null)
            {
                throw new IllegalArgumentException("Duplicate option: " + name);
            }
        }
    }

    public String text(String name, String fallback)
    {
        return this.values.getOrDefault(name, fallback);
    }

    public long number(String name, long fallback, long minimum, long maximum)
    {
        var value = Long.parseLong(this.text(name, Long.toString(fallback)));
        if (value < minimum || value > maximum)
        {
            throw new IllegalArgumentException(name + " must be in [" + minimum + "," + maximum + "]");
        }

        return value;
    }

    public int integer(String name, int fallback, int minimum, int maximum)
    {
        return (int) this.number(name, fallback, minimum, maximum);
    }

    public boolean flag(String name)
    {
        return this.values.containsKey(name);
    }

    public int ringSize(int fallback)
    {
        var value = this.integer("--ring-size", fallback, 1, 1_048_576);
        if ((value & (value - 1)) != 0)
        {
            throw new IllegalArgumentException("ring-size must be a power of two");
        }

        return value;
    }
}
