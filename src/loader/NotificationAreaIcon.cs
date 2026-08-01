using System.Runtime.InteropServices;

namespace TaskbarWidgets.Loader;

internal sealed class NotificationAreaIcon : IDisposable
{
    private const uint CallbackMessage = 0x8001;
    private const uint OpenSettingsCommand = 1;
    private const uint ToggleWidgetsCommand = 2;
    private const uint IconId = 1;
    private const uint WmClose = 0x0010;
    private const uint WmDestroy = 0x0002;
    private const uint WmNull = 0x0000;
    private const uint WmTimer = 0x0113;
    private const uint WmContextMenu = 0x007B;
    private const uint WmLButtonUp = 0x0202;
    private const uint WmRButtonUp = 0x0205;
    private const uint NinSelect = 0x0400;
    private const uint MfString = 0x0000;
    private const uint TpmRightButton = 0x0002;
    private const uint TpmReturnCommand = 0x0100;
    private const uint TpmNonotify = 0x0080;
    private const uint NifMessage = 0x0001;
    private const uint NifIcon = 0x0002;
    private const uint NifTip = 0x0004;
    private const uint NimAdd = 0x0000;
    private const uint NimModify = 0x0001;
    private const uint NimDelete = 0x0002;
    private const uint NimSetVersion = 0x0004;
    private const uint NotifyIconVersion4 = 4;
    private const nuint RegistrationHealthTimerId = 1;
    private const uint RegistrationHealthIntervalMs = 10_000;
    private const uint ImageIcon = 1;
    private const uint LrDefaultSize = 0x0040;
    private const uint LrLoadFromFile = 0x0010;
    private static readonly IntPtr DefaultApplicationIcon = new(32512);

    private readonly Action _openSettings;
    private readonly Action _toggleWidgets;
    private readonly Func<bool> _widgetsEnabled;
    private readonly Action<string> _reportError;
    private readonly ManualResetEventSlim _ready = new(false);
    private readonly WindowProcedure _windowProcedure;
    private readonly uint _taskbarCreatedMessage;
    private Thread? _thread;
    private IntPtr _window;
    private IntPtr _icon;
    private bool _ownsIcon;
    private bool _iconRegistered;
    private bool _registrationFailureReported;
    private bool _disposed;

    public NotificationAreaIcon(
        Action openSettings,
        Action toggleWidgets,
        Func<bool> widgetsEnabled,
        Action<string> reportError)
    {
        _openSettings = openSettings;
        _toggleWidgets = toggleWidgets;
        _widgetsEnabled = widgetsEnabled;
        _reportError = reportError;
        _windowProcedure = WindowProc;
        _taskbarCreatedMessage = RegisterWindowMessage("TaskbarCreated");
    }

    public void Start()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        if (_thread is not null)
        {
            return;
        }

        _thread = new Thread(RunMessageLoop)
        {
            IsBackground = true,
            Name = "TaskbarWidgets notification icon"
        };
        _thread.SetApartmentState(ApartmentState.STA);
        _thread.Start();
        _ready.Wait(TimeSpan.FromSeconds(5));
    }

    private void RunMessageLoop()
    {
        try
        {
            RunMessageLoopCore();
        }
        catch (Exception ex)
        {
            _reportError($"Notification area icon failed: {ex}");
            _ready.Set();
        }
    }

    private void RunMessageLoopCore()
    {
        var className = $"TaskbarWidgets.NotificationArea.{Environment.ProcessId}";
        var windowClass = new WindowClass
        {
            Size = (uint)Marshal.SizeOf<WindowClass>(),
            Instance = GetModuleHandle(null),
            WindowProcedure = Marshal.GetFunctionPointerForDelegate(_windowProcedure),
            ClassName = className
        };

        if (RegisterClassEx(ref windowClass) == 0)
        {
            _ready.Set();
            return;
        }

        _window = CreateWindowEx(
            0,
            className,
            "Taskbar Widgets",
            0,
            0,
            0,
            0,
            0,
            IntPtr.Zero,
            IntPtr.Zero,
            windowClass.Instance,
            IntPtr.Zero);
        if (_window == IntPtr.Zero)
        {
            _ready.Set();
            UnregisterClass(className, windowClass.Instance);
            return;
        }

        _icon = LoadProductIcon(out _ownsIcon);
        EnsureIconRegistered(forceAdd: true);
        SetTimer(
            _window,
            RegistrationHealthTimerId,
            RegistrationHealthIntervalMs,
            IntPtr.Zero);

        _ready.Set();
        while (GetMessage(out var message, IntPtr.Zero, 0, 0) > 0)
        {
            TranslateMessage(ref message);
            DispatchMessage(ref message);
        }

        if (_ownsIcon && _icon != IntPtr.Zero)
        {
            DestroyIcon(_icon);
        }
        UnregisterClass(className, windowClass.Instance);
    }

    private NotifyIconData CreateNotifyIconData() => new()
    {
        Size = (uint)Marshal.SizeOf<NotifyIconData>(),
        Window = _window,
        Id = IconId,
        Flags = NifMessage | NifIcon | NifTip,
        CallbackMessage = CallbackMessage,
        Icon = _icon,
        Tip = "Taskbar Widgets",
        Info = string.Empty,
        InfoTitle = string.Empty
    };

    private void EnsureIconRegistered(bool forceAdd)
    {
        var data = CreateNotifyIconData();
        if (!forceAdd && _iconRegistered && ShellNotifyIcon(NimModify, ref data))
        {
            return;
        }

        if (ShellNotifyIcon(NimAdd, ref data))
        {
            data.TimeoutOrVersion = NotifyIconVersion4;
            ShellNotifyIcon(NimSetVersion, ref data);
            if (_registrationFailureReported)
            {
                _reportError("Notification area icon registration recovered");
            }
            _iconRegistered = true;
            _registrationFailureReported = false;
            return;
        }

        _iconRegistered = false;
        if (!_registrationFailureReported)
        {
            _reportError(
                $"Notification area icon registration failed; will retry: {Marshal.GetLastWin32Error()}");
            _registrationFailureReported = true;
        }
    }

    private static IntPtr LoadProductIcon(out bool ownsIcon)
    {
        ownsIcon = false;
        var processPath = Environment.ProcessPath;
        if (!string.IsNullOrWhiteSpace(processPath))
        {
            var loaded = LoadImage(
                IntPtr.Zero,
                processPath,
                ImageIcon,
                0,
                0,
                LrDefaultSize | LrLoadFromFile);
            if (loaded != IntPtr.Zero)
            {
                ownsIcon = true;
                return loaded;
            }

            if (ExtractIconEx(processPath, 0, out var large, out var small, 1) > 0)
            {
                if (small != IntPtr.Zero)
                {
                    if (large != IntPtr.Zero)
                    {
                        DestroyIcon(large);
                    }
                    ownsIcon = true;
                    return small;
                }
                if (large != IntPtr.Zero)
                {
                    ownsIcon = true;
                    return large;
                }
            }
        }

        return LoadIcon(IntPtr.Zero, DefaultApplicationIcon);
    }

    private IntPtr WindowProc(IntPtr window, uint message, IntPtr wParam, IntPtr lParam)
    {
        if (message == CallbackMessage)
        {
            var mouseMessage = unchecked((uint)lParam.ToInt64() & 0xFFFF);
            if (mouseMessage is WmLButtonUp or WmRButtonUp or WmContextMenu or NinSelect)
            {
                ShowMenu();
                return IntPtr.Zero;
            }
        }
        else if (_taskbarCreatedMessage != 0 && message == _taskbarCreatedMessage)
        {
            _iconRegistered = false;
            EnsureIconRegistered(forceAdd: true);
            return IntPtr.Zero;
        }
        else if (message == WmTimer && unchecked((nuint)wParam.ToInt64()) == RegistrationHealthTimerId)
        {
            EnsureIconRegistered(forceAdd: false);
            return IntPtr.Zero;
        }
        else if (message == WmClose)
        {
            DestroyWindow(window);
            return IntPtr.Zero;
        }
        else if (message == WmDestroy)
        {
            KillTimer(window, RegistrationHealthTimerId);
            var data = CreateNotifyIconData();
            ShellNotifyIcon(NimDelete, ref data);
            _iconRegistered = false;
            PostQuitMessage(0);
            return IntPtr.Zero;
        }

        return DefWindowProc(window, message, wParam, lParam);
    }

    private void ShowMenu()
    {
        var menu = CreatePopupMenu();
        if (menu == IntPtr.Zero)
        {
            return;
        }

        try
        {
            AppendMenu(menu, MfString, OpenSettingsCommand, "Open Settings");
            AppendMenu(
                menu,
                MfString,
                ToggleWidgetsCommand,
                _widgetsEnabled() ? "Disable Widgets" : "Enable Widgets");
            GetCursorPos(out var point);
            SetForegroundWindow(_window);
            var command = TrackPopupMenu(
                menu,
                TpmRightButton | TpmReturnCommand | TpmNonotify,
                point.X,
                point.Y,
                0,
                _window,
                IntPtr.Zero);
            PostMessage(_window, WmNull, IntPtr.Zero, IntPtr.Zero);
            if (command == OpenSettingsCommand)
            {
                _openSettings();
            }
            else if (command == ToggleWidgetsCommand)
            {
                _toggleWidgets();
            }
        }
        finally
        {
            DestroyMenu(menu);
        }
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }
        _disposed = true;

        var window = _window;
        if (window != IntPtr.Zero)
        {
            PostMessage(window, WmClose, IntPtr.Zero, IntPtr.Zero);
        }
        _thread?.Join(TimeSpan.FromSeconds(3));
        _ready.Dispose();
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct WindowClass
    {
        public uint Size;
        public uint Style;
        public IntPtr WindowProcedure;
        public int ClassExtra;
        public int WindowExtra;
        public IntPtr Instance;
        public IntPtr Icon;
        public IntPtr Cursor;
        public IntPtr Background;
        public string? MenuName;
        public string ClassName;
        public IntPtr SmallIcon;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct NativeMessage
    {
        public IntPtr Window;
        public uint Message;
        public IntPtr WParam;
        public IntPtr LParam;
        public uint Time;
        public Point Point;
        public uint Private;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct Point
    {
        public int X;
        public int Y;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct NotifyIconData
    {
        public uint Size;
        public IntPtr Window;
        public uint Id;
        public uint Flags;
        public uint CallbackMessage;
        public IntPtr Icon;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)]
        public string Tip;
        public uint State;
        public uint StateMask;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)]
        public string Info;
        public uint TimeoutOrVersion;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)]
        public string InfoTitle;
        public uint InfoFlags;
        public Guid GuidItem;
        public IntPtr BalloonIcon;
    }

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    private delegate IntPtr WindowProcedure(
        IntPtr window,
        uint message,
        IntPtr wParam,
        IntPtr lParam);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern ushort RegisterClassEx(ref WindowClass windowClass);

    [DllImport(
        "user32.dll",
        EntryPoint = "RegisterWindowMessageW",
        CharSet = CharSet.Unicode,
        ExactSpelling = true,
        SetLastError = true)]
    private static extern uint RegisterWindowMessage(string message);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool UnregisterClass(string className, IntPtr instance);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr CreateWindowEx(
        uint extendedStyle,
        string className,
        string windowName,
        uint style,
        int x,
        int y,
        int width,
        int height,
        IntPtr parent,
        IntPtr menu,
        IntPtr instance,
        IntPtr parameter);

    [DllImport("user32.dll")]
    private static extern IntPtr DefWindowProc(
        IntPtr window,
        uint message,
        IntPtr wParam,
        IntPtr lParam);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool DestroyWindow(IntPtr window);

    [DllImport("user32.dll")]
    private static extern int GetMessage(
        out NativeMessage message,
        IntPtr window,
        uint minimum,
        uint maximum);

    [DllImport("user32.dll")]
    private static extern bool TranslateMessage(ref NativeMessage message);

    [DllImport("user32.dll")]
    private static extern IntPtr DispatchMessage(ref NativeMessage message);

    [DllImport("user32.dll")]
    private static extern void PostQuitMessage(int exitCode);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool PostMessage(
        IntPtr window,
        uint message,
        IntPtr wParam,
        IntPtr lParam);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern nuint SetTimer(
        IntPtr window,
        nuint timerId,
        uint intervalMilliseconds,
        IntPtr timerProcedure);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool KillTimer(IntPtr window, nuint timerId);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern IntPtr CreatePopupMenu();

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool AppendMenu(
        IntPtr menu,
        uint flags,
        uint item,
        string text);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool DestroyMenu(IntPtr menu);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern uint TrackPopupMenu(
        IntPtr menu,
        uint flags,
        int x,
        int y,
        int reserved,
        IntPtr window,
        IntPtr rectangle);

    [DllImport("user32.dll")]
    private static extern bool GetCursorPos(out Point point);

    [DllImport("user32.dll")]
    private static extern bool SetForegroundWindow(IntPtr window);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr LoadImage(
        IntPtr instance,
        string name,
        uint type,
        int desiredWidth,
        int desiredHeight,
        uint loadFlags);

    [DllImport("user32.dll")]
    private static extern IntPtr LoadIcon(IntPtr instance, IntPtr iconName);

    [DllImport("user32.dll")]
    private static extern bool DestroyIcon(IntPtr icon);

    [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
    private static extern uint ExtractIconEx(
        string file,
        int iconIndex,
        out IntPtr largeIcon,
        out IntPtr smallIcon,
        uint iconCount);

    [DllImport(
        "shell32.dll",
        EntryPoint = "Shell_NotifyIconW",
        CharSet = CharSet.Unicode,
        ExactSpelling = true,
        SetLastError = true)]
    private static extern bool ShellNotifyIcon(uint message, ref NotifyIconData data);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr GetModuleHandle(string? moduleName);
}
