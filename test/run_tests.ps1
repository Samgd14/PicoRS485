#!/usr/bin/env pwsh
<#
.SYNOPSIS
    Builds and runs the PicoRS485 host tests and static checks.

.DESCRIPTION
    The driver targets RP2040/RP2350, but most of its logic is deciding *what* to
    program rather than talking to silicon. The suite compiles the real
    src/PicoRS485.cpp against the SDK stub in test/stub, so validation, resource
    claiming, failure handling and pin/teardown state can be exercised on a
    desktop. Five checks run by default:

      1. the suite itself (exit code must be 0)
      2. the same suite built and run with PICO_PIO_USE_GPIO_BASE=0, so the
         RP2040 / RP2350A pin-mask branch is executed rather than only compiled
      3. src/PicoRS485.cpp compiles with PICO_PIO_USE_GPIO_BASE=0
      4. the % c-sdk blocks in src/rs485.pio are extracted and compiled at both
         pin-mask bases, so the helpers the suite stubs out are still type-checked
      5. the header's doxygen comments produce no warnings (skipped when doxygen
         is not installed)

.PARAMETER OnlySuite
    Run only check 1, for a faster loop.

.EXAMPLE
    pwsh test/run_tests.ps1
.EXAMPLE
    pwsh test/run_tests.ps1 -OnlySuite
#>
[CmdletBinding()]
param(
    [switch]$OnlySuite
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$testDir  = $PSScriptRoot
$repoDir  = Split-Path $testDir -Parent
$buildDir = Join-Path $testDir 'build'
$cppFlags = @('-std=c++17', '-Wall', '-Wextra')
# A second maximum burst length, so the suite can exercise two instantiations of one class.
$cppFlags += '-DPICO_RS485_EXTRA_SIZE=32'
$includes = @('-I', (Join-Path $repoDir 'include'), '-I', (Join-Path $testDir 'stub'))
$sources  = @(
    (Join-Path $testDir 'test_pico_rs485.cpp')
    (Join-Path $testDir 'suite_model.cpp')
    (Join-Path $testDir 'sections_lifecycle.cpp')
    (Join-Path $testDir 'sections_failure.cpp')
    (Join-Path $testDir 'sections_config.cpp')
    (Join-Path $testDir 'sections_transmit.cpp')
    (Join-Path $testDir 'sections_receive.cpp')
    (Join-Path $testDir 'sections_completion.cpp')
    (Join-Path $testDir 'sections_ownership.cpp')
    (Join-Path $testDir 'sections_invariants.cpp')
    (Join-Path $repoDir 'src/PicoRS485.cpp')
)

if (-not (Get-Command g++ -ErrorAction SilentlyContinue)) {
    throw 'g++ is not on PATH; install a C++17 compiler (MinGW-w64 or similar)'
}

$failures = 0
function Report {
    param([string]$Name, [bool]$Ok)
    if ($Ok) { Write-Host "  ok    $Name" } else { Write-Host "  FAIL  $Name"; $script:failures++ }
}

# Runs one suite binary under a bound, so a hang fails the run rather than blocking
# it. Process.Start is used because Start-Process -PassThru leaves ExitCode empty on
# this host; the fallback keeps a constrained shell working.
function Invoke-Suite {
    param([string]$Exe)
    try {
        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName = $Exe
        $psi.UseShellExecute = $false
        $proc = [System.Diagnostics.Process]::Start($psi)
        if ($proc.WaitForExit(120000)) { return ($proc.ExitCode -eq 0) }
        $proc.Kill()
        Write-Host '  FAIL  the suite did not finish within 120 s (hang)'
        return $false
    } catch {
        Write-Host "  note  plain run instead: $($_.Exception.Message)"
        & $Exe
        return ($LASTEXITCODE -eq 0)
    }
}

# The .pio helpers are C, but they are compiled as C++ here so the same flags and
# stub headers apply.
function New-PioHelperSource {
    param([string]$PioPath, [string]$OutPath)
    $blocks = [regex]::Matches((Get-Content $PioPath -Raw), '(?s)% c-sdk \{(.*?)%\}')
    if ($blocks.Count -eq 0) { return 0 }
    $prelude = @'
#include "hardware/pio.h"
#include "hardware/gpio.h"
#include "hardware/clocks.h"

void hw_write_masked(volatile uint32_t *addr, uint32_t values, uint32_t mask);
#define PIO_SM0_SHIFTCTRL_PUSH_THRESH_LSB  0u
#define PIO_SM0_SHIFTCTRL_PUSH_THRESH_BITS 0x1fu

// What pioasm emits alongside the c-sdk blocks.
static inline pio_sm_config rs485_tx_program_get_default_config(uint) { return pio_sm_config{}; }
static inline pio_sm_config rs485_txgap_program_get_default_config(uint) { return pio_sm_config{}; }
static inline pio_sm_config rs485_rx_program_get_default_config(uint) { return pio_sm_config{}; }
static inline pio_sm_config rs485_rxidle_program_get_default_config(uint) { return pio_sm_config{}; }
'@
    $body = ($blocks | ForEach-Object { $_.Groups[1].Value }) -join "`n"
    Set-Content -Path $OutPath -Value ($prelude + "`n" + $body) -Encoding utf8
    return $blocks.Count
}

New-Item -ItemType Directory -Force -Path $buildDir | Out-Null
Push-Location $testDir
try {
    Write-Host 'building the host suite'
    $exe = Join-Path $buildDir 'test_pico_rs485.exe'
    & g++ @cppFlags -O1 -o $exe @includes @sources
    if ($LASTEXITCODE -ne 0) { throw 'the host suite did not build' }

    Write-Host 'running the host suite'
    # A child that dies on an access violation would otherwise raise a Windows crash
    # dialog and wait for a click, which blocks a headless run. The error mode is
    # inherited by children, so set it before spawning.
    try {
        Add-Type -Namespace Native -Name ErrMode -MemberDefinition `
            '[System.Runtime.InteropServices.DllImport("kernel32.dll")] public static extern uint SetErrorMode(uint uMode);' `
            -ErrorAction Stop
        [void][Native.ErrMode]::SetErrorMode(0x0003)  # FAILCRITICALERRORS | NOGPFAULTERRORBOX
    } catch {
        Write-Host '  note  could not suppress crash dialogs in this shell'
    }
    # And bound it: a hang has to fail the run rather than block it.
    Report 'host suite' (Invoke-Suite $exe)

    if (-not $OnlySuite) {
        # The same suite for the other pin-mask configuration, so the RP2040 branch of
        # the pin handling is executed rather than only compiled.
        $exe0 = Join-Path $buildDir 'test_pico_rs485_base0.exe'
        & g++ @cppFlags '-DPICO_PIO_USE_GPIO_BASE=0' -O1 -o $exe0 @includes @sources
        if ($LASTEXITCODE -ne 0) {
            Report 'RP2040 base-0 suite' $false
        } else {
            Report 'RP2040 base-0 suite' (Invoke-Suite $exe0)
        }
        & g++ @cppFlags '-DPICO_PIO_USE_GPIO_BASE=0' -fsyntax-only @includes (Join-Path $repoDir 'src/PicoRS485.cpp')
        Report 'RP2040 / RP2350A build' ($LASTEXITCODE -eq 0)

        $generated = Join-Path $buildDir 'pio_helpers.cpp'
        $count = New-PioHelperSource -PioPath (Join-Path $repoDir 'src/rs485.pio') -OutPath $generated
        if ($count -eq 0) {
            Report 'pio helpers' $false
        } else {
            # Both pin-mask configurations, so the branch a GPIO base switches off
            # is really compiled rather than assumed.
            & g++ @cppFlags -fsyntax-only '-DPICO_PIO_USE_GPIO_BASE=1' '-I' (Join-Path $testDir 'stub') $generated
            $base1ok = ($LASTEXITCODE -eq 0)
            & g++ @cppFlags -fsyntax-only '-DPICO_PIO_USE_GPIO_BASE=0' '-I' (Join-Path $testDir 'stub') $generated
            $base0ok = ($LASTEXITCODE -eq 0)
            Report "pio helpers ($count blocks, both bases)" ($base1ok -and $base0ok)
        }

        if (Get-Command doxygen -ErrorAction SilentlyContinue) {
            # doxygen reports through stderr, and 2>&1 on a native command becomes a
            # terminating NativeCommandError under this script's
            # ErrorActionPreference='Stop'. That aborted the run at this very line,
            # before the verdict, so a documentation warning surfaced as nothing but a
            # bare exit code with four passing checks above it. Continue for the call.
            $eap = $ErrorActionPreference
            $ErrorActionPreference = 'Continue'
            $warnings = @(& doxygen 'Doxyfile' 2>&1)
            $ErrorActionPreference = $eap
            Report 'doxygen comments' ($warnings.Count -eq 0)
            foreach ($warning in $warnings) { Write-Host "        $warning" }
        } else {
            Write-Host '  skip  doxygen comments (doxygen is not installed)'
        }
    }
} finally {
    Pop-Location
}

if ($failures -ne 0) {
    Write-Host "$failures check(s) failed"
    exit 1
}
Write-Host 'all checks passed'
exit 0
