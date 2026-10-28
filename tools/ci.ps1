#Requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration,
    [Parameter(Mandatory = $true)]
    [string]$Msys2Root
)

$ErrorActionPreference = 'Stop'
# Native exit codes are checked explicitly; stderr alone is not a failure.
$PSNativeCommandUseErrorActionPreference = $false
$projectRoot = Split-Path -Parent $PSScriptRoot
$buildDirectory = Join-Path $projectRoot ('build/' + $Configuration.ToLowerInvariant())
$logDirectory = Join-Path $projectRoot ('build/ci-logs/' + $Configuration.ToLowerInvariant())
$originalPath = $env:PATH
$originalInterpreter = $env:MYB_INTERPRETER
$script:failureExit = 0

function Invoke-CiCommand {
    param([string]$Executable, [string[]]$Arguments)
    Write-Output ('> ' + $Executable + ' ' + ($Arguments -join ' '))
    & $Executable @Arguments 2>&1 | Tee-Object -FilePath $script:logPath -Append
    $commandExit = $LASTEXITCODE
    if ($commandExit -ne 0) {
        $script:failureExit = $commandExit
        throw "Command failed with exit ${commandExit}: $Executable"
    }
}

function Find-CiTool {
    param([string]$Name, [string]$PreferredDirectory)
    $preferred = Join-Path $PreferredDirectory ($Name + '.exe')
    if (Test-Path -LiteralPath $preferred -PathType Leaf) { return $preferred }
    # Native CMake/Ninja may also be supplied through the developer's PATH.
    if ($Name -in @('cmake', 'ninja')) {
        $command = Get-Command ($Name + '.exe') -CommandType Application -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if ($command) { return $command.Source }
    }
    throw "Missing tool: $preferred"
}

try {
    New-Item -ItemType Directory -Force -Path $logDirectory | Out-Null
    $script:logPath = Join-Path $logDirectory 'ci.log'
    Set-Content -LiteralPath $script:logPath -Value "MYB CI $Configuration" -Encoding UTF8
    $msysRoot = (Resolve-Path -LiteralPath $Msys2Root).Path
    $ucrtBin = Join-Path $msysRoot 'ucrt64/bin'
    $usrBin = Join-Path $msysRoot 'usr/bin'
    $env:PATH = $ucrtBin + ';' + $usrBin + ';' + $originalPath
    $gcc = Find-CiTool 'gcc' $ucrtBin
    $gxx = Find-CiTool 'g++' $ucrtBin
    $python = Find-CiTool 'python' $ucrtBin
    $cmake = Find-CiTool 'cmake' $ucrtBin
    $ninja = Find-CiTool 'ninja' $ucrtBin
    $flex = Find-CiTool 'flex' $usrBin
    $bison = Find-CiTool 'bison' $usrBin
    Push-Location -LiteralPath $projectRoot
    try {
        Invoke-CiCommand 'git' @('rev-parse', 'HEAD')
        foreach ($tool in @($gcc, $gxx, $python, $cmake, $ninja, $flex, $bison)) {
            Invoke-CiCommand $tool @('--version')
        }
        # Only remove the fixed configuration's generated directory, after checking
        # its resolved location and rejecting redirected build directories.
        $buildRoot = Join-Path $projectRoot 'build'
        foreach ($directory in @($buildRoot, $buildDirectory)) {
            if (Test-Path -LiteralPath $directory) {
                $item = Get-Item -LiteralPath $directory -Force
                if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
                    throw "Build directory cannot be a link: $directory"
                }
            }
        }
        if (Test-Path -LiteralPath $buildDirectory) {
            $resolvedBuild = (Resolve-Path -LiteralPath $buildDirectory).Path
            $expectedBuild = [IO.Path]::GetFullPath($buildDirectory)
            if ($resolvedBuild -ne $expectedBuild -or
                -not $resolvedBuild.StartsWith($projectRoot + [IO.Path]::DirectorySeparatorChar,
                    [StringComparison]::OrdinalIgnoreCase)) {
                throw "Refusing to clean unexpected build directory: $resolvedBuild"
            }
            Remove-Item -LiteralPath $resolvedBuild -Recurse -Force
        }
        $internalTests = if ($Configuration -eq 'Debug') { 'ON' } else { 'OFF' }
        Invoke-CiCommand $cmake @('-S', $projectRoot, '-B', $buildDirectory, '-G', 'Ninja',
            "-DCMAKE_BUILD_TYPE=$Configuration", "-DCMAKE_C_COMPILER=$gcc",
            "-DCMAKE_CXX_COMPILER=$gxx", "-DCMAKE_MAKE_PROGRAM=$ninja",
            "-DFLEX_EXECUTABLE=$flex", "-DBISON_EXECUTABLE=$bison",
            "-DPython3_EXECUTABLE=$python", "-DMYB_BUILD_INTERNAL_TESTS=$internalTests")
        Invoke-CiCommand $cmake @('--build', $buildDirectory, '--parallel')

        $discoverAndRun = @'
import sys, unittest
suite = unittest.defaultTestLoader.discover(sys.argv[1])
count = suite.countTestCases()
print(f'Discovered {count} tests in {sys.argv[1]}', flush=True)
if count == 0:
    sys.exit(2)
result = unittest.TextTestRunner(verbosity=2).run(suite)
sys.exit(0 if result.wasSuccessful() and result.testsRun > 0 else 1)
'@
        if ($Configuration -eq 'Debug') {
            Invoke-CiCommand (Join-Path $buildDirectory 'myb_internal_tests.exe') @()
            Invoke-CiCommand $python @('-c', $discoverAndRun, 'tests/driver')
        } else {
            $verifyRelease = @'
import json, pathlib, sys
build = pathlib.Path(sys.argv[1])
commands = json.loads((build / 'compile_commands.json').read_text(encoding='utf-8'))
bad = [entry['file'] for entry in commands if 'MYB_TEST_HOOKS' in entry['command'] or '/tests/' in (entry['command'] + ' ' + entry['file']).replace('\\', '/')]
if not commands or bad or (build / 'myb_internal_tests.exe').exists():
    sys.exit(f'Release has test dependencies or no compile commands: {bad}')
print(f'Release isolation verified: {len(commands)} compile commands, no test hooks/dependencies')
'@
            Invoke-CiCommand $python @('-c', $verifyRelease, $buildDirectory)
        }
        $env:MYB_INTERPRETER = Join-Path $buildDirectory 'myb.exe'
        Invoke-CiCommand $python @('-c', $discoverAndRun, 'tests/cli')
        Invoke-CiCommand $python @('tools/run_blackbox.py', '--interpreter', $env:MYB_INTERPRETER)
        if ($Configuration -eq 'Release') {
            # 每次保存独立结果，重跑 CI 不覆盖先前的性能证据。
            $performanceRun = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfff') + '-' +
                [Guid]::NewGuid().ToString('N')
            $performanceDirectory = Join-Path $logDirectory ('performance/' + $performanceRun)
            Invoke-CiCommand $python @('tools/check_performance.py',
                '--interpreter', $env:MYB_INTERPRETER, '--output', $performanceDirectory)
        }
    } finally {
        Pop-Location
    }
    Write-Output "MYB CI $Configuration passed; log: $script:logPath"
} catch {
    if ($script:failureExit -eq 0) { $script:failureExit = 1 }
    $message = $_.ToString()
    [Console]::Error.WriteLine($message)
    if ($script:logPath) { Add-Content -LiteralPath $script:logPath -Value $message -Encoding UTF8 }
} finally {
    $env:PATH = $originalPath
    $env:MYB_INTERPRETER = $originalInterpreter
}
exit $script:failureExit
