# Checks the expanded strip against the two cases that truncate when collapsed:
# a long "via ... (from ...)" origin, and a multi-file drop.
# Screenshots land next to this script unless you pass -OutDir.
param([string]$OutDir = (Join-Path $PSScriptRoot "screenshots"))

if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Force $OutDir | Out-Null }

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class ClipVF
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
    const int  FD_SIZE = 592, NAME_OFFSET = 72;
    const uint FD_FILESIZE = 0x40;

    static IntPtr Alloc(byte[] d)
    {
        IntPtr h = GlobalAlloc(GMEM_MOVEABLE, (UIntPtr)(uint)d.Length);
        IntPtr p = GlobalLock(h);
        Marshal.Copy(d, 0, p, d.Length);
        GlobalUnlock(h);
        return h;
    }

    public static void SetVirtual(string[] names, long[] sizes)
    {
        int n = names.Length;
        byte[] buf = new byte[4 + FD_SIZE * n];
        BitConverter.GetBytes((uint)n).CopyTo(buf, 0);
        for (int i = 0; i < n; i++)
        {
            int b = 4 + FD_SIZE * i;
            BitConverter.GetBytes(FD_FILESIZE).CopyTo(buf, b);
            BitConverter.GetBytes((uint)(sizes[i] >> 32)).CopyTo(buf, b + 64);
            BitConverter.GetBytes((uint)(sizes[i] & 0xFFFFFFFF)).CopyTo(buf, b + 68);
            byte[] nm = Encoding.Unicode.GetBytes(names[i]);
            Array.Copy(nm, 0, buf, b + NAME_OFFSET, Math.Min(nm.Length, 518));
        }
        if (!OpenClipboard(IntPtr.Zero)) throw new Exception("OpenClipboard failed");
        EmptyClipboard();
        SetClipboardData(RegisterClipboardFormat("FileGroupDescriptorW"), Alloc(buf));
        SetClipboardData(RegisterClipboardFormat("FileContents"), Alloc(new byte[] { 0 }));
        SetClipboardData(RegisterClipboardFormat("DataObject"), Alloc(new byte[] { 0, 0, 0, 0 }));
        SetClipboardData(RegisterClipboardFormat("Preferred DropEffect"), Alloc(BitConverter.GetBytes((uint)1)));
        CloseClipboard();
    }
}
'@

$exe = Join-Path $PSScriptRoot "..\build\x64\whoseclip.exe"
$ini = Join-Path $PSScriptRoot "..\build\x64\whoseclip.ini"
$wa  = [System.Windows.Forms.Screen]::PrimaryScreen.WorkingArea

function Shot($name, $height) {
    Start-Sleep -Milliseconds 1000
    $b = New-Object System.Drawing.Bitmap 520, $height
    $g = [System.Drawing.Graphics]::FromImage($b)
    $g.CopyFromScreen(($wa.Right - 505), $wa.Top, 0, 0, (New-Object System.Drawing.Size 520, $height))
    $g.Dispose(); $b.Save((Join-Path $OutDir "wc_$name.png")); $b.Dispose()
    "captured $name"
}

function Launch($flag) {
    Get-Process whoseclip -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 400
    if (Test-Path $ini) { [System.IO.File]::Delete($ini) }
    Start-Process $exe -ArgumentList "--label DEV-VM", $flag
    Start-Sleep -Milliseconds 900
}

# 1. Real local file drop with several files, so CF_HDROP lists every path.
$files = (Get-ChildItem C:\Windows\System32\*.dll | Select-Object -First 5).FullName
Launch '--collapsed'; Set-Clipboard -Path $files; Shot "files_collapsed" 110
Launch '--expanded';  Set-Clipboard -Path $files; Shot "files_expanded" 300

# 2. Virtual files, as if dragged out of a VM: names plus per-file sizes.
$names = @("payload_x64.exe", "capture_2026-09-12.pcapng", "notes.txt", "driver.sys")
$sizes = @([long]4823552, [long]1288490188, [long]4096, [long]98304)
Launch '--expanded'; [ClipVF]::SetVirtual($names, $sizes); Shot "virtual_expanded" 300

Get-Process whoseclip -ErrorAction SilentlyContinue | Stop-Process -Force
if (Test-Path $ini) { [System.IO.File]::Delete($ini) }
"done"
