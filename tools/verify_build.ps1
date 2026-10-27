#Requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string]$Msys2Root,
    [string]$OutputDirectory = 'build/m4-build',
    [ValidateSet('all', 'clean-debug', 'incremental', 'release', 'path-source', 'path-build')]
    [string[]]$Cases = @('all'),
    [ValidateRange(1, 32)] [int]$Parallel = 4
)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$projectRoot = Split-Path -Parent $PSScriptRoot
$outputRoot = [IO.Path]::GetFullPath((Join-Path $projectRoot $OutputDirectory))
$buildRoot = [IO.Path]::GetFullPath((Join-Path $projectRoot 'build'))
if (-not $outputRoot.StartsWith($buildRoot + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw "验收输出必须位于 build 的子目录：$outputRoot"
}
$savedEnvironment = @{}
foreach ($name in @('PATH', 'TEMP', 'TMP', 'MYB_INTERPRETER')) {
    $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name)
}
$results = [Collections.Generic.List[object]]::new()
$script:caseLog = $null

function Invoke-VerifyCommand {
    param([string]$Executable, [string[]]$Arguments)
    $command = '> ' + $Executable + ' ' + ($Arguments -join ' ')
    $command | Tee-Object -FilePath $script:caseLog -Append
    & $Executable @Arguments 2>&1 | Tee-Object -FilePath $script:caseLog -Append
    if ($LASTEXITCODE -ne 0) { throw "命令退出 $LASTEXITCODE：$Executable" }
}

function Find-VerifyTool {
    param([string]$Name, [string]$Directory, [string]$LocalDirectory)
    foreach ($candidate in @((Join-Path $Directory "$Name.exe"), $LocalDirectory)) {
        if ($candidate -and (Test-Path -LiteralPath $candidate -PathType Leaf)) { return $candidate }
    }
    $tool = Get-Command "$Name.exe" -CommandType Application -ErrorAction Stop | Select-Object -First 1
    return $tool.Source
}

function Reset-VerifyDirectory {
    param([string]$Directory)
    $expected = [IO.Path]::GetFullPath($Directory)
    if (-not $expected.StartsWith($outputRoot + [IO.Path]::DirectorySeparatorChar,
            [StringComparison]::OrdinalIgnoreCase)) { throw "拒绝清理输出目录之外的路径：$expected" }
    $cursor = $expected
    while ($cursor -and $cursor.StartsWith($projectRoot, [StringComparison]::OrdinalIgnoreCase)) {
        if (Test-Path -LiteralPath $cursor) {
            $item = Get-Item -LiteralPath $cursor -Force
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "路径包含链接：$cursor" }
        }
        $cursor = Split-Path -Parent $cursor
    }
    if (Test-Path -LiteralPath $expected) {
        $resolved = (Resolve-Path -LiteralPath $expected).Path
        if ($resolved -ne $expected) { throw "路径解析不一致：$resolved" }
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
    New-Item -ItemType Directory -Path $expected -Force | Out-Null
}

function Copy-VerifySource {
    param([string]$Destination)
    Reset-VerifyDirectory $Destination
    $files = & git -C $projectRoot -c core.quotepath=false ls-files --cached --others --exclude-standard
    if ($LASTEXITCODE -ne 0 -or -not $files) { throw '无法枚举源码文件' }
    foreach ($relative in $files) {
        if ($relative -match '^(build|\.git)/') { continue }
        $source = Join-Path $projectRoot $relative
        if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { continue }
        $destinationFile = Join-Path $Destination $relative
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destinationFile) | Out-Null
        Copy-Item -LiteralPath $source -Destination $destinationFile
    }
}

function Configure-VerifyBuild {
    param([string]$Source, [string]$Build, [string]$Configuration, [string]$Internal)
    Invoke-VerifyCommand $cmake @('-S', $Source, '-B', $Build, '-G', 'Ninja',
        "-DCMAKE_BUILD_TYPE=$Configuration", "-DCMAKE_C_COMPILER=$gcc", "-DCMAKE_CXX_COMPILER=$gxx",
        "-DCMAKE_MAKE_PROGRAM=$ninja", "-DFLEX_EXECUTABLE=$flex", "-DBISON_EXECUTABLE=$bison",
        "-DPython3_EXECUTABLE=$python", "-DMYB_BUILD_INTERNAL_TESTS=$Internal")
    Invoke-VerifyCommand $cmake @('--build', $Build, '--parallel', "$Parallel")
}

function Test-VerifyBuild {
    param([string]$Source, [string]$Build, [bool]$Internal)
    $discover = @'
import sys, unittest
suite = unittest.defaultTestLoader.discover(sys.argv[1])
count = suite.countTestCases()
print(f'Discovered {count} tests in {sys.argv[1]}', flush=True)
if count == 0:
    sys.exit(2)
result = unittest.TextTestRunner(verbosity=2).run(suite)
sys.exit(0 if result.wasSuccessful() and result.testsRun > 0 else 1)
'@
    $env:MYB_INTERPRETER = Join-Path $Build 'myb.exe'
    if ($Internal) {
        Invoke-VerifyCommand (Join-Path $Build 'myb_internal_tests.exe') @()
        Invoke-VerifyCommand $python @('-c', $discover, (Join-Path $Source 'tests/driver'))
    }
    Invoke-VerifyCommand $python @('-c', $discover, (Join-Path $Source 'tests/cli'))
    Invoke-VerifyCommand $python @((Join-Path $Source 'tools/run_blackbox.py'), '--interpreter', $env:MYB_INTERPRETER)
}

try {
    New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null
    $temporary = Join-Path $outputRoot 'temp'
    New-Item -ItemType Directory -Force -Path $temporary | Out-Null
    $env:TEMP = $temporary
    $env:TMP = $temporary
    $msysRoot = (Resolve-Path -LiteralPath $Msys2Root).Path
    $ucrt = Join-Path $msysRoot 'ucrt64/bin'
    $usr = Join-Path $msysRoot 'usr/bin'
    $env:PATH = "$ucrt;$usr;" + $savedEnvironment.PATH
    $gcc = Find-VerifyTool 'gcc' $ucrt ''
    $gxx = Find-VerifyTool 'g++' $ucrt ''
    $python = Find-VerifyTool 'python' $ucrt ''
    $cmake = Find-VerifyTool 'cmake' $ucrt (Join-Path $projectRoot 'build/hosttools/cmake/data/bin/cmake.exe')
    $ninja = Find-VerifyTool 'ninja' $ucrt (Join-Path $projectRoot 'build/hosttools/bin/ninja.exe')
    $flex = Find-VerifyTool 'flex' $usr ''
    $bison = Find-VerifyTool 'bison' $usr ''
    $script:caseLog = Join-Path $outputRoot 'environment.log'
    Set-Content -LiteralPath $script:caseLog -Value "MYB 构建验收 $(Get-Date -Format o)" -Encoding utf8
    Invoke-VerifyCommand 'git' @('-C', $projectRoot, 'rev-parse', 'HEAD')
    foreach ($tool in @($gcc, $gxx, $python, $cmake, $ninja, $flex, $bison)) {
        Invoke-VerifyCommand $tool @('--version')
    }
    $sourceCopy = Join-Path $outputRoot 'source'
    $sourcePrepared = $false
    $debugBuild = Join-Path $outputRoot 'clean-debug'
    $selected = if ($Cases -contains 'all') {
        @('clean-debug', 'incremental', 'release', 'path-source', 'path-build')
    } else { $Cases }
    foreach ($case in $selected) {
        $script:caseLog = Join-Path $outputRoot "$case.log"
        Set-Content -LiteralPath $script:caseLog -Value "MYB 构建验收 $case" -Encoding utf8
        try {
            switch ($case) {
                'clean-debug' {
                    Copy-VerifySource $sourceCopy
                    $sourcePrepared = $true
                    Reset-VerifyDirectory $debugBuild
                    Configure-VerifyBuild $sourceCopy $debugBuild 'Debug' 'ON'
                    Test-VerifyBuild $sourceCopy $debugBuild $true
                }
                'incremental' {
                    if (-not (Test-Path -LiteralPath (Join-Path $debugBuild 'myb.exe'))) {
                        throw '先执行 clean-debug，再执行 incremental'
                    }
                    $checks = @(
                        @{ source = 'src/frontend/lexer.l'; outputs = @('generated/lexer.cpp') },
                        @{ source = 'src/frontend/parser.y'; outputs = @('generated/parser.cpp', 'generated/parser.hpp') },
                        @{ source = 'src/frontend/scanner.hpp'; outputs = @('CMakeFiles/myb_generated.dir/generated/lexer.cpp.obj') },
                        @{ source = 'src/frontend/driver.hpp'; outputs = @('CMakeFiles/myb_generated.dir/generated/parser.cpp.obj') }
                    )
                    foreach ($check in $checks) {
                        $inputFile = Join-Path $sourceCopy $check.source
                        $originalTime = (Get-Item -LiteralPath $inputFile).LastWriteTimeUtc
                        $originalBytes = [IO.File]::ReadAllBytes($inputFile)
                        $before = @{}
                        foreach ($relative in $check.outputs) {
                            $before[$relative] = (Get-Item -LiteralPath (Join-Path $debugBuild $relative)).LastWriteTimeUtc
                        }
                        try {
                            # 只改变副本的时间，保持文法和接口内容一致。
                            (Get-Item -LiteralPath $inputFile).LastWriteTimeUtc = [DateTime]::UtcNow
                            Invoke-VerifyCommand $cmake @('--build', $debugBuild, '--parallel', "$Parallel", '--verbose')
                            foreach ($relative in $check.outputs) {
                                $after = (Get-Item -LiteralPath (Join-Path $debugBuild $relative)).LastWriteTimeUtc
                                if ($after -le $before[$relative]) { throw "未更新预期产物：$relative" }
                            }
                            if ([Convert]::ToBase64String([IO.File]::ReadAllBytes($inputFile)) -ne
                                [Convert]::ToBase64String($originalBytes)) { throw "副本内容发生变化：$inputFile" }
                            "PASS $($check.source) -> $($check.outputs -join ', ')" |
                                Tee-Object -FilePath $script:caseLog -Append
                        } finally {
                            (Get-Item -LiteralPath $inputFile).LastWriteTimeUtc = $originalTime
                        }
                    }
                    Invoke-VerifyCommand $cmake @('--build', $debugBuild, '--parallel', "$Parallel", '--verbose')
                    Test-VerifyBuild $sourceCopy $debugBuild $true
                }
                'release' {
                    if (-not $sourcePrepared) {
                        Copy-VerifySource $sourceCopy
                        $sourcePrepared = $true
                    }
                    $releaseBuild = Join-Path $outputRoot 'release'
                    Reset-VerifyDirectory $releaseBuild
                    Configure-VerifyBuild $sourceCopy $releaseBuild 'Release' 'OFF'
                    $verifyRelease = @'
import json, pathlib, sys
build = pathlib.Path(sys.argv[1])
commands = json.loads((build / 'compile_commands.json').read_text(encoding='utf-8'))
bad = [entry['file'] for entry in commands if 'MYB_TEST_HOOKS' in entry['command'] or '/tests/' in (entry['command'] + ' ' + entry['file']).replace('\\', '/')]
if not commands or bad or (build / 'myb_internal_tests.exe').exists():
    sys.exit(f'Release contains test dependencies or no compile commands: {bad}')
print(f'Release isolation: {len(commands)} compile commands, no test hooks/dependencies')
'@
                    Invoke-VerifyCommand $python @('-c', $verifyRelease, $releaseBuild)
                    foreach ($binary in @('myb.exe', 'libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll')) {
                        $imports = & (Join-Path $ucrt 'objdump.exe') -p (Join-Path $releaseBuild $binary) 2>&1
                        if ($LASTEXITCODE -ne 0) { throw "无法读取导入表：$binary" }
                        $libraries = @($imports | Select-String -Pattern '^\s*DLL Name:' | ForEach-Object { $_.Line.Trim() })
                        "$binary : $($libraries -join ', ')" | Tee-Object -FilePath $script:caseLog -Append
                        if (-not $libraries -or ($libraries -match 'msys-2\.0\.dll')) { throw "运行库导入不符合 UCRT64 要求：$binary" }
                    }
                    Test-VerifyBuild $sourceCopy $releaseBuild $false
                }
                'path-source' {
                    $unicodeSource = Join-Path $outputRoot '中文 源码/source'
                    Copy-VerifySource $unicodeSource
                    $pathBuild = Join-Path $outputRoot 'path-source-build'
                    Reset-VerifyDirectory $pathBuild
                    Configure-VerifyBuild $unicodeSource $pathBuild 'Debug' 'ON'
                    Test-VerifyBuild $unicodeSource $pathBuild $true
                }
                'path-build' {
                    if (-not $sourcePrepared) {
                        Copy-VerifySource $sourceCopy
                        $sourcePrepared = $true
                    }
                    $pathBuild = Join-Path $outputRoot '中文 构建/build'
                    Reset-VerifyDirectory $pathBuild
                    Configure-VerifyBuild $sourceCopy $pathBuild 'Debug' 'ON'
                    Test-VerifyBuild $sourceCopy $pathBuild $true
                }
            }
            $results.Add(@{ name = $case; result = 'PASS'; log = $script:caseLog })
        } catch {
            $message = $_.ToString()
            Add-Content -LiteralPath $script:caseLog -Value $message -Encoding utf8
            $results.Add(@{ name = $case; result = 'FAIL'; error = $message; log = $script:caseLog })
            Write-Warning "$case : $message"
        }
    }
    $resultName = if ($Cases -contains 'all') { 'results.json' } else { 'results-' + ($selected -join '-') + '.json' }
    $results | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $outputRoot $resultName) -Encoding utf8
    $results | ForEach-Object { Write-Output "$($_.name): $($_.result) ($($_.log))" }
} finally {
    foreach ($name in $savedEnvironment.Keys) {
        [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name])
    }
}
if (@($results | Where-Object result -eq 'FAIL').Count -gt 0) { exit 1 }
exit 0
