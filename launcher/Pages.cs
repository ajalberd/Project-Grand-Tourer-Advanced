// The pages: home, settings, mods, saves, logs, update and tools.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Threading;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Effects;
using System.Windows.Media.Imaging;

namespace GT6Launcher
{
    // A page built again each time it is shown, so it always shows the files as they are.
    public abstract class PageBase : IPage
    {
        protected readonly MainWindow Window;
        readonly ContentControl view = new ContentControl();

        protected PageBase(MainWindow window)
        {
            Window = window;
        }

        protected Launcher Launcher { get { return Window.Launcher; } }
        public FrameworkElement View { get { return view; } }

        public virtual void Shown()
        {
            view.Content = Build();
        }

        public virtual void Hidden()
        {
        }

        protected abstract FrameworkElement Build();

        public static ImageSource Image(string path)
        {
            if (path == null || !File.Exists(path))
                return null;
            try
            {
                var image = new BitmapImage();
                image.BeginInit();
                image.CacheOption = BitmapCacheOption.OnLoad; // the file is not kept open
                image.UriSource = new Uri(path);
                image.EndInit();
                return image;
            }
            catch (Exception)
            {
                return null;
            }
        }

        public static FrameworkElement Line(string label, string value, Brush valueBrush = null)
        {
            var grid = new Grid { Margin = new Thickness(0, 0, 0, 8) };
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(104) });
            grid.ColumnDefinitions.Add(new ColumnDefinition());
            grid.Children.Add(Ui.Text(label, "Caption"));
            var text = Ui.Text(value, "Body");
            text.TextWrapping = TextWrapping.Wrap;
            if (valueBrush != null)
                text.Foreground = valueBrush;
            Grid.SetColumn(text, 1);
            grid.Children.Add(text);
            return grid;
        }

        // The drop-down of the two builds (home and settings).
        protected UIElement BuildPicker()
        {
            GameBuild test = GameBuild.Read(Launcher, "test"), release = GameBuild.Read(Launcher, "release");
            bool haveTest = File.Exists(test.Bat), haveRelease = File.Exists(release.Bat);
            if (haveTest != haveRelease)   // one build in the package (no test build is out): nothing to choose
                return Ui.Chip("", (haveTest ? test : release).Describe(), Ui.Brush("SubText"));
            return Ui.Choices(Launcher.Build.Key, delegate(string key)
            {
                Launcher.SetSetting("Build", key);
                Window.Refresh();
            }, "test", test.Describe(), "release", release.Describe());
        }

        protected bool GameRunning { get { return Launcher.Runner.Running || Game.IsRunning; } }
    }

    public class HomePage : PageBase
    {
        public HomePage(MainWindow window) : base(window)
        {
        }

        protected override FrameworkElement Build()
        {
            GameFiles files = GameFiles.Check(Launcher);
            GameBuild build = Launcher.Build;
            var stack = new StackPanel { Margin = new Thickness(16, 8, 16, 24) };
            stack.Children.Add(Hero(files, build));
            Border filesCard = FilesCard(files, build), record = RecordCard(), pcCard = PcCard(PcInfo.Read());
            filesCard.Margin = record.Margin = new Thickness(0, 0, 16, 16);
            stack.Children.Add(Ui.Columns(3, filesCard, record, pcCard));
            return new ScrollViewer { Content = stack, VerticalScrollBarVisibility = ScrollBarVisibility.Auto };
        }

        FrameworkElement Hero(GameFiles files, GameBuild build)
        {
            var hero = new Grid { Height = 372, Margin = new Thickness(0, 0, 0, 18) };
            hero.SizeChanged += delegate { hero.Clip = new RectangleGeometry(new Rect(0, 0, hero.ActualWidth, hero.ActualHeight), 24, 24); };
            var gradient = new LinearGradientBrush { StartPoint = new Point(0, 0), EndPoint = new Point(1, 1) };
            gradient.GradientStops.Add(new GradientStop(Color.FromRgb(0x2A, 0x12, 0x0E), 0));
            gradient.GradientStops.Add(new GradientStop(Color.FromRgb(0x14, 0x16, 0x1E), 0.55));
            gradient.GradientStops.Add(new GradientStop(Color.FromRgb(0x0C, 0x0D, 0x12), 1));
            hero.Children.Add(new Border { Background = gradient, CornerRadius = new CornerRadius(24), BorderBrush = Ui.Brush("Line"), BorderThickness = new Thickness(1) });
            FrameworkElement art = RevArt();
            art.HorizontalAlignment = HorizontalAlignment.Right;
            art.VerticalAlignment = VerticalAlignment.Bottom;
            art.Margin = new Thickness(0);
            hero.Children.Add(art);

            // The game's own icon from the user's game data (ICON0.PNG, 320x176), top right.
            ImageSource cover = Image(files.Cover(Launcher, "ICON0.PNG"));
            if (cover != null)
            {
                hero.Children.Add(new Border
                {
                    Width = 224, Height = 123, CornerRadius = new CornerRadius(14), HorizontalAlignment = HorizontalAlignment.Right, VerticalAlignment = VerticalAlignment.Top,
                    Margin = new Thickness(0, 34, 36, 0), Background = new ImageBrush(cover) { Stretch = Stretch.UniformToFill }, BorderBrush = Ui.Brush("Line"), BorderThickness = new Thickness(1),
                    Effect = new DropShadowEffect { Color = Color.FromRgb(0xFF, 0x6A, 0x3D), BlurRadius = 30, ShadowDepth = 0, Opacity = 0.35 },
                });
            }

            var left = new StackPanel { Margin = new Thickness(44, 26, 40, 26), VerticalAlignment = VerticalAlignment.Center, HorizontalAlignment = HorizontalAlignment.Left };
            var caption = Ui.Text("PLAYSTATION 3  ·  " + GameFiles.TitleId + (files.Data ? " " + files.DataVersion : "") + L.T("  ·  네이티브 PC 포트"), "Caption");
            caption.Foreground = Ui.Brush("Accent");
            caption.FontWeight = FontWeights.SemiBold;
            left.Children.Add(caption);
            left.Children.Add(new TextBlock
            {
                Text = files.Title, FontFamily = Ui.Res<FontFamily>("TitleFont"), FontSize = 40, FontWeight = FontWeights.Bold, Foreground = Brushes.White, Margin = new Thickness(0, 8, 0, 0),
                Effect = new DropShadowEffect { Color = Color.FromRgb(0xFF, 0x5A, 0x3D), BlurRadius = 26, ShadowDepth = 0, Opacity = 0.55 },
            });
            bool fullHd = build.FullHd && MenuSettings.FullHd(build.SettingsFile);
            int realRes = fullHd && build.RealRes && Launcher.Setting("AA") != "spu" ? MenuSettings.RealRes(build.SettingsFile) : 0;
            var subtitle = Ui.Text(build.Describe(), "Caption");
            subtitle.FontSize = 14;
            subtitle.Margin = new Thickness(0, 8, 0, 16);
            left.Children.Add(subtitle);

            var chips = new WrapPanel { MaxWidth = 600, Margin = new Thickness(0, 0, 0, 18) };
            chips.Children.Add(files.Ready ? Ui.Chip("", L.T("게임 파일 확인됨"), Ui.Brush("Good")) : Ui.Chip("", L.T("게임 파일 부족"), Ui.Brush("Warn")));
            chips.Children.Add(Ui.Chip("", SettingsPage.ResolutionName(fullHd, realRes), fullHd ? Ui.Brush("Accent") : Ui.Brush("SubText")));
            if (build.Lan && Launcher.On("LanMode"))
                chips.Children.Add(Ui.Chip("", "LAN", Ui.Brush("Good")));
            List<ModInfo> mods = ModStore.List(Launcher);
            int on = mods.Count(m => m.Enabled && m.Root != null);
            int inGame = GameMods.List(Launcher, false).Count;   // mods inside the game data itself: always applied
            if (inGame > 0)
                chips.Children.Add(Ui.Chip("", L.T("모드 ") + (inGame + on) + L.T("개 적용 (게임 데이터 ") + inGame + (on > 0 ? L.T(" · 추가 ") + on : "") + ")", Ui.Brush("Accent2")));
            else if (mods.Count > 0)
                chips.Children.Add(Ui.Chip("", on > 0 ? L.T("모드 ") + on + L.T("개 켜짐") : L.T("모드 꺼짐"), on > 0 ? Ui.Brush("Accent2") : Ui.Brush("SubText")));
            int saves = SaveStore.List(Launcher).Count;
            if (saves > 0)
                chips.Children.Add(Ui.Chip("", L.T("세이브 ") + saves + L.T("개") + (Launcher.On("AutoBackup") ? L.T(" · 시작 전 자동 백업") : ""), Ui.Brush("SubText")));
            foreach (UIElement chip in chips.Children)
                ((FrameworkElement)chip).Margin = new Thickness(0, 0, 8, 8);
            left.Children.Add(chips);

            var play = new Button { Style = Ui.Style("PlayButton"), Content = Ui.Row(Ui.Icon("", 20, Brushes.White), new TextBlock { Text = L.T("  플레이"), VerticalAlignment = VerticalAlignment.Center }) };
            // Without the game files (a new copy) the button leads to the setup.
            bool needSetup = SetupPage.Running || !files.Ready || new SetupRunner(Launcher).InstallPending;
            if (needSetup)
                play.Content = Ui.Row(Ui.Icon("", 20, Brushes.White), new TextBlock { Text = L.T("  설치하기"), VerticalAlignment = VerticalAlignment.Center });
            play.Click += delegate
            {
                if (needSetup)
                    Window.Navigate("setup");
                else
                    Window.Play();
            };
            var picker = (FrameworkElement)BuildPicker();
            picker.Margin = new Thickness(18, 0, 0, 0);
            picker.VerticalAlignment = VerticalAlignment.Center;
            var buttons = Ui.Row(play, picker);
            buttons.VerticalAlignment = VerticalAlignment.Center;
            left.Children.Add(buttons);
            var keys = Ui.Text(L.T("게임 안에서 F2: 트레이너 메뉴  ·  런처는 ") + build.BatName + L.T("과 같은 환경으로 게임을 시작합니다"), "Caption");
            keys.Margin = new Thickness(4, 14, 0, 0);
            left.Children.Add(keys);
            hero.Children.Add(left);
            return hero;
        }

        // A large rev counter in neon lines at the right edge, fading out toward the text (drawn here; no game artwork).
        static FrameworkElement RevArt()
        {
            var canvas = new Canvas { Width = 560, Height = 300, Opacity = 0.6, IsHitTestVisible = false, ClipToBounds = true };
            var warm = new SolidColorBrush(Color.FromRgb(0xFF, 0x6A, 0x3D));
            var cool = new SolidColorBrush(Color.FromRgb(0x8F, 0xA3, 0xC8));
            const double cx = 400, cy = 262, r = 190;
            Func<double, double, Point> at = (angle, radius) => new Point(cx + Math.Cos(angle * Math.PI / 180) * radius, cy + Math.Sin(angle * Math.PI / 180) * radius);
            Action<double, double, Brush, double> arc = delegate(double a0, double a1, Brush brush, double thickness)
            {
                var figure = new PathFigure { StartPoint = at(a0, r), IsClosed = false };
                figure.Segments.Add(new ArcSegment(at(a1, r), new Size(r, r), 0, a1 - a0 > 180, SweepDirection.Clockwise, true));
                var geometry = new PathGeometry();
                geometry.Figures.Add(figure);
                canvas.Children.Add(new System.Windows.Shapes.Path { Data = geometry, Stroke = brush, StrokeThickness = thickness, StrokeStartLineCap = PenLineCap.Round, StrokeEndLineCap = PenLineCap.Round });
            };
            arc(160, 318, cool, 2.4);
            arc(318, 380, warm, 5);                                                     // the red line
            for (int i = 0; i <= 22; i++)
            {
                double angle = 160 + i * 220.0 / 22;
                bool major = i % 2 == 0;
                Point p0 = at(angle, r - 12), p1 = at(angle, r - (major ? 34 : 24));
                canvas.Children.Add(new System.Windows.Shapes.Line { X1 = p0.X, Y1 = p0.Y, X2 = p1.X, Y2 = p1.Y, Stroke = angle >= 318 ? warm : cool, StrokeThickness = major ? 2.6 : 1.4 });
            }
            Point tip = at(326, r - 44);
            canvas.Children.Add(new System.Windows.Shapes.Line { X1 = cx, Y1 = cy, X2 = tip.X, Y2 = tip.Y, Stroke = warm, StrokeThickness = 4, StrokeEndLineCap = PenLineCap.Round });
            var hub = new System.Windows.Shapes.Ellipse { Width = 22, Height = 22, Fill = warm };
            Canvas.SetLeft(hub, cx - 11);
            Canvas.SetTop(hub, cy - 11);
            canvas.Children.Add(hub);
            var fade = new LinearGradientBrush { StartPoint = new Point(0, 0), EndPoint = new Point(1, 0) };
            fade.GradientStops.Add(new GradientStop(Colors.Transparent, 0.34));
            fade.GradientStops.Add(new GradientStop(Colors.White, 0.72));
            canvas.OpacityMask = fade;
            canvas.Effect = new DropShadowEffect { Color = Color.FromRgb(0xFF, 0x5A, 0x3D), BlurRadius = 16, ShadowDepth = 0, Opacity = 0.8 };
            return canvas;
        }

        Border FilesCard(GameFiles files, GameBuild build)
        {
            var stack = new StackPanel();
            Func<bool, string, string, FrameworkElement> item = delegate(bool ok, string label, string value)
            {
                return Line(label, (ok ? "✓ " : "✕ ") + value, ok ? Ui.Brush("Good") : Ui.Brush("Warn"));
            };
            stack.Children.Add(item(files.Elf, "EMAIN.ELF", files.Elf ? "1.22 ELF" : L.T("없음")));
            stack.Children.Add(item(files.Data, L.T("게임 데이터"), files.Data ? GameFiles.TitleId + " " + files.DataVersion : files.DataVersion.Length > 0 ? files.DataVersion + L.T(" (1.22 필요)") : L.T("없음")));
            stack.Children.Add(item(files.Volume, "PDIPFS", files.Volume ? L.T("볼륨 폴더 있음") : L.T("USRDIR\\PDIPFS 없음")));
            stack.Children.Add(item(files.Firmware, L.T("시스템 모듈"), files.Firmware ? "libsre" : L.T("game\\firmware에 없음")));
            stack.Children.Add(item(build.Available, L.T("실행 파일"), build.Available ? build.Label : build.Label + L.T(" 없음")));
            var open = Ui.Button(L.T("game 폴더 열기"), delegate { Ui.OpenFolder(Launcher.GameDir); }, null, "", new Thickness(0, 6, 0, 0));
            open.HorizontalAlignment = HorizontalAlignment.Left;
            stack.Children.Add(open);
            return Ui.Card(L.T("게임 파일"), stack);
        }

        Border RecordCard()
        {
            long seconds;
            long.TryParse(Launcher.State("PlayTimeSeconds", "0"), out seconds);
            string last = Launcher.State("LastPlayed", ""), code = Launcher.State("LastExitCode", ""), lastBuild = Launcher.State("LastBuild", "");
            var stack = new StackPanel();
            stack.Children.Add(Line(L.T("마지막 플레이"), last.Length > 0 ? last + (lastBuild.Length > 0 ? " (" + GameBuild.LabelOf(lastBuild) + ")" : "") : L.T("런처로는 아직 없음")));
            stack.Children.Add(Line(L.T("총 플레이 시간"), seconds / 3600 + L.T("시간 ") + seconds / 60 % 60 + L.T("분")));
            if (code.Length > 0)
            {
                bool ok = code == "0";
                stack.Children.Add(Line(L.T("최근 실행"), ok ? L.T("정상 종료") : L.T("오류로 끝남 (코드 ") + code + ")", ok ? null : Ui.Brush("Bad")));
                if (!ok)
                {
                    var logs = Ui.Button(L.T("로그 보기"), delegate { Window.Navigate("logs"); }, null, "", new Thickness(0, 6, 0, 0));
                    logs.HorizontalAlignment = HorizontalAlignment.Left;
                    stack.Children.Add(logs);
                }
            }
            return Ui.Card(L.T("플레이 기록"), stack);
        }

        Border PcCard(PcInfo pc)
        {
            var stack = new StackPanel();
            stack.Children.Add(Line("CPU", pc.Cpu + " · " + pc.Threads + L.T("스레드")));
            stack.Children.Add(Line(L.T("그래픽카드"), pc.Gpu + " (" + PcInfo.Size(pc.Vram) + ")"));
            stack.Children.Add(Line(L.T("메모리"), PcInfo.Size(pc.Ram)));
            stack.Children.Add(Line(L.T("모니터"), pc.ScreenWidth + "×" + pc.ScreenHeight));
            return Ui.Card(L.T("이 PC"), stack);
        }
    }

    public class SettingsPage : PageBase
    {
        public SettingsPage(MainWindow window) : base(window)
        {
        }

        FrameworkElement Toggle(string key, string label, string hint)
        {
            return Ui.ToggleRow(label, hint, Launcher.On(key), delegate(bool on) { Launcher.SetSetting(key, on ? "1" : "0"); });
        }

        static double ResolvedAspect(string setting)
        {
            if (string.IsNullOrEmpty(setting))
                return 0;
            if (string.Equals(setting, "auto", StringComparison.OrdinalIgnoreCase))
            {
                int w = System.Windows.Forms.Screen.PrimaryScreen.Bounds.Width;
                int h = System.Windows.Forms.Screen.PrimaryScreen.Bounds.Height;
                if (w > 0 && h > 0)
                {
                    double ma = (double)w / h;
                    if (Math.Abs(ma / (16.0 / 9.0) - 1.0) > 0.01)
                        return ma;
                }
                return 0;
            }
            double v;
            return double.TryParse(setting, System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out v) && v >= 1.0 && v <= 6.0 ? v : 0;
        }

        static string[] LocalAdapterPairs(string current)
        {
            var pairs = new List<string> { "", L.T("자동") };
            var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            try
            {
                foreach (var ni in System.Net.NetworkInformation.NetworkInterface.GetAllNetworkInterfaces())
                {
                    if (ni.OperationalStatus != System.Net.NetworkInformation.OperationalStatus.Up)
                        continue;
                    if (ni.NetworkInterfaceType == System.Net.NetworkInformation.NetworkInterfaceType.Loopback)
                        continue;
                    foreach (var ua in ni.GetIPProperties().UnicastAddresses)
                    {
                        if (ua.Address.AddressFamily != System.Net.Sockets.AddressFamily.InterNetwork)
                            continue;
                        string ip = ua.Address.ToString();
                        if (ip.StartsWith("127.") || ip.StartsWith("169.254.") || !seen.Add(ip))
                            continue;
                        pairs.Add(ip);
                        pairs.Add(ip + " — " + ni.Name);
                    }
                }
            }
            catch (Exception)
            {
            }
            if (!string.IsNullOrEmpty(current) && !seen.Contains(current))
            {
                pairs.Add(current);
                pairs.Add(current);
            }
            return pairs.ToArray();
        }

        Border LanCard(GameBuild build, bool running)
        {
            var stack = new StackPanel { IsEnabled = !running, Opacity = running ? 0.5 : 1 };
            if (!build.Lan)
            {
                stack.Children.Add(Ui.Text(L.T("이 빌드에는 LAN 모드 패치가 들어 있지 않습니다."), "Caption"));
                return Ui.Card(L.T("LAN 멀티플레이"), stack);
            }
            stack.Children.Add(Toggle("LanMode", L.T("LAN 모드 (lanmode · PSN 없음)"),
                L.T("GT6의 숨겨진 LAN 모드와 방 동기화·멤버 정보 패치, 로컬 네트워크(TCP 20000, UDP 20123~20125)를 켭니다. 오픈 로비를 바로 열려면 마이 홈에서 R1 + SELECT → Develop Settings → Tutorial → Complete All을 실행하세요.")));
            string curBind = Launcher.Setting("LanBind");
            FrameworkElement bindRow = Ui.Setting(L.T("네트워크 어댑터 (바인드 IP)"),
                L.T("어댑터가 여러 개인 PC(VPN · WSL · Wi-Fi/유선 등)에서 LAN 브로드캐스트를 보낼 주소를 고릅니다."),
                Ui.Choices(curBind, delegate(string v) { Launcher.SetSetting("LanBind", v); }, LocalAdapterPairs(curBind)));
            bindRow.Margin = new Thickness(0, 8, 0, 8);
            stack.Children.Add(bindRow);

            var nameBox = new TextBox
            {
                Text = Launcher.Setting("LanName"),
                MaxLength = 16,
                Width = 260,
                HorizontalAlignment = HorizontalAlignment.Right,
            };
            Action saveName = delegate
            {
                string clean = System.Text.RegularExpressions.Regex.Replace(nameBox.Text ?? "", @"[^A-Za-z0-9_\-]", "");
                if (clean.Length > 16) clean = clean.Substring(0, 16);
                if (clean != nameBox.Text) nameBox.Text = clean;
                Launcher.SetSetting("LanName", clean);
            };
            nameBox.LostFocus += delegate { saveName(); };
            nameBox.KeyDown += delegate(object sender, System.Windows.Input.KeyEventArgs e)
            {
                if (e.Key == System.Windows.Input.Key.Enter) { saveName(); Window.Toast(L.T("LAN 이름을 저장했습니다.")); }
            };
            FrameworkElement nameRow = Ui.Setting(L.T("LAN 플레이어 이름"),
                L.T("로비에 표시되는 이름이며, 이 이름으로 PC마다 고유한 MAC 주소와 콘솔 PSID가 만들어집니다. 비워 두면 PC 이름을 씁니다."),
                nameBox);
            nameRow.Margin = new Thickness(0, 4, 0, 8);
            stack.Children.Add(nameRow);

            stack.Children.Add(Toggle("DevMenu", L.T("개발자 설정 (R1 + SELECT)"),
                L.T("마이 홈에서 R1 + SELECT로 Develop Settings 메뉴를 엽니다. LAN 모드가 켜져 있으면 자동으로 함께 켜집니다.")));
            return Ui.Card(L.T("LAN 멀티플레이"), stack);
        }

        // How the home page's chip names the resolution (realRes as in MenuSettings.RealRes).
        public static string ResolutionName(bool fullHd, int realRes)
        {
            if (!fullHd)
                return "720p";
            switch (realRes)
            {
                case 1: return L.T("1080p 선명");
                case 2: return "1440p";
                case 3: return "4K";
                default: return "Full HD 1080p";
            }
        }

        // The resolution as ONE choice of tiles (2026-11-24, to make it easy): 720p, the game's own Full HD and - an exe that
        // knows "ScreenRealRes" - the picture drawn for real at 1920x1080, 2560x1440 or 3840x2160. The tile that fits the
        // main monitor is marked; a line under the tiles says what the chosen one does. It writes the two keys of the F2
        // menu's settings file (MenuSettings.SetResolution); tile values: 0 = 720p, 1 = Full HD, 2..4 = real 1..3.
        FrameworkElement ResolutionPicker(GameBuild build, bool running)
        {
            bool fullHd = MenuSettings.FullHd(build.SettingsFile);
            int real = build.RealRes && fullHd ? MenuSettings.RealRes(build.SettingsFile) : 0;
            string selected = !fullHd ? "0" : (real == 0 ? 1 : real + 1).ToString();
            var items = new List<string[]>   // value, name, size
            {
                new[] { "0", "720p", "1280×720 · " + L.T("가장 가벼움") },
                new[] { "1", "1080p", L.T("게임 기본") + " · 1440×1080" },
            };
            if (build.RealRes)
            {
                double asp = ResolvedAspect(Launcher.Setting("Aspect"));
                int w1080 = asp > 0.5 ? (((int)Math.Round(1080 * asp) + 1) & ~1) : 1920;
                int w1440 = asp > 0.5 ? (((int)Math.Round(1440 * asp) + 1) & ~1) : 2560;
                int w2160 = asp > 0.5 ? Math.Min(7680, (((int)Math.Round(2160 * asp) + 1) & ~1)) : 3840;
                items.Add(new[] { "2", L.T("1080p 선명"), w1080 + "×1080" });
                items.Add(new[] { "3", "1440p", w1440 + "×1440" });
                items.Add(new[] { "4", "4K", w2160 + "×2160" });
            }
            int height = PcInfo.MonitorHeight();
            string fits = !build.RealRes ? (height >= 1080 ? "1" : "0") : height >= 2160 ? "4" : height >= 1440 ? "3" : height >= 1080 ? "2" : "0";

            var panel = new StackPanel { Margin = new Thickness(0, 0, 0, 8), IsEnabled = !running, Opacity = running ? 0.5 : 1 };
            var head = Ui.Text(L.T("해상도"), "Body");
            head.FontWeight = FontWeights.SemiBold;
            panel.Children.Add(head);
            var hint = Ui.Text(L.T("높을수록 선명하고 그래픽카드 부담이 큽니다. 게임을 시작할 때 정해지고, 게임 안 F2 → 설정 → 화면과 같은 값입니다."), "Caption");
            hint.Margin = new Thickness(0, 2, 0, 10);
            panel.Children.Add(hint);
            var tiles = new System.Windows.Controls.Primitives.UniformGrid { Rows = 1, Columns = items.Count, MaxWidth = 220 * items.Count, HorizontalAlignment = HorizontalAlignment.Left };   // one row, equal tiles
            panel.Children.Add(tiles);
            var note = Ui.Text("", "Caption");
            note.Margin = new Thickness(0, 8, 0, 0);
            panel.Children.Add(note);

            var paints = new List<Action>();
            Action describe = delegate
            {
                string text = selected == "0" ? L.T("PS3와 같은 1280×720입니다. 가장 가볍고, 휴대용 PC나 약한 그래픽카드에 맞습니다.")
                    : selected == "1" ? L.T("게임 자체의 Full HD 모드입니다. 게임이 1440×1080으로 그린 그림을 1920×1080 창에 넓혀 보여 줍니다.")
                    : L.T("게임 그림을 PC에서 이 해상도로 실제로 그립니다 (늘이기가 아님). 창보다 크면 줄여서 보여 주니, 창을 최대화하거나 Alt+Enter 전체 화면에서 가장 선명합니다.");
                if (selected != "0" && selected != "1" && Launcher.Setting("AA") == "spu")
                    text += " " + L.T("안티에일리어싱이 원본 SPU MLAA인 동안에는 게임 기본 1080p로 그립니다.");
                note.Text = text;
            };
            foreach (string[] item in items)
            {
                string value = item[0];
                var tile = new Border { CornerRadius = new CornerRadius(10), BorderThickness = new Thickness(1), Padding = new Thickness(12, 10, 12, 10), Margin = new Thickness(0, 0, item == items[items.Count - 1] ? 0 : 8, 0), Cursor = System.Windows.Input.Cursors.Hand };
                var stack = new StackPanel();
                var name = new TextBlock { Text = item[1], FontSize = 20, FontWeight = FontWeights.Bold };
                stack.Children.Add(name);
                stack.Children.Add(Ui.Text(item[2], "Caption"));
                if (value == fits)
                {
                    var badge = Ui.Text(L.T("이 모니터에 맞음"), "Caption");
                    badge.Foreground = Ui.Brush("Accent2");
                    badge.FontWeight = FontWeights.SemiBold;
                    badge.Margin = new Thickness(0, 4, 0, 0);
                    stack.Children.Add(badge);
                }
                tile.Child = stack;
                paints.Add(delegate
                {
                    bool now = selected == value;
                    tile.Background = now ? Ui.Brush("AccentSoft") : Brushes.Transparent;
                    tile.BorderBrush = now ? Ui.Brush("Accent") : Ui.Brush("Line");
                    name.Foreground = now ? Ui.Brush("Accent") : Ui.Brush("Text");
                });
                tile.MouseLeftButtonUp += delegate
                {
                    if (running || selected == value)
                        return;
                    int pick = int.Parse(value);
                    try
                    {
                        MenuSettings.SetResolution(build.SettingsFile, pick > 0, build.RealRes ? Math.Max(0, pick - 1) : -1);
                    }
                    catch (Exception error)
                    {
                        Window.Error(L.T("설정 파일을 쓰지 못했습니다"), error);
                        Window.Refresh();
                        return;
                    }
                    selected = value;
                    foreach (Action paint in paints)
                        paint();
                    describe();
                };
                tiles.Children.Add(tile);
            }
            foreach (Action paint in paints)
                paint();
            describe();
            return panel;
        }

        protected override FrameworkElement Build()
        {
            GameBuild build = Launcher.Build;
            bool running = GameRunning;

            var screen = new StackPanel();
            if (build.FullHd)
            {
                screen.Children.Add(ResolutionPicker(build, running));
                if (!build.RealRes && build.Available)
                    screen.Children.Add(Ui.Text(build.ExeName + L.T("는 1080p 선명 · 1440p · 4K가 생기기 전의 빌드입니다."), "Caption"));
                screen.Children.Add(Ui.Text(running ? L.T("게임이 켜져 있는 동안에는 게임 안 메뉴에서 바꿔 주세요.") : L.T("설정 파일: ") + System.IO.Path.GetFileName(build.SettingsFile) + L.T(" (이 빌드의 F2 메뉴가 쓰는 파일)"), "Caption"));
            }
            else
                screen.Children.Add(Ui.Text(build.Available ? build.ExeName + L.T("는 Full HD 스위치가 생기기 전의 빌드입니다 (720p). 시험판에서 쓸 수 있습니다.") : L.T("빌드를 찾지 못했습니다."), "Caption"));

            // Anti-aliasing and full screen are the launcher's own switches for a run (Settings.Overrides: GT6_AA, GT6_FULLSCREEN).
            if (build.Ultrawide)
            {
                FrameworkElement aspect = Ui.Setting(L.T("화면 비율 (울트라와이드)"),
                    L.T("3D 시야를 선택한 비율로 넓히고 실제 해상도의 가로 픽셀도 맞춥니다 (예: 1440p에서 3440×1440). 2D 메뉴와 HUD는 가로로 늘어납니다. 게임을 시작할 때 적용됩니다."),
                    Ui.Choices(Launcher.Setting("Aspect"), delegate(string v) { Launcher.SetSetting("Aspect", v); Window.Refresh(); },
                        "", L.T("16:9 (기본)"),
                        "auto", L.T("자동 (모니터 비율)"),
                        "2.3888889", "21:9 (3440×1440)",
                        "2.3703704", "21:9 (2560×1080)",
                        "2.4", "21:9 (3840×1600)",
                        "3.5555556", "32:9",
                        "1.6", "16:10"));
                aspect.Margin = new Thickness(0, 14, 0, 0);
                aspect.IsEnabled = !running;
                screen.Children.Add(aspect);
            }
            if (build.Aa)
            {
                FrameworkElement aa = Ui.Setting(L.T("안티에일리어싱"),
                    L.T("계단 현상을 부드럽게 하는 방식입니다. GPU 필터(기본)는 완성된 화면을 GPU에서 처리하고, 끄면 가장 가볍고, 원본 SPU MLAA는 콘솔의 방식입니다 (CPU 부담이 큽니다). 게임을 시작할 때 정해집니다."),
                    Ui.Choices(Launcher.Setting("AA"), delegate(string v) { Launcher.SetSetting("AA", v); },
                        "", L.T("GPU 필터 (기본)"), "off", L.T("끄기 (가장 가벼움)"), "spu", L.T("원본 SPU MLAA (무거움)")));
                aa.Margin = new Thickness(0, 14, 0, 14);
                screen.Children.Add(aa);
            }
            if (build.FullScreen)
                screen.Children.Add(Toggle("Fullscreen", L.T("전체 화면으로 시작"),
                    L.T("게임 창을 테두리 없는 전체 화면으로 엽니다. 게임 중에는 Alt+Enter로 바꿉니다. 화면이 16:9가 아니면 비율을 지키고 남는 곳은 검게 둡니다.")));
            else if (build.Available)
                screen.Children.Add(Ui.Text(L.T("이 빌드에는 전체 화면 스위치가 없습니다. Alt+Enter와 함께 다음 빌드부터 들어갑니다."), "Caption"));
            if (build.Aa || build.FullScreen)
            {
                var preset = Ui.Button(L.T("휴대용(UMPC) 권장 설정 적용"), delegate
                {
                    try
                    {
                        if (build.FullHd)
                            MenuSettings.SetResolution(build.SettingsFile, false, build.RealRes ? 0 : -1);
                    }
                    catch (Exception error)
                    {
                        Window.Error(L.T("설정 파일을 쓰지 못했습니다"), error);
                        return;
                    }
                    Launcher.SetSetting("AA", "off");
                    Launcher.SetSetting("Fullscreen", "1");
                    Window.Refresh();
                    Window.Toast(L.T("휴대용 권장 설정을 적용했습니다: 720p · 안티에일리어싱 끄기 · 전체 화면으로 시작"));
                });
                preset.HorizontalAlignment = HorizontalAlignment.Left;
                preset.IsEnabled = !running;
                screen.Children.Add(Ui.Text(running ? L.T("게임이 꺼진 뒤에 눌러 주세요.") : L.T("720p, 안티에일리어싱 끄기, 전체 화면으로 시작을 한 번에 켭니다. 게임을 시작할 때 적용됩니다."), "Caption"));
                screen.Children.Add(preset);
            }

            var env = new TextBox
            {
                Text = build.Problem ?? string.Join("\r\n", build.Env.OrderBy(p => p.Key, StringComparer.Ordinal).Select(p => p.Key + "=" + p.Value)),
                IsReadOnly = true, FontFamily = Ui.Res<FontFamily>("MonoFont"), FontSize = 12, Height = 170, TextWrapping = TextWrapping.NoWrap, AcceptsReturn = true,
                VerticalScrollBarVisibility = ScrollBarVisibility.Auto, HorizontalScrollBarVisibility = ScrollBarVisibility.Auto, VerticalContentAlignment = VerticalAlignment.Top, Margin = new Thickness(0, 8, 0, 8),
            };
            var openBat = Ui.Button(build.BatName + L.T(" 열기"), delegate
            {
                if (File.Exists(build.Bat))
                    Process.Start("notepad.exe", "\"" + build.Bat + "\"");
            }, null, "");
            openBat.HorizontalAlignment = HorizontalAlignment.Left;

            UIElement language = Ui.Choices(Launcher.Setting("Language"), delegate(string key)
            {
                Launcher.SetSetting("Language", key);
                try
                {
                    if (!GameRunning)
                        L.SetMenuLanguage(build.SettingsFile, key);   // the game's own menu (F2) follows
                }
                catch (Exception error)
                {
                    Window.Error(L.T("설정 파일을 쓰지 못했습니다"), error);
                }
                Window.Toast(L.T("언어는 런처를 다시 열면 바뀝니다."));
            }, "", L.T("자동 (Windows)"), "ko", "한국어", "en", "English");

            return Ui.Page(L.T("설정"), L.T("런처의 설정은 launcher.ini에, 게임 안 메뉴의 설정은 메뉴의 설정 파일에 그대로 남습니다."),
                Ui.Card("언어 · Language",
                    Ui.Setting("언어 · Language", L.T("런처와 게임 안 메뉴(F2)의 언어입니다. 자동은 Windows의 표시 언어를 따릅니다."), language)),
                Ui.Card(L.T("게임 빌드"),
                    Ui.Setting(L.T("플레이 버튼이 시작하는 빌드"), L.T("패키지에 든 두 실행 파일 중 하나입니다. 시험판은 자기 설정 파일을 따로 씁니다."), BuildPicker())),
                Ui.Card(L.T("화면"), screen),
                LanCard(build, running),
                Ui.Card(L.T("소리와 입력"),
                    Toggle("Mute", L.T("소리 끄기"), L.T("게임의 소리를 내지 않습니다.")),
                    Toggle("Keyboard", L.T("키보드를 패드로 쓰기"), L.T("끄면 키보드가 게임 패드 노릇을 하지 않습니다 (패드나 핸들만 쓸 때)."))),
                Ui.Card(L.T("세이브 백업"),
                    Toggle("AutoBackup", L.T("시작 전 자동 백업"), L.T("게임을 시작할 때마다 세이브 전체를 zip 하나로 save_backups 폴더에 남깁니다.")),
                    Ui.Setting(L.T("자동 백업 보관 개수"), L.T("넘치면 가장 오래된 자동 백업부터 지웁니다. 직접 만든 백업은 지우지 않습니다."),
                               Ui.Choices(Launcher.Setting("BackupsToKeep"), delegate(string v) { Launcher.SetSetting("BackupsToKeep", v); }, "5", L.T("5개"), "10", L.T("10개"), "20", L.T("20개"), "50", L.T("50개")))),
                Ui.Card(L.T("실행 환경"),
                    Ui.Text(L.T("런처는 ") + build.BatName + L.T("을 읽어 그 파일과 똑같은 환경으로 ") + build.ExeName + L.T("를 시작하고, 로그도 같은 파일에 남깁니다. 아래는 지금 읽은 값입니다 (")
                            + build.Env.Count + L.T("개 설정, ") + build.Unset.Count + L.T("개 제거). 런처가 더하는 것은 모드 폴더, 소리 끄기, 키보드 끄기, 안티에일리어싱, 전체 화면 시작뿐입니다."), "Caption"),
                    env, openBat));
        }
    }

    public class ModsPage : PageBase
    {
        string busy;

        public ModsPage(MainWindow window) : base(window)
        {
        }

        void AddFrom(string source)
        {
            if (source == null)
                return;
            busy = L.T("추가하는 중: ") + System.IO.Path.GetFileName(source.TrimEnd('\\'));
            Window.Refresh();
            Launcher launcher = Launcher;
            ThreadPool.QueueUserWorkItem(delegate
            {
                ModInfo added = null;
                Exception failed = null;
                try
                {
                    added = ModStore.Add(launcher, source);
                }
                catch (Exception error)
                {
                    failed = error;
                }
                Window.Dispatcher.BeginInvoke((Action)delegate
                {
                    busy = null;
                    Window.Refresh();
                    if (failed != null)
                        Window.Error(L.T("모드를 추가하지 못했습니다"), failed);
                    else
                        Window.Toast(L.T("모드를 추가하고 켰습니다: ") + added.Name + L.T(" (파일 ") + added.Files + L.T("개)"));
                });
            });
        }

        // Compares a mod's own files with the game data and, when they are all in it, records the mod as one that is
        // inside the game data (GameMods.Check). source: the mod's folder, or the folder of the RPCS3 GT6-Launcher.
        void CheckGameData(string source)
        {
            if (source == null)
                return;
            busy = L.T("게임 데이터와 비교하는 중…");
            Window.Refresh();
            Launcher launcher = Launcher;
            ThreadPool.QueueUserWorkItem(delegate
            {
                GameModCheck check = null;
                Exception failed = null;
                try
                {
                    check = GameMods.Check(launcher, source, delegate(string step)
                    {
                        Window.Dispatcher.BeginInvoke((Action)delegate
                        {
                            if (busy == null)
                                return;
                            busy = L.T("게임 데이터와 비교하는 중: ") + step;
                            Window.Refresh();
                        });
                    });
                }
                catch (Exception error)
                {
                    failed = error;
                }
                Window.Dispatcher.BeginInvoke((Action)delegate
                {
                    busy = null;
                    Window.Refresh();
                    if (failed != null)
                        Window.Error(L.T("게임 데이터의 모드를 확인하지 못했습니다"), failed);
                    else if (check.Recorded.Count == 0)
                        Window.Error(L.T("게임 데이터에 들어 있지 않습니다"), new InvalidOperationException(string.Join("\n", check.Skipped)));
                    else
                        Window.Toast(L.T("게임 데이터에 들어 있는 모드로 표시합니다: ") + string.Join(", ", check.Recorded) + (check.Skipped.Count > 0 ? " · " + string.Join(" · ", check.Skipped) : ""));
                });
            });
        }

        // One mod that is inside the game data: always applied, so no switch; what the launcher shows is its record and
        // whether the recorded files are still in the game data.
        FrameworkElement GameTile(GameMod mod, bool locked)
        {
            bool ok = mod.Complete;
            var tile = new Border { CornerRadius = new CornerRadius(10), BorderThickness = new Thickness(1), Padding = new Thickness(12, 8, 12, 8), Margin = new Thickness(0, 0, 0, 6) };
            tile.BorderBrush = ok ? Ui.Brush("Good") : Ui.Brush("Warn");
            var row = new Grid();
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            row.ColumnDefinitions.Add(new ColumnDefinition());
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            Border chip = Ui.Chip(ok ? "" : "", ok ? L.T("적용됨") : L.T("확인 필요"), ok ? Ui.Brush("Good") : Ui.Brush("Warn"));
            chip.VerticalAlignment = VerticalAlignment.Center;
            row.Children.Add(chip);
            var text = new StackPanel { VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(12, 0, 12, 0) };
            var name = Ui.Text(mod.Name, "Body");
            name.FontWeight = FontWeights.SemiBold;
            text.Children.Add(name);
            string detail = L.T("게임 데이터에 설치됨 · 파일 ") + mod.Files.ToString("N0") + L.T("개 · ") + PcInfo.Size(mod.Size);
            if (mod.Added >= 0)
                detail += L.T(" · 새 파일 ") + mod.Added.ToString("N0") + L.T("개");
            if (mod.Replaced >= 0)
                detail += L.T(" · 바꾼 원본 ") + mod.Replaced.ToString("N0") + L.T("개");
            if (mod.Superseded > 0)
                detail += L.T(" · 뒤의 모드가 다시 바꾼 파일 ") + mod.Superseded.ToString("N0") + L.T("개");
            text.Children.Add(Ui.Text(detail, "Caption"));
            var origin = new List<string>();
            if (!string.IsNullOrEmpty(mod.Source))
                origin.Add(L.T("출처 ") + mod.Source);
            if (!string.IsNullOrEmpty(mod.From))
                origin.Add(mod.From + (string.IsNullOrEmpty(mod.Installed) ? "" : " " + mod.Installed));
            if (origin.Count > 0)
                text.Children.Add(Ui.Text(string.Join(" · ", origin), "Caption"));
            var check = Ui.Text(ok ? L.T("확인: 기록된 파일 ") + mod.Files.ToString("N0") + L.T("개가 모두 게임 데이터에 있습니다")
                                   : L.T("확인: 기록된 파일 ") + mod.Files.ToString("N0") + L.T("개 중 ") + (mod.Files - Math.Max(0, mod.Present)).ToString("N0") + L.T("개가 게임 데이터에 없거나 크기가 다릅니다"), "Caption");
            check.Foreground = ok ? Ui.Brush("Good") : Ui.Brush("Warn");
            text.Children.Add(check);
            Grid.SetColumn(text, 1);
            row.Children.Add(text);
            if (!ok)
            {
                Button forget = Ui.Button(L.T("기록 지우기"), delegate
                {
                    Window.Confirm(L.T("모드 기록 지우기"), mod.Name + L.T("의 기록만 지웁니다 (이 목록에서 사라집니다). 게임 데이터는 바뀌지 않습니다."), L.T("지우기"), delegate
                    {
                        try
                        {
                            GameMods.Forget(Launcher, mod);
                        }
                        catch (Exception error)
                        {
                            Window.Error(L.T("기록을 지우지 못했습니다"), error);
                        }
                        Window.Refresh();
                    });
                }, null, "");
                forget.IsEnabled = !locked;
                forget.VerticalAlignment = VerticalAlignment.Center;
                Grid.SetColumn(forget, 2);
                row.Children.Add(forget);
            }
            tile.Child = row;
            return tile;
        }

        protected override FrameworkElement Build()
        {
            List<ModInfo> mods = ModStore.List(Launcher);
            List<GameMod> inGame = GameMods.List(Launcher, true);
            GameBuild build = Launcher.Build;
            bool locked = busy != null || GameRunning;

            var top = new StackPanel();
            top.Children.Add(Ui.Text(L.T("모드는 게임 파일을 대신하는 파일 묶음입니다 (예: PDIPFS 폴더). 런처는 게임 데이터를 바꾸지 않고, 켜 둔 모드의 파일을 게임이 원본보다 먼저 읽게 합니다. ")
                                     + L.T("그래서 끄거나 제거하면 곧바로 원래대로입니다. 목록에서 아래에 있는 모드가 위의 모드보다 우선합니다. 바뀐 내용은 다음에 게임을 시작할 때 적용됩니다. ")
                                     + L.T("게임 데이터 자체에 이미 들어 있는 모드는 아래 목록에 따로 표시되고 항상 적용됩니다."), "Body"));
            int on = mods.Count(m => m.Enabled && m.Root != null);
            string summary = mods.Count == 0 ? L.T("아직 모드가 없습니다.") : L.T("켜진 모드 ") + on + L.T("개") + (on > 0 ? L.T(" · 대신 읽히는 파일 ") + ModStore.ActiveFiles(mods) + L.T("개") : "");
            if (inGame.Count > 0)
                summary = L.T("게임 데이터에 들어 있는 모드 ") + inGame.Count + L.T("개 (항상 적용) · ") + (mods.Count == 0 ? L.T("런처로 추가한 모드 없음") : summary);
            var status = Ui.Text(busy ?? summary, "Caption");
            status.Margin = new Thickness(0, 10, 0, 0);
            if (busy != null)
                status.Foreground = Ui.Brush("Accent");
            top.Children.Add(status);
            if (on > 0 && build.Available && !build.Overlay)
            {
                var warn = Ui.Text(build.ExeName + L.T("는 모드 폴더를 읽지 못하는 빌드입니다. 이 빌드로는 시작하지 않습니다."), "Caption");
                warn.Foreground = Ui.Brush("Warn");
                top.Children.Add(warn);
            }
            var buttons = new WrapPanel { Margin = new Thickness(0, 12, 0, 0) };
            Button fromFolder = Ui.Button(L.T("폴더에서 추가"), delegate { AddFrom(Ui.FolderDialog(L.T("모드 폴더 (PDIPFS 폴더가 들어 있는 폴더)를 고르세요. mods 폴더로 복사됩니다."), null)); }, "AccentButton", "", new Thickness(0, 0, 8, 8));
            Button fromZip = Ui.Button(L.T("zip에서 추가"), delegate
            {
                var dialog = new Microsoft.Win32.OpenFileDialog { Title = L.T("모드 zip"), Filter = "zip (*.zip)|*.zip" };
                if (dialog.ShowDialog(Window) == true)
                    AddFrom(dialog.FileName);
            }, null, "", new Thickness(0, 0, 8, 8));
            Button inData = Ui.Button(L.T("게임 데이터에 넣은 모드 확인"), delegate
            {
                CheckGameData(Ui.FolderDialog(L.T("게임 데이터에 직접 넣은 모드의 폴더(PDIPFS가 든 폴더)나, 그 모드를 설치했던 RPCS3용 GT6-Launcher 폴더를 고르세요. ")
                                              + L.T("파일을 게임 데이터와 비교만 하고 아무것도 바꾸지 않습니다."), null));
            }, null, "", new Thickness(0, 0, 8, 8));
            fromFolder.IsEnabled = fromZip.IsEnabled = inData.IsEnabled = !locked;
            buttons.Children.Add(fromFolder);
            buttons.Children.Add(fromZip);
            buttons.Children.Add(inData);
            buttons.Children.Add(Ui.Button(L.T("mods 폴더 열기"), delegate { Ui.OpenFolder(Launcher.ModsDir); }, null, "", new Thickness(0, 0, 8, 8)));
            top.Children.Add(buttons);

            var list = new StackPanel();
            if (inGame.Count > 0)
            {
                var head = Ui.Text(L.T("게임 데이터에 들어 있는 모드 — 항상 적용됩니다 (런처가 끄거나 지우지 않습니다)"), "Caption");
                head.Margin = new Thickness(0, 0, 0, 6);
                list.Children.Add(head);
                foreach (GameMod mod in inGame)
                    list.Children.Add(GameTile(mod, locked));
                var added = Ui.Text(L.T("런처로 추가한 모드 (mods 폴더) — 게임 데이터의 모드보다 먼저 읽힙니다"), "Caption");
                added.Margin = new Thickness(0, 10, 0, 6);
                list.Children.Add(added);
            }
            if (mods.Count == 0)
                list.Children.Add(Ui.Text((inGame.Count > 0 ? L.T("아직 없습니다. ") : "") + L.T("mods 폴더에 모드 폴더를 직접 넣어도 여기에 보입니다 (mods\\<이름>\\PDIPFS\\...)."), "Caption"));
            for (int i = 0; i < mods.Count; i++)
            {
                ModInfo mod = mods[i];
                var tile = new Border { CornerRadius = new CornerRadius(10), BorderThickness = new Thickness(1), Padding = new Thickness(12, 8, 12, 8), Margin = new Thickness(0, 0, 0, 6) };
                tile.BorderBrush = mod.Enabled && mod.Root != null ? Ui.Brush("Accent") : Ui.Brush("Line");
                tile.Background = mod.Enabled && mod.Root != null ? Ui.Brush("AccentSoft") : Brushes.Transparent;
                var row = new Grid();
                row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
                row.ColumnDefinitions.Add(new ColumnDefinition());
                row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
                CheckBox toggle = Ui.Switch(mod.Enabled, delegate(bool value)
                {
                    ModStore.SetEnabled(Launcher, mod.Name, value);
                    Window.Refresh();
                });
                toggle.VerticalAlignment = VerticalAlignment.Center;
                toggle.IsEnabled = mod.Root != null && !locked;
                row.Children.Add(toggle);
                var text = new StackPanel { VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(12, 0, 12, 0) };
                var name = Ui.Text((i + 1) + ".  " + mod.Name, "Body");
                name.FontWeight = FontWeights.SemiBold;
                text.Children.Add(name);
                var detail = Ui.Text(mod.Root == null ? L.T("PDIPFS(또는 USRDIR) 폴더가 없어 쓸 수 없습니다")
                    : L.T("파일 ") + mod.Files + L.T("개 · ") + PcInfo.Size(mod.Size) + " · " + mod.Changed.ToLocalTime().ToString("yyyy-MM-dd HH:mm") + (mod.Enabled ? L.T(" · 켜짐") : L.T(" · 꺼짐")), "Caption");
                if (mod.Root == null)
                    detail.Foreground = Ui.Brush("Warn");
                text.Children.Add(detail);
                Grid.SetColumn(text, 1);
                row.Children.Add(text);
                Button up = Ui.Button(L.T("위로"), delegate { ModStore.Move(Launcher, mod.Name, -1); Window.Refresh(); }, null, "", new Thickness(0, 0, 6, 0));
                Button down = Ui.Button(L.T("아래로"), delegate { ModStore.Move(Launcher, mod.Name, 1); Window.Refresh(); }, null, "", new Thickness(0, 0, 6, 0));
                Button remove = Ui.Button(L.T("제거"), delegate
                {
                    Window.Confirm(L.T("모드 제거"), "mods\\" + mod.Name + L.T(" 폴더를 지웁니다 (") + PcInfo.Size(mod.Size) + L.T("). 게임 데이터는 처음부터 바뀌지 않았으므로 그대로입니다."), L.T("제거"), delegate
                    {
                        try
                        {
                            ModStore.Remove(Launcher, mod.Name);
                            Window.Toast(L.T("제거했습니다: ") + mod.Name);
                        }
                        catch (Exception error)
                        {
                            Window.Error(L.T("모드를 제거하지 못했습니다"), error);
                        }
                        Window.Refresh();
                    });
                }, null, "");
                up.IsEnabled = i > 0 && !locked;
                down.IsEnabled = i < mods.Count - 1 && !locked;
                remove.IsEnabled = !locked;
                StackPanel actions = Ui.Row(up, down, remove);
                actions.VerticalAlignment = VerticalAlignment.Center;
                Grid.SetColumn(actions, 2);
                row.Children.Add(actions);
                tile.Child = row;
                list.Children.Add(tile);
            }
            return Ui.Page(L.T("모드"), L.T("게임 데이터를 건드리지 않는 모드 관리 (mods 폴더)."), CareerCard(locked), Ui.Card(L.T("모드 추가"), top), Ui.Card(L.T("설치된 모드"), list));
        }

        // GT6 Career Mod (+ Spec II): the archive from GTPlanet in one step (CareerMod in Mods.cs). [GT6 0089]
        Border CareerCard(bool locked)
        {
            var stack = new StackPanel();
            stack.Children.Add(Ui.Text(L.T("만든 사람: Lunard. Spec II Beta 7(Nenkai, TheAdmiester)을 바탕으로 Strato mod(Emivebe)와 Performance mod(Plunke)의 기능을 더했습니다. 게임 버전 1.22가 필요합니다."), "Caption"));
            string version = CareerMod.Version(Launcher);
            bool installed = CareerMod.Installed(Launcher), on = installed && CareerMod.On(Launcher), ready = CareerMod.FirstRunDone(Launcher);
            var facts = new StackPanel { Margin = new Thickness(0, 12, 0, 0) };
            // Put into the game data itself by another launcher (recorded in mods\.game): always in force already.
            GameMod inData = GameMods.List(Launcher, false).FirstOrDefault(m => m.Id.Equals("career", StringComparison.OrdinalIgnoreCase) || m.Name.IndexOf("Career", StringComparison.OrdinalIgnoreCase) >= 0);
            if (inData != null)
                facts.Children.Add(Line(L.T("게임 데이터"), L.T("이미 들어 있습니다 (항상 적용): ") + inData.Name + (string.IsNullOrEmpty(inData.Source) ? "" : " · " + inData.Source), Ui.Brush("Good")));
            facts.Children.Add(Line(L.T("가져옴"), installed ? (version.Length > 0 ? "ver. " + version : L.T("있음")) : L.T("아직 없음")));
            facts.Children.Add(Line(L.T("상태"), !installed ? L.T("없음") : on ? L.T("켜짐 (Spec II + 커리어 모드)") : L.T("꺼짐 (원래 게임)"), on ? Ui.Brush("Accent") : null));
            stack.Children.Add(facts);
            if (!ready)
            {
                var wait = Ui.Text(L.T("먼저 게임을 한 번 시작해 메인 화면(My Home)까지 가서 세이브가 만들어진 뒤, 게임을 끄고 나서 넣으세요. 1.22 설치와 첫 실행이 끝나기 전에 넣으면 게임에 오류가 납니다."), "Caption");
                wait.Foreground = Ui.Brush("Warn");
                wait.Margin = new Thickness(0, 0, 0, 4);
                stack.Children.Add(wait);
            }
            var buttons = new WrapPanel { Margin = new Thickness(0, 10, 0, 0) };
            var margin = new Thickness(0, 0, 8, 8);
            buttons.Children.Add(Ui.Button(L.T("GTPlanet에서 받기"), delegate
            {
                try
                {
                    Process.Start(CareerMod.PageUrl);
                }
                catch (Exception error)
                {
                    Window.Error(L.T("브라우저를 열지 못했습니다"), error);
                }
            }, null, "", margin));
            Button import = Ui.Button(installed ? L.T("새 버전 가져오기…") : L.T("압축 파일 가져오기…"), delegate
            {
                var dialog = new Microsoft.Win32.OpenFileDialog { Title = L.T("GT6 Career mod 압축 파일"), Filter = "zip (*.zip)|*.zip" };
                if (dialog.ShowDialog(Window) == true)
                    ImportCareer(dialog.FileName);
            }, "AccentButton", "", margin);
            import.IsEnabled = ready && !locked;
            buttons.Children.Add(import);
            if (installed)
            {
                Button toggle = Ui.Button(on ? L.T("모두 끄기 (원래 게임으로)") : L.T("다시 켜기"), delegate
                {
                    CareerMod.Switch(Launcher, !on);
                    Window.Refresh();
                }, null, "", margin);
                toggle.IsEnabled = !locked && (on || ready);
                buttons.Children.Add(toggle);
            }
            stack.Children.Add(buttons);
            stack.Children.Add(Ui.Text(L.T("모드는 게임 데이터를 바꾸지 않고 게임이 먼저 읽게만 하므로, 끄면 바로 원래 게임입니다. 커리어 모드를 처음 쓸 때는 새 세이브로 시작하세요."), "Caption"));
            return Ui.Card("GT6 Career Mod (+ Spec II mod)", stack);
        }

        void ImportCareer(string archive)
        {
            busy = L.T("커리어 모드를 가져오는 중: ") + System.IO.Path.GetFileName(archive);
            Window.Refresh();
            Launcher launcher = Launcher;
            ThreadPool.QueueUserWorkItem(delegate
            {
                string version = null;
                Exception failed = null;
                try
                {
                    version = CareerMod.Import(launcher, archive);
                }
                catch (Exception error)
                {
                    failed = error;
                }
                Window.Dispatcher.BeginInvoke((Action)delegate
                {
                    busy = null;
                    Window.Refresh();
                    if (failed != null)
                        Window.Error(L.T("커리어 모드를 가져오지 못했습니다"), failed);
                    else
                        Window.Toast(L.T("Spec II + 커리어 모드를 켰습니다") + (version.Length > 0 ? " (ver. " + version + ")" : "") + L.T(". 다음에 게임을 시작할 때 적용됩니다."));
                });
            });
        }
    }

    public class SavePage : PageBase
    {
        public SavePage(MainWindow window) : base(window)
        {
        }

        protected override FrameworkElement Build()
        {
            List<SaveInfo> saves = SaveStore.List(Launcher);
            var list = new StackPanel();
            if (saves.Count == 0)
                list.Children.Add(Ui.Text(L.T("아직 세이브가 없습니다. 게임에서 저장하면 여기에 보입니다."), "Caption"));
            foreach (SaveInfo save in saves)
            {
                var row = new Grid { Margin = new Thickness(0, 0, 0, 12) };
                row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
                row.ColumnDefinitions.Add(new ColumnDefinition());
                ImageSource icon = Image(save.Icon);
                row.Children.Add(new Border
                {
                    Width = 160, Height = 88, CornerRadius = new CornerRadius(10), Margin = new Thickness(0, 0, 16, 0), BorderBrush = Ui.Brush("Line"), BorderThickness = new Thickness(1),
                    Background = icon != null ? (Brush)new ImageBrush(icon) { Stretch = Stretch.UniformToFill } : Ui.Brush("Line"),
                });
                var text = new StackPanel { VerticalAlignment = VerticalAlignment.Center };
                text.Children.Add(new TextBlock { Text = save.Title.Length > 0 ? save.Title : save.Name, FontFamily = Ui.Res<FontFamily>("TitleFont"), FontSize = 15, FontWeight = FontWeights.SemiBold, Foreground = Ui.Brush("Text") });
                if (save.SubTitle.Length > 0 || save.Detail.Length > 0)
                    text.Children.Add(new TextBlock { Text = (save.SubTitle + " " + save.Detail).Trim(), FontFamily = Ui.Res<FontFamily>("TitleFont"), FontSize = 13, Foreground = Ui.Brush("SubText"), TextWrapping = TextWrapping.Wrap });
                text.Children.Add(Ui.Text(save.Name + " · " + save.Changed.ToString("yyyy-MM-dd HH:mm") + " · " + PcInfo.Size(save.Size), "Caption"));
                Grid.SetColumn(text, 1);
                row.Children.Add(text);
                list.Children.Add(row);
            }
            list.Children.Add(Ui.Row(
                Ui.Button(L.T("지금 백업"), delegate
                {
                    try
                    {
                        string zip = SaveStore.Backup(Launcher, "manual");
                        Window.Toast(zip != null ? L.T("백업했습니다: ") + System.IO.Path.GetFileName(zip) : L.T("백업할 세이브가 없습니다"));
                        Window.Refresh();
                    }
                    catch (Exception error)
                    {
                        Window.Error(L.T("백업하지 못했습니다"), error);
                    }
                }, "AccentButton", "", new Thickness(0, 6, 8, 0)),
                Ui.Button(L.T("세이브 폴더 열기"), delegate { Ui.OpenFolder(Launcher.SavesDir); }, null, "", new Thickness(0, 6, 0, 0))));

            List<string> backups = SaveStore.Backups(Launcher);
            var backupList = new StackPanel();
            if (backups.Count == 0)
                backupList.Children.Add(Ui.Text(L.T("아직 백업이 없습니다. 런처로 게임을 시작하면 그 전에 자동으로 만들어집니다 (설정 → 세이브 백업)."), "Caption"));
            bool running = GameRunning;
            foreach (string backup in backups)
            {
                string file = backup;
                var row = new Grid { Margin = new Thickness(0, 0, 0, 8) };
                row.ColumnDefinitions.Add(new ColumnDefinition());
                row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
                var label = new StackPanel { VerticalAlignment = VerticalAlignment.Center };
                label.Children.Add(Ui.Text(SaveStore.Describe(file), "Body"));
                long size = 0;
                try
                {
                    size = new FileInfo(file).Length;
                }
                catch (Exception)
                {
                }
                label.Children.Add(Ui.Text(System.IO.Path.GetFileName(file) + " · " + PcInfo.Size(size), "Caption"));
                row.Children.Add(label);
                Button restore = Ui.Button(L.T("복원"), delegate
                {
                    Window.Confirm(L.T("세이브 복원"), SaveStore.Describe(file) + L.T(" 백업으로 세이브를 되돌립니다. 지금 세이브는 먼저 \"복원 전\" 백업으로 남깁니다."), L.T("복원"), delegate
                    {
                        try
                        {
                            SaveStore.Restore(Launcher, file);
                            Window.Toast(L.T("복원했습니다"));
                            Window.Refresh();
                        }
                        catch (Exception error)
                        {
                            Window.Error(L.T("복원하지 못했습니다"), error);
                        }
                    });
                }, null, "");
                restore.IsEnabled = !running;
                if (running)
                    restore.ToolTip = L.T("게임을 끈 다음 복원할 수 있습니다.");
                Grid.SetColumn(restore, 1);
                row.Children.Add(restore);
                backupList.Children.Add(row);
            }
            Button openBackups = Ui.Button(L.T("백업 폴더 열기"), delegate { Ui.OpenFolder(Launcher.BackupsDir); }, null, "", new Thickness(0, 6, 0, 0));
            openBackups.HorizontalAlignment = HorizontalAlignment.Left;
            backupList.Children.Add(openBackups);

            return Ui.Page(L.T("세이브"), L.T("게임의 세이브(game\\dev_hdd0\\home\\00000001\\savedata)와 백업. 백업은 세이브 전체를 zip 하나로 save_backups 폴더에 남깁니다."),
                Ui.Card(L.T("세이브 데이터"), list),
                Ui.Card(L.T("백업"), backupList));
        }
    }

    public class LogPage : PageBase
    {
        string selected;

        public LogPage(MainWindow window) : base(window)
        {
        }

        protected override FrameworkElement Build()
        {
            List<LogInfo> logs = Logs.List(Launcher);
            if (selected == null || !logs.Any(l => l.Path == selected))
                selected = logs.Count > 0 ? logs[0].Path : null;
            var list = new ListBox();
            if (logs.Count == 0)
                list.Items.Add(new ListBoxItem { Content = Ui.Text(L.T("아직 로그가 없습니다."), "Caption"), IsEnabled = false });
            foreach (LogInfo log in logs)
            {
                var item = new StackPanel();
                var label = Ui.Text(log.Label, "Body");
                label.FontWeight = FontWeights.SemiBold;
                item.Children.Add(label);
                item.Children.Add(Ui.Text(log.Changed.ToString("yyyy-MM-dd HH:mm:ss") + " · " + System.IO.Path.GetFileName(log.Path) + " · " + PcInfo.Size(log.Size), "Caption"));
                var listItem = new ListBoxItem { Content = item, Tag = log.Path };
                list.Items.Add(listItem);
                if (log.Path == selected)
                    list.SelectedItem = listItem;
            }
            list.SelectionChanged += delegate
            {
                var item = list.SelectedItem as ListBoxItem;
                if (item != null && (string)item.Tag != selected)
                {
                    selected = (string)item.Tag;
                    Window.Refresh();
                }
            };

            var right = new DockPanel();
            string text = selected != null ? Logs.Tail(selected, 4000) : L.T("아직 로그가 없습니다. 게임을 실행하면 여기에 기록됩니다.");
            // The last run through the launcher ended with an error and this is its log: the reason in plain words.
            int code;
            GameBuild last = GameBuild.Read(Launcher, Launcher.State("LastBuild", "") == "release" ? "release" : "test");
            if (selected != null && int.TryParse(Launcher.State("LastExitCode", "0"), out code) && code != 0 && selected.Equals(last.LogErr, StringComparison.OrdinalIgnoreCase))
            {
                Diagnosis d = Diagnosis.Of(code, Logs.Tail(selected, 300));
                if (d != null)
                {
                    Border card = Ui.Card(null, Ui.Row(Ui.Icon("", 20, Ui.Brush("Bad")), new TextBlock { Text = "  " + d.Title, FontSize = 15, FontWeight = FontWeights.SemiBold, Foreground = Ui.Brush("Text") }),
                                          Ui.Text(d.Detail, "Caption"), Ui.Text(d.Advice, "Body"));
                    card.BorderBrush = Ui.Brush("Bad");
                    card.Margin = new Thickness(0, 0, 0, 12);
                    DockPanel.SetDock(card, Dock.Top);
                    right.Children.Add(card);
                }
            }
            var viewer = new TextBox
            {
                Text = text, IsReadOnly = true, FontFamily = Ui.Res<FontFamily>("MonoFont"), FontSize = 12, TextWrapping = TextWrapping.NoWrap,
                AcceptsReturn = true, VerticalScrollBarVisibility = ScrollBarVisibility.Auto, HorizontalScrollBarVisibility = ScrollBarVisibility.Auto,
                VerticalContentAlignment = VerticalAlignment.Top,
            };
            viewer.Loaded += delegate { viewer.ScrollToEnd(); };
            StackPanel buttons = Ui.Row(
                Ui.Button(L.T("복사"), delegate
                {
                    Clipboard.SetText(viewer.Text);
                    Window.Toast(L.T("로그를 복사했습니다"));
                }, null, "", new Thickness(0, 0, 8, 0)),
                Ui.Button(L.T("메모장으로 열기"), delegate
                {
                    if (selected != null)
                        Process.Start("notepad.exe", "\"" + selected + "\"");
                }, null, "", new Thickness(0, 0, 8, 0)),
                Ui.Button(L.T("폴더 열기"), delegate { Ui.OpenFolder(Launcher.LogsDir); }, null, ""));
            buttons.Margin = new Thickness(0, 12, 0, 0);
            DockPanel.SetDock(buttons, Dock.Bottom);
            right.Children.Add(buttons);
            right.Children.Add(viewer);

            var body = new Grid { Margin = new Thickness(16, 8, 16, 24) };
            body.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            body.RowDefinitions.Add(new RowDefinition());
            body.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(320) });
            body.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(16) });
            body.ColumnDefinitions.Add(new ColumnDefinition());
            var head = new StackPanel { Margin = new Thickness(0, 0, 0, 20) };
            head.Children.Add(Ui.Text(L.T("로그"), "H1"));
            var sub = Ui.Text(L.T("게임이 남긴 기록 (logs 폴더: bat으로 실행해도 같은 파일). 파일의 끝부분 4000줄을 보여 줍니다."), "Caption");
            sub.Margin = new Thickness(0, 4, 0, 0);
            head.Children.Add(sub);
            Grid.SetColumnSpan(head, 3);
            body.Children.Add(head);
            Grid.SetRow(list, 1);
            body.Children.Add(list);
            Grid.SetRow(right, 1);
            Grid.SetColumn(right, 2);
            body.Children.Add(right);
            return body;
        }
    }

    public class ToolsPage : PageBase
    {
        public ToolsPage(MainWindow window) : base(window)
        {
        }

        protected override FrameworkElement Build()
        {
            return Ui.Page(L.T("업데이트 · 도구"), L.T("새 버전 받기, 바로가기, 폴더."),
                UpdateCard(),
                Ui.Card(L.T("바로가기"),
                    Ui.Setting(L.T("바탕화면 바로가기 (바로 플레이)"), L.T("더블클릭하면 런처 창 없이 바로 게임이 시작됩니다 (--play). 오류로 끝날 때만 런처 창이 뜹니다."), Ui.Button(L.T("만들기"), delegate { MakeShortcut(true); }, null, "")),
                    Ui.Setting(L.T("바탕화면 바로가기 (런처)"), L.T("런처 창을 엽니다."), Ui.Button(L.T("만들기"), delegate { MakeShortcut(false); }, null, ""))),
                Ui.Card(L.T("폴더"),
                    Ui.Row(Ui.Button(L.T("패키지 폴더"), delegate { Ui.OpenFolder(Launcher.Dir); }, null, "", new Thickness(0, 0, 8, 0)),
                           Ui.Button("game", delegate { Ui.OpenFolder(Launcher.GameDir); }, null, "", new Thickness(0, 0, 8, 0)),
                           Ui.Button("mods", delegate { Ui.OpenFolder(Launcher.ModsDir); }, null, "", new Thickness(0, 0, 8, 0)),
                           Ui.Button(L.T("세이브 백업"), delegate { Ui.OpenFolder(Launcher.BackupsDir); }, null, "", new Thickness(0, 0, 8, 0)),
                           Ui.Button(L.T("로그"), delegate { Ui.OpenFolder(Launcher.LogsDir); }, null, "", new Thickness(0, 0, 8, 0)),
                           Ui.Button(L.T("launcher.ini 열기"), delegate
                           {
                               if (!File.Exists(Launcher.StatePath))
                                   Launcher.SetSetting("AutoBackup", Launcher.Setting("AutoBackup"));
                               Process.Start("notepad.exe", "\"" + Launcher.StatePath + "\"");
                           }, null, ""))),
                Ui.Card(L.T("정보"),
                    Ui.Text("Gran Turismo 6 (" + GameFiles.TitleId + L.T(", 1.22) 네이티브 PC 포트의 런처입니다. 비영리 팬 프로젝트이며 Sony Interactive Entertainment, Polyphony Digital과 관계없습니다."), "Body"),
                    Ui.Text(L.T("게임과 그 안의 모든 데이터·코드의 권리는 권리자에게 있습니다. 런처에는 게임 데이터가 들어 있지 않고, 본인이 가진 게임 파일로만 동작합니다."), "Caption"),
                    Ui.Text(L.T("런처의 틀은 사카츠쿠 PC 런처에서 가져왔습니다."), "Caption")));
        }

        void MakeShortcut(bool play)
        {
            try
            {
                string path = Shortcuts.Desktop(Launcher, play);
                Window.Toast(L.T("바탕화면에 만들었습니다: ") + System.IO.Path.GetFileName(path));
            }
            catch (Exception error)
            {
                Window.Error(L.T("바로가기를 만들지 못했습니다"), error);
            }
        }

        bool checking, working;
        string updateLine;

        void CheckNow()
        {
            checking = true;
            Window.Refresh();
            Launcher launcher = Launcher;
            ThreadPool.QueueUserWorkItem(delegate
            {
                ReleaseInfo found = PortUpdate.Check(launcher);
                Window.Dispatcher.BeginInvoke((Action)delegate
                {
                    checking = false;
                    Window.LatestRelease = found;
                    Window.UpdateBadge.Visibility = found.Newer ? Visibility.Visible : Visibility.Collapsed;
                    if (found.Newer)
                        Window.UpdateBadge.Text = L.T("새 버전 ") + found.Tag + L.T(" 있음");
                    Window.Refresh();
                });
            });
        }

        // Updates: the newest release of the repository against this folder's version, installing it, and going back.
        Border UpdateCard()
        {
            ReleaseInfo info = Window.LatestRelease;
            bool configured = PortUpdate.Configured(Launcher), git = PortUpdate.IsGitCheckout(Launcher);
            var stack = new StackPanel();
            stack.Children.Add(Line(L.T("이 폴더 버전"), PortUpdate.LocalStamp(Launcher) + (git ? L.T(" (개발 폴더: git)") : "")));

            var address = new TextBox { Text = Launcher.State("Repository", ""), Width = 330, VerticalContentAlignment = VerticalAlignment.Center };
            Button saveAddress = Ui.Button(L.T("저장"), delegate
            {
                string typed = address.Text.Trim(), repository = PortUpdate.Normalize(typed);
                if (typed.Length > 0 && repository.Length == 0)
                {
                    Window.ShowProblem(L.T("저장소 주소를 알아보지 못했습니다. owner/name 또는 https://github.com/owner/name 으로 적어 주세요."));
                    return;
                }
                Launcher.SetState("Repository", repository);
                Launcher.SetState("LastUpdateCheck", "");
                Window.LatestRelease = null;
                Window.Toast(repository.Length > 0 ? L.T("저장소 주소를 저장했습니다: ") + repository : L.T("저장소 주소를 지웠습니다"));
                Window.Refresh();
            }, null, null, new Thickness(8, 0, 0, 0));
            stack.Children.Add(Ui.Setting(L.T("GitHub 저장소 주소"), L.T("이 포트의 릴리스가 올라가는 저장소 (owner/name 또는 https://github.com/owner/name). 비어 있으면 이 포트의 공개 저장소를 봅니다."), Ui.Row(address, saveAddress)));
            string feed = Launcher.State("UpdateFeed", "");
            if (feed.Length > 0)
                stack.Children.Add(Line(L.T("확인 주소"), feed + L.T(" (launcher.ini의 UpdateFeed: 저장소 주소 대신 씁니다)"), Ui.Brush("Warn")));

            // A private repository: a token the user pastes here (kept encrypted for this Windows account), or the
            // login of the GitHub CLI on this PC. The launcher never shows a token again once it is saved.
            var tokenBox = new PasswordBox
            {
                Width = 250, Padding = new Thickness(10, 7, 10, 7), VerticalContentAlignment = VerticalAlignment.Center,
                Background = Ui.Brush("Field"), Foreground = Ui.Brush("Text"), BorderBrush = Ui.Brush("Line"), CaretBrush = Ui.Brush("Text"),
            };
            Button saveToken = Ui.Button(L.T("저장"), delegate
            {
                string typed = tokenBox.Password.Trim();
                if (typed.Length < 20 || typed.Contains(" "))
                {
                    Window.ShowProblem(L.T("토큰으로 보이지 않습니다. GitHub에서 만든 토큰 전체를 붙여 넣어 주세요."));
                    return;
                }
                try
                {
                    Auth.Save(Launcher, typed);
                    Launcher.SetState("LastUpdateCheck", "");
                    Window.LatestRelease = null;
                    Window.Toast(L.T("토큰을 저장했습니다 (이 윈도우 계정으로 암호화)"));
                }
                catch (Exception error)
                {
                    Window.Error(L.T("토큰을 저장하지 못했습니다"), error);
                }
                Window.Refresh();
            }, null, null, new Thickness(8, 0, 0, 0));
            Button clearToken = Ui.Button(L.T("지우기"), delegate
            {
                Auth.Clear(Launcher);
                Window.LatestRelease = null;
                Window.Toast(L.T("저장된 토큰을 지웠습니다"));
                Window.Refresh();
            }, null, null, new Thickness(8, 0, 0, 0));
            clearToken.IsEnabled = Auth.HasSaved(Launcher);
            stack.Children.Add(Ui.Setting(L.T("비공개 저장소용 토큰"),
                L.T("저장소가 비공개일 때만 필요합니다. GitHub의 fine-grained 토큰(이 저장소 하나, Contents 읽기 권한)이면 됩니다. 이 PC의 윈도우 계정으로 암호화해서 ")
                + L.T("launcher.ini에 저장하고 api.github.com에만 보냅니다. 지금: ") + (Auth.HasSaved(Launcher) ? L.T("저장된 토큰 있음") : L.T("저장된 토큰 없음")) + ".",
                Ui.Row(tokenBox, saveToken, clearToken)));
            string gh = Auth.GhPath();
            CheckBox useGh = Ui.Switch(Launcher.Setting("UseGhCli") == "1", delegate(bool on)
            {
                Launcher.SetSetting("UseGhCli", on ? "1" : "0");
                Launcher.SetState("LastUpdateCheck", "");
                Window.LatestRelease = null;
                Window.Refresh();
            }, gh != null ? L.T("저장된 토큰이 없으면 이 PC의 GitHub CLI(gh) 로그인을 사용") : L.T("GitHub CLI(gh)가 이 PC에 없습니다 (있으면 그 로그인을 쓸 수 있습니다)"));
            useGh.IsEnabled = gh != null;
            useGh.Margin = new Thickness(0, 0, 0, 12);
            stack.Children.Add(useGh);

            if (!configured)
                stack.Children.Add(Ui.Text(L.T("저장소 주소가 아직 없습니다. GitHub에 올린 뒤 주소를 넣으면, 런처와 게임 실행 파일을 함께 새 버전으로 받아 설치합니다."), "Caption"));
            else if (info != null && info.Error != null)
                stack.Children.Add(Line(L.T("최신 릴리스"), L.T("확인하지 못함: ") + info.Error, Ui.Brush("Warn")));
            else if (info != null && info.NoRelease)
                stack.Children.Add(Line(L.T("최신 릴리스"), L.T("저장소는 확인했습니다. 아직 올라온 릴리스가 없습니다 (") + Auth.Describe(Launcher) + ")."));
            else if (info != null)
            {
                stack.Children.Add(Line(L.T("최신 릴리스"), (info.Tag ?? "?") + (info.Stamp != null ? " (" + info.Stamp + ")" : L.T(" (GT6_PC_<날짜>.zip 없음)")), info.Newer ? Ui.Brush("Accent") : null));
                stack.Children.Add(Ui.Text(info.Newer ? L.T("새 버전이 있습니다. 설치하면 런처, 게임 실행 파일, bat, 트레이너 파일만 바뀌고 game(게임 데이터·세이브), mods, 설정, 로그는 그대로입니다. 바뀌는 파일은 먼저 update\\backup에 남겨, 아래 버튼으로 되돌릴 수 있습니다.")
                                                      : L.T("최신 버전입니다."), "Caption"));
            }
            string[] ready = PortUpdate.Ready(Launcher);
            if (ready != null)
                stack.Children.Add(Line(L.T("받아 둔 버전"), ready[1] + " (" + ready[0] + "): " + (PortUpdate.AutoEnabled(Launcher) ? L.T("다음에 런처를 열 때 적용됩니다") : L.T("아래 버튼으로 적용합니다")), Ui.Brush("Accent")));
            string previous = PortUpdate.Previous(Launcher);
            if (previous != null)
                stack.Children.Add(Line(L.T("이전 버전"), previous + L.T(" (update\\backup에 보관 중)")));
            if (updateLine != null)
                stack.Children.Add(Ui.Text(updateLine, "Body"));

            var buttons = new WrapPanel { Margin = new Thickness(0, 8, 0, 0) };
            Button check = Ui.Button(checking ? L.T("확인 중...") : L.T("업데이트 확인"), CheckNow, null, "", new Thickness(0, 0, 8, 8));
            check.IsEnabled = configured && !checking && !working;
            buttons.Children.Add(check);
            if (info != null && info.Newer && !git)
            {
                Button install = Ui.Button(working ? L.T("설치 중...") : ready != null ? L.T("지금 적용하고 런처 다시 열기") : L.T("설치하고 런처 다시 열기"), delegate { Install(info); }, "AccentButton", "", new Thickness(0, 0, 8, 8));
                install.IsEnabled = !working;
                buttons.Children.Add(install);
            }
            if (previous != null)
            {
                Button back = Ui.Button(L.T("이전 버전으로 되돌리기"), delegate { Rollback(previous); }, null, "", new Thickness(0, 0, 8, 8));
                back.IsEnabled = !working;
                buttons.Children.Add(back);
            }
            string page = info != null && info.PageUrl != null ? info.PageUrl : PortUpdate.ReleasesPage(Launcher);
            if (page != null)
                buttons.Children.Add(Ui.Button(L.T("릴리스 페이지"), delegate { Process.Start(page); }, null, "", new Thickness(0, 0, 8, 8)));
            stack.Children.Add(buttons);
            stack.Children.Add(Ui.Switch(Launcher.Setting("UpdateCheck") != "0", delegate(bool on) { Launcher.SetSetting("UpdateCheck", on ? "1" : "0"); }, L.T("런처를 열 때 하루 한 번 확인")));
            CheckBox auto = Ui.Switch(Launcher.Setting("AutoUpdate") != "0", delegate(bool on) { Launcher.SetSetting("AutoUpdate", on ? "1" : "0"); }, L.T("새 버전을 묻지 않고 받아 두고, 다음에 런처를 열 때 적용"));
            auto.Margin = new Thickness(0, 6, 0, 0);
            stack.Children.Add(auto);
            if (git)
                stack.Children.Add(Ui.Text(L.T("이 폴더는 개발(git) 폴더라 확인만 하고 받거나 바꾸지 않습니다."), "Caption"));
            return Ui.Card(L.T("업데이트 (런처와 게임 실행 파일)"), stack);
        }

        void Run(string title, Func<Action<string>, string> work)
        {
            working = true;
            updateLine = L.T("준비 중...");
            Window.Refresh();
            ThreadPool.QueueUserWorkItem(delegate
            {
                string next = null;
                Exception failed = null;
                try
                {
                    next = work(delegate(string line)
                    {
                        Window.Dispatcher.BeginInvoke((Action)delegate
                        {
                            updateLine = line;
                            Window.Refresh();
                        });
                    });
                }
                catch (Exception error)
                {
                    failed = error;
                }
                Window.Dispatcher.BeginInvoke((Action)delegate
                {
                    working = false;
                    if (failed != null)
                    {
                        updateLine = L.T("실패: ") + failed.Message;
                        Window.Error(title, failed);
                        Window.Refresh();
                        return;
                    }
                    Process.Start(new ProcessStartInfo(next) { WorkingDirectory = Launcher.Dir });
                    Application.Current.Shutdown();
                });
            });
        }

        void Install(ReleaseInfo info)
        {
            Launcher launcher = Launcher;
            Window.Confirm(L.T("업데이트 설치"), info.Tag + " (" + PcInfo.Size(info.ZipSize) + L.T(")를 받아 설치하고 런처를 다시 엽니다. 게임 데이터와 세이브, 모드, 설정은 바뀌지 않고, 바뀌는 파일은 먼저 백업합니다."), L.T("설치"), delegate
            {
                Run(L.T("업데이트를 설치하지 못했습니다"), progress => PortUpdate.Install(launcher, info, progress));
            });
        }

        void Rollback(string previous)
        {
            Launcher launcher = Launcher;
            Window.Confirm(L.T("이전 버전으로 되돌리기"), L.T("지난 업데이트가 바꾼 파일을 ") + previous + L.T(" 버전의 것으로 되돌리고 런처를 다시 엽니다. 같은 새 버전이 다시 설치되지 않도록 자동 업데이트는 꺼집니다."), L.T("되돌리기"), delegate
            {
                Run(L.T("되돌리지 못했습니다"), progress => PortUpdate.Rollback(launcher, progress));
            });
        }
    }

    public static class Shortcuts
    {
        // A .lnk on the desktop: the launcher, with --play for the one that starts the game at once.
        public static string Desktop(Launcher launcher, bool play)
        {
            string path = System.IO.Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory), play ? "GT6 PC.lnk" : L.T("GT6 PC 런처.lnk"));
            string target = System.IO.Path.Combine(launcher.Dir, Launcher.ExeName);
            Type shellType = Type.GetTypeFromProgID("WScript.Shell");
            object shell = Activator.CreateInstance(shellType);
            object link = shellType.InvokeMember("CreateShortcut", BindingFlags.InvokeMethod, null, shell, new object[] { path });
            Type linkType = link.GetType();
            linkType.InvokeMember("TargetPath", BindingFlags.SetProperty, null, link, new object[] { target });
            linkType.InvokeMember("Arguments", BindingFlags.SetProperty, null, link, new object[] { play ? "--play" : "" });
            linkType.InvokeMember("WorkingDirectory", BindingFlags.SetProperty, null, link, new object[] { launcher.Dir });
            linkType.InvokeMember("Description", BindingFlags.SetProperty, null, link, new object[] { L.T("Gran Turismo 6 네이티브 PC 포트") });
            linkType.InvokeMember("IconLocation", BindingFlags.SetProperty, null, link, new object[] { target + ",0" });
            linkType.InvokeMember("Save", BindingFlags.InvokeMethod, null, link, null);
            return path;
        }
    }
}
