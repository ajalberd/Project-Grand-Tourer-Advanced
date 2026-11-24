// GT6 PC launcher (the Sakatsuku PC launcher's frame): WPF on the .NET Framework 4 that is part of Windows, built by
// build_launcher.bat with its C# compiler (C# 5: no string interpolation, no ?.). It sits in the port's package folder
// beside PLAY_GT6.bat and starts the game the way that file does: the environment of a run is read from the bat itself
// (GameBuild.Read), so the launcher and the bat cannot drift apart. The launcher adds only its own switches on top
// (mods folder, mute, keyboard pad), backs up the save data, shows the logs and updates the package from GitHub.
//   GT6 Launcher.exe               the launcher window
//   GT6 Launcher.exe --play        starts the game at once (desktop shortcut); a window only when it fails
//   GT6 Launcher.exe --update      looks for a new version, downloads and installs it, no window (logs\launcher\update.log)
//   GT6 Launcher.exe --rollback    puts back the version the last update replaced, no window
//   GT6 Launcher.exe --shot <dir>  every page rendered off screen to <dir>\<page>.png (layout check; no network)
//   GT6 Launcher.exe --check-game-mods <folder>  records the mods that are inside the game data (GameMods.Check), no window
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using System.Windows;
using System.Windows.Markup;
using System.Windows.Threading;

namespace GT6Launcher
{
    static class Program
    {
        [DllImport("user32.dll")]
        static extern bool SetProcessDPIAware();

        static bool Has(string[] args, string name)
        {
            return args.Any(a => a.Equals(name, StringComparison.OrdinalIgnoreCase));
        }

        [STAThread]
        static int Main(string[] args)
        {
            try
            {
                SetProcessDPIAware();
            }
            catch (Exception)
            {
            }
            var launcher = new Launcher(AppDomain.CurrentDomain.BaseDirectory.TrimEnd('\\', '/'));
            L.Init(launcher);   // the launcher's language, before any text is made
            // The setup's helper on its hidden desktop (SetupRun.cs), and the setup without a window.
            int agent = Array.FindIndex(args, a => a.Equals("--setup-agent", StringComparison.OrdinalIgnoreCase));
            if (agent >= 0)
                return SetupAgent.Run(launcher, args.Skip(agent + 1).ToArray());
            int setup = Array.FindIndex(args, a => a.Equals("--setup", StringComparison.OrdinalIgnoreCase));
            if (setup >= 0)
                return SetupRunner.RunHeadless(launcher, setup + 1 < args.Length ? args[setup + 1] : null);
            PortUpdate.CleanUp(launcher);
            if (Has(args, "--update"))
                return PortUpdate.RunHeadless(launcher);
            if (Has(args, "--rollback"))
                return PortUpdate.RollbackHeadless(launcher);
            int checkMods = Array.FindIndex(args, a => a.Equals("--check-game-mods", StringComparison.OrdinalIgnoreCase));
            if (checkMods >= 0 && checkMods + 1 < args.Length)
                return GameMods.CheckHeadless(launcher, args[checkMods + 1]);
            int importCareer = Array.FindIndex(args, a => a.Equals("--import-career", StringComparison.OrdinalIgnoreCase));
            if (importCareer >= 0 && importCareer + 1 < args.Length)
                return CareerMod.ImportHeadless(launcher, args[importCareer + 1]);
            var app = new Application { ShutdownMode = ShutdownMode.OnExplicitShutdown };
            using (Stream theme = typeof(Program).Assembly.GetManifestResourceStream("Theme.xaml"))
                app.Resources = (ResourceDictionary)XamlReader.Load(theme);
            int shot = Array.FindIndex(args, a => a.Equals("--shot", StringComparison.OrdinalIgnoreCase));
            if (shot >= 0 && shot + 1 < args.Length)
                return Shots.Run(app, launcher, args[shot + 1]);
            if (PortUpdate.ApplyAtStart(launcher, args))   // a new version downloaded earlier: put in place, start it
                return 0;
            bool play = Has(args, "--play");
            MainWindow window = null;
            Func<MainWindow> showWindow = delegate
            {
                if (window == null)
                {
                    window = new MainWindow(launcher);
                    window.Closed += delegate { app.Shutdown(); };
                }
                window.Show();
                window.Activate();
                return window;
            };
            launcher.Runner.Finished += delegate(RunResult result)
            {
                if (window == null && result.Diagnosis == null)
                    app.Shutdown(); // --play: the game ended normally
                else
                    showWindow().GameFinished(result);
            };
            if (play)
            {
                string problem = launcher.Runner.Start();
                if (problem != null)
                    showWindow().ShowProblem(problem);
            }
            else
                showWindow();
            return app.Run();
        }
    }

    // --shot <dir>: every page rendered off screen to <dir>\<page>.png, then exit (to check the layout without a desktop).
    public static class Shots
    {
        public static int Run(Application app, Launcher launcher, string dir)
        {
            Directory.CreateDirectory(dir);
            PortUpdate.Disabled = true;
            var window = new MainWindow(launcher) { WindowStartupLocation = WindowStartupLocation.Manual, Left = -20000, Top = 0, ShowActivated = false, ShowInTaskbar = false };
            int shotHeight; // LAUNCHER_SHOT_HEIGHT=<px>: a taller window, to see the whole of a long page
            if (int.TryParse(Environment.GetEnvironmentVariable("LAUNCHER_SHOT_HEIGHT"), out shotHeight) && shotHeight > 0)
                window.Height = shotHeight;
            window.Show();
            string[] keys = MainWindow.PageKeys;
            int i = 0;
            var timer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(800) };
            timer.Tick += delegate
            {
                if (i > 0)
                    Save(window, Path.Combine(dir, keys[i - 1] + ".png"));
                if (i == keys.Length)
                {
                    timer.Stop();
                    app.Shutdown();
                    return;
                }
                window.Navigate(keys[i++]);
            };
            timer.Start();
            return app.Run();
        }

        static void Save(Window window, string path)
        {
            var content = (FrameworkElement)window.Content;
            int w = (int)content.ActualWidth, h = (int)content.ActualHeight;
            var visual = new System.Windows.Media.DrawingVisual();
            using (System.Windows.Media.DrawingContext dc = visual.RenderOpen())
            {
                dc.DrawRectangle(window.Background, null, new Rect(0, 0, w, h));
                dc.DrawRectangle(new System.Windows.Media.VisualBrush(content), null, new Rect(0, 0, w, h));
            }
            var bitmap = new System.Windows.Media.Imaging.RenderTargetBitmap(w, h, 96, 96, System.Windows.Media.PixelFormats.Pbgra32);
            bitmap.Render(visual);
            var encoder = new System.Windows.Media.Imaging.PngBitmapEncoder();
            encoder.Frames.Add(System.Windows.Media.Imaging.BitmapFrame.Create(bitmap));
            using (FileStream file = File.Create(path))
                encoder.Save(file);
        }
    }

    // The package folder (the launcher sits in it, next to PLAY_GT6.bat) and the files the launcher reads and writes there.
    public class Launcher
    {
        public const string ExeName = "GT6 Launcher.exe";
        public readonly string Dir;
        public readonly GameRunner Runner;

        public Launcher(string dir)
        {
            Dir = dir;
            Runner = new GameRunner(this);
        }

        public string StatePath { get { return Path.Combine(Dir, "launcher.ini"); } }
        public string LogsDir { get { return Path.Combine(Dir, "logs"); } }                    // the bats' own folder
        public string LauncherLogsDir { get { return Path.Combine(Dir, "logs", "launcher"); } } // runs.log, update.log
        public string GameDir { get { return Path.Combine(Dir, "game"); } }
        public string Elf { get { return Path.Combine(GameDir, "EMAIN.ELF"); } }
        public string TitleDir { get { return Path.Combine(GameDir, "dev_hdd0", "game", GameFiles.TitleId); } }
        // PS3_GAMEDATA_ROOT=game in the bats: cellSaveData keeps the saves here.
        public string SavesDir { get { return Path.Combine(GameDir, "dev_hdd0", "home", "00000001", "savedata"); } }
        public string BackupsDir { get { return Path.Combine(Dir, "save_backups"); } }
        public string ModsDir { get { return Path.Combine(Dir, "mods"); } }

        // launcher.ini [Launcher]: the launcher's own state and play record; [Settings]: what the user chose.
        public string State(string key, string fallback)
        {
            return new IniFile(StatePath).Get("Launcher", key, fallback);
        }

        public void SetState(string key, string value)
        {
            Set("Launcher", key, value);
        }

        public string Setting(string key)
        {
            return new IniFile(StatePath).Get("Settings", key, Settings.Default(key));
        }

        public bool On(string key)
        {
            return Setting(key) == "1";
        }

        public void SetSetting(string key, string value)
        {
            Set("Settings", key, value);
        }

        public string Value(string section, string key, string fallback)
        {
            return new IniFile(StatePath).Get(section, key, fallback);
        }

        public void Set(string section, string key, string value)
        {
            var ini = new IniFile(StatePath);
            ini.Set(section, key, value);
            ini.Save();
        }

        // The build the play button starts: [Settings] Build, else the test build when the package has one (it is the
        // newer of the two while test builds are being handed out), else the release.
        public GameBuild Build
        {
            get
            {
                string key = Setting("Build");
                GameBuild chosen = GameBuild.Read(this, key == "release" ? "release" : "test");
                if (!chosen.Available)
                {
                    // (the build named in launcher.ini is not in the package any more - a test build that became the
                    // release - or none was named: the one that is there)
                    GameBuild other = GameBuild.Read(this, chosen.Key == "test" ? "release" : "test");
                    if (other.Available || key.Length == 0)
                        return other;
                }
                return chosen;
            }
        }

        public void AddPlayTime(TimeSpan time, int exitCode, string build)
        {
            long seconds;
            long.TryParse(State("PlayTimeSeconds", "0"), out seconds);
            var ini = new IniFile(StatePath);
            ini.Set("Launcher", "PlayTimeSeconds", (seconds + (long)time.TotalSeconds).ToString());
            ini.Set("Launcher", "LastPlayed", DateTime.Now.ToString("yyyy-MM-dd HH:mm"));
            ini.Set("Launcher", "LastExitCode", exitCode.ToString());
            ini.Set("Launcher", "LastBuild", build);
            ini.Save();
        }
    }

    // What the user can choose in the launcher. Everything else about a run comes from the bat (GameBuild).
    public static class Settings
    {
        static readonly Dictionary<string, string> defaults = new Dictionary<string, string>
        {
            { "Build", "" },            // "release" / "test"; empty = the test build when there is one
            { "Mute", "0" },            // PS3_MUTE=1
            { "Keyboard", "1" },        // off: PAD_NO_KEYBOARD=1 (the keyboard no longer acts as a pad)
            { "AutoBackup", "1" },      // save data backup before each run
            { "BackupsToKeep", "10" },
            { "UpdateCheck", "0" },     // off by default for custom Ultrawide/LAN builds (re-enable manually in Updates if desired)
            { "AutoUpdate", "0" },      // ...never silently overwrite custom GT6Recomp.exe / GT6 Launcher.exe
            { "Language", "" },         // "" = by Windows' display language, "ko", "en" (Lang.cs)
            { "UseGhCli", "0" },        // a private repository: the GitHub CLI's login of this PC when no token was pasted
            { "AA", "" },               // anti-aliasing of a run: "" = the game's own default (GPU filter), "off", "spu" (GT6_AA)
            { "Fullscreen", "0" },      // start in borderless full screen (GT6_FULLSCREEN=1)
            { "Aspect", "" },           // aspect ratio override: "" = 16:9, "auto", "2.3888889", "2.3703704", "2.4", "3.5555556", "1.6" (GT6_ASPECT)
            { "LanMode", "0" },         // hidden LAN mode (GT6_LAN=1 -> lanmode argv, room sync + member info patches, Winsock2)
            { "LanBind", "" },          // LAN NIC IPv4 bind address (GT6_LAN_BIND -> PS3_NET_BIND_IP)
            { "LanName", "" },          // LAN player name / PS3_NP_ONLINE_ID (GT6_LAN_NAME; blank = PC name)
            { "DevMenu", "0" },         // Develop Settings R1+SELECT (GT6_DEV_MENU=1; auto-enabled with LanMode)
            { "SpursSpus", "" },        // SPURS SPU thread count override: "" = default (5), "6", "7", "8" (GT6_SPURS_SPUS)
        };

        public static string Default(string key)
        {
            string value;
            return defaults.TryGetValue(key, out value) ? value : "";
        }

        // The launcher's own switches for one run, on top of the bat's environment (null = the variable is removed).
        public static Dictionary<string, string> Overrides(Launcher launcher, string overlay)
        {
            var env = new Dictionary<string, string>();
            env["PS3_USRDIR_OVERLAY"] = overlay;                     // mods: read before the game's own files
            if (launcher.On("Mute"))
                env["PS3_MUTE"] = "1";
            if (!launcher.On("Keyboard"))
                env["PAD_NO_KEYBOARD"] = "1";                        // any value turns it off, so it is set only then
            string aa = launcher.Setting("AA");
            if (aa == "off" || aa == "spu")
                env["GT6_AA"] = aa;                                  // "" = the game's own default, the GPU filter
            if (launcher.On("Fullscreen"))
                env["GT6_FULLSCREEN"] = "1";                         // an exe that does not know it ignores it
            string aspect = launcher.Setting("Aspect");
            if (!string.IsNullOrEmpty(aspect))
                env["GT6_ASPECT"] = aspect;
            if (launcher.On("LanMode"))
            {
                env["GT6_LAN"] = "1";
                string bindIp = launcher.Setting("LanBind").Trim();
                if (!string.IsNullOrEmpty(bindIp))
                    env["GT6_LAN_BIND"] = bindIp;
                string lanName = launcher.Setting("LanName").Trim();
                if (!string.IsNullOrEmpty(lanName))
                    env["GT6_LAN_NAME"] = lanName;
            }
            if (launcher.On("DevMenu"))
                env["GT6_DEV_MENU"] = "1";
            string spursSpus = launcher.Setting("SpursSpus");
            if (spursSpus == "6" || spursSpus == "7" || spursSpus == "8")
                env["GT6_SPURS_SPUS"] = spursSpus;
            return env;
        }
    }

    // One of the package's two game builds, as its PLAY_*.bat starts it: the exe, the log files and the environment
    // are read from the bat (set "NAME=value", for %%V in (...) do set "%%V=", the exe line with its redirections).
    public class GameBuild
    {
        public string Key, Label, BatName, Bat, Exe, SettingsFile, LogOut, LogErr, Problem;
        public readonly Dictionary<string, string> Env = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        public readonly List<string> Unset = new List<string>();
        public readonly List<KeyValuePair<string, string>> Copies = new List<KeyValuePair<string, string>>();   // the bat's log rotation
        public bool FullHd, Overlay, Aa, FullScreen, RealRes, Ultrawide, Lan, SpursSpus;   // what this exe knows (looked for in the exe itself)

        public bool Available { get { return Problem == null && Exe != null && File.Exists(Exe); } }
        public string ExeName { get { return Exe == null ? "" : Path.GetFileName(Exe); } }

        public static string LabelOf(string key)
        {
            return key == "test" ? L.T("시험판") : L.T("정식판");
        }

        public static GameBuild Read(Launcher launcher, string key)
        {
            var b = new GameBuild { Key = key, Label = LabelOf(key), BatName = key == "test" ? "PLAY_GT6_TEST.bat" : "PLAY_GT6.bat" };
            b.Bat = Path.Combine(launcher.Dir, b.BatName);
            b.SettingsFile = Path.Combine(launcher.Dir, "GT6Menu.settings.json");   // the menu's default: beside the exe
            if (!File.Exists(b.Bat))
            {
                b.Problem = b.BatName + L.T("이 없습니다. 패키지의 파일을 함께 두어야 합니다.");
                return b;
            }
            string dir = launcher.Dir + "\\";
            const RegexOptions I = RegexOptions.IgnoreCase;
            foreach (string raw in File.ReadAllLines(b.Bat))
            {
                string line = raw.Trim();
                Match m = Regex.Match(line, "^set \"([A-Za-z0-9_]+)=(.*)\"$", I);
                if (m.Success)
                {
                    string name = m.Groups[1].Value, value = m.Groups[2].Value.Replace("%~dp0", dir);
                    if (value.Contains("%"))
                        continue;   // made of another variable (GT6_EXIT=%ERRORLEVEL%): not a setting of the game
                    if (value.Length == 0)
                    {
                        b.Env.Remove(name);
                        b.Unset.Add(name);
                    }
                    else
                        b.Env[name] = value;
                    continue;
                }
                m = Regex.Match(line, "^for %%V in \\(([^)]*)\\) do set \"%%V=\"$", I);
                if (m.Success)
                {
                    foreach (string name in m.Groups[1].Value.Split(new[] { ' ' }, StringSplitOptions.RemoveEmptyEntries))
                    {
                        b.Env.Remove(name);
                        b.Unset.Add(name);
                    }
                    continue;
                }
                m = Regex.Match(line, "^if exist \"[^\"]+\" copy /y \"([^\"]+)\" \"([^\"]+)\"", I);
                if (m.Success)
                {
                    b.Copies.Add(new KeyValuePair<string, string>(Path.Combine(launcher.Dir, m.Groups[1].Value), Path.Combine(launcher.Dir, m.Groups[2].Value)));
                    continue;
                }
                m = Regex.Match(line, "^\"%~dp0([^\"]+\\.exe)\"\\s*>\\s*\"([^\"]+)\"\\s*2>\\s*\"([^\"]+)\"$", I);
                if (m.Success)
                {
                    b.Exe = Path.Combine(launcher.Dir, m.Groups[1].Value);
                    b.LogOut = Path.Combine(launcher.Dir, m.Groups[2].Value);
                    b.LogErr = Path.Combine(launcher.Dir, m.Groups[3].Value);
                }
            }
            if (b.Exe == null)
                b.Problem = b.BatName + L.T("에서 게임을 시작하는 줄을 찾지 못했습니다 (파일 형식이 바뀌었습니다).");
            string settings;
            if (b.Env.TryGetValue("GT6_MENU_SETTINGS", out settings))
                b.SettingsFile = settings;
            if (b.Available)
                Capabilities(launcher, b);
            return b;
        }

        // Which switches the exe has: the names are looked for in the file once and remembered by its size and time
        // (launcher.ini [Exe]). An older exe without "MenuFullHd" ignores the Full HD switch; the launcher says so.
        static void Capabilities(Launcher launcher, GameBuild b)
        {
            try
            {
                var file = new FileInfo(b.Exe);
                string stamp = "6:" + file.Length + ":" + file.LastWriteTimeUtc.Ticks;   // (6: added GT6_SPURS_SPUS)
                string[] known = launcher.Value("Exe", b.ExeName, "").Split('|');
                if (known.Length != 2 || known[0] != stamp)
                {
                    bool[] found = Scan(b.Exe, "MenuFullHd", "PS3_USRDIR_OVERLAY", "GT6_AA", "GT6_FULLSCREEN", "ScreenRealRes", "GT6_ASPECT", "GT6_LAN", "GT6_SPURS_SPUS");
                    known = new[] { stamp, (found[0] ? "F" : "") + (found[1] ? "O" : "") + (found[2] ? "A" : "") + (found[3] ? "S" : "") + (found[4] ? "R" : "") + (found[5] ? "W" : "") + (found[6] ? "L" : "") + (found[7] ? "P" : "") };
                    launcher.Set("Exe", b.ExeName, known[0] + "|" + known[1]);
                }
                b.FullHd = known[1].Contains("F");
                b.Overlay = known[1].Contains("O");
                b.Aa = known[1].Contains("A");
                b.FullScreen = known[1].Contains("S");
                b.RealRes = known[1].Contains("R");   // [GT6 0102] the F2 menu's real resolution ("ScreenRealRes")
                b.Ultrawide = known[1].Contains("W"); // GT6_ASPECT ultrawide support
                b.Lan = known[1].Contains("L");       // GT6_LAN LAN mode + patches
                b.SpursSpus = known[1].Contains("P"); // GT6_SPURS_SPUS SPU thread override
            }
            catch (Exception)
            {
            }
        }

        static bool[] Scan(string path, params string[] names)
        {
            var found = new bool[names.Length];
            byte[][] needles = names.Select(n => Encoding.ASCII.GetBytes(n)).ToArray();
            const int Chunk = 4 << 20, Overlap = 64;
            var buffer = new byte[Chunk + Overlap];
            using (var file = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite, 1 << 16, FileOptions.SequentialScan))
            {
                int keep = 0;
                while (!found.All(f => f))
                {
                    int read = file.Read(buffer, keep, Chunk);
                    if (read <= 0)
                        break;
                    int length = keep + read;
                    for (int k = 0; k < needles.Length; k++)
                        if (!found[k] && IndexOf(buffer, length, needles[k]) >= 0)
                            found[k] = true;
                    keep = Math.Min(Overlap, length);
                    Buffer.BlockCopy(buffer, length - keep, buffer, 0, keep);
                }
            }
            return found;
        }

        static int IndexOf(byte[] hay, int length, byte[] needle)
        {
            int at = 0, last = length - needle.Length;
            while (at <= last)
            {
                at = Array.IndexOf(hay, needle[0], at, last - at + 1);
                if (at < 0)
                    return -1;
                int j = 1;
                while (j < needle.Length && hay[at + j] == needle[j])
                    j++;
                if (j == needle.Length)
                    return at;
                at++;
            }
            return -1;
        }

        public string Describe()
        {
            if (!Available)
                return Label + L.T(" (없음)");
            return Label + " · " + ExeName + " · " + File.GetLastWriteTime(Exe).ToString("yyyy-MM-dd HH:mm");
        }
    }

    // The F2 menu's settings file (JSON written by the game). The launcher changes the resolution keys only - Full HD
    // ("MenuFullHd") and the real resolution ("ScreenRealRes": 0 = off, 1 = 1920x1080, 2 = 2560x1440, 3 = 3840x2160),
    // both read by the game when it starts - by editing those values in place: every other key stays as the menu wrote
    // it. Never while the game runs (the menu writes the file itself).
    public static class MenuSettings
    {
        static readonly Regex Key = new Regex("\"MenuFullHd\"\\s*:\\s*(true|false)");
        static readonly Regex RealKey = new Regex("\"ScreenRealRes\"\\s*:\\s*(-?\\d+)");

        public static bool FullHd(string path)
        {
            try
            {
                Match m = Key.Match(File.ReadAllText(path));
                return m.Success && m.Groups[1].Value == "true";
            }
            catch (Exception)
            {
                return false;
            }
        }

        // 0 = off (the game's own Full HD picture), 1..3 = drawn for real at 1920x1080, 2560x1440, 3840x2160.
        public static int RealRes(string path)
        {
            try
            {
                Match m = RealKey.Match(File.ReadAllText(path));
                int value;
                return m.Success && int.TryParse(m.Groups[1].Value, out value) ? Math.Max(0, Math.Min(3, value)) : 0;
            }
            catch (Exception)
            {
                return 0;
            }
        }

        public static void SetFullHd(string path, bool on)
        {
            SetResolution(path, on, -1);
        }

        // Both resolution keys in one write; realRes < 0 leaves "ScreenRealRes" as it is (an exe that does not know it).
        public static void SetResolution(string path, bool fullHd, int realRes)
        {
            string text = File.Exists(path) ? File.ReadAllText(path) : "";
            string next = Put(text, Key, "MenuFullHd", fullHd ? "true" : "false");
            if (realRes >= 0)
                next = Put(next, RealKey, "ScreenRealRes", Math.Min(3, realRes).ToString());
            if (next == text)
                return;
            if (File.Exists(path))
                File.Copy(path, path + ".launcher.bak", true);
            string temp = path + ".launcher.tmp";
            File.WriteAllText(temp, next, new UTF8Encoding(false));
            if (File.Exists(path))
                File.Delete(path);
            File.Move(temp, path);
        }

        // One key's value replaced in place, or the key added first in the object (no file yet: the menu adds the rest
        // with its defaults).
        static string Put(string text, Regex key, string name, string value)
        {
            Match m = key.Match(text);
            if (m.Success)
                return text.Substring(0, m.Groups[1].Index) + value + text.Substring(m.Groups[1].Index + m.Groups[1].Length);
            int brace = text.IndexOf('{');
            if (brace < 0)
                return "{\n  \"" + name + "\": " + value + "\n}\n";
            bool empty = text.Substring(brace + 1).TrimStart().StartsWith("}");
            return text.Substring(0, brace + 1) + "\n  \"" + name + "\": " + value + (empty ? "\n" : ",") + text.Substring(brace + 1);
        }
    }

    // The game files the port needs: the 1.22 ELF, the game data (BCES01893, 01.22) with its PDIPFS volume, and the
    // lifted system module in game\firmware.
    public class GameFiles
    {
        public const string TitleId = "BCES01893", Version = "01.22";
        public bool Elf, Data, Volume, Firmware;
        public string Title = "Gran Turismo 6", DataVersion = "";

        public static GameFiles Check(Launcher launcher)
        {
            var files = new GameFiles { Elf = File.Exists(launcher.Elf) };
            Dictionary<string, string> sfo = Sfo.Read(Path.Combine(launcher.TitleDir, "PARAM.SFO"));
            files.DataVersion = Sfo.Get(sfo, "APP_VER");
            files.Data = Sfo.Get(sfo, "TITLE_ID") == TitleId && files.DataVersion == Version;
            if (Sfo.Get(sfo, "TITLE").Length > 0)
                files.Title = Sfo.Get(sfo, "TITLE").Replace("\n", " ").Trim();
            files.Volume = Directory.Exists(Path.Combine(launcher.TitleDir, "USRDIR", "PDIPFS"));
            files.Firmware = File.Exists(Path.Combine(launcher.GameDir, "firmware", "libsre.prx"));
            return files;
        }

        public bool Ready { get { return Elf && Data && Volume && Firmware; } }

        public string Cover(Launcher launcher, string name)
        {
            string path = Path.Combine(launcher.TitleDir, name);
            return File.Exists(path) ? path : null;
        }
    }

    public static class Game
    {
        public static bool IsRunning
        {
            get
            {
                try
                {
                    return Process.GetProcesses().Any(p => p.ProcessName.StartsWith("GT6Recomp", StringComparison.OrdinalIgnoreCase));
                }
                catch (Exception)
                {
                    return false;
                }
            }
        }

        // Why the game cannot start, or null.
        public static string Problem(Launcher launcher, GameBuild build)
        {
            if (build.Problem != null)
                return build.Problem;
            if (!File.Exists(build.Exe))
                return build.ExeName + L.T("가 없습니다. 패키지의 파일을 함께 두어야 합니다 (런처는 ") + build.BatName + L.T("과 같은 폴더).");
            if (IsRunning)
                return L.T("게임이 이미 실행 중입니다.");
            GameFiles files = GameFiles.Check(launcher);
            if (!files.Elf)
                return L.T("game\\EMAIN.ELF(1.22)가 없습니다.");
            if (!files.Data)
                return "game\\dev_hdd0\\game\\" + GameFiles.TitleId + L.T("에 1.22 게임 데이터가 없습니다") + (files.DataVersion.Length > 0 ? L.T(" (지금 ") + files.DataVersion + ")" : "") + ".";
            if (!files.Volume)
                return "game\\dev_hdd0\\game\\" + GameFiles.TitleId + L.T("\\USRDIR\\PDIPFS 폴더가 없습니다.");
            if (!files.Firmware)
                return L.T("game\\firmware에 시스템 모듈(libsre.prx)이 없습니다.");
            return null;
        }
    }

    public class RunResult
    {
        public int ExitCode;
        public TimeSpan Ran;
        public string LogPath, Build;
        public Diagnosis Diagnosis;
    }

    // Starts the game as the bat does: the same environment, the same log files (cmd's own > and 2> redirection, so the
    // game writes its logs straight to the files), no console window.
    public class GameRunner
    {
        readonly Launcher launcher;
        Process process;
        DateTime started;
        public event Action<RunResult> Finished;

        public GameRunner(Launcher launcher)
        {
            this.launcher = launcher;
        }

        public bool Running { get { return process != null; } }

        public string Start()
        {
            if (process != null)
                return L.T("게임이 이미 실행 중입니다.");
            GameBuild build = launcher.Build;
            string problem = Game.Problem(launcher, build);
            if (problem != null)
                return problem;
            string overlay;
            try
            {
                overlay = ModStore.Prepare(launcher);
            }
            catch (Exception error)
            {
                return L.T("모드를 준비하지 못했습니다: ") + error.Message;
            }
            if (overlay != null && !build.Overlay)
                return L.T("켜 둔 모드가 있지만 ") + build.ExeName + L.T("는 모드 폴더를 읽지 못하는 빌드입니다. 다른 빌드를 고르거나 모드를 꺼 주세요.");
            if (launcher.On("AutoBackup"))
            {
                try
                {
                    SaveStore.Backup(launcher, "auto");
                }
                catch (Exception error)   // the game still starts; the record says the backup was not made
                {
                    Logs.Record(launcher, DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss") + " save backup failed: " + error.Message + "\r\n");
                }
            }
            Directory.CreateDirectory(Path.GetDirectoryName(build.LogOut));
            Directory.CreateDirectory(Path.GetDirectoryName(build.LogErr));
            foreach (KeyValuePair<string, string> copy in build.Copies)
            {
                try
                {
                    if (File.Exists(copy.Key))
                        File.Copy(copy.Key, copy.Value, true);
                }
                catch (Exception)
                {
                }
            }
            string command = "\"" + build.Exe + "\" >\"" + build.LogOut + "\" 2>\"" + build.LogErr + "\"";
            var info = new ProcessStartInfo(Environment.GetEnvironmentVariable("ComSpec") ?? "cmd.exe", "/d /s /c \"" + command + "\"")
            {
                WorkingDirectory = launcher.Dir, UseShellExecute = false, CreateNoWindow = true,
            };
            var record = new StringBuilder();
            record.AppendLine(DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss") + " start " + build.Key + " " + build.ExeName + " (" + build.BatName + ": " + build.Env.Count + " set, " + build.Unset.Count + " removed)");
            foreach (string name in build.Unset)
                info.EnvironmentVariables.Remove(name);
            foreach (KeyValuePair<string, string> pair in build.Env)
                info.EnvironmentVariables[pair.Key] = pair.Value;
            foreach (KeyValuePair<string, string> pair in Settings.Overrides(launcher, overlay))
            {
                if (pair.Value == null)
                    info.EnvironmentVariables.Remove(pair.Key);
                else
                {
                    info.EnvironmentVariables[pair.Key] = pair.Value;
                    record.AppendLine("    " + pair.Key + "=" + pair.Value);
                }
            }
            var p = new Process { StartInfo = info, EnableRaisingEvents = true };
            string buildKey = build.Key, logErr = build.LogErr;
            p.Exited += delegate { OnExited(p, buildKey, logErr); };
            try
            {
                p.Start();
            }
            catch (Exception error)
            {
                return L.T("게임을 시작하지 못했습니다: ") + error.Message;
            }
            process = p;
            started = DateTime.Now;
            Logs.Record(launcher, record.ToString());
            return null;
        }

        void OnExited(Process p, string build, string logErr)
        {
            int code = p.ExitCode;
            TimeSpan ran = DateTime.Now - started;
            var result = new RunResult { ExitCode = code, Ran = ran, LogPath = logErr, Build = build };
            result.Diagnosis = Diagnosis.Of(code, Logs.Tail(logErr, 300));
            Logs.Record(launcher, DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss") + " exit " + code + " (0x" + code.ToString("X8") + ") after " + ran.ToString(@"hh\:mm\:ss") + "\r\n");
            Application.Current.Dispatcher.BeginInvoke((Action)delegate
            {
                process = null;
                p.Dispose();
                launcher.AddPlayTime(ran, code, build);
                if (Finished != null)
                    Finished(result);
            });
        }
    }

    // A failed run in plain words, from the end of its log and the exit code.
    public class Diagnosis
    {
        public string Title, Detail, Advice;

        static Diagnosis Make(string title, string detail, string advice)
        {
            return new Diagnosis { Title = title, Detail = detail, Advice = advice };
        }

        public static Diagnosis Of(int code, string log)
        {
            string text = log ?? "";
            uint status = unchecked((uint)code);
            if (code == 0)
                return null;
            // What the game itself said last before it stopped: its own "ERROR:" line or the reason it logs as [gt6-fatal].
            string own = text.Split('\n').Select(l => l.Trim()).LastOrDefault(l => l.StartsWith("ERROR:") || l.Contains("[gt6-fatal]"));
            if (Regex.IsMatch(text, "cannot open ELF|failed to load ELF|ELF load failed|not an ELF|not a 64-bit big-endian ELF|could not load .*\\.ELF", RegexOptions.IgnoreCase))
                return Make(L.T("EMAIN.ELF를 읽지 못했습니다"), own ?? LastError(text, L.T("game\\EMAIN.ELF가 손상되었거나 1.22 ELF가 아닙니다.")), L.T("game\\EMAIN.ELF가 손상되었거나 1.22 ELF가 아닙니다. 패키지의 game 폴더가 온전한지 확인해 주세요."));
            if (Regex.IsMatch(text, "DXGI_ERROR|DEVICE_REMOVED|DEVICE_HUNG|device removed|D3D12CreateDevice", RegexOptions.IgnoreCase))
                return Make(L.T("그래픽카드 오류로 끝났습니다"), LastError(text, L.T("그래픽 드라이버가 응답하지 않았습니다.")), L.T("그래픽 드라이버를 최신으로 업데이트하고 다시 실행해 보세요. 설정에서 Full HD를 끄면 부담이 줄어듭니다."));
            if (text.Contains("bad_alloc") || status == 0xC0000017u)
                return Make(L.T("메모리가 부족했습니다"), L.T("게임이 메모리를 더 받지 못했습니다."), L.T("다른 프로그램을 닫고 다시 실행해 보세요."));
            if (status == 0xC0000135u || status == 0xC0000139u || status == 0xC000007Bu)
                return Make(L.T("필요한 DLL이 없습니다"), L.T("게임 폴더의 DLL(SDL2.dll, vcruntime140.dll 등)을 찾지 못했습니다 (0x") + status.ToString("X8") + ").", L.T("패키지의 파일을 모두 같은 폴더에 두었는지 확인해 주세요."));
            if (own != null && status < 0xC0000000u)
                return Make(L.T("게임이 스스로 멈췄습니다"), own + L.T(" (종료 코드 ") + code + ")", L.T("로그 페이지에서 그 줄의 앞뒤를 확인해 주세요. 계속되면 로그를 함께 알려 주세요."));
            if (status == 0xFFFFFFFFu || code == 1)   // also what the task manager's "작업 끝내기" leaves
                return Make(L.T("게임이 강제로 종료되었습니다"), L.T("작업 관리자 등 밖에서 게임을 끝냈습니다 (종료 코드 ") + code + ").", L.T("직접 끄신 것이 아니라면 로그를 확인해 주세요."));
            if (status >= 0xC0000000u)
                return Make(L.T("게임이 오류로 멈췄습니다"), L.T("종료 코드 0x") + status.ToString("X8") + (status == 0xC0000005u ? L.T(" (메모리 접근 오류)") : "") + (own != null ? "\n" + own : ""), L.T("다시 실행해 보고, 계속되면 로그 페이지의 내용을 함께 알려 주세요."));
            return Make(L.T("게임이 오류로 끝났습니다"), LastError(text, L.T("종료 코드 ") + code), L.T("로그에서 마지막 오류를 확인해 주세요."));
        }

        static string LastError(string text, string fallback)
        {
            string line = text.Split('\n').Select(l => l.Trim()).LastOrDefault(l => Regex.IsMatch(l, "error|fatal|fail", RegexOptions.IgnoreCase) && !l.StartsWith("#"));
            return line ?? fallback;
        }
    }

    public class LogInfo
    {
        public string Path, Label;
        public DateTime Changed;
        public long Size;
    }

    // The log files of the package: each build's output and error files (and the copies of the run before), as the bats
    // name them, and the launcher's own records.
    public static class Logs
    {
        public static List<LogInfo> List(Launcher launcher)
        {
            var logs = new List<LogInfo>();
            Action<string, string> add = delegate(string path, string label)
            {
                try
                {
                    var file = new FileInfo(path);
                    if (file.Exists && !logs.Any(l => l.Path.Equals(path, StringComparison.OrdinalIgnoreCase)))
                        logs.Add(new LogInfo { Path = path, Label = label, Changed = file.LastWriteTime, Size = file.Length });
                }
                catch (Exception)
                {
                }
            };
            foreach (string key in new[] { "test", "release" })
            {
                GameBuild b = GameBuild.Read(launcher, key);
                if (b.Exe == null)
                    continue;
                add(b.LogErr, b.Label + L.T(" · 진단 출력"));
                add(b.LogOut, b.Label + L.T(" · 일반 출력"));
                foreach (KeyValuePair<string, string> copy in b.Copies)
                    add(copy.Value, b.Label + L.T(" · 그 전 실행 (") + (copy.Key.Equals(b.LogErr, StringComparison.OrdinalIgnoreCase) ? L.T("진단") : L.T("일반")) + ")");
            }
            add(Path.Combine(launcher.LauncherLogsDir, "runs.log"), L.T("런처 · 실행 기록"));
            add(Path.Combine(launcher.LauncherLogsDir, "update.log"), L.T("런처 · 업데이트 기록"));
            return logs.OrderByDescending(l => l.Changed).ToList();
        }

        // The last lines of a log (it may be large: only its end is read).
        public static string Tail(string path, int lines)
        {
            try
            {
                using (var file = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
                {
                    long start = Math.Max(0, file.Length - 256 * 1024);
                    file.Position = start;
                    string text = new StreamReader(file, Encoding.UTF8).ReadToEnd();
                    string[] all = text.Split('\n');
                    return string.Join("\n", all.Skip(Math.Max(start > 0 ? 1 : 0, all.Length - lines)));
                }
            }
            catch (Exception)
            {
                return "";
            }
        }

        public static void Record(Launcher launcher, string text)
        {
            try
            {
                Directory.CreateDirectory(launcher.LauncherLogsDir);
                File.AppendAllText(Path.Combine(launcher.LauncherLogsDir, "runs.log"), text, new UTF8Encoding(false));
            }
            catch (Exception)
            {
            }
        }
    }

    public class SaveInfo
    {
        public string Dir, Name, Title, SubTitle, Detail, Icon;
        public DateTime Changed;
        public long Size;
    }

    // The save data (game\dev_hdd0\home\00000001\savedata\*) and its backups: one zip of every save,
    // save_backups\savedata-<time>-<why>.zip.
    public static class SaveStore
    {
        public static List<SaveInfo> List(Launcher launcher)
        {
            var saves = new List<SaveInfo>();
            if (!Directory.Exists(launcher.SavesDir))
                return saves;
            foreach (string dir in Directory.GetDirectories(launcher.SavesDir).OrderBy(d => d, StringComparer.OrdinalIgnoreCase))
            {
                Dictionary<string, string> sfo = Sfo.Read(Path.Combine(dir, "PARAM.SFO"));
                var files = new DirectoryInfo(dir).GetFiles();
                string icon = Path.Combine(dir, "ICON0.PNG");
                saves.Add(new SaveInfo
                {
                    Dir = dir, Name = Path.GetFileName(dir), Title = Sfo.Get(sfo, "TITLE"), SubTitle = Sfo.Get(sfo, "SUB_TITLE"),
                    Detail = Sfo.Get(sfo, "DETAIL"), Icon = File.Exists(icon) ? icon : null,
                    Changed = files.Length > 0 ? files.Max(f => f.LastWriteTime) : Directory.GetLastWriteTime(dir),
                    Size = files.Sum(f => f.Length),
                });
            }
            return saves;
        }

        public static List<string> Backups(Launcher launcher)
        {
            if (!Directory.Exists(launcher.BackupsDir))
                return new List<string>();
            return Directory.GetFiles(launcher.BackupsDir, "savedata-*.zip").OrderByDescending(f => f, StringComparer.Ordinal).ToList();
        }

        // A zip of every save; the oldest automatic ones beyond the kept number are removed. null when there is nothing.
        public static string Backup(Launcher launcher, string why)
        {
            List<SaveInfo> saves = List(launcher);
            if (saves.Count == 0)
                return null;
            Directory.CreateDirectory(launcher.BackupsDir);
            string target = Path.Combine(launcher.BackupsDir, "savedata-" + DateTime.Now.ToString("yyyyMMdd-HHmmss") + "-" + why + ".zip");
            using (ZipArchive zip = ZipFile.Open(target, ZipArchiveMode.Create))
                foreach (SaveInfo save in saves)
                    foreach (string file in Directory.GetFiles(save.Dir, "*", SearchOption.AllDirectories))
                        zip.CreateEntryFromFile(file, save.Name + "/" + file.Substring(save.Dir.Length + 1).Replace('\\', '/'));
            int keep;
            if (!int.TryParse(launcher.Setting("BackupsToKeep"), out keep) || keep < 1)
                keep = 10;
            // Only the automatic ones are thinned out: a backup made by hand or before a restore stays until the user removes it.
            foreach (string old in Backups(launcher).Where(f => f.EndsWith("-auto.zip", StringComparison.OrdinalIgnoreCase)).Skip(keep))
                File.Delete(old);
            return target;
        }

        // Puts a backup back: the saves there now are backed up first ("before-restore"), then replaced.
        public static void Restore(Launcher launcher, string backup)
        {
            if (Game.IsRunning)
                throw new InvalidOperationException(L.T("게임이 실행 중입니다. 게임을 끈 다음 복원해 주세요."));
            Backup(launcher, "before-restore");
            string temp = Path.Combine(launcher.BackupsDir, "restore-" + DateTime.Now.ToString("yyyyMMdd-HHmmss"));
            Zip.Extract(backup, temp);
            Directory.CreateDirectory(launcher.SavesDir);
            foreach (string dir in Directory.GetDirectories(temp))
            {
                string target = Path.Combine(launcher.SavesDir, Path.GetFileName(dir));
                if (Directory.Exists(target))
                    Directory.Delete(target, true);   // only the saves the backup holds are replaced
                Directory.Move(dir, target);
            }
            Directory.Delete(temp, true);
        }

        public static string Describe(string backup)
        {
            Match m = Regex.Match(Path.GetFileNameWithoutExtension(backup), @"savedata-(\d{8})-(\d{6})-(.+)");
            if (!m.Success)
                return Path.GetFileName(backup);
            DateTime when;
            string why = m.Groups[3].Value == "auto" ? L.T("실행 전 자동") : m.Groups[3].Value == "before-restore" ? L.T("복원 전") : m.Groups[3].Value == "manual" ? L.T("직접") : m.Groups[3].Value;
            return DateTime.TryParseExact(m.Groups[1].Value + m.Groups[2].Value, "yyyyMMddHHmmss", null, System.Globalization.DateTimeStyles.None, out when)
                ? when.ToString("yyyy-MM-dd HH:mm:ss") + " · " + why : Path.GetFileName(backup);
        }
    }

    public static class Zip
    {
        // Unpacks a zip into a new folder; an entry that points outside it (..\) is refused.
        public static void Extract(string zip, string target)
        {
            string root = Path.GetFullPath(target).TrimEnd('\\') + "\\";
            Directory.CreateDirectory(root);
            using (ZipArchive archive = ZipFile.OpenRead(zip))
            {
                foreach (ZipArchiveEntry entry in archive.Entries)
                {
                    string path = Path.GetFullPath(Path.Combine(root, entry.FullName.Replace('/', '\\')));
                    if (!path.StartsWith(root, StringComparison.OrdinalIgnoreCase))
                        throw new InvalidDataException(L.T("zip 안의 경로가 폴더 밖을 가리킵니다: ") + entry.FullName);
                    if (entry.FullName.EndsWith("/") || entry.FullName.EndsWith("\\"))
                    {
                        Directory.CreateDirectory(path);
                        continue;
                    }
                    Directory.CreateDirectory(Path.GetDirectoryName(path));
                    entry.ExtractToFile(path, true);
                }
            }
        }
    }
}
