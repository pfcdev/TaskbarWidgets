using System.Net.Http.Json;
using System.Text;
using System.Text.Encodings.Web;
using System.Text.Json;
using System.Text.Json.Serialization;
using TaskbarWidgets.Loader.Core;

namespace TaskbarWidgets.Loader;

internal static class WeatherWorker
{
    private const string DefaultCity = "İzmir";
    private static readonly TimeSpan RefreshInterval = TimeSpan.FromMinutes(15);
    private static readonly TimeSpan SettingsPollInterval = TimeSpan.FromSeconds(10);
    private static readonly string AppDirectory = AppPaths.AppDirectory;
    private static readonly string LogsDirectory = Path.Combine(AppDirectory, "Logs");
    private static readonly string LogPath = Path.Combine(LogsDirectory, "loader.log");
    private static readonly string WidgetSettingsPath = Path.Combine(AppDirectory, "config.json");
    private static readonly WidgetStateStore StateStore = new();
    private static readonly HttpClient Http = new()
    {
        Timeout = TimeSpan.FromSeconds(12)
    };

    public static async Task RunAsync(CancellationToken cancellationToken)
    {
        Directory.CreateDirectory(AppDirectory);
        Directory.CreateDirectory(LogsDirectory);

        string? lastCity = null;
        var nextRefreshAt = DateTimeOffset.MinValue;

        while (!cancellationToken.IsCancellationRequested)
        {
            var city = ReadConfiguredCity();
            var now = DateTimeOffset.UtcNow;
            var cityChanged = !string.Equals(city, lastCity, StringComparison.OrdinalIgnoreCase);

            try
            {
                if (cityChanged || now >= nextRefreshAt)
                {
                    var snapshot = await GetWeatherByCityAsync(city, cancellationToken);
                    WriteStatus(snapshot);
                    lastCity = city;
                    nextRefreshAt = now + RefreshInterval;
                }
            }
            catch (OperationCanceledException)
            {
                throw;
            }
            catch (Exception ex)
            {
                Log($"Weather update failed for {city}: {ex.Message}");
                StateStore.Write("weather-static", new { loaded = false }, "error", ex.Message);
                lastCity = city;
                nextRefreshAt = now + TimeSpan.FromMinutes(2);
            }

            await Task.Delay(SettingsPollInterval, cancellationToken);
        }
    }

    private static string ReadConfiguredCity()
    {
        try
        {
            if (!File.Exists(WidgetSettingsPath))
            {
                return DefaultCity;
            }

            using var document = JsonDocument.Parse(File.ReadAllText(WidgetSettingsPath, Encoding.UTF8));
            if (TryGetWidgetSettings(document.RootElement, "weather-static", out var widgetSettings) &&
                widgetSettings.TryGetProperty("city", out var cityElement) &&
                cityElement.ValueKind == JsonValueKind.String)
            {
                var city = cityElement.GetString()?.Trim();
                if (!string.IsNullOrWhiteSpace(city))
                {
                    return city;
                }
            }
        }
        catch (Exception ex)
        {
            Log($"Weather settings read failed: {ex.Message}");
        }

        return DefaultCity;
    }

    private static bool TryGetWidgetSettings(JsonElement root, string id, out JsonElement settings)
    {
        settings = default;
        if (!root.TryGetProperty("widgets", out var widgets) || widgets.ValueKind != JsonValueKind.Array)
        {
            return false;
        }

        foreach (var widget in widgets.EnumerateArray())
        {
            if (widget.TryGetProperty("id", out var widgetId) && widgetId.GetString() == id &&
                widget.TryGetProperty("settings", out settings))
            {
                return true;
            }
        }

        return false;
    }

    private static async Task<WeatherSnapshot> GetWeatherByCityAsync(
        string city,
        CancellationToken cancellationToken)
    {
        var geoUrl =
            "https://geocoding-api.open-meteo.com/v1/search" +
            $"?name={Uri.EscapeDataString(city)}" +
            "&count=1&language=tr&countryCode=TR&format=json";

        var geo = await Http.GetFromJsonAsync<GeoResponse>(
            geoUrl,
            JsonOptions(),
            cancellationToken);
        var location = geo?.Results?.FirstOrDefault() ??
                       throw new InvalidOperationException("Şehir bulunamadı.");

        var weatherUrl =
            "https://api.open-meteo.com/v1/forecast" +
            $"?latitude={location.Latitude.ToString(System.Globalization.CultureInfo.InvariantCulture)}" +
            $"&longitude={location.Longitude.ToString(System.Globalization.CultureInfo.InvariantCulture)}" +
            "&current=temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,wind_speed_10m" +
            "&daily=weather_code,temperature_2m_max,temperature_2m_min" +
            "&forecast_days=7&timezone=auto";

        var weather = await Http.GetFromJsonAsync<OpenMeteoResponse>(
            weatherUrl,
            JsonOptions(),
            cancellationToken) ??
                      throw new InvalidOperationException("Hava durumu verisi boş döndü.");

        var current = weather.Current ??
                      throw new InvalidOperationException("Güncel hava durumu verisi boş döndü.");

        return new WeatherSnapshot
        {
            Loaded = true,
            UpdatedAtUnix = DateTimeOffset.UtcNow.ToUnixTimeSeconds(),
            Location = $"{location.Name}, {location.Admin1 ?? location.Country}",
            Latitude = location.Latitude,
            Longitude = location.Longitude,
            Temperature = current.Temperature,
            ApparentTemperature = current.ApparentTemperature,
            Humidity = current.Humidity,
            WeatherCode = current.WeatherCode,
            WindSpeed = current.WindSpeed,
            Days = BuildDays(weather.Daily)
        };
    }

    private static List<WeatherDay> BuildDays(DailyWeather? daily)
    {
        var days = new List<WeatherDay>();
        if (daily?.Time is null ||
            daily.WeatherCode is null ||
            daily.TemperatureMax is null ||
            daily.TemperatureMin is null)
        {
            return days;
        }

        var count = new[]
        {
            daily.Time.Length,
            daily.WeatherCode.Length,
            daily.TemperatureMax.Length,
            daily.TemperatureMin.Length
        }.Min();

        for (var i = 0; i < Math.Min(7, count); i++)
        {
            var label = i == 0
                ? "Bugün"
                : DateTime.TryParse(daily.Time[i], out var date)
                    ? TurkishDayLabel(date.DayOfWeek)
                    : daily.Time[i];

            days.Add(new WeatherDay
            {
                Label = label,
                WeatherCode = daily.WeatherCode[i],
                Max = daily.TemperatureMax[i],
                Min = daily.TemperatureMin[i]
            });
        }

        return days;
    }

    private static void WriteStatus(WeatherSnapshot snapshot)
    {
        StateStore.Write("weather-static", snapshot);
    }

    private static string TurkishDayLabel(DayOfWeek day) => day switch
    {
        DayOfWeek.Monday => "Pzt",
        DayOfWeek.Tuesday => "Sal",
        DayOfWeek.Wednesday => "Çar",
        DayOfWeek.Thursday => "Per",
        DayOfWeek.Friday => "Cum",
        DayOfWeek.Saturday => "Cmt",
        DayOfWeek.Sunday => "Paz",
        _ => ""
    };

    private static JsonSerializerOptions JsonOptions() => new()
    {
        Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        WriteIndented = true
    };

    private static void Log(string message)
    {
        try
        {
            Directory.CreateDirectory(LogsDirectory);
            File.AppendAllText(
                LogPath,
                $"{DateTimeOffset.Now:O} [loader] {message}{Environment.NewLine}");
        }
        catch
        {
            // Logging must never break the loader.
        }
    }

    private sealed class GeoResponse
    {
        [JsonPropertyName("results")]
        public GeoLocation[]? Results { get; set; }
    }

    private sealed class GeoLocation
    {
        [JsonPropertyName("name")]
        public string Name { get; set; } = "";

        [JsonPropertyName("admin1")]
        public string? Admin1 { get; set; }

        [JsonPropertyName("country")]
        public string Country { get; set; } = "";

        [JsonPropertyName("latitude")]
        public double Latitude { get; set; }

        [JsonPropertyName("longitude")]
        public double Longitude { get; set; }
    }

    private sealed class OpenMeteoResponse
    {
        [JsonPropertyName("current")]
        public CurrentWeather? Current { get; set; }

        [JsonPropertyName("daily")]
        public DailyWeather? Daily { get; set; }
    }

    private sealed class CurrentWeather
    {
        [JsonPropertyName("temperature_2m")]
        public double Temperature { get; set; }

        [JsonPropertyName("apparent_temperature")]
        public double ApparentTemperature { get; set; }

        [JsonPropertyName("relative_humidity_2m")]
        public double Humidity { get; set; }

        [JsonPropertyName("weather_code")]
        public int WeatherCode { get; set; }

        [JsonPropertyName("wind_speed_10m")]
        public double WindSpeed { get; set; }
    }

    private sealed class DailyWeather
    {
        [JsonPropertyName("time")]
        public string[]? Time { get; set; }

        [JsonPropertyName("weather_code")]
        public int[]? WeatherCode { get; set; }

        [JsonPropertyName("temperature_2m_max")]
        public double[]? TemperatureMax { get; set; }

        [JsonPropertyName("temperature_2m_min")]
        public double[]? TemperatureMin { get; set; }
    }

    private sealed class WeatherSnapshot
    {
        public bool Loaded { get; set; }
        public long UpdatedAtUnix { get; set; }
        public string Location { get; set; } = "";
        public double Latitude { get; set; }
        public double Longitude { get; set; }
        public double Temperature { get; set; }
        public double ApparentTemperature { get; set; }
        public double Humidity { get; set; }
        public int WeatherCode { get; set; }
        public double WindSpeed { get; set; }
        public List<WeatherDay> Days { get; set; } = [];
    }

    private sealed class WeatherDay
    {
        public string Label { get; set; } = "";
        public int WeatherCode { get; set; }
        public double Max { get; set; }
        public double Min { get; set; }
    }
}
