using System;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Windows.ApplicationModel.DataTransfer;
using Windows.Storage;
using Windows.Storage.Pickers;

namespace EGtools.Gui;

public sealed partial class ComparePage : Page
{
    private string? _lastOut;

    public ComparePage()
    {
        this.InitializeComponent();
        ApplyLoc();
        Loc.Changed += (_, _) => ApplyLoc();
    }

    private void ApplyLoc()
    {
        Title.Text = Loc.T("compare.title");
        OldLabel.Text = Loc.T("compare.old");
        NewLabel.Text = Loc.T("compare.new");
        OldDropText.Text = Loc.T("compare.drop");
        NewDropText.Text = Loc.T("compare.drop");
        OldBtn.Content = Loc.T("compare.old.pick");
        NewBtn.Content = Loc.T("compare.new.pick");
        OutBtn.Content = Loc.T("compare.out.pick");
        RunBtn.Content = Loc.T("compare.run");
        OpenBtn.Content = Loc.T("compare.open");
    }

    private void Log(string line)
    {
        DispatcherQueue.TryEnqueue(() => { LogBox.Text += line + "\n"; });
    }

    private async void OldBtn_Click(object sender, RoutedEventArgs e)
    {
        var f = await PickExcel();
        if (f != null) OldBox.Text = f;
    }

    private async void NewBtn_Click(object sender, RoutedEventArgs e)
    {
        var f = await PickExcel();
        if (f != null) NewBox.Text = f;
    }

    private async void OutBtn_Click(object sender, RoutedEventArgs e)
    {
        var picker = new FileSavePicker();
        PickerHelper.Initialize(picker, App.MainWindow!);
        picker.FileTypeChoices.Add("Excel 工作簿", new[] { ".xlsx" });
        picker.SuggestedFileName = "图纸变化清单_" + DateTime.Now.ToString("yyyyMMdd_HHmmss");
        var file = await picker.PickSaveFileAsync();
        if (file != null) OutBox.Text = file.Path;
    }

    private async Task<string?> PickExcel()
    {
        var picker = new FileOpenPicker();
        PickerHelper.Initialize(picker, App.MainWindow!);
        picker.FileTypeFilter.Add(".xlsx");
        var file = await picker.PickSingleFileAsync();
        return file?.Path;
    }

    private async void OldDrop_Drop(object sender, Microsoft.UI.Xaml.DragEventArgs e)
    {
        var p = await PickFromDrop(e);
        if (p != null) OldBox.Text = p;
    }

    private async void NewDrop_Drop(object sender, Microsoft.UI.Xaml.DragEventArgs e)
    {
        var p = await PickFromDrop(e);
        if (p != null) NewBox.Text = p;
    }

    private void OldDrop_DragOver(object sender, Microsoft.UI.Xaml.DragEventArgs e)
    {
        e.AcceptedOperation = DataPackageOperation.Copy;
        e.DragUIOverride.Caption = Loc.T("compare.drop");
        e.DragUIOverride.IsCaptionVisible = true;
    }

    private void NewDrop_DragOver(object sender, Microsoft.UI.Xaml.DragEventArgs e)
    {
        e.AcceptedOperation = DataPackageOperation.Copy;
        e.DragUIOverride.Caption = Loc.T("compare.drop");
        e.DragUIOverride.IsCaptionVisible = true;
    }

    private static async Task<string?> PickFromDrop(Microsoft.UI.Xaml.DragEventArgs e)
    {
        if (!e.DataView.Contains(StandardDataFormats.StorageItems)) return null;
        var items = await e.DataView.GetStorageItemsAsync();
        foreach (var it in items)
            if (it is StorageFile f && f.FileType.Equals(".xlsx", StringComparison.OrdinalIgnoreCase))
                return f.Path;
        return null;
    }

    private async void RunBtn_Click(object sender, RoutedEventArgs e)
    {
        if (string.IsNullOrWhiteSpace(OldBox.Text) || string.IsNullOrWhiteSpace(NewBox.Text))
        { Log("[错误] " + Loc.T("compare.err.nofile")); return; }
        if (!File.Exists(OldBox.Text)) { Log($"[错误] 找不到旧图纸: {OldBox.Text}"); return; }
        if (!File.Exists(NewBox.Text)) { Log($"[错误] 找不到新图纸: {NewBox.Text}"); return; }

        string output = !string.IsNullOrWhiteSpace(OutBox.Text)
            ? OutBox.Text
            : Path.Combine(Path.GetDirectoryName(OldBox.Text) ?? ".",
                           "图纸变化清单_" + DateTime.Now.ToString("yyyyMMdd_HHmmss") + ".xlsx");

        RunBtn.IsEnabled = false; OpenBtn.IsEnabled = false;
        LogBox.Text = "";
        ProgressBar.IsIndeterminate = true;
        Log($"[EGtools] {Loc.T("common.working")}");

        try
        {
            var changes = await Task.Run(() =>
            {
                var recs = EGtools.Core.ExcelTools.CompareFiles(OldBox.Text, NewBox.Text, null, null, Log);
                EGtools.Core.ExcelTools.WriteChanges(output, OldBox.Text, NewBox.Text, recs);
                return recs;
            });

            if (changes.Count == 0)
                Log("\n[结果] ✓ " + Loc.T("compare.same"));
            else
                Log($"\n[结果] 变化项目总数: {changes.Count}，涉及 PIPE: {changes.Select(c => c.PipeNo).Distinct().Count()}");
            _lastOut = output;
        }
        catch (Exception ex) { Log($"[异常] {ex.Message}"); }

        ProgressBar.IsIndeterminate = false; ProgressBar.Value = 100;
        RunBtn.IsEnabled = true; OpenBtn.IsEnabled = true;
        if (_lastOut != null) Log($"[完成] 报告已生成: {_lastOut}");
    }

    private void OpenBtn_Click(object sender, RoutedEventArgs e)
    {
        if (!string.IsNullOrEmpty(_lastOut))
            System.Diagnostics.Process.Start("explorer.exe", $"/select,\"{_lastOut}\"");
    }
}
