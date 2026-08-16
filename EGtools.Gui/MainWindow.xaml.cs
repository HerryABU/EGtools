using System;
using System.IO;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media.Imaging;

namespace EGtools.Gui;

public sealed partial class MainWindow : Window
{
    public MainWindow()
    {
        try
        {
            this.InitializeComponent();
        }
        catch (Exception ex)
        {
            // A XAML parse error (e.g. an invalid image URI) must not silently
            // kill the app — surface it so the user sees what broke.
            Program.Fatal("MainWindow.InitializeComponent", ex);
            return;
        }
        // Unpackaged apps have NO package identity, so the ms-appx:/// URI scheme
        // is invalid and throws during InitializeComponent. Load the logo from the
        // app's local Assets folder (deployed next to the EXE) instead.
        try
        {
            var logoPath = Path.Combine(AppContext.BaseDirectory, "Assets", "logo.png");
            if (File.Exists(logoPath))
                LogoImg.Source = new BitmapImage(new Uri(logoPath, UriKind.Absolute));
        }
        catch { }
        ApplyLoc();
        Loc.Changed += (_, _) => ApplyLoc();
        this.Activated += (_, _) => Program.Boot("MainWindow Activated (visible)");
        // XAML IsSelected="True" does NOT raise SelectionChanged, so navigate to
        // the initial page explicitly — otherwise the frame starts blank.
        ContentFrame.Navigate(typeof(ExtractPage));
    }

    private void ApplyLoc()
    {
        NavExtract.Content = Loc.T("nav.extract");
        NavCompare.Content = Loc.T("nav.compare");
        NavPipeline.Content = Loc.T("nav.pipeline");
        NavSettings.Content = Loc.T("nav.settings");
        NavAbout.Content = Loc.T("nav.about");
        LangBtn.Content = Loc.T("lang.toggle");
    }

    private void NavView_SelectionChanged(NavigationView sender, NavigationViewSelectionChangedEventArgs args)
    {
        if (args.SelectedItem is not NavigationViewItem item) return;
        switch (item.Tag?.ToString())
        {
            case "Extract": ContentFrame.Navigate(typeof(ExtractPage)); break;
            case "Compare": ContentFrame.Navigate(typeof(ComparePage)); break;
            case "Pipeline": ContentFrame.Navigate(typeof(PipelinePage)); break;
            case "Settings": ContentFrame.Navigate(typeof(SettingsPage)); break;
            case "About": ContentFrame.Navigate(typeof(AboutPage)); break;
        }
    }

    private void LangBtn_Click(object sender, RoutedEventArgs e)
    {
        Loc.SetLanguage(Loc.Language == Lang.Zh ? Lang.En : Lang.Zh);
    }
}
