namespace TaskbarWidgets.Loader.Core;

internal static class SystemMetricMath
{
    public static double ClampPercent(double value) =>
        double.IsFinite(value) ? Math.Round(Math.Clamp(value, 0, 100), 1) : 0;

    public static double ComputeRate(ulong previous, ulong current, double elapsedSeconds)
    {
        if (elapsedSeconds <= 0 || !double.IsFinite(elapsedSeconds) || current < previous)
        {
            return 0;
        }

        var rate = (current - previous) / elapsedSeconds;
        return double.IsFinite(rate) && rate >= 0 ? rate : 0;
    }

    public static double NormalizeRefreshSeconds(double value)
    {
        if (!double.IsFinite(value) || value < 0.1 || value > 10)
        {
            return 3;
        }

        return Math.Round(value * 10, MidpointRounding.AwayFromZero) / 10;
    }

    public static double ComputeLinkUtilization(
        double bytesPerSecond,
        ulong linkSpeedBitsPerSecond,
        bool automaticBandwidth,
        double manualBandwidthKiloBytes)
    {
        var capacityBytesPerSecond = automaticBandwidth
            ? linkSpeedBitsPerSecond / 8d
            : Math.Max(0, manualBandwidthKiloBytes) * 1000d;
        if (!double.IsFinite(bytesPerSecond) || bytesPerSecond <= 0 ||
            !double.IsFinite(capacityBytesPerSecond) || capacityBytesPerSecond <= 0)
        {
            return 0;
        }

        return ClampPercent(bytesPerSecond * 100d / capacityBytesPerSecond);
    }
}
