using System.Runtime.InteropServices;
using System.Text;
using System.Diagnostics;

namespace TaskbarWidgets.Loader;

internal static class MediaWorker
{
    private const byte VkMediaPlayPause = 0xB3;
    private const uint KeyeventfKeyup = 0x0002;

    private static readonly string AppDirectory = AppPaths.AppDirectory;
    private static readonly string LogsDirectory = Path.Combine(AppDirectory, "Logs");
    private static readonly string LogPath = Path.Combine(LogsDirectory, "loader.log");
    private static readonly string HelperPath = Path.Combine(AppPaths.InstallDirectory, "TaskbarWidgets.MediaHelper.exe");

    public static void RequestToggle()
    {
        try
        {
            if (TryToggleWithHelper())
            {
                return;
            }

            SendMediaKeyFallback();
        }
        catch (Exception ex)
        {
            Log($"Media toggle failed: {ex.Message}");
        }
    }

    public static async Task RunAsync(CancellationToken cancellationToken)
    {
        Directory.CreateDirectory(AppDirectory);
        Directory.CreateDirectory(LogsDirectory);

        Process? helper = null;

        try
        {
            while (!cancellationToken.IsCancellationRequested)
            {
                try
                {
                    if (helper is null || helper.HasExited)
                    {
                        helper?.Dispose();
                        helper = File.Exists(HelperPath) ? StartHelper("--watch") : null;
                        if (helper is null)
                        {
                            Log("Media helper missing; media key fallback only");
                        }
                    }
                }
                catch (Exception ex)
                {
                    Log($"Media helper start failed: {ex.Message}");
                    helper?.Dispose();
                    helper = null;
                }

                await Task.Delay(TimeSpan.FromMilliseconds(250), cancellationToken);
            }
        }
        finally
        {
            try
            {
                if (helper is not null && !helper.HasExited)
                {
                    helper.Kill(entireProcessTree: true);
                    helper.WaitForExit(3000);
                    Log("Media helper stopped");
                }
            }
            catch
            {
                // Best effort shutdown.
            }
            finally
            {
                helper?.Dispose();
            }
        }
    }

    private static Process? StartHelper(string mode)
    {
        var info = new ProcessStartInfo
        {
            FileName = HelperPath,
            Arguments = $"{mode} --app-dir \"{AppDirectory}\"",
            UseShellExecute = true,
            WorkingDirectory = AppPaths.InstallDirectory,
            WindowStyle = ProcessWindowStyle.Hidden
        };

        var process = Process.Start(info);
        Log($"Media helper started: {mode}; pid={process?.Id.ToString() ?? "unknown"}");
        return process;
    }

    private static bool TryToggleWithHelper()
    {
        if (!File.Exists(HelperPath))
        {
            return false;
        }

        try
        {
            using var process = Process.Start(new ProcessStartInfo
            {
                FileName = HelperPath,
                Arguments = $"--toggle --app-dir \"{AppDirectory}\"",
                UseShellExecute = false,
                CreateNoWindow = true,
                WorkingDirectory = AppPaths.InstallDirectory,
                WindowStyle = ProcessWindowStyle.Hidden
            });

            if (process is null)
            {
                return false;
            }

            if (!process.WaitForExit(1200))
            {
                try
                {
                    process.Kill(entireProcessTree: true);
                }
                catch
                {
                    // Best effort cleanup before fallback.
                }

                return false;
            }

            if (process.ExitCode == 0)
            {
                Log("Media helper toggle requested");
                return true;
            }
        }
        catch (Exception ex)
        {
            Log($"Media helper toggle failed: {ex.Message}");
        }

        return false;
    }

    private static void SendMediaKeyFallback()
    {
        keybd_event(VkMediaPlayPause, 0, 0, UIntPtr.Zero);
        keybd_event(VkMediaPlayPause, 0, KeyeventfKeyup, UIntPtr.Zero);
        Log("Media play/pause key fallback sent");
    }

    private static void Log(string message)
    {
        try
        {
            File.AppendAllText(
                LogPath,
                $"{DateTimeOffset.Now:yyyy-MM-dd HH:mm:ss.fff} {message}{Environment.NewLine}",
                Encoding.UTF8);
        }
        catch
        {
            // Logging must never stop command handling.
        }
    }

    [DllImport("user32.dll", SetLastError = false)]
    private static extern void keybd_event(
        byte bVk,
        byte bScan,
        uint dwFlags,
        UIntPtr dwExtraInfo);
}
