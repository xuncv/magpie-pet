# interp-bake.ps1 - interpolate a baked pet atlas and rebuild it
#
# Takes a pet folder that already has spritesheet.png + layout.txt (fixed grid),
# interpolates every row with ffmpeg's optical-flow minterpolate, and writes a new
# pet folder with more frames per row.
#
# usage:
#   powershell -ExecutionPolicy Bypass -File tools\interp-bake.ps1 `
#       -SrcDir pets\<name> -OutDir pets\<name>-hd -Interp 4 -MsPerFrame 50
#
# Why ffmpeg and not C#: minterpolate (mci + aobmc + bidir) is a proven optical-flow
# implementation; writing one by hand is not worth it. Alpha is preserved (rgba in -> rgba out).
param(
  [string]$SrcDir = "pets\deepseek-girl",
  [string]$OutDir = "pets\deepseek-girl-hd",
  [int]$Interp = 4,          # frame multiplier
  [int]$MsPerFrame = 50,     # playback speed of the RESULT
  [string]$Ffmpeg = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$src  = Join-Path $root $SrcDir
$out  = Join-Path $root $OutDir

if (-not $Ffmpeg) { $Ffmpeg = $env:FFMPEG }
if (-not $Ffmpeg) { $Ffmpeg = (Get-Command ffmpeg -ErrorAction SilentlyContinue).Source }
if (-not $Ffmpeg) { throw "ffmpeg not found (put it on PATH or set FFMPEG=<path>)" }
if (-not (Test-Path (Join-Path $src "layout.txt"))) { throw "no layout.txt in $src" }

# ---- read layout.txt ----
$layout = @{}
$tracks = @()
foreach ($line in Get-Content (Join-Path $src "layout.txt")) {
  $l = $line.Trim()
  if (-not $l) { continue }
  if ($l -match '^cell=(\d+)x(\d+)') {
    $layout.cw = [int]$Matches[1]; $layout.ch = [int]$Matches[2]
  } elseif ($l -match '^cols=(\d+)') {
    $layout.cols = [int]$Matches[1]
  } elseif ($l -match '^rows=(\d+)') {
    $layout.rows = [int]$Matches[1]
  } elseif ($l -match '^track=(\S+)\s+row=(\d+)\s+frames=(\d+)\s+total=(\d+)') {
    $tracks += [pscustomobject]@{
      name = $Matches[1]; row = [int]$Matches[2]; frames = [int]$Matches[3]; total = [int]$Matches[4]
    }
  }
}
if (-not $layout.cw) { throw "layout.txt has no cell=" }

$cw = $layout.cw; $ch = $layout.ch; $cols = $layout.cols; $rows = $layout.rows
Write-Host ("src {0}  cell {1}x{2}  {3} cols x {4} rows  {5} tracks" -f $SrcDir, $cw, $ch, $cols, $rows, $tracks.Count)

$tmp = Join-Path $env:TEMP ("interpbake_" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force $tmp, (Join-Path $tmp "rows") | Out-Null

$sheet = Join-Path $src "spritesheet.png"
$newFrames = @{}     # row -> frame count
$newTotal  = @{}     # row -> total ms

for ($r = 0; $r -lt $rows; $r++) {
  $rd = Join-Path $tmp ("r" + $r)
  New-Item -ItemType Directory -Force $rd | Out-Null

  # 1) cut this row's cells
  for ($c = 0; $c -lt $cols; $c++) {
    $x = $c * $cw
    & $Ffmpeg -y -loglevel error -i $sheet -vf "crop=${cw}:${ch}:${x}:$($r*$ch)" (Join-Path $rd ("{0:d2}.png" -f $c))
    if ($LASTEXITCODE -ne 0) { throw "crop failed at row $r col $c" }
  }

  # 2) 追加两帧（f0, f1）给 minterpolate 留跑道。
  #    只追加 1 帧不够：它需要“后面还有帧”才肯插值，否则接缝那段会被截掉（12 帧只出 45 帧，
  #    导致最后一帧≈f11，接回 f0 就是硬跳）。追加 2 帧后出 53 帧，取前 cols*Interp 帧
  #    正好覆盖完整循环（末帧≈f11.75，接回 f0 只差 1/4 帧）。
  Copy-Item (Join-Path $rd "00.png") (Join-Path $rd ("{0:d2}.png" -f $cols))
  Copy-Item (Join-Path $rd "01.png") (Join-Path $rd ("{0:d2}.png" -f ($cols + 1)))

  # 3) interpolate: N source frames -> N*Interp output frames
  $od = Join-Path $tmp ("o" + $r)
  New-Item -ItemType Directory -Force $od | Out-Null
  $want = $cols * $Interp
  $fps = $cols * $Interp          # input framerate = $cols, so 1s -> $fps frames
  & $Ffmpeg -y -loglevel error -framerate $cols -i (Join-Path $rd "%02d.png") `
      -vf "minterpolate=fps=${fps}:mi_mode=mci:mc_mode=aobmc:me_mode=bidir:vsbmc=1" `
      -frames:v $want (Join-Path $od "%03d.png")
  if ($LASTEXITCODE -ne 0) { throw "minterpolate failed at row $r" }

  $got = (Get-ChildItem $od -Filter *.png | Measure-Object).Count
  Write-Host ("  row {0}: {1} -> {2} frames" -f $r, $cols, $got)
  $newFrames[$r] = $got

  # 4) tile this row into a strip
  & $Ffmpeg -y -loglevel error -i (Join-Path $od "%03d.png") -vf "tile=${got}x1" -frames:v 1 (Join-Path $tmp ("rows\r$r.png"))
  if ($LASTEXITCODE -ne 0) { throw "tile failed at row $r" }
}

# 5) stack rows into the final sheet
$inputs = @(); for ($r = 0; $r -lt $rows; $r++) { $inputs += @("-i", (Join-Path $tmp ("rows\r$r.png"))) }
$filt = ""; for ($r = 0; $r -lt $rows; $r++) { $filt += "[$r`:v]" }
$filt += "vstack=inputs=$rows"
New-Item -ItemType Directory -Force $out | Out-Null
& $Ffmpeg -y -loglevel error @inputs -filter_complex $filt (Join-Path $out "spritesheet.png")
if ($LASTEXITCODE -ne 0) { throw "vstack failed" }

# 6) write layout.txt
$maxFrames = ($newFrames.Values | Measure-Object -Maximum).Maximum
$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine("cell=${cw}x${ch}")
[void]$sb.AppendLine("cols=$maxFrames")
[void]$sb.AppendLine("rows=$rows")
foreach ($t in $tracks) {
  $n = $newFrames[$t.row]
  if (-not $n) { $n = $maxFrames }
  $total = $n * $MsPerFrame
  [void]$sb.AppendLine("track=$($t.name) row=$($t.row) frames=$n total=$total")
}
Set-Content -Path (Join-Path $out "layout.txt") -Value $sb.ToString() -Encoding ASCII -NoNewline

Write-Host ""
Write-Host ("OK -> {0}" -f $OutDir) -ForegroundColor Green
Write-Host ("  {0} cols x {1} rows, cell {2}x{3}, {4} ms/frame" -f $maxFrames, $rows, $cw, $ch, $MsPerFrame)
$sz = [math]::Round((Get-Item (Join-Path $out "spritesheet.png")).Length / 1KB)
Write-Host ("  spritesheet.png {0} KB" -f $sz)

Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
