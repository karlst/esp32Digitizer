# Copyright (C) 2026 Karl Stock
# SPDX-License-Identifier: GPL-3.0-or-later
# Set P4 UTC and select/run an SD test. No build or upload is performed.
param(
    [string]$Python = 'python',
    [string]$P4 = 'COM3',
    [ValidateSet('fresh', 'addFiles', 'existing')]
    [string]$Test,
    [int]$Timeout = 900
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
Push-Location $repoRoot
try {
    $runnerArguments = @('espP4/test/runSdChoke.py', '--p4', $P4, '--timeout', $Timeout)
    if ($Test)
    {
        $runnerArguments += @('--test', $Test)
    }
    & $Python @runnerArguments
    if ($LASTEXITCODE -ne 0)
    {
        exit $LASTEXITCODE
    }
}
finally { Pop-Location }
