using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Navigation;
using System;

namespace EGtools.Gui;

public partial class App : Application
{
    // Exposed so pages can obtain the window handle for file/folder pickers.
    public static Window? MainWindow { get; set; }

    public const string Version = "3.0.0";

    public App()
    {
        Program.Boot("App.ctor start");
        // Catch UI-thread (XAML / managed) exceptions and keep the process alive
        // long enough to show the error instead of silently exiting.
        this.UnhandledException += (_, e) =>
        {
            e.Handled = true;
            Program.Fatal("UnhandledException (XAML)", e.Exception);
        };
        try
        {
            this.InitializeComponent();
        }
        catch (Exception ex)
        {
            Program.Fatal("App.InitializeComponent", ex);
            return;
        }
        Program.Boot("App.ctor done (XAML loaded)");
    }

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        Program.Boot("OnLaunched start");
        try
        {
            Loc.Init();
            Program.Boot("Loc.Init done");
            MainWindow = new MainWindow();
            Program.Boot("MainWindow created");
            MainWindow.Activate();
            Program.Boot("MainWindow activated");
            Program.Boot("GUI launched OK — window should be visible");
        }
        catch (Exception ex)
        {
            Program.Fatal("OnLaunched", ex);
        }
    }
}
