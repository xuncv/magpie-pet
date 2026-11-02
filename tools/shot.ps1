# Capture the pet window (re-locates every time, so a dragged window won't break the shot)
# usage: shot.ps1 -Out out.png [-Zoom 1.3]
param([string]$Out = "shot.png", [double]$Zoom = 1.3, [string]$ProcName = "MagpiePet")

Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type @"
using System;using System.Runtime.InteropServices;
public class PetWin {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left,Top,Right,Bottom; }
}
"@
$target = Get-Process $ProcName -ErrorAction SilentlyContinue
if (-not $target) { throw "$ProcName is not running" }
$petId = $target.Id
$script:rr = $null
$cb = [PetWin+EnumProc]{ param($h,$l)
  $p = 0; [void][PetWin]::GetWindowThreadProcessId($h,[ref]$p)
  if ($p -eq $petId -and [PetWin]::IsWindowVisible($h)) {
    $r = New-Object PetWin+RECT; [void][PetWin]::GetWindowRect($h,[ref]$r)
    # 过掉 0x0 的控制台 / IME 等辅助窗口
    if (($r.Right - $r.Left) -gt 50 -and ($r.Bottom - $r.Top) -gt 50) { $script:rr = $r }
  }
  return $true }
[void][PetWin]::EnumWindows($cb, [IntPtr]::Zero)
if (-not $script:rr) { throw "window not found" }

$r = $script:rr
$w = [int]($r.Right - $r.Left); $h = [int]($r.Bottom - $r.Top)
$bmp = New-Object System.Drawing.Bitmap($w, $h)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r.Left, $r.Top, 0, 0, $bmp.Size)
$big = New-Object System.Drawing.Bitmap -ArgumentList $bmp, ([int]($w * $Zoom)), ([int]($h * $Zoom))
$big.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose(); $big.Dispose()
Write-Host ("shot -> {0}  rect=({1},{2}) {3}x{4}" -f $Out, $r.Left, $r.Top, $w, $h)
