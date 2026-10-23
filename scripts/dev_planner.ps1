param(
    [string]$BuildDirectory = '',
    [string]$VisualStudioRoot = '',
    [string]$CMakePath = 'cmake',
    [int]$Jobs = 2
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $repo 'build-planner' }
if ($Jobs -lt 1) { throw 'Jobs must be positive.' }
if ($env:OS -eq 'Windows_NT' -and -not $env:VCINSTALLDIR) {
    if (-not $VisualStudioRoot) {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
        if (Test-Path -LiteralPath $vswhere) { $VisualStudioRoot = & $vswhere -latest -property installationPath }
    }
    if (-not $VisualStudioRoot) { throw 'Run in a VS developer shell or pass -VisualStudioRoot.' }
    Import-Module (Join-Path $VisualStudioRoot 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
    Enter-VsDevShell -VsInstallPath $VisualStudioRoot -SkipAutomaticLocation -DevCmdArguments '-arch=amd64'
}
$timer = [Diagnostics.Stopwatch]::StartNew()
& $CMakePath -S (Join-Path $repo 'tools/curved_transition') -B $BuildDirectory -G Ninja -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { throw 'Planner configure failed.' }
& $CMakePath --build $BuildDirectory --parallel $Jobs
if ($LASTEXITCODE -ne 0) { throw 'Planner compile failed.' }
$ctest = Join-Path (Split-Path (Get-Command $CMakePath).Source) 'ctest.exe'
if ($env:OS -ne 'Windows_NT') { $ctest = 'ctest' }
& $ctest --test-dir $BuildDirectory --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Planner tests failed.' }
Write-Output ('Planner configure/build/test: {0:N2} seconds. Full GUI/native/package validation is separate.' -f $timer.Elapsed.TotalSeconds)
