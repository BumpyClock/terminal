# Copyright (c) Microsoft Corporation.
# Licensed under the MIT license.

if (Test-Path variable:global:__WindowsTerminalShellIntegration) {
    return
}

if ($ExecutionContext.SessionState.LanguageMode -ne 'FullLanguage') {
    return
}

$Global:__WindowsTerminalShellIntegration = @{
    OriginalPrompt = $function:Prompt
    OriginalReadLine = $null
    InExecution = $false
    LastHistoryId = -1
    Escape = [char] 0x1b
}

function Global:__WindowsTerminalEscapeValue([string] $Value) {
    $builder = [System.Text.StringBuilder]::new()
    foreach ($character in $Value.ToCharArray()) {
        $code = [int] $character
        if ($code -le 0x1f -or $character -eq '\' -or $character -eq ';') {
            foreach ($byte in [System.Text.Encoding]::UTF8.GetBytes([string] $character)) {
                [void] $builder.AppendFormat('\x{0:x2}', $byte)
            }
        }
        else {
            [void] $builder.Append($character)
        }
    }
    $builder.ToString()
}

function Global:Prompt {
    $succeeded = $global:?
    $lastExitCode = $global:LASTEXITCODE
    $history = Get-History -Count 1 -ErrorAction SilentlyContinue
    $result = [System.Text.StringBuilder]::new()

    if ($Global:__WindowsTerminalShellIntegration.InExecution -or
        ($Global:__WindowsTerminalShellIntegration.LastHistoryId -ge 0 -and
         $null -ne $history -and
         $history.Id -ne $Global:__WindowsTerminalShellIntegration.LastHistoryId)) {
        [void] $result.Append("$($Global:__WindowsTerminalShellIntegration.Escape)]633;D`a")
        $Global:__WindowsTerminalShellIntegration.InExecution = $false
    }

    if ($pwd.Provider.Name -eq 'FileSystem') {
        [void] $result.Append("$($Global:__WindowsTerminalShellIntegration.Escape)]633;P;Cwd=$(__WindowsTerminalEscapeValue $pwd.ProviderPath)`a")
    }
    else {
        [void] $result.Append("$($Global:__WindowsTerminalShellIntegration.Escape)]633;P;Cwd=`a")
    }
    [void] $result.Append("$($Global:__WindowsTerminalShellIntegration.Escape)]633;A`a")

    if (-not $succeeded) {
        Write-Error 'restore previous command status' -ErrorAction Ignore
    }

    if ($null -ne $Global:__WindowsTerminalShellIntegration.OriginalPrompt) {
        [void] $result.Append(($Global:__WindowsTerminalShellIntegration.OriginalPrompt.Invoke() -join ''))
    }
    else {
        [void] $result.Append("PS $pwd> ")
    }
    [void] $result.Append("$($Global:__WindowsTerminalShellIntegration.Escape)]633;B`a")

    $Global:__WindowsTerminalShellIntegration.LastHistoryId = if ($null -eq $history) { -1 } else { $history.Id }
    $global:LASTEXITCODE = $lastExitCode
    $result.ToString()
}

if (Get-Module -Name PSReadLine) {
    $Global:__WindowsTerminalShellIntegration.OriginalReadLine = $function:PSConsoleHostReadLine
    function Global:PSConsoleHostReadLine {
        $lastExitCode = $global:LASTEXITCODE
        $commandLine = $Global:__WindowsTerminalShellIntegration.OriginalReadLine.Invoke()
        $Global:__WindowsTerminalShellIntegration.InExecution = $true
        [Console]::Write("$($Global:__WindowsTerminalShellIntegration.Escape)]633;C`a")
        $global:LASTEXITCODE = $lastExitCode
        $commandLine
    }
}
