# Builds the Switch executable on Windows with a native devkitPro install (no Docker): out/nx/masseffect-nx.nro.
# Same CMake configuration as tools/build_nro.sh (which runs the official devkitpro/devkita64 Docker image).
#
#   powershell -ExecutionPolicy Bypass -File tools\build_nro.ps1 [-MesaSdk <...\opt\devkitpro\portlibs\switch>] [-Jobs 8]
#
# Needs devkitPro (C:\devkitPro), CMake 3.25+, Ninja, glslangValidator, the generated code (tools/codegen.sh) and the
# patched NVK driver (mesa/README.md). CMake runs through Start-Process: in Windows PowerShell 5.1 every line a native
# program writes to stderr becomes an error that stops the script. The output goes to out/nx/configure.log, build.log
# and their .err files.
param(
    [string]$MesaSdk = 'E:/Dev/SHIP/x360sw/mesa-me/mesa-unified-install/opt/devkitpro/portlibs/switch',
    [int]$Jobs = 8,
    [string]$CMake = 'C:/Program Files/CMake/bin/cmake.exe',
    [string]$Ninja = 'C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe',
    [string]$Glslang = 'E:/Dev/SHIP/x360sw/sf4-nx/out/glslang/StandAlone/glslangValidator.exe',
    [string]$Version = '1.0.0',
    [string]$IndirectDispatch = '0'
)
$ErrorActionPreference = 'Stop'
$root = (Split-Path -Parent $PSScriptRoot).Replace('\', '/')
$out = "$root/out/nx"
$MesaSdk = $MesaSdk.Replace('\', '/')
if (-not $env:DEVKITPRO -or $env:DEVKITPRO -like '/*') { $env:DEVKITPRO = 'C:/devkitPro' }
$nm = "$env:DEVKITPRO/devkitA64/bin/aarch64-none-elf-nm.exe"

function Invoke-Tool([string]$exe, [string]$name, [string[]]$arguments) {
    $quoted = $arguments | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }
    $p = Start-Process -FilePath $exe -ArgumentList $quoted -NoNewWindow -Wait -PassThru `
        -RedirectStandardOutput "$out/$name.log" -RedirectStandardError "$out/$name.err"
    if ($p.ExitCode) { throw "$name failed with $($p.ExitCode), see $out/$name.log and $name.err" }
}

if (-not (Test-Path "$root/app/generated/rexglue.cmake")) { throw 'app/generated/rexglue.cmake not found: run tools/codegen.sh first' }
# An older driver links successfully while silently omitting the project NVK patches: check the archive itself.
New-Item -ItemType Directory -Force $out | Out-Null
Invoke-Tool $nm 'nvk_symbols' @('--defined-only', "$MesaSdk/lib/libvulkan.a")
if (-not (Select-String -Path "$out/nvk_symbols.log" -Pattern ' [BD] nvk_switch_draw$' -Quiet)) {
    throw "$MesaSdk/lib/libvulkan.a lacks the Mass Effect NVK patches (mesa/mesa-switch-masseffect.patch)"
}

if (-not (Test-Path "$out/CMakeCache.txt")) {
    Invoke-Tool $CMake 'configure' @('-S', "$root/app", '-B', $out, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
        "-DCMAKE_MAKE_PROGRAM=$Ninja", "-DCMAKE_TOOLCHAIN_FILE=$root/tools/switch/cmake/switch-devkitA64.cmake",
        "-DDEVKITPRO=$env:DEVKITPRO", "-DREXSDK_DIR=$root/sdk", "-DMASSEFFECT_VERSION=$Version",
        '-DMASSEFFECT_LTO=OFF', '-DMASSEFFECT_PGO=', '-DMASSEFFECT_GEN_OPT=', '-DMASSEFFECT_FAST_MATH=ON',
        "-DMASSEFFECT_INDIRECT_DISPATCH=$IndirectDispatch", '-DMASSEFFECT_INDIRECT_DISPATCH_VERIFY=OFF',
        "-DREXGLUE_SWITCH_NVK_SDK=$MesaSdk", '-DREXGLUE_UPSCALE_SHADERS=ON',
        "-DMASSEFFECT_GLSLANG_VALIDATOR=$Glslang")
}
$start = Get-Date
Invoke-Tool $CMake 'build' @('--build', $out, '-j', "$Jobs")
# crt0 must be first in .text or the NRO does not boot.
Invoke-Tool $nm 'elf_symbols' @("$out/masseffect")
if (-not (Select-String -Path "$out/elf_symbols.log" -Pattern '^0000000000000000 T _start$' -Quiet)) {
    throw '_start is not at address 0 in out/nx/masseffect: the NRO would not boot'
}
Copy-Item "$out/masseffect.nro" "$out/masseffect-nx.nro" -Force
Write-Output "Built $out/masseffect-nx.nro in $([int]((Get-Date) - $start).TotalMinutes) min (ELF with symbols: $out/masseffect)"
