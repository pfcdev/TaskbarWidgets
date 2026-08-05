using System.Globalization;

namespace TaskbarWidgets.Loader;

internal static class WeatherLocationQuery
{
    private const string Endpoint = "https://geocoding-api.open-meteo.com/v1/search";

    public static string Build(string city, string? language = null)
    {
        if (string.IsNullOrWhiteSpace(city))
        {
            throw new ArgumentException("A city name is required.", nameof(city));
        }

        var resultLanguage = NormalizeLanguage(
            language ?? CultureInfo.CurrentUICulture.TwoLetterISOLanguageName);

        return Endpoint +
               $"?name={Uri.EscapeDataString(city.Trim())}" +
               $"&count=1&language={Uri.EscapeDataString(resultLanguage)}&format=json";
    }

    private static string NormalizeLanguage(string? language)
    {
        if (string.IsNullOrWhiteSpace(language))
        {
            return "en";
        }

        var normalized = language.Trim().ToLowerInvariant();
        return normalized.Length == 2 && normalized.All(char.IsAsciiLetter)
            ? normalized
            : "en";
    }
}
