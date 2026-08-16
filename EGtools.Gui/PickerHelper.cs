using Microsoft.UI.Xaml;
using WinRT.Interop;
using Windows.Storage.Pickers;

namespace EGtools.Gui;

// WinUI 3 unpackaged apps must associate pickers with the owning window handle,
// otherwise the picker throws. This helper does exactly that.
public static class PickerHelper
{
    public static void Initialize(object picker, Window window)
    {
        var hwnd = WindowNative.GetWindowHandle(window);
        InitializeWithWindow.Initialize(picker, hwnd);
    }
}
