# Puts synthetic clipboard payloads on the clipboard to exercise WhoseClip's
# code paths, and screenshots the strip after each one.
# Screenshots land next to this script unless you pass -OutDir.
param([string]$OutDir = (Join-Path $PSScriptRoot "screenshots"))

if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Force $OutDir | Out-Null }

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class ClipTest
{
    [DllImport("user32.dll", SetLastError = true)] static extern bool OpenClipboard(IntPtr h);
    [DllImport("user32.dll")] static extern bool CloseClipboard();
    [DllImport("user32.dll")] static extern bool EmptyClipboard();
    [DllImport("user32.dll")] static extern IntPtr SetClipboardData(uint f, IntPtr h);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern uint RegisterClipboardFormat(string s);
    [DllImport("kernel32.dll")] static extern IntPtr GlobalAlloc(uint f, UIntPtr b);
    [DllImport("kernel32.dll")] static extern IntPtr GlobalLock(IntPtr h);
    [DllImport("kernel32.dll")] static extern bool GlobalUnlock(IntPtr h);

    const uint GMEM_MOVEABLE = 0x0002;
    const uint CF_UNICODETEXT = 13;

    static IntPtr Alloc(byte[] data)
    {
        IntPtr h = GlobalAlloc(GMEM_MOVEABLE, (UIntPtr)(uint)data.Length);
        IntPtr p = GlobalLock(h);
        Marshal.Copy(data, 0, p, data.Length);
        GlobalUnlock(h);
        return h;
    }

    static byte[] Wide(string s) { return Encoding.Unicode.GetBytes(s + "\0"); }

    // Mimics what a password manager puts on the clipboard.
    public static void SetSensitive(string text)
    {
        if (!OpenClipboard(IntPtr.Zero)) throw new Exception("OpenClipboard failed");
        EmptyClipboard();
        SetClipboardData(CF_UNICODETEXT, Alloc(Wide(text)));
        SetClipboardData(RegisterClipboardFormat("ExcludeClipboardContentFromMonitorProcessing"), Alloc(new byte[] { 0, 0, 0, 0 }));
        SetClipboardData(RegisterClipboardFormat("CanIncludeInClipboardHistory"), Alloc(new byte[] { 0, 0, 0, 0 }));
        SetClipboardData(RegisterClipboardFormat("CanUploadToCloudClipboard"), Alloc(new byte[] { 0, 0, 0, 0 }));
        CloseClipboard();
    }

    // Mimics a browser copy: plain text plus CF_HTML carrying the source page.
    public static void SetHtml(string plain, string url)
    {
        string body = "<html><body><!--StartFragment-->" + plain + "<!--EndFragment--></body></html>";
        string head = "Version:0.9\r\nStartHTML:{0:D10}\r\nEndHTML:{1:D10}\r\nStartFragment:{2:D10}\r\nEndFragment:{3:D10}\r\nSourceURL:" + url + "\r\n";
        int hlen = string.Format(head, 0, 0, 0, 0).Length;
        int flen = Encoding.UTF8.GetByteCount(body);
        string full = string.Format(head, hlen, hlen + flen, hlen + 32, hlen + flen - 32) + body;

        if (!OpenClipboard(IntPtr.Zero)) throw new Exception("OpenClipboard failed");
        EmptyClipboard();
        SetClipboardData(CF_UNICODETEXT, Alloc(Wide(plain)));
        byte[] utf8 = Encoding.UTF8.GetBytes(full + "\0");
        SetClipboardData(RegisterClipboardFormat("HTML Format"), Alloc(utf8));
        CloseClipboard();
    }
}
'@

$wa = [System.Windows.Forms.Screen]::PrimaryScreen.WorkingArea

function Shot([string]$name) {
    Start-Sleep -Milliseconds 800
    $b = New-Object System.Drawing.Bitmap 510, 90
    $g = [System.Drawing.Graphics]::FromImage($b)
    $g.CopyFromScreen(($wa.Right - 505), $wa.Top, 0, 0, (New-Object System.Drawing.Size 510, 90))
    $g.Dispose()
    $b.Save((Join-Path $OutDir "whoseclip_$name.png"))
    $b.Dispose()
    "captured $name"
}

[System.Windows.Forms.Clipboard]::Clear()
Shot "empty"

[ClipTest]::SetSensitive("hunter2-correct-horse-battery-staple")
Shot "sensitive"

[ClipTest]::SetHtml("The clipboard owner is the window that last placed data in the clipboard.", "https://learn.microsoft.com/en-us/windows/win32/dataxchg/clipboard-operations")
Shot "html"

Set-Clipboard -Path (Get-ChildItem C:\Windows\System32\*.dll | Select-Object -First 3).FullName
Shot "files"

$bmp = New-Object System.Drawing.Bitmap 640, 360
[System.Windows.Forms.Clipboard]::SetImage($bmp)
$bmp.Dispose()
Shot "image"
