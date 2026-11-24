// The setup page: the user's game disc is found (or picked), one button does everything, and from then on a progress
// bar is all there is to see. The work (Setup.cs, SetupRun.cs) runs on a worker thread; RPCS3 runs on a desktop nobody
// sees and the game's own installation in a hidden window. [GT6 0089]
using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;

namespace GT6Launcher
{
    public class SetupPage : PageBase
    {
        // One setup at a time; kept while the page is made again.
        static SetupRunner runner;
        static Thread worker;
        static SetupProgress progress;
        static string failure;
        static bool closing, searching, searched;
        static GameSource source;

        // The progress card of the page now shown.
        Grid bar;
        TextBlock percentText, stepText, detailText;

        public SetupPage(MainWindow window) : base(window)
        {
        }

        public static bool Running { get { return worker != null && worker.IsAlive; } }

        public static bool Closing { get { return closing; } }

        // The window is closing: the setup stops (what is done stays done) and then() runs once it has stopped.
        public static void Stop(MainWindow window, Action then)
        {
            closing = true;
            if (!Running)
            {
                then();
                return;
            }
            runner.Cancel();
            Thread running = worker;
            new Thread(delegate()
            {
                running.Join();
                window.Dispatcher.BeginInvoke(then);
            }) { IsBackground = true }.Start();
        }

        public override void Shown()
        {
            // The first time: the disc is looked for by itself (names and sizes only, 25 s at most).
            if (!searched && !Running && source == null && !new SetupRunner(Launcher).DiscReady)
                Search(false);
            base.Shown();
        }

        static string Gb(long bytes)
        {
            return (bytes / 1073741824.0).ToString("0.0") + " GB";
        }

        protected override FrameworkElement Build()
        {
            bar = null;
            if (Running)
                return Ui.Page(L.T("설치"), L.T("설치하는 동안 다른 창은 뜨지 않습니다. 다른 일을 하셔도 됩니다."), ProgressCard());
            var r = new SetupRunner(Launcher);
            if (r.Complete)
                return Ui.Page(L.T("설치"), L.T("게임 파일이 모두 준비되었습니다."), DoneCard(), FirstStartCard());
            return Ui.Page(L.T("설치"), L.T("가진 게임 디스크에서 이 PC용 게임 파일을 만듭니다. 버튼 하나로 끝까지 진행됩니다."), DiscCard(r), InstallCard(r), FirstStartCard());
        }

        Border DiscCard(SetupRunner r)
        {
            var stack = new StackPanel();
            stack.Children.Add(Ui.Text(L.T("유럽판 Gran Turismo 6 (BCES01893) 디스크가 필요합니다: 복호화된 ISO 파일, 또는 디스크를 복사한 폴더(PS3_GAME 폴더가 들어 있는 폴더). 다른 지역판은 지원하지 않습니다."), "Body"));
            string state;
            Brush brush;
            if (r.DiscReady)
            {
                state = "✓ " + L.T("디스크의 파일을 이미 꺼내 두었습니다 (game\\dev_bdvd).");
                brush = Ui.Brush("Good");
            }
            else if (searching)
            {
                state = L.T("찾는 중…");
                brush = Ui.Brush("Accent");
            }
            else if (source != null && source.Usable)
            {
                state = "✓ " + source.Path;
                brush = Ui.Brush("Good");
            }
            else if (source != null)
            {
                state = "✕ " + source.Path + "\n" + source.Problem;
                brush = Ui.Brush("Warn");
            }
            else
            {
                state = searched ? L.T("자동으로 찾지 못했습니다. 아래 버튼으로 직접 골라 주세요.") : L.T("아직 고르지 않았습니다.");
                brush = Ui.Brush("SubText");
            }
            var line = Ui.Text(state, "Body");
            line.Foreground = brush;
            line.Margin = new Thickness(0, 12, 0, 12);
            stack.Children.Add(line);
            if (!r.DiscReady)
            {
                var buttons = new WrapPanel();
                var margin = new Thickness(0, 0, 8, 8);
                Button find = Ui.Button(L.T("자동으로 찾기"), delegate { Search(true); }, null, "", margin);
                Button iso = Ui.Button(L.T("ISO 파일 고르기…"), delegate
                {
                    var dialog = new Microsoft.Win32.OpenFileDialog { Title = L.T("게임 디스크 이미지 (ISO)"), Filter = "PS3 ISO (*.iso)|*.iso|" + L.T("모든 파일") + " (*.*)|*.*" };
                    if (dialog.ShowDialog(Window) == true)
                        Use(dialog.FileName);
                }, "AccentButton", "", margin);
                Button folder = Ui.Button(L.T("디스크 폴더 고르기…"), delegate
                {
                    Use(Ui.FolderDialog(L.T("디스크를 복사한 폴더(PS3_GAME 폴더가 들어 있는 폴더)를 고르세요."), null));
                }, null, "", margin);
                find.IsEnabled = iso.IsEnabled = folder.IsEnabled = !searching;
                buttons.Children.Add(iso);
                buttons.Children.Add(folder);
                buttons.Children.Add(find);
                stack.Children.Add(buttons);
            }
            return Ui.Card(L.T("1. 게임 디스크"), stack);
        }

        Border InstallCard(SetupRunner r)
        {
            var stack = new StackPanel();
            stack.Children.Add(Ui.Text(L.T("게임의 공식 업데이트 1.01~1.22(약 12.5 GB)와 PS3 시스템 소프트웨어를 소니 서버에서 받아 설치하고, 게임이 처음 켜질 때 하는 업데이트 합치기까지 끝냅니다. ")
                                       + L.T("모든 과정은 화면에 보이지 않는 곳에서 진행되고, 여기에는 진행 바만 보입니다. 중간에 멈춰도 다시 시작하면 멈춘 곳부터 이어집니다."), "Body"));
            var facts = new StackPanel { Margin = new Thickness(0, 14, 0, 0) };
            long need = r.SpaceNeeded, free = r.SpaceFree;
            bool spaceOk = free >= need;
            facts.Children.Add(Line(L.T("필요한 공간"), L.T("약 ") + Gb(need) + L.T(" (설치하는 동안)") + "  ·  " + L.T("남은 공간 ") + (free == long.MaxValue ? "?" : Gb(free)), spaceOk ? null : Ui.Brush("Warn")));
            facts.Children.Add(Line(L.T("걸리는 시간"), L.T("받기는 인터넷 속도에 따라 다릅니다 (100 Mbps에서 약 20분). 설치는 약 15분입니다.")));
            facts.Children.Add(Line(L.T("지원"), L.T("Windows 10 / 11 PC (64비트). Steam Deck, 맥, 리눅스, 안드로이드, iOS는 지원하지 않습니다.")));
            stack.Children.Add(facts);
            if (!r.UpdatesReady || !r.ModuleReady)
                stack.Children.Add(CacheSection());
            if (failure != null)
            {
                var failed = Ui.Text(failure + "\n" + L.T("다시 시작하면 멈춘 곳부터 이어서 진행합니다."), "Body");
                failed.Foreground = Ui.Brush("Bad");
                failed.Margin = new Thickness(0, 6, 0, 6);
                stack.Children.Add(failed);
            }
            bool haveDisc = r.DiscReady || (source != null && source.Usable);
            var start = new Button { Style = Ui.Style("PlayButton"), Content = Ui.Row(Ui.Icon("", 20, Brushes.White), new TextBlock { Text = "  " + L.T("설치 시작"), VerticalAlignment = VerticalAlignment.Center }) };
            start.IsEnabled = haveDisc && spaceOk && !searching && !GameRunning;
            start.Click += delegate { Start(); };
            start.HorizontalAlignment = HorizontalAlignment.Left;
            start.Margin = new Thickness(0, 12, 0, 0);
            stack.Children.Add(start);
            string why = !haveDisc ? L.T("먼저 게임 디스크를 골라 주세요.") : !spaceOk ? L.T("디스크 공간이 부족합니다.") : GameRunning ? L.T("게임이 실행 중입니다. 게임을 끈 뒤에 설치할 수 있습니다.") : null;
            if (why != null)
            {
                var hint = Ui.Text(why, "Caption");
                hint.Margin = new Thickness(4, 8, 0, 0);
                stack.Children.Add(hint);
            }
            return Ui.Card(L.T("2. 설치"), stack);
        }

        UIElement CacheSection()
        {
            var box = new StackPanel { Margin = new Thickness(0, 14, 0, 4) };
            var title = Ui.Text(L.T("이미 받아 둔 업데이트 폴더 (선택)"), "H2");
            title.Margin = new Thickness(0, 0, 0, 4);
            box.Children.Add(title);
            string cachePath = Launcher.Value("Setup", "Cache", "");
            if (string.IsNullOrEmpty(cachePath))
            {
                var desc = Ui.Text(L.T("BCES01893 업데이트 .pkg 파일이나 PS3UPDAT.PUP이 이미 있는 폴더를 고르면 다시 받지 않고 바로 씁니다. 원본 파일은 지워지지 않습니다."), "Caption");
                desc.Margin = new Thickness(0, 0, 0, 8);
                box.Children.Add(desc);
            }
            else
            {
                SetupRunner.CacheInfo info = SetupRunner.InspectCache(cachePath);
                string status;
                Brush brush;
                if (!info.Exists)
                {
                    status = "✕ " + cachePath + "\n" + L.T("폴더를 찾지 못했습니다.");
                    brush = Ui.Brush("Warn");
                }
                else if (info.FoundUpdates == 0 && !info.HasFirmware && !info.HasTool)
                {
                    status = "! " + cachePath + "\n" + L.T("이 폴더(하위 2단계 포함)에서 일치하는 BCES01893 업데이트 .pkg나 PS3UPDAT.PUP을 찾지 못했습니다.");
                    brush = Ui.Brush("Warn");
                }
                else
                {
                    var parts = new List<string>();
                    if (info.FoundUpdates > 0)
                        parts.Add(info.FoundUpdates + " / " + info.TotalUpdates + L.T("개 업데이트 (") + Gb(info.FoundBytes) + ")");
                    if (info.HasFirmware)
                        parts.Add(L.T("PS3 시스템 소프트웨어"));
                    if (info.HasTool)
                        parts.Add("RPCS3");
                    string summary = string.Join(", ", parts.ToArray());
                    if (info.MissingBytes > 0)
                        summary += "  ·  " + L.T("추가로 받을 용량 ") + Gb(info.MissingBytes);
                    else if (info.FoundUpdates == info.TotalUpdates)
                        summary += "  ·  " + L.T("모든 업데이트 준비됨");
                    status = "✓ " + cachePath + "\n" + summary;
                    brush = Ui.Brush("Good");
                }
                var line = Ui.Text(status, "Body");
                line.Foreground = brush;
                line.Margin = new Thickness(0, 2, 0, 8);
                box.Children.Add(line);
            }
            var buttons = new WrapPanel();
            var margin = new Thickness(0, 0, 8, 4);
            Button pick = Ui.Button(L.T("업데이트 폴더 고르기…"), delegate
            {
                string picked = Ui.FolderDialog(L.T("이미 받아 둔 업데이트 .pkg 파일이나 PS3UPDAT.PUP이 들어 있는 폴더를 고르세요."), string.IsNullOrEmpty(cachePath) ? null : cachePath);
                if (!string.IsNullOrEmpty(picked))
                {
                    Launcher.Set("Setup", "Cache", picked);
                    Window.Refresh();
                }
            }, null, "", margin);
            pick.IsEnabled = !searching;
            buttons.Children.Add(pick);
            if (!string.IsNullOrEmpty(cachePath))
            {
                Button clear = Ui.Button(L.T("비우기"), delegate
                {
                    Launcher.Set("Setup", "Cache", "");
                    Window.Refresh();
                }, null, "", margin);
                clear.IsEnabled = !searching;
                buttons.Children.Add(clear);
            }
            box.Children.Add(buttons);
            return box;
        }

        Border ProgressCard()
        {
            var stack = new StackPanel();
            stepText = Ui.Text("", "H2");
            stack.Children.Add(stepText);
            bar = new Grid { Height = 14, Margin = new Thickness(0, 10, 0, 10) };
            bar.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(0, GridUnitType.Star) });
            bar.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            var track = new Border { Background = Ui.Brush("Field"), CornerRadius = new CornerRadius(7) };
            Grid.SetColumnSpan(track, 2);
            bar.Children.Add(track);
            bar.Children.Add(new Border { Background = Ui.Brush("PlayGradient"), CornerRadius = new CornerRadius(7) });
            stack.Children.Add(bar);
            var row = new DockPanel();
            percentText = Ui.Text("", "Body");
            percentText.FontWeight = FontWeights.Bold;
            percentText.Margin = new Thickness(0, 0, 14, 0);
            DockPanel.SetDock(percentText, Dock.Left);
            row.Children.Add(percentText);
            detailText = Ui.Text("", "Caption");
            row.Children.Add(detailText);
            stack.Children.Add(row);
            Button stop = Ui.Button(L.T("멈추기"), delegate
            {
                Window.Confirm(L.T("설치를 멈출까요?"), L.T("지금까지 한 것은 그대로 남고, 다시 시작하면 멈춘 곳부터 이어집니다."), L.T("멈추기"), delegate
                {
                    if (runner != null)
                        runner.Cancel();
                });
            }, null, "", new Thickness(0, 18, 0, 0));
            stop.HorizontalAlignment = HorizontalAlignment.Left;
            stack.Children.Add(stop);
            Paint();
            return Ui.Card(null, stack);
        }

        void Paint()
        {
            if (bar == null || progress == null)
                return;
            double f = Math.Max(0, Math.Min(1, progress.Fraction));
            bar.ColumnDefinitions[0].Width = new GridLength(f, GridUnitType.Star);
            bar.ColumnDefinitions[1].Width = new GridLength(1 - f, GridUnitType.Star);
            percentText.Text = (int)(f * 100) + "%";
            stepText.Text = progress.Title;
            detailText.Text = progress.Detail;
        }

        Border DoneCard()
        {
            var stack = new StackPanel();
            var ok = Ui.Text("✓ " + L.T("설치가 끝났습니다. 게임을 시작할 수 있습니다."), "Body");
            ok.Foreground = Ui.Brush("Good");
            stack.Children.Add(ok);
            var play = new Button { Style = Ui.Style("PlayButton"), Content = Ui.Row(Ui.Icon("", 20, Brushes.White), new TextBlock { Text = L.T("  플레이"), VerticalAlignment = VerticalAlignment.Center }) };
            play.Click += delegate { Window.Play(); };
            play.HorizontalAlignment = HorizontalAlignment.Left;
            play.Margin = new Thickness(0, 14, 0, 0);
            stack.Children.Add(play);
            return Ui.Card(L.T("설치 완료"), stack);
        }

        static Border FirstStartCard()
        {
            var stack = new StackPanel();
            foreach (string text in new[]
            {
                L.T("화면 조정(Screen Adjust)은 90%에서 시작합니다. 위쪽 방향을 길게 눌러 맞춘 뒤 결정하세요."),
                L.T("이름을 정하는 단계에서 Windows 입력 상자가 열립니다. 이름을 쓰고 Enter를 누르세요."),
                L.T("첫 주행은 브랜즈 해치의 트랙 데이입니다. 끝까지 달리면 메인 화면(My Home)이 나오고 세이브가 만들어집니다."),
                L.T("모드(커리어 모드 등)는 이 첫 실행을 마치고 메인 화면까지 본 뒤, 게임을 끄고 나서 켜세요."),
            })
            {
                var line = Ui.Text("•  " + text, "Body");
                line.Margin = new Thickness(0, 0, 0, 6);
                stack.Children.Add(line);
            }
            return Ui.Card(L.T("처음 시작할 때"), stack);
        }

        void Search(bool refresh)
        {
            if (searching)
                return;
            searching = searched = true;
            if (refresh)
                Window.Refresh();
            Launcher launcher = Launcher;
            MainWindow window = Window;
            ThreadPool.QueueUserWorkItem(delegate
            {
                List<GameSource> found = null;
                try
                {
                    found = GameFinder.Find(launcher, null);
                }
                catch (Exception)
                {
                }
                window.Dispatcher.BeginInvoke((Action)delegate
                {
                    searching = false;
                    GameSource best = found == null ? null : found.FirstOrDefault(s => s.Usable) ?? found.FirstOrDefault();
                    if (best != null)
                        source = best;
                    window.Refresh();
                });
            });
        }

        void Use(string path)
        {
            if (string.IsNullOrEmpty(path))
                return;
            searching = true;
            Window.Refresh();
            MainWindow window = Window;
            ThreadPool.QueueUserWorkItem(delegate
            {
                GameSource inspected = GameFinder.Inspect(path);
                window.Dispatcher.BeginInvoke((Action)delegate
                {
                    searching = false;
                    source = inspected;
                    window.Refresh();
                });
            });
        }

        void Start()
        {
            if (Running)
                return;
            failure = null;
            progress = new SetupProgress { Title = L.T("준비하는 중"), Fraction = 0 };
            runner = new SetupRunner(Launcher);
            GameSource chosen = source;
            MainWindow window = Window;
            runner.Progress += delegate(SetupProgress p)
            {
                window.Dispatcher.BeginInvoke((Action)delegate
                {
                    progress = p;
                    Paint();
                });
            };
            SetupRunner work = runner;
            worker = new Thread(delegate()
            {
                string error = null;
                bool cancelled = false;
                try
                {
                    work.Run(chosen);
                }
                catch (SetupFailed e)
                {
                    error = e.Message;
                }
                catch (OperationCanceledException)
                {
                    cancelled = true;
                }
                catch (Exception e)
                {
                    error = e.Message;
                }
                window.Dispatcher.BeginInvoke((Action)delegate
                {
                    failure = error;
                    if (closing)
                        return;
                    window.Refresh();
                    if (error == null && !cancelled)
                        window.Toast(L.T("설치가 끝났습니다. 이제 게임을 시작할 수 있습니다."));
                    else if (cancelled)
                        window.Toast(L.T("설치를 멈췄습니다. 다시 시작하면 이어서 진행합니다."));
                });
            }) { IsBackground = true, Name = "setup" };
            worker.Start();
            Window.Refresh();
        }
    }
}
