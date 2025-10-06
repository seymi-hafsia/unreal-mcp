param(
  [Parameter(Mandatory=$true)][string]$EngineRoot,
  [string]$PluginUplugin = "$(Resolve-Path -LiteralPath './MCPGameProject/Plugins/UnrealMCP/UnrealMCP.uplugin')",
  [string]$OutDir = "$(Resolve-Path -LiteralPath './_package/UnrealMCP_Win64' -ErrorAction SilentlyContinue)",
  [string]$TargetPlatforms = "Win64",
  [switch]$Rocket,
  [switch]$Clean,
  [switch]$VerboseLog
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Resolve-Full([string]$p) {
  if (-not $p) { return $null }
  try { return (Resolve-Path -LiteralPath $p).Path } catch { return $p }
}

$EngineRoot = Resolve-Full $EngineRoot
$PluginUplugin = Resolve-Full $PluginUplugin
if (-not $OutDir) { $OutDir = "./_package/UnrealMCP_Win64" }
$OutDir = Resolve-Full $OutDir

if (-not $EngineRoot) {
  throw "EngineRoot was not resolved to a valid path."
}

$uatPath = Join-Path $EngineRoot "Engine/Build/BatchFiles/RunUAT.bat"
if (-not (Test-Path $uatPath)) {
  throw "RunUAT.bat not found under EngineRoot: $EngineRoot"
}
if (-not (Test-Path $PluginUplugin)) {
  throw "uplugin not found: $PluginUplugin"
}

$pluginRoot = Split-Path -Parent $PluginUplugin
if ($Clean) {
  Write-Host "Cleaning plugin intermediates…" -ForegroundColor Cyan
  foreach ($d in @("Binaries", "Intermediate", "DerivedDataCache")) {
    $path = Join-Path $pluginRoot $d
    if (Test-Path $path) {
      Remove-Item -Recurse -Force $path
    }
  }
  if ($OutDir -and (Test-Path $OutDir)) {
    Remove-Item -Recurse -Force $OutDir
  }
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
New-Item -ItemType Directory -Force -Path "logs" | Out-Null
$logRootPath = (Resolve-Path -LiteralPath "logs").Path
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$log = Join-Path $logRootPath "BuildPlugin-$stamp.txt"

$args = @(
  "BuildPlugin",
  "-Plugin=`"$PluginUplugin`"",
  "-Package=`"$OutDir`"",
  "-TargetPlatforms=$TargetPlatforms"
)
if ($Rocket) { $args += "-Rocket" }

Write-Host "Running UAT: `"$uatPath`" $($args -join ' ')" -ForegroundColor Yellow

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $uatPath
$psi.Arguments = ($args -join ' ')
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.UseShellExecute = $false
$psi.CreateNoWindow = $true
$psi.WorkingDirectory = $pluginRoot

$process = New-Object System.Diagnostics.Process
$process.StartInfo = $psi
$null = $process.Start()

$writer = New-Object System.IO.StreamWriter($log, $false, [System.Text.Encoding]::UTF8)
try {
  while (-not $process.HasExited) {
    while (-not $process.StandardOutput.EndOfStream) {
      $line = $process.StandardOutput.ReadLine()
      if ($VerboseLog) { Write-Host $line }
      $writer.WriteLine($line)
    }
    while (-not $process.StandardError.EndOfStream) {
      $eline = $process.StandardError.ReadLine()
      Write-Host $eline -ForegroundColor Red
      $writer.WriteLine($eline)
    }
    Start-Sleep -Milliseconds 50
  }

  while (-not $process.StandardOutput.EndOfStream) {
    $line = $process.StandardOutput.ReadLine()
    if ($VerboseLog) { Write-Host $line }
    $writer.WriteLine($line)
  }
  while (-not $process.StandardError.EndOfStream) {
    $eline = $process.StandardError.ReadLine()
    Write-Host $eline -ForegroundColor Red
    $writer.WriteLine($eline)
  }
} finally {
  $writer.Flush()
  $writer.Close()
}

$process.WaitForExit()

$exitCode = $process.ExitCode
if ($exitCode -ne 0) {
  Write-Host "`nBUILD FAILED (ExitCode=$exitCode). Log: $log" -ForegroundColor Red
  Write-Host "---- Tail (last 50 lines) ----" -ForegroundColor DarkGray
  Get-Content $log -Tail 50 | ForEach-Object { Write-Host $_ }
  exit $exitCode
}

Write-Host "`nBUILD SUCCESS. Output: $OutDir" -ForegroundColor Green
Write-Host "Log: $log"
