param(
    [string]$LuauSourceDir = "",
    [switch]$OnlyC,
    [switch]$Graphics,
    [string]$RaylibSourceDir = ""
)
$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent $MyInvocation.MyCommand.Path
$args = @('-S', $project, '-B', "$project/build", '-G', 'Visual Studio 17 2022', '-A', 'x64')
if ($OnlyC -and $Graphics) { throw "Graphics requires Luau. Do not combine -OnlyC and -Graphics." }
if ($OnlyC) { $args += '-DRV_WITH_LUAU=OFF' } else { $args += '-DRV_WITH_LUAU=ON' }
if ($Graphics) { $args += '-DRV_WITH_GRAPHICS=ON' } else { $args += '-DRV_WITH_GRAPHICS=OFF' }
if ($RaylibSourceDir) { $args += "-DRAYLIB_SOURCE_DIR=$RaylibSourceDir" }
if ($LuauSourceDir) { $args += "-DLUAU_SOURCE_DIR=$LuauSourceDir" }
& cmake @args
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& cmake --build "$project/build" --config Release --parallel
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& ctest --test-dir "$project/build" -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host "Build/test OK"
