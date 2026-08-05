using System.Globalization;

namespace TaskbarWidgets.Loader;

internal static class WeatherDayLabel
{
    public static string Format(DateTime date, bool isToday) =>
        isToday
            ? "Today"
            : CultureInfo.InvariantCulture.DateTimeFormat.GetAbbreviatedDayName(date.DayOfWeek);
}
