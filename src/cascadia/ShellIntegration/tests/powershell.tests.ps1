# Copyright (c) Microsoft Corporation.
# Licensed under the MIT license.

param(
    [Parameter(Mandatory = $true)]
    [string] $IntegrationScript
)

$ErrorActionPreference = 'Stop'

function Assert-True([bool] $Condition, [string] $Message) {
    if (-not $Condition) {
        throw $Message
    }
}

function Global:Prompt {
    $Global:ObservedPromptSuccess = $global:?
    $Global:ObservedPromptLastExitCode = $global:LASTEXITCODE
    'ORIGINAL>'
}

. $IntegrationScript

$global:LASTEXITCODE = 17
Write-Error 'expected status probe' -ErrorAction SilentlyContinue
$filesystemPrompt = Prompt
Assert-True ($filesystemPrompt.Contains("$([char] 0x1b)]633;P;Cwd=")) 'Filesystem prompt did not report Cwd.'
Assert-True ($filesystemPrompt.Contains("$([char] 0x1b)]633;A`a")) 'Prompt-start phase was not reported.'
Assert-True ($filesystemPrompt.Contains("$([char] 0x1b)]633;B`a")) 'Prompt-end phase was not reported.'
Assert-True ($filesystemPrompt.Contains('ORIGINAL>')) 'Original prompt output was not preserved.'
Assert-True (-not $Global:ObservedPromptSuccess) 'The original prompt did not observe the previous failure status.'
Assert-True ($Global:ObservedPromptLastExitCode -eq 17) 'The original prompt did not observe LASTEXITCODE.'

Push-Location Env:
try {
    $providerPrompt = Prompt
    Assert-True ($providerPrompt.Contains("$([char] 0x1b)]633;P;Cwd=`a")) 'Non-filesystem location did not clear Cwd.'
}
finally {
    Pop-Location
}

'PowerShell shell integration checks passed.'
