# Copyright (C) 2026 Karl Stock
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of ESP32 Digitizer.
# You may redistribute and/or modify it under the GNU General Public License
# as published by the Free Software Foundation, either version 3 of the License,
# or (at your option) any later version.
#
# Distributed WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
# Public License in the repository LICENSE file or <https://www.gnu.org/licenses/>.

# Run existing P4 and Feather PC checks in order, stopping on the first failure.
# Paths come from this script, so it works from any current directory. Child
# PowerShell processes isolate runner variables and provide reliable exit codes.
# This compiles PC test programs only: no firmware build, upload or hardware I/O.
param(
    [string]$Python = 'python',
    [switch]$IncludeBrowser,
    [string]$Node = 'node'
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repoRoot = Split-Path $PSScriptRoot -Parent
$powerShellExe = (Get-Process -Id $PID).Path

# Keep test implementations in their board projects. Each entry names an existing
# runner or test suite; uiSmoke.cjs already delegates to controlsSmoke.cjs.
$checks = @(
    @{ Name = 'P4 recording control'; Kind = 'PowerShell'; Path = 'espP4/test/runRecordingControlChecks.ps1' },
    @{ Name = 'Feather ADC driver'; Kind = 'PowerShell'; Path = 'espFeather/test/runAdcDriverChecks.ps1' },
    @{ Name = 'P4 decoder, recording format and buffered writer'; Kind = 'PowerShell'; Path = 'espP4/test/runDecoderChecks.ps1' },
    @{ Name = 'P4 exact sample values'; Kind = 'PowerShell'; Path = 'espP4/test/runExactSampleChecks.ps1' },
    @{ Name = 'P4 RAM waveform'; Kind = 'PowerShell'; Path = 'espP4/test/runRamWaveformChecks.ps1' },
    @{ Name = 'P4 profiling arithmetic'; Kind = 'PowerShell'; Path = 'espP4/test/runProfileChecks.ps1' },
    @{ Name = 'Feather DAC arithmetic'; Kind = 'PowerShell'; Path = 'espFeather/test/runBenchDacChecks.ps1' },
    @{ Name = 'Feather status parsing'; Kind = 'PowerShell'; Path = 'espFeather/test/runMonitorChecks.ps1' },
    @{ Name = 'P4 recording-file reader'; Kind = 'Python'; Path = 'espP4/test/analogReaderChecks.py' },
    @{ Name = 'P4 waveform comparison'; Kind = 'Python'; Path = 'espP4/test/comparisonChecks.py' },
    @{ Name = 'Hardware smoke disk parser and timing'; Kind = 'PowerShell'; Path = 'espP4/test/runHardwareSmokeChecks.ps1' },
    @{ Name = 'Hardware smoke pass/fail rules'; Kind = 'Python'; Path = 'espP4/test/hardwareSmokeReportChecks.py' },
    @{ Name = 'P4 constant-input analysis'; Kind = 'Python'; Path = 'espP4/test/constantChecks.py' }
)
if ($IncludeBrowser) {
    $checks += @{ Name = 'Feather browser controls'; Kind = 'Node'; Path = 'espFeather/test/uiSmoke.cjs' }
}

$passed = 0
$currentCheck = 'Prerequisites'
Push-Location $repoRoot
try {
    # Check executables and script paths before spending time compiling. Individual
    # C++ runners diagnose missing Microsoft tools; the optional browser suite
    # diagnoses missing Playwright/Edge. Dependencies are never installed here.
    $pythonExe = (Get-Command $Python -CommandType Application -ErrorAction Stop).Source
    $nodeExe = ''
    if ($IncludeBrowser) {
        $nodeExe = (Get-Command $Node -CommandType Application -ErrorAction Stop).Source
    }
    foreach ($check in $checks) {
        if (!(Test-Path -LiteralPath (Join-Path $repoRoot $check.Path) -PathType Leaf)) {
            throw "Missing check: $($check.Path)"
        }
    }

    Write-Host "Running $($checks.Count) PC-only smoke checks. No boards required."
    foreach ($check in $checks) {
        $currentCheck = $check.Name
        $scriptPath = Join-Path $repoRoot $check.Path
        Write-Host "`n[$($passed + 1)/$($checks.Count)] $currentCheck"
        switch ($check.Kind) {
            'PowerShell' { & $powerShellExe -NoProfile -File $scriptPath }
            'Python' { & $pythonExe $scriptPath }
            'Node' { & $nodeExe $scriptPath }
        }
        # PowerShell does not automatically turn every native nonzero exit into
        # an exception. Check it explicitly before counting this suite as passed.
        if ($LASTEXITCODE -ne 0) {
            throw "Process exited with code $LASTEXITCODE."
        }
        $passed++
        Write-Host "PASS: $currentCheck"
    }
    Write-Host "`nSMOKE PASS: $passed/$($checks.Count) checks passed."
    if (!$IncludeBrowser) {
        Write-Host 'Browser checks not run; use -IncludeBrowser to include them.'
    }
    Write-Host 'Firmware builds and hardware behavior have not been checked by this run.'
}
catch {
    Write-Host "`nSMOKE FAILED: $currentCheck"
    Write-Host "$passed/$($checks.Count) checks passed before failure; later checks were not run."
    throw
}
finally { Pop-Location }
