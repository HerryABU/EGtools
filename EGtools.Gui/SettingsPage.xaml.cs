using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace EGtools.Gui;

public sealed partial class SettingsPage : Page
{
    public SettingsPage()
    {
        this.InitializeComponent();
        ApplyLoc();
        Loc.Changed += (_, _) => ApplyLoc();
        // reflect current values
        LangZh.IsChecked = Loc.Language == Lang.Zh;
        LangEn.IsChecked = Loc.Language == Lang.En;
        var theme = ThemePref;
        ThemeSystem.IsChecked = theme == "system";
        ThemeLight.IsChecked = theme == "light";
        ThemeDark.IsChecked = theme == "dark";
    }

    private void ApplyLoc()
    {
        Title.Text = Loc.T("settings.title");
        LangLabel.Text = Loc.T("settings.lang");
        LangZh.Content = Loc.T("settings.lang.zh");
        LangEn.Content = Loc.T("settings.lang.en");
        ThemeLabel.Text = Loc.T("settings.theme");
        ThemeSystem.Content = Loc.T("settings.theme.system");
        ThemeLight.Content = Loc.T("settings.theme.light");
        ThemeDark.Content = Loc.T("settings.theme.dark");
    }

    private void LangZh_Checked(object sender, RoutedEventArgs e) => Loc.SetLanguage(Lang.Zh);
    private void LangEn_Checked(object sender, RoutedEventArgs e) => Loc.SetLanguage(Lang.En);

    private void ThemeSystem_Checked(object sender, RoutedEventArgs e) => ApplyTheme("system");
    private void ThemeLight_Checked(object sender, RoutedEventArgs e) => ApplyTheme("light");
    private void ThemeDark_Checked(object sender, RoutedEventArgs e) => ApplyTheme("dark");

    private static string ThemePref
    {
        get
        {
            try
            {
                var p = System.IO.Path.Combine(System.Environment.GetFolderPath(System.Environment.SpecialFolder.LocalApplicationData), "EGtools", "theme.txt");
                return System.IO.File.Exists(p) ? System.IO.File.ReadAllText(p).Trim() : "system";
            }
            catch { return "system"; }
        }
        set
        {
            try
            {
                var p = System.IO.Path.Combine(System.Environment.GetFolderPath(System.Environment.SpecialFolder.LocalApplicationData), "EGtools", "theme.txt");
                System.IO.Directory.CreateDirectory(System.IO.Path.GetDirectoryName(p)!);
                System.IO.File.WriteAllText(p, value);
            }
            catch { }
        }
    }

    private static void ApplyTheme(string theme)
    {
        ThemePref = theme;
        var w = App.MainWindow;
        if (w == null) return;
        // set the theme on the window root
        if (w.Content is FrameworkElement fe) fe.RequestedTheme = theme switch
        {
            "light" => ElementTheme.Light,
            "dark" => ElementTheme.Dark,
            _ => ElementTheme.Default
        };
    }
}
