using System;
using System.Collections.ObjectModel;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Windows.ApplicationModel.DataTransfer;
using Windows.Storage;
using Windows.Storage.Pickers;

namespace EGtools.Gui;

public sealed partial class ExtractPage : Page
{
    private readonly ObservableCollection<string> _pdfFiles = new();
    private string? _lastOutputDir;

    public ExtractPage()
    {
        this.InitializeComponent();
        FileList.ItemsSource = _pdfFiles;
        ApplyLoc();
        Loc.Changed += (_, _) => ApplyLoc();
    }

    private void ApplyLoc()
    {
        Title.Text = Loc.T("extract.title");
        DropText.Text = Loc.T("extract.drop");
        AddBtn.Content = Loc.T("extract.add");
        ClearBtn.Content = Loc.T("extract.clear");
        FilesLabel.Text = Loc.T("extract.files");
        FormatLabel.Text = Loc.T("extract.format");
        LayoutLabel.Text = Loc.T("extract.layout");
        GroupLabel.Text = Loc.T("extract.group");
        RefBtn.Content = Loc.T("extract.ref.pick");
        TagLabel.Text = Loc.T("extract.tag");
        OutBtn.Content = Loc.T("extract.out.pick");
        OutLabel.Text = Loc.T("extract.out");
        RunBtn.Content = Loc.T("extract.run");
        OpenBtn.Content = Loc.T("extract.open");
        FillCombo(FormatCombo, "extract.fmt.csv", "extract.fmt.xlsx", "extract.fmt.both");
        FillCombo(LayoutCombo, "extract.layout.merged", "extract.layout.separate");
        FillCombo(GroupCombo, "extract.group.embed", "extract.group.omit");
        UpdateCount();
    }

    private static void FillCombo(ComboBox cb, params string[] keys)
    {
        int prev = cb.SelectedIndex;
        cb.Items.Clear();
        foreach (var k in keys) cb.Items.Add(Loc.T(k));
        cb.SelectedIndex = (prev >= 0 && prev < keys.Length) ? prev : 0;
    }

    private void UpdateCount() => CountText.Text = Loc.T("extract.count", _pdfFiles.Count);

    private void Log(string line)
    {
        DispatcherQueue.TryEnqueue(() => { LogBox.Text += line + "\n"; });
    }

    private void AddFile(string path)
    {
        if (!_pdfFiles.Contains(path)) _pdfFiles.Add(path);
        UpdateCount();
    }

    private async void AddBtn_Click(object sender, RoutedEventArgs e)
    {
        var picker = new FileOpenPicker();
        PickerHelper.Initialize(picker, App.MainWindow!);
        picker.ViewMode = PickerViewMode.List;
        picker.FileTypeFilter.Add(".pdf");
        var files = await picker.PickMultipleFilesAsync();
        if (files != null) foreach (var f in files) AddFile(f.Path);
    }

    private void ClearBtn_Click(object sender, RoutedEventArgs e)
    {
        _pdfFiles.Clear();
        UpdateCount();
    }

    private void RemoveItem_Click(object sender, RoutedEventArgs e)
    {
        if ((sender as Button)?.Tag is string path) { _pdfFiles.Remove(path); UpdateCount(); }
    }

    private async void OutBtn_Click(object sender, RoutedEventArgs e)
    {
        var picker = new FolderPicker();
        PickerHelper.Initialize(picker, App.MainWindow!);
        picker.SuggestedStartLocation = PickerLocationId.DocumentsLibrary;
        var folder = await picker.PickSingleFolderAsync();
        if (folder != null) OutBox.Text = folder.Path;
    }

    private async void RefBtn_Click(object sender, RoutedEventArgs e)
    {
        var picker = new FileOpenPicker();
        PickerHelper.Initialize(picker, App.MainWindow!);
        picker.FileTypeFilter.Add(".xlsx");
        var file = await picker.PickSingleFileAsync();
        if (file != null) RefBox.Text = file.Path;
    }

    private async void DropZone_Drop(object sender, Microsoft.UI.Xaml.DragEventArgs e)
    {
        if (e.DataView.Contains(StandardDataFormats.StorageItems))
        {
            var items = await e.DataView.GetStorageItemsAsync();
            foreach (var it in items)
                if (it is StorageFile f && f.FileType.Equals(".pdf", StringComparison.OrdinalIgnoreCase))
                    AddFile(f.Path);
        }
    }

    private void DropZone_DragOver(object sender, Microsoft.UI.Xaml.DragEventArgs e)
    {
        e.AcceptedOperation = DataPackageOperation.Copy;
        e.DragUIOverride.Caption = Loc.T("extract.drop");
        e.DragUIOverride.IsCaptionVisible = true;
    }

    private async void RunBtn_Click(object sender, RoutedEventArgs e)
    {
        if (_pdfFiles.Count == 0) { Log("[错误] " + Loc.T("extract.err.nofile")); return; }

        var args = new System.Collections.Generic.List<string>();
        args.AddRange(_pdfFiles);
        if (!string.IsNullOrWhiteSpace(OutBox.Text)) { args.Add("-o"); args.Add(OutBox.Text); }
        args.Add("-f"); args.Add(FormatCombo.SelectedIndex switch { 1 => "xlsx", 2 => "both", _ => "csv" });
        args.Add("-C"); args.Add(LayoutCombo.SelectedIndex == 1 ? "separate" : "merged");
        args.Add("-G"); args.Add(GroupCombo.SelectedIndex == 1 ? "omit" : "embed");
        if (!string.IsNullOrWhiteSpace(RefBox.Text)) { args.Add("-r"); args.Add(RefBox.Text); }
        args.Add("--tag"); args.Add(string.IsNullOrWhiteSpace(TagBox.Text) ? "V3" : TagBox.Text);

        _lastOutputDir = !string.IsNullOrWhiteSpace(OutBox.Text)
            ? OutBox.Text : Path.GetDirectoryName(_pdfFiles[0]);

        RunBtn.IsEnabled = false; OpenBtn.IsEnabled = false;
        LogBox.Text = "";
        ProgressBar.IsIndeterminate = true;
        Log($"[EGtools] {Loc.T("common.working")}");

        int rc = 0;
        try
        {
            await Task.Run(() => rc = EGtools.Core.PdfExtractor.Run(args.ToArray(), Log));
        }
        catch (Exception ex) { Log($"[异常] {ex.Message}"); }

        ProgressBar.IsIndeterminate = false; ProgressBar.Value = 100;
        RunBtn.IsEnabled = true; OpenBtn.IsEnabled = true;
        Log(rc == 0 ? "[完成] " + Loc.T("extract.done") : $"[完成] 退出码 {rc}。");
    }

    private void OpenBtn_Click(object sender, RoutedEventArgs e)
    {
        if (!string.IsNullOrEmpty(_lastOutputDir) && Directory.Exists(_lastOutputDir))
            System.Diagnostics.Process.Start("explorer.exe", _lastOutputDir);
    }
}
