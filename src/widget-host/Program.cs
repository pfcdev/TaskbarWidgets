using System.Net;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;
using Jint;
using Jint.Native;

namespace TaskbarWidgets.WidgetHost;

internal sealed record HostRequest(
    string Source,
    JsonObject Settings,
    string[] NetworkHosts,
    JsonObject SystemMetrics,
    string WidgetId,
    string InstanceId);

internal sealed class BrokerHttp
{
    private readonly HashSet<string> _hosts;
    private readonly HttpClient _client = new(new HttpClientHandler
    {
        AllowAutoRedirect = false,
        AutomaticDecompression = DecompressionMethods.GZip | DecompressionMethods.Deflate
    }) { Timeout = TimeSpan.FromSeconds(4) };

    public BrokerHttp(IEnumerable<string> hosts) =>
        _hosts = new HashSet<string>(hosts, StringComparer.OrdinalIgnoreCase);

    public object GetJson(string address)
    {
        if (!Uri.TryCreate(address, UriKind.Absolute, out var uri) ||
            uri.Scheme != Uri.UriSchemeHttps || !_hosts.Contains(uri.IdnHost))
        {
            throw new InvalidOperationException("HTTP target is not allowed by the widget manifest.");
        }
        using var response = _client.GetAsync(uri, HttpCompletionOption.ResponseHeadersRead)
            .GetAwaiter().GetResult();
        response.EnsureSuccessStatusCode();
        if (response.Content.Headers.ContentLength is > 262144)
        {
            throw new InvalidDataException("HTTP response exceeds 256 KB.");
        }
        using var stream = response.Content.ReadAsStream();
        using var limited = new LimitedReadStream(stream, 262144);
        return JsonNode.Parse(limited) ?? new JsonObject();
    }
}

internal sealed class LimitedReadStream(Stream inner, long maximum) : Stream
{
    private long _read;
    public override int Read(byte[] buffer, int offset, int count)
    {
        var amount = inner.Read(buffer, offset, (int)Math.Min(count, maximum - _read + 1));
        _read += amount;
        if (_read > maximum) throw new InvalidDataException("HTTP response exceeds 256 KB.");
        return amount;
    }
    public override int Read(Span<byte> buffer)
    {
        var amount = inner.Read(buffer[..Math.Min(buffer.Length, (int)Math.Max(0, maximum - _read + 1))]);
        _read += amount;
        if (_read > maximum) throw new InvalidDataException("HTTP response exceeds 256 KB.");
        return amount;
    }
    public override bool CanRead => inner.CanRead;
    public override bool CanSeek => false;
    public override bool CanWrite => false;
    public override long Length => throw new NotSupportedException();
    public override long Position { get => throw new NotSupportedException(); set => throw new NotSupportedException(); }
    public override void Flush() => throw new NotSupportedException();
    public override long Seek(long offset, SeekOrigin origin) => throw new NotSupportedException();
    public override void SetLength(long value) => throw new NotSupportedException();
    public override void Write(byte[] buffer, int offset, int count) => throw new NotSupportedException();
}

internal static partial class Program
{
    [GeneratedRegex(@"\bexport\s+(?:default\s+)?(?:async\s+)?function\s+update", RegexOptions.CultureInvariant)]
    private static partial Regex ExportedUpdate();

    private static int Main()
    {
        try
        {
            var input = Console.In.ReadToEnd();
            if (input.Length is 0 or > 400_000) throw new InvalidDataException("Invalid host request size.");
            var request = JsonSerializer.Deserialize<HostRequest>(input, JsonOptions())
                          ?? throw new InvalidDataException("Invalid host request.");
            if (request.Source.Length > 262_144) throw new InvalidDataException("Provider source exceeds 256 KB.");

            var source = ExportedUpdate().Replace(request.Source, "function update");
            // The public API looks asynchronous, while broker calls are deliberately
            // synchronous inside this short-lived host process. This keeps promises
            // and continuations out of the IPC contract.
            source = Regex.Replace(source, @"\bawait\s+", "", RegexOptions.CultureInvariant);
            var engine = new Engine(options => options
                .TimeoutInterval(TimeSpan.FromMilliseconds(200))
                .LimitMemory(64_000_000)
                .MaxStatements(100_000)
                .Strict());
            var broker = new BrokerHttp(request.NetworkHosts);
            engine.SetValue("__httpGetJson", new Func<string, object>(broker.GetJson));
            var settingsJson = request.Settings.ToJsonString();
            var systemMetricsJson = (request.SystemMetrics ?? new JsonObject()).ToJsonString();
            engine.Execute($$"""
                var ctx = Object.freeze({
                  settings: Object.freeze({{settingsJson}}),
                  systemMetrics: Object.freeze({{systemMetricsJson}}),
                  nowUnix: {{DateTimeOffset.UtcNow.ToUnixTimeSeconds()}},
                  http: Object.freeze({ getJson: __httpGetJson })
                });
                """);
            engine.Execute(source);
            var value = engine.Invoke("update", engine.GetValue("ctx"));
            var json = JsonSerializer.Serialize(value.ToObject(), JsonOptions());
            if (json.Length > 65_536) throw new InvalidDataException("Provider result exceeds 64 KB.");
            Console.Out.Write(json);
            return 0;
        }
        catch (Exception ex)
        {
            Console.Error.Write(ex.Message);
            return 1;
        }
    }

    private static JsonSerializerOptions JsonOptions() => new()
    {
        PropertyNameCaseInsensitive = true
    };
}
