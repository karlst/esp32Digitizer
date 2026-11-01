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

# Select and run one real 30-, 300-, or 600-second board test. This starts acquisition and creates an SD
# recording; it does not build/upload firmware. Close monitors and reset boards.
param(
    [string]$Python = 'python',
    [string]$Feather = 'COM4',
    [string]$P4 = 'COM3',
    [ValidateSet(0, 30, 300, 600)][int]$Seconds = 0,
    [switch]$LinkOnly,
    [switch]$External,
    [switch]$Noise,
    [ValidateSet(0.5, 1000)][double]$Frequency = 0.5
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
Push-Location $repoRoot
try {
    if ($Frequency -eq 1000 -and !$External)
    {
        throw 'Frequency 1000 requires -External.'
    }
    $durationArguments = @('--frequency', $Frequency.ToString([System.Globalization.CultureInfo]::InvariantCulture))
    if ($Seconds -ne 0)
    {
        $durationArguments += @('--seconds', $Seconds)
    }
    if ($LinkOnly)
    {
        $durationArguments += '--link-only'
    }
    if ($External)
    {
        $durationArguments += '--external'
    }
    if ($Noise)
    {
        $durationArguments += '--noise'
    }
    & $Python 'espP4/test/runHardwareSmoke.py' --feather $Feather --p4 $P4 @durationArguments
    # Python already printed the short failure reason. Preserve its exit status
    # without adding a PowerShell exception and stack-location report.
    if ($LASTEXITCODE -ne 0)
    {
        exit $LASTEXITCODE
    }
}
finally { Pop-Location }
