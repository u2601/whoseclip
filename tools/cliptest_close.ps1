# End-to-end test of the strip's close button: locates the strip by its accent
# bar, clicks the X, and checks the strip is gone and showStrip was persisted.
# Guarded so a coordinate error cannot click something else on the desktop.
# Screenshots land next to this script unless you pass -OutDir.
param([string]$OutDir = (Join-Path $PSScriptRoot "screenshots"))

if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Force $OutDir | Out-Null }

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class Mouse
{
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
    [DllImport("user32.dll")] static extern void mouse_event(uint f, int dx, int dy, uint d, IntPtr e);
    public struct POINT { public int X; public int Y; }
    const uint LEFTDOWN = 0x0002, LEFTUP = 0x0004;
    public static void Click() { mouse_event(LEFTDOWN, 0, 0, 0, IntPtr.Zero); System.Threading.Thread.Sleep(60); mouse_event(LEFTUP, 0, 0, 0, IntPtr.Zero); }
}
'@

$exe = Join-Path $PSScriptRoot "..\build\x64\whoseclip.exe"
$ini = Join-Path $PSScriptRoot "..\build\x64\whoseclip.ini"
if (Test-Path $ini) { [System.IO.File]::Delete($ini) }

Get-Process whoseclip -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 400
Start-Process $exe -ArgumentList "--label HOST", "--collapsed"
Start-Sleep -Milliseconds 1200
Set-Clipboard -Value "close button test"
Start-Sleep -Milliseconds 600

$wa = [System.Windows.Forms.Screen]::PrimaryScreen.WorkingArea
$w = 470; $margin = 16
$sx = $wa.Right - $w - $margin
$sy = $wa.Top + $margin

# Guard: confirm the accent bar is where we expect before clicking anything.
$probe = New-Object System.Drawing.Bitmap 6, 30
$g = [System.Drawing.Graphics]::FromImage($probe)
$g.CopyFromScreen($sx, ($sy + 10), 0, 0, (New-Object System.Drawing.Size 6, 30))
$g.Dispose()
$px = $probe.GetPixel(1, 15)
$probe.Dispose()
"accent probe at ($sx,$($sy+10)): R=$($px.R) G=$($px.G) B=$($px.B)"

if (-not ($px.B -gt 120 -and $px.B -gt $px.R + 40)) {
    "GUARD FAILED - strip not found at the expected position, not clicking"
    Get-Process whoseclip -ErrorAction SilentlyContinue | Stop-Process -Force
    exit 1
}

$cx = $sx + $w - 9 - 7
$cy = $sy + 9 + 7
"clicking close at ($cx,$cy)"

$saved = New-Object Mouse+POINT
[void][Mouse]::GetCursorPos([ref]$saved)
[void][Mouse]::SetCursorPos($cx, $cy)
Start-Sleep -Milliseconds 250      # let the hover repaint happen
[Mouse]::Click()
Start-Sleep -Milliseconds 700
[void][Mouse]::SetCursorPos($saved.X, $saved.Y)

$b = New-Object System.Drawing.Bitmap 520, 110
$g = [System.Drawing.Graphics]::FromImage($b)
$g.CopyFromScreen(($wa.Right - 505), $wa.Top, 0, 0, (New-Object System.Drawing.Size 520, 110))
$g.Dispose(); $b.Save((Join-Path $OutDir "wc_after_close.png")); $b.Dispose()

$proc = Get-Process whoseclip -ErrorAction SilentlyContinue
"process still running (tray should survive): $([bool]$proc)"
if (Test-Path $ini) {
    "persisted ini:"
    Get-Content $ini | Where-Object { $_ -match 'showStrip|expanded' } | ForEach-Object { "  $_" }
} else { "  no ini written" }

Get-Process whoseclip -ErrorAction SilentlyContinue | Stop-Process -Force
"done"
