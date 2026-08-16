using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Windows.ApplicationModel.DataTransfer;
using Windows.Storage;
using Windows.Storage.Pickers;

namespace EGtools.Gui;

public sealed partial class PipelinePage : Page
{
    private string? _lastOut;

    public PipelinePage()
    {
        this.InitializeComponent();
        ApplyLoc();
        Loc.Changed += (_, _) => ApplyLoc();
    }

    private void ApplyLoc()
    {
        Title.Text = Loc.T("pipeline.title");
        Desc.Text = Loc.T("pipeline.desc");
        OldLabel.Text = Loc.T("pipeline.old");
        NewLabel.Text = Loc.T("pipeline.new");
        OldDropText.Text = Loc.T("pipeline.drop");
        NewDropText.Text = Loc.T("pipeline.drop");
        OldBtn.Content = Loc.T("pipeline.pick");
        NewBtn.Content = Loc.T("pipeline.pick");
        LayoutLabel.Text = Loc.T("pipeline.layout");
        GroupLabel.Text = Loc.T("pipeline.group");
        FillCombo(LayoutCombo, "extract.layout.merged", "extract.layout.separate");
        FillCombo(GroupCombo, "extract.group.embed", "extract.group.omit");
        OutBtn.Content = Loc.T("pipeline.out.pick");
        RunBtn.Content = Loc.T("pipeline.run");
        OpenBtn.Content = Loc.T("pipeline.open");
    }

    private static void FillCombo(ComboBox cb, params string[] keys)
    {
        int prev = cb.SelectedIndex;
        cb.Items.Clear();
        foreach (var k in keys) cb.Items.Add(Loc.T(k));
        cb.SelectedIndex = (prev >= 0 && prev < keys.Length) ? prev : 0;
    }

    private void Log(string line)
    {
        DispatcherQueue.TryEnqueue(() => { LogBox.Text += line + "\n"; });
    }

    // Resolve a side: if PDF, extract to a temp xlsx and return that path;
    // if xlsx, return directly; otherwise null.
    private async Task<string?> ResolveSide(string src, string sideTag, string tempDir)
    {
        if (src.EndsWith(".pdf", StringComparison.OrdinalIgnoreCase))
        {
            Log($"[串联] 提取{sideTag} PDF: {Path.GetFileName(src)}");
            string outBase = Path.Combine(tempDir, sideTag + "_" + Path.GetFileNameWithoutExtension(src));
            var args = new List<string> { src, "-o", tempDir, "-f", "xlsx",
                "-C", LayoutCombo.SelectedIndex == 1 ? "separate" : "merged",
                "-G", GroupCombo.SelectedIndex == 1 ? "omit" : "embed", "--tag", sideTag };
            int rc = await System.Threading.Tasks.Task.Run(() => EGtools.Core.PdfExtractor.Run(args.ToArray(), Log));
            if (rc != 0) { Log($"[错误] {sideTag} 提取失败 (退出码 {rc})"); return null; }
            string xlsx = outBase + ".xlsx";
            return File.Exists(xlsx) ? xlsx : null;
        }
        return src; // already xlsx
    }

    private async void OldBtn_Click(object sender, RoutedEventArgs e)
    {
        var p = await PickSource();
        if (p != null) OldBox.Text = p;
    }

    private async void NewBtn_Click(object sender, RoutedEventArgs e)
    {
        var p = await PickSource();
        if (p != null) NewBox.Text = p;
    }

    private async Task<string?> PickSource()
    {
        var picker = new FileOpenPicker();
        PickerHelper.Initialize(picker, App.MainWindow!);
        picker.FileTypeFilter.Add(".pdf");
        picker.FileTypeFilter.Add(".xlsx");
        var file = await picker.PickSingleFileAsync();
        return file?.Path;
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

    private void OldDrop_DragOver(object sender, Microsoft.UI.Xaml.DragEventArgs e) => ApplyDragOver(e);
    private void NewDrop_DragOver(object sender, Microsoft.UI.Xaml.DragEventArgs e) => ApplyDragOver(e);

    private void ApplyDragOver(Microsoft.UI.Xaml.DragEventArgs e)
    {
        e.AcceptedOperation = DataPackageOperation.Copy;
        e.DragUIOverride.Caption = Loc.T("pipeline.drop");
        e.DragUIOverride.IsCaptionVisible = true;
    }

    private static async Task<string?> PickFromDrop(Microsoft.UI.Xaml.DragEventArgs e)
    {
        if (!e.DataView.Contains(StandardDataFormats.StorageItems)) return null;
        var items = await e.DataView.GetStorageItemsAsync();
        foreach (var it in items)
            if (it is StorageFile f &&
                (f.FileType.Equals(".pdf", StringComparison.OrdinalIgnoreCase) ||
                 f.FileType.Equals(".xlsx", StringComparison.OrdinalIgnoreCase)))
                return f.Path;
        return null;
    }

    private async void RunBtn_Click(object sender, RoutedEventArgs e)
    {
        if (string.IsNullOrWhiteSpace(OldBox.Text) || string.IsNullOrWhiteSpace(NewBox.Text))
        { Log("[错误] " + Loc.T("pipeline.err")); return; }

        string output = !string.IsNullOrWhiteSpace(OutBox.Text)
            ? OutBox.Text
            : Path.Combine(Path.GetDirectoryName(OldBox.Text) ?? ".",
                           "图纸变化清单_" + DateTime.Now.ToString("yyyyMMdd_HHmmss") + ".xlsx");

        string tempDir = Path.Combine(Path.GetTempPath(), "EGtools_Pipeline_" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(tempDir);

        RunBtn.IsEnabled = false; OpenBtn.IsEnabled = false;
        LogBox.Text = "";
        ProgressBar.IsIndeterminate = true;
        Log($"[EGtools] {Loc.T("common.working")}");

        try
        {
            string? oldX = await Task.Run(() => ResolveSide(OldBox.Text, "OLD", tempDir));
            if (oldX == null) return;
            string? newX = await Task.Run(() => ResolveSide(NewBox.Text, "NEW", tempDir));
            if (newX == null) return;

            var changes = await Task.Run(() =>
            {
                var recs = EGtools.Core.ExcelTools.CompareFiles(oldX, newX, null, null, Log);
                EGtools.Core.ExcelTools.WriteChanges(output, oldX, newX, recs);
                return recs;
            });

            if (changes.Count == 0)
                Log("\n[结果] ✓ " + Loc.T("compare.same"));
            else
                Log($"\n[结果] 变化项目总数: {changes.Count}，涉及 PIPE: {changes.Select(c => c.PipeNo).Distinct().Count()}");
            _lastOut = output;
        }
        catch (Exception ex) { Log($"[异常] {ex.Message}"); }
        finally
        {
            ProgressBar.IsIndeterminate = false; ProgressBar.Value = 100;
            RunBtn.IsEnabled = true; OpenBtn.IsEnabled = true;
            if (_lastOut != null) Log($"[完成] 报告已生成: {_lastOut}");
            try { Directory.Delete(tempDir, true); } catch { }
        }
    }

    private void OpenBtn_Click(object sender, RoutedEventArgs e)
    {
        if (!string.IsNullOrEmpty(_lastOut))
            System.Diagnostics.Process.Start("explorer.exe", $"/select,\"{_lastOut}\"");
    }
}
