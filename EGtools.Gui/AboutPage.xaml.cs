using System;
using System.Diagnostics;
using System.IO;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media.Imaging;

namespace EGtools.Gui;

public sealed partial class AboutPage : Page
{
    public AboutPage()
    {
        this.InitializeComponent();
        // Unpackaged apps have no package identity → ms-appx:/// is invalid.
        // Load the logo from the local Assets folder (deployed next to the EXE).
        try
        {
            var logoPath = Path.Combine(AppContext.BaseDirectory, "Assets", "logo.png");
            if (File.Exists(logoPath))
                AboutLogoImg.Source = new BitmapImage(new Uri(logoPath, UriKind.Absolute));
        }
        catch { }
        ApplyLoc();
        Loc.Changed += (_, _) => ApplyLoc();
    }

    private void ApplyLoc()
    {
        VerText.Text = "EGtools v" + App.Version;
        Desc.Text = Loc.T("about.desc");
        FeaturesLabel.Text = Loc.T("about.features");
        Feat1.Text = Loc.T("about.feature1");
        Feat2.Text = Loc.T("about.feature2");
        Feat3.Text = Loc.T("about.feature3");
        Feat4.Text = Loc.T("about.feature4");
        AuthorLabel.Text = Loc.T("about.author");
        VersionLabel.Text = Loc.T("about.version");
        LicenseLabel.Text = Loc.T("about.license");
        VersionValue.Text = App.Version;
        DocsBtn.Content = Loc.T("about.docs");
    }

    private void DocsBtn_Click(object sender, RoutedEventArgs e)
    {
        // Locate the bundled usage document for the active language.
        var baseDir = System.AppContext.BaseDirectory;
        var name = Loc.Language == Lang.En ? "README_en.md" : "README_zh.md";
        var candidates = new[]
        {
            Path.Combine(baseDir, "docs", name),
            Path.Combine(baseDir, name),
            Path.Combine(baseDir, "docs", "README_zh.md"),
            Path.Combine(baseDir, "docs", "README_en.md"),
        };
        foreach (var p in candidates)
        {
            if (File.Exists(p))
            {
                try { Process.Start(new ProcessStartInfo(p) { UseShellExecute = true }); return; }
                catch { }
            }
        }
        // Fallback: open the application folder so the user can find docs manually.
        try { Process.Start(new ProcessStartInfo(baseDir) { UseShellExecute = true }); } catch { }
    }
}
