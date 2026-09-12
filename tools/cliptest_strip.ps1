# Screenshots the collapsed and expanded strip with multi-line clipboard
# content, to check the buttons, the flattened one-line view and the wrapped
# full view.
# Screenshots land next to this script unless you pass -OutDir.
param([string]$OutDir = (Join-Path $PSScriptRoot "screenshots"))

if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Force $OutDir | Out-Null }

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms

$exe = Join-Path $PSScriptRoot "..\build\x64\whoseclip.exe"
$wa  = [System.Windows.Forms.Screen]::PrimaryScreen.WorkingArea

$content = @'
static void SnapshotCapture(HWND hwnd, const Config& cfg, ClipSnapshot& s)
{
    SnapshotClear(s);
    s.seq = GetClipboardSequenceNumber();

    HWND owner = GetClipboardOwner();
    if (owner) {
        DWORD pid = 0;
        GetWindowThreadProcessId(owner, &pid);
    }
}
'@

function Shot($name, $height) {
    Start-Sleep -Milliseconds 1000
    $b = New-Object System.Drawing.Bitmap 520, $height
    $g = [System.Drawing.Graphics]::FromImage($b)
    $g.CopyFromScreen(($wa.Right - 505), $wa.Top, 0, 0, (New-Object System.Drawing.Size 520, $height))
    $g.Dispose()
    $b.Save((Join-Path $OutDir "wc_$name.png"))
    $b.Dispose()
    "captured $name"
}

foreach ($mode in @('collapsed', 'expanded')) {
    Get-Process whoseclip -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 400
    $flag = if ($mode -eq 'expanded') { '--expanded' } else { '--collapsed' }
    Start-Process $exe -ArgumentList "--label HOST", $flag
    Start-Sleep -Milliseconds 900
    Set-Clipboard -Value $content
    Shot "strip_$mode" $(if ($mode -eq 'expanded') { 300 } else { 110 })
}

Get-Process whoseclip -ErrorAction SilentlyContinue | Stop-Process -Force
"done"
