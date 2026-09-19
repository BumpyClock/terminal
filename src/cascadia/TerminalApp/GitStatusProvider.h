// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "GitStatus.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace TerminalApp
{
    struct GitStatusProviderOptions
    {
        // Empty paths use the installed Git for Windows and System32 wsl.exe.
        std::wstring windowsGitPath;
        std::wstring wslExecutablePath;
        std::wstring wslGitPath{ L"git" };
    };

    class GitStatusProvider final
    {
    public:
        GitStatusProvider();
        explicit GitStatusProvider(GitStatusProviderOptions options);

        void QueryAsync(
            Microsoft::Terminal::StatusBar::GitQueryRequest request,
            Microsoft::Terminal::StatusBar::GitQueryCompletion completion) const;

    private:
        GitStatusProviderOptions _options;
    };

    namespace GitStatus::details
    {
        std::optional<std::wstring> BuildCommandLine(
            const std::wstring& executable,
            const std::vector<std::wstring>& arguments);

        std::vector<std::wstring> BuildWslGitArguments(
            const Microsoft::Terminal::StatusBar::ExecutionEnvironment& environment,
            const std::wstring& git,
            std::vector<std::wstring> gitArguments);

        std::vector<std::wstring> BuildWslCleanupArguments(
            const Microsoft::Terminal::StatusBar::ExecutionEnvironment& environment,
            uint32_t processGroup,
            std::wstring_view signal);
    }
}
