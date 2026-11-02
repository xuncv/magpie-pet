# 导入一个 Codex Custom Pet（pet.json + spritesheet.webp/png）到本项目
#   - 校验图集规格（8 x 11）
#   - webp 用 ffmpeg 转成 PNG（WIC 的 webp 解码会丢 alpha，必须转）
#   - 复制到 pets\<name>\，并把它设为当前皮肤
#
# 用法:
#   powershell -ExecutionPolicy Bypass -File tools\import-pet.ps1 -Source "D:\some\pet" -Name mypet
#   powershell -ExecutionPolicy Bypass -File tools\import-pet.ps1 -Source ".\pet"            # 名字取 pet.json 的 id
param(
  [Parameter(Mandatory = $true)][string]$Source,
  [string]$Name = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$src  = (Resolve-Path $Source).Path

$jsonPath = Join-Path $src "pet.json"
if (-not (Test-Path $jsonPath)) { throw "找不到 pet.json：$jsonPath" }
$meta = [System.IO.File]::ReadAllText($jsonPath, [System.Text.Encoding]::UTF8) | ConvertFrom-Json
Write-Host ("pet.json: id={0} name={1} version={2}" -f $meta.id, $meta.displayName, $meta.spriteVersionNumber)

if (-not $Name) { $Name = $meta.id }
if (-not $Name) { throw "pet.json 里没有 id，请用 -Name 指定" }

$sheetName = $meta.spritesheetPath
if (-not $sheetName) { $sheetName = "spritesheet.webp" }
$sheet = Join-Path $src $sheetName
if (-not (Test-Path $sheet)) { throw "找不到图集：$sheet" }

$dst = Join-Path $root "pets\$Name"
New-Item -ItemType Directory -Force $dst | Out-Null
Copy-Item $jsonPath (Join-Path $dst "pet.json") -Force

$ext = [System.IO.Path]::GetExtension($sheet).ToLower()
$outSheet = Join-Path $dst ([System.IO.Path]::GetFileName($sheet))

if ($ext -eq ".png") {
  Copy-Item $sheet $outSheet -Force
  Write-Host "png 直接复制"
} else {
  # webp 必须用 ffmpeg 转：本机 WIC 的 webp 解码器会解出错误内容（见 README）
  $ff = $env:FFMPEG
  if (-not $ff) { $ff = (Get-Command ffmpeg -ErrorAction SilentlyContinue).Source }
  if (-not $ff) { throw "需要 ffmpeg 把 $ext 转成 png（WIC 的 webp 解码不可靠；装到 PATH 或设 FFMPEG=<path>）" }
  $outSheet = Join-Path $dst "spritesheet.png"
  & $ff -y -loglevel error -i $sheet $outSheet
  if ($LASTEXITCODE -ne 0) { throw "ffmpeg 转换失败" }
  Write-Host "webp -> png（ffmpeg，保留 alpha 且内容正确）"
}

# 校验规格 + alpha
Add-Type -AssemblyName PresentationCore, WindowsBase
$s = [System.IO.File]::OpenRead($outSheet)
try {
  # 关键：必须用 PreservePixelFormat；默认的 None 解 webp 会丢 alpha
  $dec = [System.Windows.Media.Imaging.BitmapDecoder]::Create($s, [System.Windows.Media.Imaging.BitmapCreateOptions]::PreservePixelFormat, [System.Windows.Media.Imaging.BitmapCacheOption]::OnLoad)
  $fr = $dec.Frames[0]
  $w = $fr.PixelWidth; $h = $fr.PixelHeight

  # 官方规则：spriteVersionNumber 决定行数（1=9行 1536x1872，2=11行 1536x2288）
  $ver = 1
  if ($null -ne $meta.spriteVersionNumber) { $ver = [int]$meta.spriteVersionNumber }
  $rows = if ($ver -eq 2) { 11 } else { 9 }
  $canonW = 8 * 192; $canonH = $rows * 208
  Write-Host ("声明版本 {0} -> {1} 行，单格 {2}x{3}" -f $ver, $rows, [int]($w/8), [int]($h/$rows))
  if ($w % 8 -ne 0 -or $h % $rows -ne 0) {
    Write-Warning "尺寸 $w x $h 不能被 8 列 x $rows 行整除，Codex 会拒绝"
  }
  elseif ([long]$w * $canonH -ne [long]$h * $canonW) {
    Write-Warning ("宽高比不符合 canonical {0}x{1}，Codex 的版本检测会判为不匹配" -f $canonW, $canonH)
  }
  else {
    Write-Host ("尺寸校验通过（{0}x{1}，缩放 {2:0.00}x）" -f $w, $h, ($w / $canonW))
  }
  if ($ver -ne 2 -and $h / $rows -gt 209) { Write-Warning "可能是 v2 图集但未声明 spriteVersionNumber: 2（会默认按 v1 解读）" }
  $conv = New-Object System.Windows.Media.Imaging.FormatConvertedBitmap($fr, [System.Windows.Media.PixelFormats]::Bgra32, $null, 0)
  $stride = $w * 4
  $px = New-Object byte[] ($stride * $h)
  $conv.CopyPixels($px, $stride, 0)
  $i = (100 * $stride) + (5 * 4)
  if ($px[$i + 3] -ne 0) { Write-Warning "左上角像素不是透明的（alpha=$($px[$i+3])），这张图可能丢了 alpha" }
  else { Write-Host "alpha 正常" -ForegroundColor Green }
}
finally { $s.Dispose() }

Write-Host ("已安装到 pets\{0}" -f $Name) -ForegroundColor Green
Write-Host ("用这个参数启动：--sheet pets\{0}\spritesheet.png" -f $Name) -ForegroundColor DarkGray
