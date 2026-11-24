// The setup's work (see Setup.cs for the plan): a Windows desktop nobody sees, the agent that drives RPCS3 on it,
// and the steps in order (SetupRunner). [GT6 0089]
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Windows.Automation;

namespace GT6Launcher
{
    // A Windows desktop of its own. A program started on it opens its windows there: they are never on the user's
    // screen and never take the keyboard. The desktop goes away with the last handle to it.
    public sealed class HiddenDesktop : IDisposable
    {
        [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        static extern IntPtr CreateDesktopW(string name, IntPtr device, IntPtr devmode, int flags, uint access, IntPtr security);
        [DllImport("user32.dll")]
        static extern bool CloseDesktop(IntPtr desktop);
        [DllImport("kernel32.dll")]
        internal static extern uint SetErrorMode(uint mode);

        public readonly string Name;
        IntPtr handle;

        public HiddenDesktop()
        {
            Name = "GT6Setup_" + Process.GetCurrentProcess().Id;
            handle = CreateDesktopW(Name, IntPtr.Zero, IntPtr.Zero, 0, 0x10000000 /* GENERIC_ALL */, IntPtr.Zero);
            if (handle == IntPtr.Zero)
                throw new Win32Exception(Marshal.GetLastWin32Error());
        }

        public const int TimedOut = -1, Cancelled = -2;

        // Runs a program on the desktop and waits for it; TimedOut / Cancelled when it had to be ended.
        public int Run(string exe, string arguments, string directory, int timeoutMs, Func<bool> cancelled)
        {
            return RunOn(Name, exe, arguments, directory, timeoutMs, cancelled);
        }

        // Runs a program on the named desktop (null = current/normal desktop) with the same wait/cancel/job handling.
        public static int RunOn(string desktopName, string exe, string arguments, string directory, int timeoutMs, Func<bool> cancelled)
        {
            // Suppress Windows "application was unable to start correctly" popups on hidden-desktop probes
            // (inherited by child processes so a 0xC0000142 DLL-init failure falls back silently).
            uint prevMode = SetErrorMode(0x0001u | 0x0002u);   // SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX
            try
            {
                using (Child child = Child.Start(exe, arguments, directory, desktopName, false, null, null, null))
                {
                    var watch = Stopwatch.StartNew();
                    while (!child.Wait(250))
                    {
                        bool stop = cancelled != null && cancelled();
                        if (stop || watch.ElapsedMilliseconds > timeoutMs)
                        {
                            child.Kill();
                            return stop ? Cancelled : TimedOut;
                        }
                    }
                    return child.ExitCode;
                }
            }
            finally
            {
                SetErrorMode(prevMode);
            }
        }

        public void Dispose()
        {
            if (handle != IntPtr.Zero)
                CloseDesktop(handle);
            handle = IntPtr.Zero;
        }
    }

    // A program started with what the .NET Process class does not offer together: a desktop of its own, a main window
    // that stays hidden, its output straight into files, an environment of its own.
    public sealed class Child : IDisposable
    {
        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        struct STARTUPINFO
        {
            public int cb;
            public string reserved, desktop, title;
            public int x, y, xSize, ySize, xChars, yChars, fill, flags;
            public short show, reserved2;
            public IntPtr reserved3, stdin, stdout, stderr;
        }

        [StructLayout(LayoutKind.Sequential)]
        struct PROCESS_INFORMATION
        {
            public IntPtr process, thread;
            public uint pid, tid;
        }

        [StructLayout(LayoutKind.Sequential)]
        struct SECURITY_ATTRIBUTES
        {
            public int length;
            public IntPtr descriptor;
            public int inherit;
        }

        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        static extern bool CreateProcessW(string application, StringBuilder commandLine, IntPtr processSecurity, IntPtr threadSecurity, bool inherit, uint flags,
                                          IntPtr environment, string directory, ref STARTUPINFO startup, out PROCESS_INFORMATION information);
        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        static extern IntPtr CreateFileW(string name, uint access, uint share, ref SECURITY_ATTRIBUTES security, uint disposition, uint flags, IntPtr template);
        [DllImport("kernel32.dll")]
        static extern uint WaitForSingleObject(IntPtr handle, uint milliseconds);
        [DllImport("kernel32.dll")]
        static extern bool GetExitCodeProcess(IntPtr handle, out uint code);
        [DllImport("kernel32.dll")]
        static extern bool TerminateProcess(IntPtr handle, uint code);
        [DllImport("kernel32.dll")]
        static extern bool CloseHandle(IntPtr handle);
        [DllImport("kernel32.dll")]
        static extern uint ResumeThread(IntPtr thread);

        IntPtr process;
        public readonly int Id;

        Child(IntPtr process, int id)
        {
            this.process = process;
            Id = id;
        }

        // A file (or NUL) as a handle the new program inherits.
        static IntPtr Open(string name, bool write)
        {
            var security = new SECURITY_ATTRIBUTES { length = Marshal.SizeOf(typeof(SECURITY_ATTRIBUTES)), inherit = 1 };
            IntPtr file = CreateFileW(name, write ? 0x40000000u : 0x80000000u, 7, ref security, write && name != "NUL" ? 2u : 3u, 0x80, IntPtr.Zero);
            if (file == new IntPtr(-1))
                throw new Win32Exception(Marshal.GetLastWin32Error(), name);
            return file;
        }

        // desktop: null = this one. hidden: the program's first ShowWindow hides its window (as "start /min" would
        // minimise it). environment: null = this process's. outFile / errFile: the program's output goes there.
        public static Child Start(string exe, string arguments, string directory, string desktop, bool hidden,
                                  IDictionary<string, string> environment, string outFile, string errFile)
        {
            var startup = new STARTUPINFO();
            startup.cb = Marshal.SizeOf(typeof(STARTUPINFO));
            startup.desktop = desktop;
            if (hidden)
            {
                startup.flags |= 1;   // STARTF_USESHOWWINDOW
                startup.show = 0;     // SW_HIDE
            }
            var files = new List<IntPtr>();
            GCHandle block = default(GCHandle);
            try
            {
                bool inherit = false;
                if (outFile != null || errFile != null)
                {
                    files.Add(startup.stdin = Open("NUL", false));
                    files.Add(startup.stdout = Open(outFile ?? "NUL", true));
                    files.Add(startup.stderr = Open(errFile ?? "NUL", true));
                    startup.flags |= 0x100;   // STARTF_USESTDHANDLES
                    inherit = true;
                }
                uint flags = 0x08000000 | 0x4;   // CREATE_NO_WINDOW: no console window; CREATE_SUSPENDED: into the job first
                IntPtr environmentBlock = IntPtr.Zero;
                if (environment != null)
                {
                    var text = new StringBuilder();
                    foreach (KeyValuePair<string, string> pair in environment.OrderBy(p => p.Key, StringComparer.OrdinalIgnoreCase))
                        text.Append(pair.Key).Append('=').Append(pair.Value).Append('\0');
                    text.Append('\0');
                    block = GCHandle.Alloc(Encoding.Unicode.GetBytes(text.ToString()), GCHandleType.Pinned);
                    environmentBlock = block.AddrOfPinnedObject();
                    flags |= 0x400;           // CREATE_UNICODE_ENVIRONMENT
                }
                PROCESS_INFORMATION information;
                var command = new StringBuilder("\"" + exe + "\"" + (string.IsNullOrEmpty(arguments) ? "" : " " + arguments));
                if (!CreateProcessW(null, command, IntPtr.Zero, IntPtr.Zero, inherit, flags, environmentBlock, directory, ref startup, out information))
                    throw new Win32Exception(Marshal.GetLastWin32Error());
                SetupJob.Add(information.process);
                ResumeThread(information.thread);
                CloseHandle(information.thread);
                return new Child(information.process, (int)information.pid);
            }
            finally
            {
                foreach (IntPtr file in files)
                    CloseHandle(file);
                if (block.IsAllocated)
                    block.Free();
            }
        }

        public bool Wait(int milliseconds)
        {
            return WaitForSingleObject(process, (uint)milliseconds) == 0;
        }

        public bool Exited { get { return Wait(0); } }

        public int ExitCode
        {
            get
            {
                uint code;
                GetExitCodeProcess(process, out code);
                return (int)code;
            }
        }

        public void Kill()
        {
            if (!Exited)
            {
                TerminateProcess(process, 1);
                Wait(5000);
            }
        }

        public void Dispose()
        {
            if (process != IntPtr.Zero)
                CloseHandle(process);
            process = IntPtr.Zero;
        }
    }

    // The programs the setup starts are in a job that Windows ends with the launcher: a launcher that is closed, or ends
    // any other way, leaves no RPCS3 and no game behind (the agent's RPCS3 is in the job too, as the agent's child).
    static class SetupJob
    {
        [StructLayout(LayoutKind.Sequential)]
        struct BASIC
        {
            public long processTime, jobTime;
            public uint flags;
            public UIntPtr minimumSet, maximumSet;
            public uint processes;
            public UIntPtr affinity;
            public uint priority, scheduling;
        }

        [StructLayout(LayoutKind.Sequential)]
        struct IO
        {
            public ulong reads, writes, others, readBytes, writeBytes, otherBytes;
        }

        [StructLayout(LayoutKind.Sequential)]
        struct EXTENDED
        {
            public BASIC basic;
            public IO io;
            public UIntPtr processMemory, jobMemory, peakProcessMemory, peakJobMemory;
        }

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
        static extern IntPtr CreateJobObjectW(IntPtr security, string name);
        [DllImport("kernel32.dll")]
        static extern bool SetInformationJobObject(IntPtr job, int infoClass, ref EXTENDED info, int length);
        [DllImport("kernel32.dll")]
        static extern bool AssignProcessToJobObject(IntPtr job, IntPtr process);

        static IntPtr job;
        static readonly object gate = new object();

        public static void Add(IntPtr process)
        {
            try
            {
                lock (gate)
                {
                    if (job == IntPtr.Zero)
                    {
                        job = CreateJobObjectW(IntPtr.Zero, null);
                        var info = new EXTENDED();
                        info.basic.flags = 0x2000;   // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
                        SetInformationJobObject(job, 9 /* JobObjectExtendedLimitInformation */, ref info, Marshal.SizeOf(typeof(EXTENDED)));
                    }
                }
                AssignProcessToJobObject(job, process);   // (best effort: the setup works without it)
            }
            catch (Exception)
            {
            }
        }
    }

    // The launcher exe in agent mode, started ON the hidden desktop (GT6 Launcher.exe --setup-agent <what> ...): it
    // starts RPCS3 there for one job, answers its dialog through UI Automation, waits for the line in RPCS3's log that
    // says the job is done, ends RPCS3 and exits with 0, or with the reason:
    public static class SetupAgent
    {
        public const int Ok = 0, NoDialog = 2, Timeout = 3, Failed = 4, WrongResult = 5, BadCall = 6;

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        static extern IntPtr FindWindowW(string className, string title);
        [DllImport("user32.dll")]
        static extern bool PostMessageW(IntPtr window, uint message, IntPtr w, IntPtr l);

        static string logFile;

        static void Note(string text)
        {
            try
            {
                if (logFile != null)
                    File.AppendAllText(logFile, DateTime.Now.ToString("HH:mm:ss.fff") + " agent " + text + "\r\n");
            }
            catch (Exception)
            {
            }
        }

        public static int Run(Launcher launcher, string[] args)
        {
            try
            {
                HiddenDesktop.SetErrorMode(0x0001u | 0x0002u);   // SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX
                Directory.CreateDirectory(launcher.LauncherLogsDir);
                logFile = Path.Combine(launcher.LauncherLogsDir, "setup.log");
                if (args.Length >= 3 && args[0] == "fw")
                    return Firmware(args[1], args[2]);
                if (args.Length >= 3 && args[0] == "pkg")
                    return Package(args[1], args[2]);
                if (args.Length >= 3 && args[0] == "dec")
                    return Decrypt(args[1], args[2], null) ? Ok : Failed;
                if (args.Length >= 5 && args[0] == "key")
                    return Key(args[1], args[2], args[3], args[4]);
                return BadCall;
            }
            catch (Exception error)
            {
                Note("failed: " + error.GetType().Name + ": " + error.Message);
                return Failed;
            }
        }

        static Process Start(string tool, string arguments)
        {
            // (no shell, no desktop named: the child opens its windows on this process's desktop - the hidden one)
            var info = new ProcessStartInfo(Path.Combine(tool, "rpcs3.exe"), arguments) { WorkingDirectory = tool, UseShellExecute = false };
            return Process.Start(info);
        }

        static void End(Process process)
        {
            try
            {
                if (!process.HasExited)
                    process.Kill();
                process.WaitForExit(5000);
            }
            catch (Exception)
            {
            }
        }

        // RPCS3's log of this run (it starts the file anew each time), read while RPCS3 writes it.
        static string Log(string tool)
        {
            try
            {
                using (var f = new FileStream(Path.Combine(tool, "log", "RPCS3.log"), FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
                using (var reader = new StreamReader(f, Encoding.UTF8))
                    return reader.ReadToEnd();
            }
            catch (IOException)
            {
                return "";
            }
        }

        static AutomationElement Named(AutomationElement window, string name)
        {
            return window.FindFirst(TreeScope.Descendants, new PropertyCondition(AutomationElement.NameProperty, name));
        }

        static bool Press(AutomationElement window, string name)
        {
            AutomationElement button = Named(window, name);
            object pattern;
            if (button == null || !button.TryGetCurrentPattern(InvokePattern.Pattern, out pattern))
                return false;
            ((InvokePattern)pattern).Invoke();
            return true;
        }

        static IntPtr WaitWindow(string title, Process process, int timeoutMs)
        {
            var watch = Stopwatch.StartNew();
            while (watch.ElapsedMilliseconds < timeoutMs && !process.HasExited)
            {
                IntPtr window = FindWindowW(null, title);
                if (window != IntPtr.Zero)
                    return window;
                Thread.Sleep(150);
            }
            return IntPtr.Zero;
        }

        // "RPCS3 Firmware Installer": "Install firmware: <file>?" - Yes. Done when the log says so (a few seconds).
        static int Firmware(string tool, string pup)
        {
            Process process = Start(tool, "--installfw \"" + pup + "\"");
            try
            {
                IntPtr window = WaitWindow("RPCS3 Firmware Installer", process, 60000);
                if (window == IntPtr.Zero)
                {
                    Note("firmware: no question box (" + (process.HasExited ? "rpcs3 exited 0x" + process.ExitCode.ToString("X8") : "timed out") + ")");
                    return NoDialog;
                }
                Thread.Sleep(300);
                var watch = Stopwatch.StartNew();
                int answers = 0;
                double nextAnswer = 0;
                while (watch.Elapsed.TotalSeconds < 600 && !process.HasExited)
                {
                    string log = Log(tool);
                    if (log.Contains("Successfully installed PS3 firmware"))
                    {
                        Note("firmware: installed in " + watch.Elapsed.TotalSeconds.ToString("0.0") + " s (" + answers + " answer(s))");
                        return Ok;
                    }
                    if (Regex.IsMatch(log, "Firmware installation failed|Failed to install firmware", RegexOptions.IgnoreCase))
                    {
                        Note("firmware: RPCS3 reports a failure");
                        return Failed;
                    }
                    // The answer: the box's own key for Yes first (the way that was tried), the button by name next,
                    // again every few seconds while the box is still there.
                    if (answers < 6 && watch.Elapsed.TotalSeconds >= nextAnswer && (window = FindWindowW(null, "RPCS3 Firmware Installer")) != IntPtr.Zero)
                    {
                        bool pressed = false;
                        if (answers % 2 == 1)
                        {
                            try
                            {
                                pressed = Press(AutomationElement.FromHandle(window), "Yes");
                            }
                            catch (Exception)
                            {
                            }
                        }
                        if (!pressed)
                        {
                            PostMessageW(window, 0x100, (IntPtr)0x59, IntPtr.Zero);
                            PostMessageW(window, 0x101, (IntPtr)0x59, (IntPtr)unchecked((int)0xC0000001));
                        }
                        answers++;
                        nextAnswer = watch.Elapsed.TotalSeconds + 4;
                    }
                    Thread.Sleep(250);
                }
                Note("firmware: not installed in time");
                return Timeout;
            }
            finally
            {
                End(process);
            }
        }

        // "PKG Installation": "Precompile caches" off (it would compile the game for the emulator), Install.
        static int Package(string tool, string package)
        {
            Process process = Start(tool, "--installpkg \"" + package + "\"");
            try
            {
                IntPtr window = WaitWindow("PKG Installation", process, 60000);
                if (window == IntPtr.Zero)
                {
                    Note("package: no dialog for " + Path.GetFileName(package));
                    return NoDialog;
                }
                AutomationElement dialog = AutomationElement.FromHandle(window);
                foreach (string box in new[] { "Precompile caches", "Add desktop shortcut(s)", "Add Start menu shortcut(s)", "Add Steam Shortcut(s) (Steam must be closed)" })
                {
                    AutomationElement check = Named(dialog, box);
                    object pattern;
                    if (check != null && check.TryGetCurrentPattern(TogglePattern.Pattern, out pattern) && ((TogglePattern)pattern).Current.ToggleState == ToggleState.On)
                        ((TogglePattern)pattern).Toggle();
                }
                if (!Press(dialog, "Install"))
                {
                    Note("package: no Install button");
                    return NoDialog;
                }
                var watch = Stopwatch.StartNew();
                while (watch.Elapsed.TotalSeconds < 1800 && !process.HasExited)
                {
                    string log = Log(tool);
                    if (log.Contains("Successfully installed"))
                    {
                        Note("package: " + Path.GetFileName(package) + " installed in " + watch.Elapsed.TotalSeconds.ToString("0.0") + " s");
                        Thread.Sleep(300);
                        return Ok;
                    }
                    Match failure = Regex.Match(log, "Failed to install[^\r\n]*|Installation failed[^\r\n]*|PKG decryption failed[^\r\n]*");
                    if (failure.Success)
                    {
                        Note("package: " + failure.Value);
                        return Failed;
                    }
                    Thread.Sleep(200);
                }
                Note("package: " + Path.GetFileName(package) + " not installed in time");
                return Timeout;
            }
            finally
            {
                End(process);
            }
        }

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool AllocConsole();
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool AttachConsole(uint pid);
        [DllImport("kernel32.dll")]
        static extern bool FreeConsole();
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern IntPtr CreateFileW(string name, uint access, uint share, IntPtr security, uint disposition, uint flags, IntPtr template);
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool WriteConsoleInputW(IntPtr input, KeyRecord[] records, int count, out int written);
        [DllImport("kernel32.dll")]
        static extern bool CloseHandle(IntPtr handle);

        delegate bool EnumWindowsProc(IntPtr hwnd, IntPtr lParam);
        [DllImport("user32.dll")]
        static extern bool EnumWindows(EnumWindowsProc callback, IntPtr lParam);
        [DllImport("user32.dll")]
        static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        static extern int GetClassNameW(IntPtr hwnd, StringBuilder text, int maxCount);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        static extern int GetWindowTextW(IntPtr hwnd, StringBuilder text, int maxCount);

        [StructLayout(LayoutKind.Explicit, CharSet = CharSet.Unicode)]
        struct KeyRecord   // INPUT_RECORD holding a KEY_EVENT_RECORD
        {
            [FieldOffset(0)] public ushort eventType;
            [FieldOffset(4)] public int keyDown;
            [FieldOffset(8)] public ushort repeat;
            [FieldOffset(10)] public ushort virtualKey;
            [FieldOffset(12)] public ushort scanCode;
            [FieldOffset(14)] public char character;
            [FieldOffset(16)] public uint controlKeys;
        }

        // Types a line into the currently attached console's CONIN$ buffer.
        static bool TypeIntoConsole(string line)
        {
            IntPtr input = CreateFileW("CONIN$", 0xC0000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero);
            if (input == new IntPtr(-1))
                return false;
            try
            {
                var records = new List<KeyRecord>();
                foreach (char c in line + "\r")
                {
                    ushort vk = (ushort)(c == '\r' ? 0x0D : (c <= 0x7F ? (int)char.ToUpperInvariant(c) : 0));
                    for (int down = 1; down >= 0; down--)
                        records.Add(new KeyRecord { eventType = 1, keyDown = down, repeat = 1, virtualKey = vk, character = c });
                }
                int written;
                return WriteConsoleInputW(input, records.ToArray(), records.Count, out written) && written > 0;
            }
            finally
            {
                CloseHandle(input);
            }
        }

        // Fallback when AttachConsole fails (e.g. terminal host boundary): find the console window for RPCS3 and post WM_CHAR.
        static IntPtr FindConsoleWindow(int pid)
        {
            IntPtr byPid = IntPtr.Zero, byTitle = IntPtr.Zero;
            EnumWindows(delegate(IntPtr hwnd, IntPtr lp)
            {
                uint wpid;
                GetWindowThreadProcessId(hwnd, out wpid);
                var cls = new StringBuilder(64);
                GetClassNameW(hwnd, cls, cls.Capacity);
                string c = cls.ToString();
                if (wpid == (uint)pid && (c == "ConsoleWindowClass" || c == "CASCADIA_HOSTING_WINDOW_CLASS"))
                {
                    byPid = hwnd;
                    return false;
                }
                if (c == "ConsoleWindowClass" || c == "CASCADIA_HOSTING_WINDOW_CLASS")
                {
                    var title = new StringBuilder(256);
                    GetWindowTextW(hwnd, title, title.Capacity);
                    if (title.ToString().IndexOf("rpcs3", StringComparison.OrdinalIgnoreCase) >= 0)
                        byTitle = hwnd;
                }
                return true;
            }, IntPtr.Zero);
            return byPid != IntPtr.Zero ? byPid : byTitle;
        }

        static bool PostKeysToConsoleWindow(int pid, string line)
        {
            IntPtr hwnd = FindConsoleWindow(pid);
            if (hwnd == IntPtr.Zero)
                return false;
            foreach (char c in line)
            {
                PostMessageW(hwnd, 0x0102 /* WM_CHAR */, (IntPtr)c, IntPtr.Zero);
                Thread.Sleep(5);
            }
            PostMessageW(hwnd, 0x0100 /* WM_KEYDOWN */, (IntPtr)0x0D, IntPtr.Zero);
            PostMessageW(hwnd, 0x0102 /* WM_CHAR */, (IntPtr)0x0D, IntPtr.Zero);
            PostMessageW(hwnd, 0x0101 /* WM_KEYUP */, (IntPtr)0x0D, (IntPtr)unchecked((int)0xC0000001));
            return true;
        }

        // rpcs3 --decrypt <file>: writes <name>.elf / <name>.prx beside the file. A file that needs a licence key is
        // asked for on a console: RPCS3 attaches to its parent's console (this agent's, on the hidden desktop) or opens
        // one of its own, and reads the key there or from standard input - it is given both ways, and nowhere else.
        static bool Decrypt(string tool, string file, string key)
        {
            var info = new ProcessStartInfo(Path.Combine(tool, "rpcs3.exe"), "--decrypt \"" + file + "\"")
            {
                WorkingDirectory = tool, UseShellExecute = false, CreateNoWindow = (key == null),
                RedirectStandardInput = true, RedirectStandardOutput = true, RedirectStandardError = true,
            };
            using (Process process = Process.Start(info))
            {
                process.OutputDataReceived += delegate { };
                process.ErrorDataReceived += delegate { };
                process.BeginOutputReadLine();
                process.BeginErrorReadLine();
                if (key != null)
                {
                    process.StandardInput.WriteLine(key);
                    process.StandardInput.Flush();
                }
                process.StandardInput.Close();
                if (key != null)
                {
                    // RPCS3 is a GUI binary that calls AllocConsole() of its own when prompting for a KLIC.
                    // Attach directly to RPCS3's own console (AttachConsole(pid)) and type the KLIC into its CONIN$,
                    // with a WM_CHAR fallback if AttachConsole is blocked (e.g. Windows Terminal host boundary).
                    var watch = Stopwatch.StartNew();
                    int consoleSent = 0, wmSent = 0;
                    long nextTry = 400;
                    bool loggedAttachErr = false;
                    while (!process.WaitForExit(250) && watch.ElapsedMilliseconds < 180000)
                    {
                        if (watch.ElapsedMilliseconds < nextTry)
                            continue;
                        if (consoleSent < 2)
                        {
                            FreeConsole();
                            if (AttachConsole((uint)process.Id))
                            {
                                Thread.Sleep(150);
                                bool ok = TypeIntoConsole(key);
                                FreeConsole();
                                consoleSent++;
                                Note("decrypt: AttachConsole TypeIntoConsole=" + ok + " (attempt " + consoleSent + ")");
                                nextTry = watch.ElapsedMilliseconds + 2000;
                                continue;
                            }
                            if (!loggedAttachErr)
                            {
                                Note("decrypt: AttachConsole failed (err " + Marshal.GetLastWin32Error() + ")");
                                loggedAttachErr = true;
                            }
                        }
                        if (watch.ElapsedMilliseconds >= 2000 && wmSent < 3)
                        {
                            if (PostKeysToConsoleWindow(process.Id, key))
                            {
                                wmSent++;
                                Note("decrypt: sent key via WM_CHAR (attempt " + wmSent + ")");
                                nextTry = watch.ElapsedMilliseconds + 2500;
                            }
                            else
                            {
                                nextTry = watch.ElapsedMilliseconds + 500;
                            }
                        }
                        else
                        {
                            nextTry = watch.ElapsedMilliseconds + 500;
                        }
                    }
                    if (!process.HasExited)
                    {
                        End(process);
                        return false;
                    }
                }
                else if (!process.WaitForExit(180000))
                {
                    End(process);
                    return false;
                }
            }
            string made = Path.ChangeExtension(file, file.EndsWith(".sprx", StringComparison.OrdinalIgnoreCase) ? ".prx" : ".elf");
            for (int i = 0; i < 20 && !File.Exists(made); i++)
                Thread.Sleep(150);
            return File.Exists(made);
        }

        // The licence key of the main executable: the game's loader hands it to the system when it asks whether the
        // executable may run. RPCS3 runs the loader (interpreter, no picture) and writes that question into its log,
        // key included; it is ended at that line - before the game itself would start - and the key is used once, here.
        static int Key(string tool, string loader, string self, string wantedSha256)
        {
            string key = null;
            bool exitedEarly = false;
            int exitCode = 0;
            Process process = Start(tool, "--no-gui \"" + loader + "\"");
            try
            {
                var watch = Stopwatch.StartNew();
                while (watch.Elapsed.TotalSeconds < 240 && !process.HasExited && key == null)
                {
                    Match match = Regex.Match(Log(tool), "KLIC=0x([0-9A-Fa-f]{32})");
                    if (match.Success)
                        key = match.Groups[1].Value;
                    else
                        Thread.Sleep(100);
                }
                if (key == null && process.HasExited)
                {
                    exitedEarly = true;
                    exitCode = process.ExitCode;
                }
            }
            finally
            {
                End(process);
            }
            try
            {
                File.Delete(Path.Combine(tool, "log", "RPCS3.log"));   // the line with the key goes with it
                File.Delete(Path.Combine(tool, "log", "RPCS3.log.gz"));
            }
            catch (Exception)
            {
            }
            if (key == null)
            {
                Note("key: the loader did not get as far as asking (" + (exitedEarly ? "rpcs3 exited 0x" + exitCode.ToString("X8") : "timed out") + ")");
                return exitedEarly ? NoDialog : Timeout;
            }
            bool made = Decrypt(tool, self, key);
            key = null;
            string elf = Path.ChangeExtension(self, ".elf");
            if (!made)
            {
                Note("key: nothing decrypted");
                return Failed;
            }
            if (Hashes.Sha256(elf) != wantedSha256)
            {
                Note("key: the decrypted executable is not the expected one");
                return WrongResult;
            }
            Note("key: executable decrypted and checked");
            return Ok;
        }
    }

    public class SetupProgress
    {
        public int Step, Steps;
        public string Title = "", Detail = "";
        public double Fraction;          // of the whole setup, 0 - 1
    }

    public class SetupFailed : Exception
    {
        public SetupFailed(string message) : base(message)
        {
        }
    }

    // The steps, in order, on a worker thread. Each one is skipped when its result is already there and right, so a
    // setup that was interrupted goes on where it stopped.
    public class SetupRunner
    {
        readonly Launcher launcher;
        volatile bool cancel;
        public event Action<SetupProgress> Progress;
        readonly SetupProgress now = new SetupProgress();
        double done, weight;             // the share of the whole that is finished / the current step's share

        public SetupRunner(Launcher launcher)
        {
            this.launcher = launcher;
        }

        public string WorkDir { get { return Path.Combine(launcher.Dir, "_setup"); } }
        string ToolDir { get { return Path.Combine(WorkDir, "rpcs3"); } }
        string DiscDir { get { return Path.Combine(launcher.GameDir, "dev_bdvd"); } }
        string UsrDir { get { return Path.Combine(launcher.TitleDir, "USRDIR"); } }
        string LogFile { get { return Path.Combine(launcher.LauncherLogsDir, "setup.log"); } }

        public void Cancel()
        {
            cancel = true;
        }

        bool Cancelled()
        {
            return cancel;
        }

        void Check()
        {
            if (cancel)
                throw new OperationCanceledException();
        }

        internal void Note(string text)
        {
            try
            {
                Directory.CreateDirectory(launcher.LauncherLogsDir);
                File.AppendAllText(LogFile, DateTime.Now.ToString("HH:mm:ss.fff") + " " + text + "\r\n");
            }
            catch (Exception)
            {
            }
        }

        void Begin(string title, double share)
        {
            Check();
            done += weight;
            weight = share;
            now.Step++;
            now.Title = title;
            now.Detail = "";
            now.Fraction = done;
            Note("step " + now.Step + ": " + title);
            Report();
        }

        void At(double fraction, string detail)
        {
            now.Fraction = done + weight * Math.Max(0, Math.Min(1, fraction));
            if (detail != null)
                now.Detail = detail;
            Report();
        }

        long lastReport;

        void Report()
        {
            long tick = Environment.TickCount;
            if (now.Fraction < 1 && tick - lastReport < 120 && tick - lastReport >= 0 && now.Detail.Length > 0)
                return;   // not more often than the eye follows
            lastReport = tick;
            Action<SetupProgress> handler = Progress;
            if (handler != null)
                handler(new SetupProgress { Step = now.Step, Steps = now.Steps, Title = now.Title, Detail = now.Detail, Fraction = now.Fraction });
        }

        static string Gb(long bytes)
        {
            return (bytes / 1073741824.0).ToString("0.0") + " GB";
        }

        // What is there already. (The data file was checked against its hash when it was copied; a folder copied off a
        // disc may lack PS3_DISC.SFB and PS3LOGO.DAT.)
        public bool DiscReady
        {
            get
            {
                string volume = Path.Combine(DiscDir, "PS3_GAME", "USRDIR", "GT.VOL");
                return File.Exists(volume) && new FileInfo(volume).Length == SetupFacts.VolumeSize && File.Exists(Path.Combine(DiscDir, "PS3_GAME", "PARAM.SFO"));
            }
        }

        public string InstalledVersion { get { return Sfo.Get(Sfo.Read(Path.Combine(launcher.TitleDir, "PARAM.SFO")), "APP_VER"); } }

        // The updates are in when the data says 1.22 and the 1.22 executable is there (in the patch folder the packages
        // leave, or in its final place once the game has merged them).
        public bool UpdatesReady
        {
            get
            {
                if (InstalledVersion != GameFiles.Version)
                    return false;
                string final = Path.Combine(UsrDir, "EMAIN.SELF"), patch = Path.Combine(UsrDir, SetupFacts.FinalSelf);
                return File.Exists(patch) || (File.Exists(final) && new FileInfo(final).Length == 9209104);
            }
        }

        public bool ExecutableReady { get { return File.Exists(launcher.Elf) && new FileInfo(launcher.Elf).Length == 23787792 && Hashes.Sha256(launcher.Elf) == SetupFacts.ElfSha256; } }

        public bool ModuleReady
        {
            get
            {
                string image = Path.Combine(launcher.GameDir, "firmware", "libsre_image.bin");
                return File.Exists(image) && Hashes.Sha256(image) == SetupFacts.LibsreImageSha256 && File.Exists(Path.Combine(launcher.GameDir, "firmware", "libsre.prx"));
            }
        }

        // The folders the update packages leave for the game to merge on its first start.
        public string[] PatchFolders
        {
            get
            {
                try
                {
                    return Directory.Exists(UsrDir) ? Directory.GetDirectories(UsrDir, "TPPS-*") : new string[0];
                }
                catch (Exception)
                {
                    return new string[0];
                }
            }
        }

        // The game's own installation is still to run (or to finish): the patch folders, their list (USRDIR\update.txt) or
        // the game's own mark of an installation under way (USRDIR\UPDATING, the last thing it removes when done).
        public bool InstallPending
        {
            get { return PatchFolders.Length > 0 || File.Exists(Path.Combine(UsrDir, "update.txt")) || File.Exists(Path.Combine(UsrDir, "UPDATING")); }
        }

        public bool Complete { get { return DiscReady && UpdatesReady && ExecutableReady && ModuleReady && !InstallPending; } }

        // Whether the play button has to wait for the setup: files missing, or the game's own installation not done
        // (the game would run it in its window and not come back from its restart).
        public static bool Needed(Launcher launcher)
        {
            return !GameFiles.Check(launcher).Ready || new SetupRunner(launcher).InstallPending;
        }

        // Free space needed on the launcher's drive for what is still to do (the temporary files included).
        public long SpaceNeeded
        {
            get
            {
                long need = 0;
                if (!DiscReady)
                    need += SetupFacts.VolumeSize;
                if (!UpdatesReady)
                {
                    CacheInfo ci = InspectCache(launcher.Value("Setup", "Cache", ""));
                    long downloadNeed = Math.Max(0, SetupFacts.UpdatesSize - ci.FoundBytes);
                    need += downloadNeed + 12L * 1073741824;                 // remaining packages to download, and what they install
                }
                if (PatchFolders.Length > 0 || !UpdatesReady)
                    need += 4L * 1073741824;                                 // the game's merge works beside its data
                return need + 1073741824;
            }
        }

        public long SpaceFree
        {
            get
            {
                try
                {
                    return new DriveInfo(Path.GetPathRoot(Path.GetFullPath(launcher.Dir))).AvailableFreeSpace;
                }
                catch (Exception)
                {
                    return long.MaxValue;
                }
            }
        }

        public class CacheInfo
        {
            public bool Exists;
            public int FoundUpdates;
            public int TotalUpdates;
            public long FoundBytes;
            public long MissingBytes;
            public bool HasFirmware;
            public bool HasTool;
        }

        static Dictionary<string, string> ScanCacheDir(string dir)
        {
            var map = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            if (string.IsNullOrEmpty(dir) || !Directory.Exists(dir))
                return map;
            var dirs = new List<string> { dir };
            try
            {
                foreach (string sub in Directory.GetDirectories(dir))
                {
                    dirs.Add(sub);
                    try
                    {
                        foreach (string sub2 in Directory.GetDirectories(sub))
                            dirs.Add(sub2);
                    }
                    catch (Exception)
                    {
                    }
                }
            }
            catch (Exception)
            {
            }
            foreach (string d in dirs)
            {
                try
                {
                    foreach (string file in Directory.GetFiles(d))
                    {
                        string name = Path.GetFileName(file);
                        if (!map.ContainsKey(name))
                            map[name] = file;
                    }
                }
                catch (Exception)
                {
                }
            }
            return map;
        }

        public static CacheInfo InspectCache(string dir)
        {
            var info = new CacheInfo { TotalUpdates = SetupFacts.Updates.Length, MissingBytes = SetupFacts.UpdatesSize };
            if (string.IsNullOrEmpty(dir) || !Directory.Exists(dir))
                return info;
            info.Exists = true;
            Dictionary<string, string> files = ScanCacheDir(dir);
            foreach (UpdatePackage u in SetupFacts.Updates)
            {
                string path;
                if (files.TryGetValue(u.FileName, out path))
                {
                    try
                    {
                        if (new FileInfo(path).Length == u.Size)
                        {
                            info.FoundUpdates++;
                            info.FoundBytes += u.Size;
                        }
                    }
                    catch (Exception)
                    {
                    }
                }
            }
            info.MissingBytes = Math.Max(0, SetupFacts.UpdatesSize - info.FoundBytes);
            string pup;
            if (files.TryGetValue("PS3UPDAT.PUP", out pup))
            {
                try
                {
                    info.HasFirmware = new FileInfo(pup).Length == SetupFacts.FirmwareSize;
                }
                catch (Exception)
                {
                }
            }
            string tool;
            if (files.TryGetValue(Path.GetFileName(SetupFacts.ToolUrl), out tool))
            {
                try
                {
                    info.HasTool = new FileInfo(tool).Length == SetupFacts.ToolSize;
                }
                catch (Exception)
                {
                }
            }
            return info;
        }

        // A folder that may hold the downloads already (launcher.ini [Setup] Cache): its files and those of its folders
        // up to two levels down.
        string cache;
        Dictionary<string, string> cached;

        string Cached(string fileName)
        {
            if (cached == null)
                cached = ScanCacheDir(cache);
            string path;
            return cached.TryGetValue(fileName, out path) ? path : null;
        }

        // Everything. Throws SetupFailed with words for the user, or OperationCanceledException.
        public void Run(GameSource source)
        {
            cancel = false;
            done = weight = 0;
            now.Step = 0;
            cache = launcher.Value("Setup", "Cache", "");
            cached = null;
            bool needDisc = !DiscReady, needUpdates = !UpdatesReady, needExe = !ExecutableReady, needModule = !ModuleReady;
            bool needTool = needUpdates || needExe || needModule, gameInstall = needUpdates || InstallPending;
            now.Steps = (needDisc ? 1 : 0) + (needTool ? 2 : 0) + (needUpdates ? 2 : 0) + (needExe ? 1 : 0) + (needModule ? 1 : 0) + (needTool ? 1 : 0) + (gameInstall ? 1 : 0);
            Note("setup starts: disc " + needDisc + ", updates " + needUpdates + " (installed " + InstalledVersion + "), executable " + needExe + ", module " + needModule +
                 ", patch folders " + PatchFolders.Length + ", cache '" + cache + "'");
            if (needDisc && (source == null || !source.Usable))
                throw new SetupFailed(L.T("게임 디스크(ISO 또는 폴더)를 먼저 골라 주세요."));
            if (SpaceFree < SpaceNeeded)
                throw new SetupFailed(L.T("디스크 공간이 부족합니다. 필요: ") + Gb(SpaceNeeded) + L.T(", 남은 공간: ") + Gb(SpaceFree));
            // the shares of the whole (they add up to 1 whatever is skipped)
            double total = (needDisc ? 14 : 0) + (needTool ? 3 : 0) + (needUpdates ? 40 : 0) + (needExe ? 1 : 0) + (needModule ? 1 : 0) + (needTool ? 1 : 0) + (gameInstall ? 40 : 0);
            if (total <= 0)
                total = 1;
            try
            {
                if (needDisc)
                {
                    Begin(L.T("디스크의 게임 파일을 꺼내는 중"), 14 / total);
                    Disc(source);
                }
                if (needTool)
                {
                    using (var desktop = new HiddenDesktop())
                    {
                        Begin(L.T("설치 도구를 준비하는 중"), 1 / total);
                        Tool();
                        Begin(L.T("시스템 파일을 준비하는 중"), 2 / total);
                        Firmware(desktop);
                        if (needUpdates)
                        {
                            Begin(L.T("게임 업데이트를 받는 중"), 34 / total);
                            List<UpdatePackage> packages = Download();
                            Begin(L.T("게임 업데이트를 설치하는 중"), 6 / total);
                            Install(desktop, packages);
                        }
                        if (needExe)
                        {
                            Begin(L.T("게임 실행 파일을 준비하는 중"), 1 / total);
                            Executable(desktop);
                        }
                        if (needModule)
                        {
                            Begin(L.T("시스템 모듈을 준비하는 중"), 1 / total);
                            Module(desktop);
                        }
                        Begin(L.T("임시 파일을 지우는 중"), 1 / total);
                        CleanUp();
                    }
                }
                if (gameInstall)
                {
                    Begin(L.T("게임 데이터를 합치는 중 (게임의 첫 설치 단계)"), 40 / total);
                    GameInstall();
                }
                done += weight;
                weight = 0;
                now.Title = L.T("설치가 끝났습니다");
                now.Detail = "";
                now.Fraction = 1;
                Note("setup complete");
                Report();
            }
            catch (OperationCanceledException)
            {
                Note("cancelled");
                throw;
            }
            catch (SetupFailed error)
            {
                Note("FAILED: " + error.Message);
                throw;
            }
            catch (Exception error)
            {
                Note("FAILED: " + error);
                throw new SetupFailed(now.Title + L.T(" 단계에서 실패했습니다: ") + error.Message);
            }
        }

        // 1. The disc's files into game\dev_bdvd (straight out of the image, nothing mounted); the data file is checked
        // against its hash while it is copied.
        void Disc(GameSource source)
        {
            Directory.CreateDirectory(DiscDir);
            List<IsoFile> files = source.IsIso ? Iso9660.List(source.Path) : null;
            var todo = new List<KeyValuePair<string, long>>();   // name below the disc's root, size
            long total = 0, copied = 0;
            foreach (string name in SetupFacts.DiscFiles)
            {
                long size;
                if (files != null)
                {
                    IsoFile file = Iso9660.Find(files, name);
                    if (file == null)
                        throw new SetupFailed(L.T("디스크 이미지에 이 파일이 없습니다: ") + name);
                    size = file.Size;
                }
                else
                {
                    string from = Path.Combine(source.Path, name.Replace('/', '\\'));
                    if (!File.Exists(from))
                    {
                        if (name == "PS3_DISC.SFB" || name.EndsWith("PS3LOGO.DAT"))
                            continue;   // a folder copied off a disc may lack them; the game does not open them
                        throw new SetupFailed(L.T("디스크 폴더에 이 파일이 없습니다: ") + name);
                    }
                    size = new FileInfo(from).Length;
                }
                string target = Path.Combine(DiscDir, name.Replace('/', '\\'));
                if (File.Exists(target) && new FileInfo(target).Length == size)
                    continue;
                todo.Add(new KeyValuePair<string, long>(name, size));
                total += size;
            }
            var buffer = new byte[4 << 20];
            using (Stream iso = files == null ? null : new FileStream(source.Path, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20, FileOptions.SequentialScan))
            {
                foreach (KeyValuePair<string, long> item in todo)
                {
                    string name = item.Key, target = Path.Combine(DiscDir, name.Replace('/', '\\'));
                    bool volume = name.EndsWith("GT.VOL", StringComparison.OrdinalIgnoreCase);
                    Stream from = iso;
                    if (files != null)
                        iso.Seek(Iso9660.Find(files, name).Offset, SeekOrigin.Begin);
                    else
                        from = new FileStream(Path.Combine(source.Path, name.Replace('/', '\\')), FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20, FileOptions.SequentialScan);
                    Directory.CreateDirectory(Path.GetDirectoryName(target));
                    bool right = true;
                    try
                    {
                        using (var hash = new SHA256Cng())
                        using (var to = new FileStream(target + ".part", FileMode.Create, FileAccess.Write, FileShare.None, 1 << 20))
                        {
                            long left = item.Value;
                            while (left > 0)
                            {
                                Check();
                                int n = from.Read(buffer, 0, (int)Math.Min(buffer.Length, left));
                                if (n <= 0)
                                    throw new SetupFailed(L.T("디스크를 끝까지 읽지 못했습니다 (파일이 온전하지 않습니다)."));
                                if (volume)
                                    hash.TransformBlock(buffer, 0, n, null, 0);
                                to.Write(buffer, 0, n);
                                left -= n;
                                copied += n;
                                At((double)copied / Math.Max(1, total), Gb(copied) + " / " + Gb(total));
                            }
                            if (volume)
                            {
                                hash.TransformFinalBlock(buffer, 0, 0);
                                right = Hashes.Hex(hash.Hash) == SetupFacts.VolumeSha256;
                            }
                        }
                    }
                    finally
                    {
                        if (files == null)
                            from.Dispose();
                    }
                    if (!right)
                    {
                        File.Delete(target + ".part");
                        Note("disc: GT.VOL has other bytes than the expected file");
                        throw new SetupFailed(L.T("디스크의 데이터 파일(GT.VOL)이 기대한 파일과 다릅니다. 이미지가 손상되었거나, 암호화된 채로 덤프한 이미지입니다."));
                    }
                    if (File.Exists(target))
                        File.Delete(target);
                    File.Move(target + ".part", target);
                }
            }
            if (!DiscReady)
                throw new SetupFailed(L.T("디스크의 파일을 다 꺼내지 못했습니다."));
        }

        // 2. RPCS3 into _setup\rpcs3, with settings that keep it silent and small.
        void Tool()
        {
            string exe = Path.Combine(ToolDir, "rpcs3.exe");
            if (!File.Exists(exe))
            {
                string name = SetupFacts.ToolUrl.Substring(SetupFacts.ToolUrl.LastIndexOf('/') + 1);
                string archive = Cached(name);
                if (archive == null)
                {
                    archive = Path.Combine(WorkDir, "dl", name);
                    if (!File.Exists(archive))
                    {
                        long got = 0;
                        Fetch.ToFile(SetupFacts.ToolUrl, archive, SetupFacts.ToolSize, delegate(long n)
                        {
                            got += n;
                            At(0.8 * got / SetupFacts.ToolSize, null);
                        }, Cancelled);
                    }
                }
                if (Hashes.Sha256(archive) != SetupFacts.ToolSha256)
                {
                    if (archive.StartsWith(WorkDir, StringComparison.OrdinalIgnoreCase))
                        File.Delete(archive);
                    throw new SetupFailed(L.T("받은 설치 도구 파일이 온전하지 않습니다. 다시 시도해 주세요."));
                }
                At(0.85, null);
                if (Directory.Exists(ToolDir))
                    Directory.Delete(ToolDir, true);
                Directory.CreateDirectory(ToolDir);
                // Windows' own tar reads the archive where it can (bsdtar: Windows 11 reads 7z). The tar of Windows 10 has no
                // 7z reader - the setup stopped there on a fully updated Windows 10 (issue 1 of the public repository) -:
                // there, and wherever tar fails, the launcher's own reader (SevenZip.cs) unpacks the same files.
                string tar = Path.Combine(Environment.SystemDirectory, "tar.exe");
                var info = new ProcessStartInfo(tar, "-xf \"" + archive + "\" -C \"" + ToolDir + "\"") { UseShellExecute = false, CreateNoWindow = true };
                bool extracted = false;
                if (File.Exists(tar))
                    using (Process p = Process.Start(info))
                    {
                        if (!p.WaitForExit(300000))
                        {
                            try
                            {
                                p.Kill();
                            }
                            catch (Exception)
                            {
                            }
                            p.WaitForExit(10000);
                        }
                        extracted = p.HasExited && p.ExitCode == 0 && File.Exists(exe);
                        if (!extracted)
                            Note("tool: tar did not unpack the archive" + (p.HasExited ? " (exit code " + p.ExitCode + ")" : ""));
                    }
                else
                    Note("tool: this Windows has no tar.exe");
                if (!extracted)
                    UnpackOwn(archive);
            }
            Directory.CreateDirectory(Path.Combine(ToolDir, "GuiConfigs"));
            File.WriteAllText(Path.Combine(ToolDir, "GuiConfigs", "CurrentSettings.ini"),
                "[main_window]\r\ninfoBoxEnabledWelcome=false\r\ninfoBoxEnabledInstallPKG=false\r\ninfoBoxEnabledInstallPUP=false\r\nconfirmationBoxBootGame=false\r\nconfirmationBoxExitGame=false\r\n\r\n[Meta]\r\ncheckUpdateStart=false\r\n");
            Directory.CreateDirectory(Path.Combine(ToolDir, "config"));
            // RPCS3 opens log\RPCS3.log before it makes its folders: without this one its first run has no log, and the
            // log is how the agent sees each job finish.
            Directory.CreateDirectory(Path.Combine(ToolDir, "log"));
            PointTool(Path.Combine(launcher.GameDir, "dev_hdd0"));
            At(1, null);
        }

        // The launcher's own 7z reader (SevenZip.cs). It unpacks beside the target folder and renames it when everything is
        // there, so that a setup that is stopped half way never leaves a folder that has rpcs3.exe but not the rest.
        void UnpackOwn(string archive)
        {
            string part = ToolDir + ".part";
            try
            {
                if (Directory.Exists(ToolDir))
                    Directory.Delete(ToolDir, true);   // what a tar that failed half way left
                if (Directory.Exists(part))
                    Directory.Delete(part, true);
                int files = SevenZip.Extract(archive, part, delegate(long bytes, long all)
                {
                    At(0.85 + 0.14 * bytes / Math.Max(1, all), null);
                }, Cancelled);
                if (!File.Exists(Path.Combine(part, "rpcs3.exe")))
                    throw new InvalidDataException("rpcs3.exe is not in the archive");
                Directory.Move(part, ToolDir);
                Note("tool: " + files + " files unpacked by the launcher's own 7z reader");
            }
            catch (OperationCanceledException)
            {
                throw;
            }
            catch (Exception ex)
            {
                Note("tool: the launcher's own 7z reader failed: " + ex);
                throw new SetupFailed(L.T("설치 도구의 압축을 풀지 못했습니다") + ": " + ex.Message);
            }
            finally
            {
                try
                {
                    if (Directory.Exists(part))
                        Directory.Delete(part, true);
                }
                catch (Exception)
                {
                }
            }
        }

        static string YamlPath(string path)
        {
            return "\"" + path.Replace('\\', '/').TrimEnd('/') + "/\"";
        }

        // Which folder is the tool's hard disc (config\vfs.yml: that line changed, the others kept), and where the disc
        // is (config\games.yml).
        void PointTool(string hdd0)
        {
            Directory.CreateDirectory(hdd0);
            string vfs = Path.Combine(ToolDir, "config", "vfs.yml"), line = "/dev_hdd0/: " + YamlPath(hdd0);
            string text = File.Exists(vfs) ? File.ReadAllText(vfs) : "";
            var entry = new Regex(@"(?m)^/dev_hdd0/:[^\r\n]*");
            text = entry.IsMatch(text) ? entry.Replace(text, line.Replace("$", "$$"), 1) : line + "\n" + text;
            File.WriteAllText(vfs, text);
            File.WriteAllText(Path.Combine(ToolDir, "config", "games.yml"), GameFiles.TitleId + ": " + YamlPath(DiscDir) + "\n");
        }

        // The tool's settings for the moment it runs the game's loader: interpreters (nothing is compiled), no picture,
        // no sound. RPCS3 writes its own full settings file the first time it runs; the four lines are changed in it.
        void QuietTool()
        {
            string file = Path.Combine(ToolDir, "config", "config.yml");
            if (!File.Exists(file))
                return;
            string text = File.ReadAllText(file);
            text = Regex.Replace(text, @"(?m)^  PPU Decoder: .*$", "  PPU Decoder: Interpreter (static)");
            text = Regex.Replace(text, @"(?m)^  SPU Decoder: .*$", "  SPU Decoder: Interpreter (static)");
            text = Regex.Replace(text, @"(?m)^(Video:\r?\n  Renderer: ).*$", "${1}\"Null\"");
            text = Regex.Replace(text, @"(?m)^(Audio:\r?\n  Renderer: ).*$", "${1}\"Null\"");
            File.WriteAllText(file, text);
        }

        bool normalDesktop;

        int Agent(HiddenDesktop desktop, string arguments, int timeoutMs)
        {
            if (!normalDesktop && launcher.Value("Setup", "NormalDesktop", "0") == "1")
                normalDesktop = true;
            string exe = Process.GetCurrentProcess().MainModule.FileName;
            int code = HiddenDesktop.RunOn(normalDesktop ? null : desktop.Name, exe, "--setup-agent " + arguments, launcher.Dir, timeoutMs, Cancelled);
            EndTool();
            if (code == HiddenDesktop.Cancelled)
                throw new OperationCanceledException();
            if (!normalDesktop && (code == SetupAgent.NoDialog || code == SetupAgent.Failed || (code < 0 && code != HiddenDesktop.TimedOut)))
            {
                Note("agent: hidden desktop failed (code " + code + " / 0x" + code.ToString("X8") + "); retrying on normal desktop");
                normalDesktop = true;
                launcher.Set("Setup", "NormalDesktop", "1");
                code = HiddenDesktop.RunOn(null, exe, "--setup-agent " + arguments, launcher.Dir, timeoutMs, Cancelled);
                EndTool();
                if (code == HiddenDesktop.Cancelled)
                    throw new OperationCanceledException();
            }
            return code;
        }

        // No RPCS3 of ours is left behind (the agent ends it; this is for an agent that was ended itself).
        void EndTool()
        {
            try
            {
                foreach (Process p in Process.GetProcessesByName("rpcs3"))
                {
                    try
                    {
                        if (p.MainModule.FileName.StartsWith(ToolDir, StringComparison.OrdinalIgnoreCase))
                        {
                            p.Kill();
                            p.WaitForExit(3000);
                        }
                    }
                    catch (Exception)
                    {
                    }
                }
            }
            catch (Exception)
            {
            }
        }

        static string Quote(string path)
        {
            return "\"" + path.TrimEnd('\\') + "\"";
        }

        // 3. The firmware into the tool (it needs it to run the game's loader, and libsre comes out of it).
        void Firmware(HiddenDesktop desktop)
        {
            string module = Path.Combine(ToolDir, "dev_flash", "sys", "external", "libsre.sprx");
            if (File.Exists(module) && File.Exists(Path.Combine(ToolDir, "dev_flash", "vsh", "etc", "version.txt")))
            {
                At(1, null);
                return;
            }
            string pup = Cached("PS3UPDAT.PUP");
            if (pup != null && (new FileInfo(pup).Length != SetupFacts.FirmwareSize || Hashes.Sha256(pup) != SetupFacts.FirmwareSha256))
                pup = null;
            if (pup == null)
            {
                pup = Path.Combine(WorkDir, "dl", "PS3UPDAT.PUP");
                if (!File.Exists(pup) || new FileInfo(pup).Length != SetupFacts.FirmwareSize)
                {
                    long got = 0;
                    Action<long> progress = delegate(long n)
                    {
                        got += n;
                        At(0.8 * got / SetupFacts.FirmwareSize, Gb(got) + " / " + Gb(SetupFacts.FirmwareSize));
                    };
                    try
                    {
                        Fetch.ToFile(SetupFacts.FirmwareUrl, pup, SetupFacts.FirmwareSize, progress, Cancelled);
                    }
                    catch (OperationCanceledException)
                    {
                        throw;
                    }
                    catch (Exception)
                    {
                        // Sony moved the file: its own list says where the current one is.
                        Match current = Regex.Match(Fetch.Text(SetupFacts.FirmwareList), @"CDN=(http[^;]+PS3UPDAT\.PUP)");
                        if (!current.Success)
                            throw new SetupFailed(L.T("PS3 시스템 소프트웨어를 받지 못했습니다 (소니 서버의 목록에서 주소를 찾지 못함)."));
                        got = 0;
                        Fetch.ToFile(current.Groups[1].Value, pup, 0, progress, Cancelled);
                    }
                }
            }
            At(0.85, L.T("설치하는 중"));
            int code = Agent(desktop, "fw " + Quote(ToolDir) + " " + Quote(pup), 900000);
            if (code != SetupAgent.Ok || !File.Exists(module))
                throw new SetupFailed(L.T("PS3 시스템 소프트웨어를 설치하지 못했습니다 (코드 ") + code + L.T("). logs\\launcher\\setup.log를 봐 주세요."));
            QuietTool();
            At(1, null);
        }

        // 4a. The update packages still to install, downloaded (four at a time; each checks itself).
        List<UpdatePackage> Download()
        {
            string have = InstalledVersion;
            List<UpdatePackage> todo = SetupFacts.Updates.Where(u => string.CompareOrdinal(u.Version, have) > 0).ToList();
            long total = todo.Sum(u => u.Size), got = 0;
            var paths = new Dictionary<string, string>();
            var queue = new Queue<UpdatePackage>();
            foreach (UpdatePackage package in todo)
            {
                string cached = Cached(package.FileName);
                if (cached != null && new FileInfo(cached).Length == package.Size)
                {
                    paths[package.Version] = cached;
                    got += package.Size;
                    continue;
                }
                string local = Path.Combine(WorkDir, "dl", package.FileName);
                paths[package.Version] = local;
                if (File.Exists(local) && new FileInfo(local).Length == package.Size)
                    got += package.Size;
                else
                {
                    if (File.Exists(local + ".part"))
                        got += Math.Min(package.Size, new FileInfo(local + ".part").Length);
                    queue.Enqueue(package);
                }
            }
            At((double)got / Math.Max(1, total), Gb(got) + " / " + Gb(total));
            Exception failure = null;
            object gate = new object();
            var workers = new List<Thread>();
            for (int i = 0; i < 4; i++)
            {
                var worker = new Thread(delegate()
                {
                    while (true)
                    {
                        UpdatePackage package;
                        lock (gate)
                        {
                            if (queue.Count == 0 || failure != null || cancel)
                                return;
                            package = queue.Dequeue();
                        }
                        try
                        {
                            Fetch.ToFile(package.Url, paths[package.Version], package.Size, delegate(long n)
                            {
                                lock (gate)
                                {
                                    got += n;
                                    At((double)got / Math.Max(1, total), Gb(got) + " / " + Gb(total));
                                }
                            }, Cancelled);
                        }
                        catch (Exception error)
                        {
                            lock (gate)
                                if (failure == null)
                                    failure = error;
                        }
                    }
                }) { IsBackground = true };
                workers.Add(worker);
                worker.Start();
            }
            foreach (Thread worker in workers)
                worker.Join();
            Check();
            if (failure != null)
                throw new SetupFailed(L.T("게임 업데이트를 받지 못했습니다: ") + failure.Message + L.T(" (인터넷 연결을 확인하고 다시 시작하면 받던 곳부터 이어집니다.)"));
            At(1, L.T("받은 파일을 검사하는 중"));
            foreach (UpdatePackage package in todo)
            {
                Check();
                if (!Hashes.PackageIntact(paths[package.Version], package.Sha1))
                {
                    if (paths[package.Version].StartsWith(WorkDir, StringComparison.OrdinalIgnoreCase))
                        File.Delete(paths[package.Version]);
                    throw new SetupFailed(L.T("받은 업데이트 파일이 온전하지 않습니다 (") + package.Version + L.T("). 다시 시작하면 그 파일만 다시 받습니다."));
                }
            }
            packagePaths = paths;
            return todo;
        }

        Dictionary<string, string> packagePaths = new Dictionary<string, string>();

        // 4b. Installed in order by RPCS3's own installer, straight into game\dev_hdd0.
        void Install(HiddenDesktop desktop, List<UpdatePackage> packages)
        {
            PointTool(Path.Combine(launcher.GameDir, "dev_hdd0"));
            for (int i = 0; i < packages.Count; i++)
            {
                Check();
                UpdatePackage package = packages[i];
                At((double)i / packages.Count, package.Version + "  (" + (i + 1) + " / " + packages.Count + ")");
                int code = Agent(desktop, "pkg " + Quote(ToolDir) + " " + Quote(packagePaths[package.Version]), 2400000);
                if (code != SetupAgent.Ok || string.CompareOrdinal(InstalledVersion, package.Version) < 0)
                    throw new SetupFailed(L.T("업데이트 ") + package.Version + L.T("을 설치하지 못했습니다 (코드 ") + code + L.T("). logs\\launcher\\setup.log를 봐 주세요."));
            }
            if (!UpdatesReady)
                throw new SetupFailed(L.T("업데이트를 설치했지만 게임 데이터가 1.22가 아닙니다 (지금 ") + InstalledVersion + ").");
            At(1, null);
        }

        // 5. game\EMAIN.ELF: the 1.22 executable (in the last patch folder, or already in its final place), decrypted.
        void Executable(HiddenDesktop desktop)
        {
            string self = Path.Combine(UsrDir, SetupFacts.FinalSelf);
            if (!File.Exists(self))
                self = Path.Combine(UsrDir, "EMAIN.SELF");
            string loader = Path.Combine(UsrDir, "EBOOT.BIN");
            if (!File.Exists(self) || !File.Exists(loader) || Hashes.Sha256(self) != SetupFacts.SelfSha256)
                throw new SetupFailed(L.T("1.22의 게임 실행 파일(EMAIN.SELF)을 찾지 못했거나 기대한 파일이 아닙니다."));
            // The loader runs on a tiny hard disc of its own, so that nothing can reach the game data from it.
            string small = Path.Combine(WorkDir, "key"), title = Path.Combine(small, "dev_hdd0", "game", GameFiles.TitleId);
            if (Directory.Exists(small))
                Directory.Delete(small, true);
            Directory.CreateDirectory(Path.Combine(title, "USRDIR"));
            Directory.CreateDirectory(Path.Combine(small, "work"));
            File.Copy(Path.Combine(launcher.TitleDir, "PARAM.SFO"), Path.Combine(title, "PARAM.SFO"));
            if (File.Exists(Path.Combine(launcher.TitleDir, "ICON0.PNG")))
                File.Copy(Path.Combine(launcher.TitleDir, "ICON0.PNG"), Path.Combine(title, "ICON0.PNG"));
            File.Copy(loader, Path.Combine(title, "USRDIR", "EBOOT.BIN"));
            File.Copy(self, Path.Combine(title, "USRDIR", "EMAIN.SELF"));
            File.Copy(self, Path.Combine(small, "work", "EMAIN.SELF"));
            At(0.3, null);
            QuietTool();
            PointTool(Path.Combine(small, "dev_hdd0"));
            int code;
            try
            {
                code = Agent(desktop, "key " + Quote(ToolDir) + " " + Quote(Path.Combine(title, "USRDIR", "EBOOT.BIN")) + " " + Quote(Path.Combine(small, "work", "EMAIN.SELF")) + " " + SetupFacts.ElfSha256, 600000);
            }
            finally
            {
                PointTool(Path.Combine(launcher.GameDir, "dev_hdd0"));
            }
            string elf = Path.Combine(small, "work", "EMAIN.elf");
            if (code != SetupAgent.Ok || !File.Exists(elf) || Hashes.Sha256(elf) != SetupFacts.ElfSha256)
                throw new SetupFailed(L.T("게임 실행 파일을 준비하지 못했습니다 (코드 ") + code + L.T("). logs\\launcher\\setup.log를 봐 주세요."));
            Directory.CreateDirectory(launcher.GameDir);
            File.Copy(elf, launcher.Elf, true);
            Directory.Delete(small, true);
            At(1, null);
        }

        // 6. game\firmware: libsre decrypted, and as the exe loads it (placed at its address, relocations applied).
        void Module(HiddenDesktop desktop)
        {
            string from = Path.Combine(ToolDir, "dev_flash", "sys", "external", "libsre.sprx");
            string work = Path.Combine(WorkDir, "module");
            if (Directory.Exists(work))
                Directory.Delete(work, true);
            Directory.CreateDirectory(work);
            string sprx = Path.Combine(work, "libsre.sprx"), prx = Path.Combine(work, "libsre.prx");
            File.Copy(from, sprx);
            int code = Agent(desktop, "dec " + Quote(ToolDir) + " " + Quote(sprx), 300000);
            if (code != SetupAgent.Ok || !File.Exists(prx))
                throw new SetupFailed(L.T("시스템 모듈을 준비하지 못했습니다 (코드 ") + code + ").");
            if (Hashes.Sha256(prx) != SetupFacts.LibsreSha256)
                throw new SetupFailed(L.T("받은 PS3 시스템 소프트웨어의 모듈이 이 포트가 쓰는 것(4.93)과 다릅니다. 런처의 새 버전이 필요합니다."));
            byte[] image = Prx.Relocate(File.ReadAllBytes(prx), SetupFacts.LibsreBase);
            if (Hashes.Sha256(image) != SetupFacts.LibsreImageSha256)
                throw new SetupFailed(L.T("시스템 모듈을 배치한 결과가 기대와 다릅니다."));
            string firmware = Path.Combine(launcher.GameDir, "firmware");
            Directory.CreateDirectory(firmware);
            File.Copy(prx, Path.Combine(firmware, "libsre.prx"), true);
            File.WriteAllBytes(Path.Combine(firmware, "libsre_image.bin"), image);
            Directory.Delete(work, true);
            At(1, null);
        }

        // 7. The tool, the firmware file and the packages are gone again; what the tool left in the game's hard disc too.
        void CleanUp()
        {
            EndTool();
            for (int attempt = 0; attempt < 5 && Directory.Exists(WorkDir); attempt++)
            {
                try
                {
                    Directory.Delete(WorkDir, true);
                }
                catch (Exception)
                {
                    Thread.Sleep(600);
                }
            }
            try
            {
                // RPCS3's memory file (sparse): it is ended at each step, so it does not remove it itself.
                File.Delete(Path.Combine(Path.GetTempPath(), "rpcs3_vm_sparse.tmp"));
            }
            catch (Exception)
            {
            }
            try
            {
                string locks = Path.Combine(launcher.GameDir, "dev_hdd0", "game", "＄locks");   // RPCS3's "$locks" (full-width $)
                if (Directory.Exists(locks))
                    Directory.Delete(locks, true);
                Directory.CreateDirectory(Path.Combine(launcher.GameDir, "dev_hdd1"));
            }
            catch (Exception)
            {
            }
            At(1, null);
        }

        // 8. The game's own installation: on a first start it merges the update packages' patch folders into its data
        // (about ten minutes). The game runs for that hidden, muted and with the keyboard off; the patch files going
        // away are the progress. Its last step is removing its mark USRDIR\UPDATING; then it waits for a button
        // ("Installation complete.") and is ended here (its own restart does not come back natively).
        void GameInstall()
        {
            if (!InstallPending)
            {
                At(1, null);
                return;
            }
            int start = Math.Max(1, CountPatchFiles());
            GameBuild build = launcher.Build;
            string problem = Game.Problem(launcher, build);
            if (problem != null)
                throw new SetupFailed(problem);
            Directory.CreateDirectory(launcher.LauncherLogsDir);
            var environment = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            foreach (System.Collections.DictionaryEntry pair in Environment.GetEnvironmentVariables())
                environment[(string)pair.Key] = (string)pair.Value;
            foreach (string name in build.Unset)
                environment.Remove(name);
            foreach (KeyValuePair<string, string> pair in build.Env)
                environment[pair.Key] = pair.Value;
            environment.Remove("PS3_USRDIR_OVERLAY");   // no mod may stand in while the game merges its own files
            environment.Remove("PS3_FSLOG");            // set at all (the bat's "0" too) = a line for every file operation
            environment.Remove("PS3_VERBOSE");
            environment.Remove("GT6_LAN");              // keep first-time patch merge out of lanmode even if copied from another PC
            environment.Remove("GT6_LAN_BIND");
            environment.Remove("GT6_LAN_NAME");
            environment.Remove("GT6_DEV_MENU");
            environment.Remove("PS3_NET_ONLINE");
            environment.Remove("PS3_ARGV");
            environment["PS3_MUTE"] = "1";
            environment["PAD_NO_KEYBOARD"] = "1";        // what the user types meanwhile goes elsewhere
            environment["PS3_OSK_TEXT"] = "Player";      // and no text box can open
            Note("game installation: " + start + " patch files, " + build.ExeName);
            using (Child game = Child.Start(build.Exe, "", launcher.Dir, null, true, environment,
                                            Path.Combine(launcher.LauncherLogsDir, "setup_game_out.txt"), Path.Combine(launcher.LauncherLogsDir, "setup_game_err.txt")))
            {
                try
                {
                    var watch = Stopwatch.StartNew();
                    int last = -1;
                    long lastChange = 0, finishedAt = -1;
                    while (true)
                    {
                        Check();
                        bool exited = game.Wait(1000);
                        int left = CountPatchFiles();
                        if (left != last)
                        {
                            last = left;
                            lastChange = watch.ElapsedMilliseconds;
                        }
                        At(1.0 - (double)left / start, left > 0 ? L.T("남은 파일 ") + left.ToString("N0") : L.T("마무리하는 중"));
                        if (!InstallPending)
                        {
                            if (finishedAt < 0)
                                finishedAt = watch.ElapsedMilliseconds;
                            else if (watch.ElapsedMilliseconds - finishedAt > 3000)
                                break;
                        }
                        else if (exited)
                            throw new SetupFailed(L.T("게임이 데이터를 합치다가 꺼졌습니다 (코드 ") + game.ExitCode + L.T("). logs\\launcher\\setup_game_err.txt를 봐 주세요."));
                        else if (watch.ElapsedMilliseconds - lastChange > 900000)
                            throw new SetupFailed(L.T("게임이 데이터를 합치다가 15분 동안 진행이 없었습니다. logs\\launcher\\setup_game_err.txt를 봐 주세요."));
                    }
                    Note("game installation done in " + watch.ElapsedMilliseconds / 1000 + " s");
                }
                finally
                {
                    game.Kill();
                }
            }
            At(1, null);
        }

        int CountPatchFiles()
        {
            int count = 0;
            foreach (string folder in PatchFolders)
            {
                try
                {
                    count += Directory.EnumerateFiles(folder, "*", SearchOption.AllDirectories).Count();
                }
                catch (Exception)
                {
                }
            }
            return count;
        }

        // --setup [<iso or disc folder>]: the whole setup without a window (tests, and people who prefer it); the steps
        // and the progress go to logs\launcher\setup.log. Exit code 0 = done, 1 = failed, 2 = cancelled.
        public static int RunHeadless(Launcher launcher, string sourcePath)
        {
            var runner = new SetupRunner(launcher);
            GameSource source = null;
            if (!string.IsNullOrEmpty(sourcePath))
                source = GameFinder.Inspect(sourcePath);
            else if (!runner.DiscReady)
                source = GameFinder.Find(launcher, null).FirstOrDefault(s => s.Usable);
            if (source != null)
                runner.Note("source: " + source.Path + (source.Usable ? "" : " - " + source.Problem));
            int lastPercent = -1;
            long lastNote = 0;
            runner.Progress += delegate(SetupProgress p)
            {
                int percent = (int)(p.Fraction * 100);
                long tick = Environment.TickCount;
                if (percent != lastPercent && (tick - lastNote > 10000 || tick < lastNote || percent >= 100))
                {
                    lastPercent = percent;
                    lastNote = tick;
                    runner.Note("progress " + percent + "%  " + p.Title + (p.Detail.Length > 0 ? "  " + p.Detail : ""));
                }
            };
            try
            {
                runner.Run(source);
                return 0;
            }
            catch (SetupFailed)
            {
                return 1;
            }
            catch (OperationCanceledException)
            {
                return 2;
            }
        }
    }

    // A decrypted PRX placed at its load base with its relocations applied (as ps3recomp's tools/lift_prx.py; the same
    // code as in the launcher of an earlier port).
    public static class Prx
    {
        public static byte[] Relocate(byte[] d, uint baseAddress)
        {
            Func<int, ulong> be64 = delegate(int o) { ulong v = 0; for (int i = 0; i < 8; i++) v = v << 8 | d[o + i]; return v; };
            Func<int, uint> be32 = delegate(int o) { return (uint)(d[o] << 24 | d[o + 1] << 16 | d[o + 2] << 8 | d[o + 3]); };
            if (d.Length < 0x40 || d[0] != 0x7F || d[1] != 'E' || d[4] != 2 || d[5] != 2)
                throw new InvalidDataException("not a decrypted PRX");
            int phoff = (int)be64(0x20), phnum = d[0x38] << 8 | d[0x39];
            var loads = new List<ulong[]>();   // offset, vaddr, filesz, memsz
            ulong relaOff = 0, relaSize = 0;
            for (int i = 0; i < phnum; i++)
            {
                int o = phoff + i * 0x38;
                uint type = be32(o);
                if (type == 1)
                    loads.Add(new[] { be64(o + 8), be64(o + 16), be64(o + 32), be64(o + 40) });
                else if (type == 0x700000A4)
                {
                    relaOff = be64(o + 8);
                    relaSize = be64(o + 32);
                }
            }
            ulong top = loads.Max(l => l[1] + l[3]);
            var img = new byte[top];
            var segBase = new List<ulong>();
            foreach (ulong[] l in loads)
            {
                Array.Copy(d, (long)l[0], img, (long)l[1], (long)l[2]);
                segBase.Add(baseAddress + l[1]);
            }
            Action<ulong, ulong, int> put = delegate(ulong at, ulong value, int bytes)
            {
                for (int i = 0; i < bytes; i++)
                    img[at + (ulong)i] = (byte)(value >> (8 * (bytes - 1 - i)));
            };
            for (ulong i = 0; i < relaSize / 24; i++)
            {
                int o = (int)(relaOff + i * 24);
                ulong offset = be64(o), ptr = be64(o + 16);
                int indexValue = d[o + 10], indexAddr = d[o + 11];
                uint type = be32(o + 12);
                ulong target = segBase[indexAddr] - baseAddress + offset;
                ulong value = (indexValue == 0xFF ? 0 : segBase[indexValue]) + ptr;
                switch (type)
                {
                    case 1: put(target, value & 0xFFFFFFFF, 4); break;                    // ADDR32
                    case 4: put(target, value & 0xFFFF, 2); break;                        // ADDR16_LO
                    case 5: put(target, (value >> 16) & 0xFFFF, 2); break;                // ADDR16_HI
                    case 6: put(target, ((value + 0x8000) >> 16) & 0xFFFF, 2); break;     // ADDR16_HA
                    case 44: put(target, value, 8); break;                                // REL64
                    default: throw new InvalidDataException("relocation type " + type);
                }
            }
            return img;
        }
    }
}
