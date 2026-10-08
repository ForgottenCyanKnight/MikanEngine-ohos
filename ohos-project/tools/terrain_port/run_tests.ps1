param([Parameter(Mandatory=$true)][string]$Python)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$out = Join-Path $root '.codex-runtime/heightmap-validation/host-tests'
New-Item -ItemType Directory -Force $out | Out-Null
& $Python (Join-Path $PSScriptRoot 'generate_fixture.py') --output (Join-Path $out 'fixtures')
if ($LASTEXITCODE -ne 0) { throw 'Fixture generation failed' }
Copy-Item (Join-Path $root 'ohos-project/entry/src/main/cpp/application/scene/default_main.json') (Join-Path $out 'fixtures/scenes/default_main.json')
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -property installationPath
if (-not $vs) { throw 'Visual Studio C++ tools are required for host tests' }
$vcvars = Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat'
$app = Join-Path $root 'ohos-project/entry/src/main/cpp/application'
$test = Join-Path $PSScriptRoot 'terrain_tests.cpp'
$exe = Join-Path $out 'terrain_tests.exe'
$command = 'call "{0}" >nul && cl /nologo /std:c++17 /EHsc /W4 /I"{1}" /I"{2}" /I"{7}" "{3}" "{4}" "{5}" /Fe:"{6}"' -f $vcvars,$app,(Join-Path $root 'src/video'),$test,(Join-Path $app 'terrain/heightmap_terrain.cpp'),(Join-Path $app 'scene/scene_definition.cpp'),$exe,(Join-Path $root 'include')
Push-Location $out
try {
    & cmd.exe /d /c $command
    if ($LASTEXITCODE -ne 0) { throw 'Host terrain test compilation failed' }
    & $exe (Join-Path $out 'fixtures')
    if ($LASTEXITCODE -ne 0) { throw 'Terrain tests failed' }
} finally { Pop-Location }
