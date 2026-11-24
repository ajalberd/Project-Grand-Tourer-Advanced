// Taken from the Sakatsuku PC launcher by tools\take_frame.py; change it there (or here, then keep the two alike).
using System;
using System.Collections.Generic;
using System.Linq;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Media;
using System.Windows.Threading;

namespace GT6Launcher
{
    // Page building blocks with the theme's styles.
    public static class Ui
    {
        public static T Res<T>(string key)
        {
            return (T)Application.Current.Resources[key];
        }

        public static Brush Brush(string key)
        {
            return Res<Brush>(key);
        }

        public static Style Style(string key)
        {
            return Res<Style>(key);
        }

        public static TextBlock Text(string text, string style = "Body")
        {
            return new TextBlock { Text = text, Style = Style(style) };
        }

        public static TextBlock Icon(string glyph, double size = 16, Brush brush = null)
        {
            var icon = new TextBlock { Text = glyph, Style = Style("Icon"), FontSize = size };
            if (brush != null)
                icon.Foreground = brush;
            return icon;
        }

        // A scrolling page: title, a line under it, then the content.
        public static FrameworkElement Page(string title, string subtitle, params UIElement[] content)
        {
            var stack = new StackPanel { Margin = new Thickness(16, 8, 16, 24) };
            var head = Text(title, "H1");
            stack.Children.Add(head);
            if (subtitle != null)
            {
                var sub = Text(subtitle, "Caption");
                sub.Margin = new Thickness(0, 4, 0, 20);
                stack.Children.Add(sub);
            }
            foreach (UIElement element in content)
                if (element != null)
                    stack.Children.Add(element);
            return new ScrollViewer { Content = stack, VerticalScrollBarVisibility = ScrollBarVisibility.Auto };
        }

        public static Border Card(string title, params UIElement[] children)
        {
            var stack = new StackPanel();
            if (title != null)
                stack.Children.Add(Text(title, "H2"));
            foreach (UIElement child in children)
                stack.Children.Add(child);
            return new Border { Style = Style("Card"), Child = stack };
        }

        // label (and a hint under it) on the left, the control on the right.
        public static FrameworkElement Setting(string label, string hint, UIElement control)
        {
            var grid = new Grid { Margin = new Thickness(0, 0, 0, 14) };
            grid.ColumnDefinitions.Add(new ColumnDefinition());
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            var left = new StackPanel { VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(0, 0, 20, 0) };
            var name = Text(label, "Body");
            name.FontWeight = FontWeights.SemiBold;
            left.Children.Add(name);
            if (hint != null)
            {
                var h = Text(hint, "Caption");
                h.Margin = new Thickness(0, 2, 0, 0);
                left.Children.Add(h);
            }
            grid.Children.Add(left);
            var element = control as FrameworkElement;
            if (element != null)
                element.VerticalAlignment = VerticalAlignment.Center;
            Grid.SetColumn(control, 1);
            grid.Children.Add(control);
            return grid;
        }

        // An on/off row for long lists: the switch and a "켜짐/꺼짐" word sit right before the label, and the whole row
        // lights up (accent border and tint) while on. A click anywhere on the row flips it.
        public static FrameworkElement ToggleRow(string label, string hint, bool on, Action<bool> changed)
        {
            return ToggleRow(label, hint, on, changed, null);
        }

        // extra: controls under the hint (value boxes, Lock); clicks on them do not flip the row.
        public static FrameworkElement ToggleRow(string label, string hint, bool on, Action<bool> changed, UIElement extra)
        {
            var tile = new Border { CornerRadius = new CornerRadius(10), BorderThickness = new Thickness(1), Padding = new Thickness(12, 8, 12, 8), Margin = new Thickness(0, 0, 0, 6), Cursor = System.Windows.Input.Cursors.Hand };
            var grid = new Grid();
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            grid.ColumnDefinitions.Add(new ColumnDefinition());
            var box = new CheckBox { Style = Style("Switch"), IsChecked = on, VerticalAlignment = VerticalAlignment.Center };
            var state = new TextBlock { Width = 44, FontSize = 13, FontWeight = FontWeights.Bold, VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(10, 0, 8, 0) };
            var left = new StackPanel { VerticalAlignment = VerticalAlignment.Center };
            var name = Text(label, "Body");
            name.FontWeight = FontWeights.SemiBold;
            left.Children.Add(name);
            if (hint != null)
            {
                var h = Text(hint, "Caption");
                h.Margin = new Thickness(0, 2, 0, 0);
                left.Children.Add(h);
            }
            if (extra != null)
                left.Children.Add(extra);
            Action paint = delegate
            {
                bool now = box.IsChecked == true;
                tile.Background = now ? Brush("AccentSoft") : Brushes.Transparent;
                tile.BorderBrush = now ? Brush("Accent") : Brush("Line");
                state.Text = now ? L.T("켜짐") : L.T("꺼짐");
                state.Foreground = now ? Brush("Accent") : Brush("Muted");
                name.Foreground = now ? Brush("Accent") : Brush("Text");
            };
            paint();
            box.Checked += delegate { paint(); changed(true); };
            box.Unchecked += delegate { paint(); changed(false); };
            tile.MouseLeftButtonUp += delegate(object sender, System.Windows.Input.MouseButtonEventArgs e)
            {
                // The switch flips itself; a click elsewhere on the row flips it here.
                for (var hit = e.OriginalSource as DependencyObject; hit != null && hit != tile; hit = VisualTreeHelper.GetParent(hit))
                    if (hit == box || (extra != null && hit == extra))
                        return;
                box.IsChecked = box.IsChecked != true;
            };
            Grid.SetColumn(state, 1);
            Grid.SetColumn(left, 2);
            grid.Children.Add(box);
            grid.Children.Add(state);
            grid.Children.Add(left);
            tile.Child = grid;
            return tile;
        }

        public static CheckBox Switch(bool on, Action<bool> changed, string text = null)
        {
            var box = new CheckBox { Style = Style("Switch"), IsChecked = on, Content = text };
            box.Checked += delegate { changed(true); };
            box.Unchecked += delegate { changed(false); };
            return box;
        }

        class Choice
        {
            public string Value, Label;

            public override string ToString()
            {
                return Label;
            }
        }

        // A drop-down of (value, label) pairs; a value it does not list (written by hand) is kept as an extra item.
        public static ComboBox Choices(string value, Action<string> changed, params string[] valueLabelPairs)
        {
            var box = new ComboBox { MinWidth = 300 };
            for (int i = 0; i + 1 < valueLabelPairs.Length; i += 2)
                box.Items.Add(new Choice { Value = valueLabelPairs[i], Label = valueLabelPairs[i + 1] });
            Choice selected = box.Items.Cast<Choice>().FirstOrDefault(c => c.Value == value);
            if (selected == null)
                box.Items.Add(selected = new Choice { Value = value, Label = value + L.T(" (직접 쓴 값)") });
            box.SelectedItem = selected;
            box.SelectionChanged += delegate
            {
                var choice = box.SelectedItem as Choice;
                if (choice != null)
                    changed(choice.Value);
            };
            return box;
        }

        public static FrameworkElement Slider(double min, double max, double value, double step, Func<double, string> format, Action<double> changed)
        {
            var panel = new StackPanel { Orientation = Orientation.Horizontal };
            var slider = new Slider { Minimum = min, Maximum = max, Value = value, Width = 240, IsSnapToTickEnabled = true, TickFrequency = step, SmallChange = step, LargeChange = step * 5, VerticalAlignment = VerticalAlignment.Center };
            var label = new TextBlock { Text = format(value), Width = 56, Foreground = Ui.Brush("SubText"), TextAlignment = TextAlignment.Right, VerticalAlignment = VerticalAlignment.Center };
            // Saved when the drag ends (or after a click/key), not at every step.
            var save = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(400) };
            save.Tick += delegate
            {
                save.Stop();
                changed(slider.Value);
            };
            slider.ValueChanged += delegate
            {
                label.Text = format(slider.Value);
                save.Stop();
                save.Start();
            };
            panel.Children.Add(slider);
            panel.Children.Add(label);
            return panel;
        }

        public static Button Button(string text, Action click, string style = null, string glyph = null, Thickness? margin = null)
        {
            object content = text;
            if (glyph != null)
            {
                var row = new StackPanel { Orientation = Orientation.Horizontal };
                row.Children.Add(new TextBlock { Text = glyph, Style = Style("Icon"), FontSize = 14, Margin = new Thickness(0, 0, 8, 0) });
                row.Children.Add(new TextBlock { Text = text, VerticalAlignment = VerticalAlignment.Center });
                content = row;
            }
            var button = new Button { Content = content };
            if (style != null)
                button.Style = Style(style);
            if (margin.HasValue)
                button.Margin = margin.Value;
            button.Click += delegate { click(); };
            return button;
        }

        public static Border Chip(string glyph, string text, Brush color)
        {
            var row = new StackPanel { Orientation = Orientation.Horizontal };
            row.Children.Add(new TextBlock { Text = glyph, Style = Style("Icon"), FontSize = 12, Foreground = color, Margin = new Thickness(0, 0, 7, 0) });
            row.Children.Add(new TextBlock { Text = text, FontSize = 12, Foreground = Ui.Brush("Text"), VerticalAlignment = VerticalAlignment.Center });
            return new Border { Style = Style("Chip"), Child = row, HorizontalAlignment = HorizontalAlignment.Left };
        }

        public static StackPanel Row(params UIElement[] children)
        {
            var row = new StackPanel { Orientation = Orientation.Horizontal };
            foreach (UIElement child in children)
                row.Children.Add(child);
            return row;
        }

        public static UniformGrid Columns(int count, params UIElement[] children)
        {
            var grid = new UniformGrid { Columns = count };
            foreach (UIElement child in children)
                grid.Children.Add(child);
            return grid;
        }

        public static string FolderDialog(string description, string start)
        {
            using (var dialog = new System.Windows.Forms.FolderBrowserDialog { Description = description, ShowNewFolderButton = true })
            {
                if (!string.IsNullOrEmpty(start) && System.IO.Directory.Exists(start))
                    dialog.SelectedPath = start;
                return dialog.ShowDialog() == System.Windows.Forms.DialogResult.OK ? dialog.SelectedPath : null;
            }
        }

        public static void OpenFolder(string path)
        {
            System.IO.Directory.CreateDirectory(path);
            System.Diagnostics.Process.Start("explorer.exe", "\"" + path + "\"");
        }
    }
}
