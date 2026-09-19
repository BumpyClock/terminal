// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <cstdint>
#include <guiddef.h>
#include <string>

namespace Microsoft::Terminal::StatusBar
{
    enum class EnvironmentKind : uint32_t
    {
        Unsupported = 0,
        LocalWindows = 1,
        Wsl = 2,
    };

    struct ExecutionEnvironment
    {
        EnvironmentKind kind{ EnvironmentKind::Unsupported };
        std::wstring wslDistro;
        // Empty selects the distribution's default user.
        std::wstring wslUser;

        [[nodiscard]] bool IsEligible() const noexcept
        {
            return kind == EnvironmentKind::LocalWindows ||
                   (kind == EnvironmentKind::Wsl && !wslDistro.empty());
        }

        bool operator==(const ExecutionEnvironment&) const noexcept = default;
    };

    enum class ShellContextPathState : uint32_t
    {
        Unknown = 0,
        FileSystem = 1,
        NonFileSystem = 2,
    };

    enum class ShellContextPhase : uint32_t
    {
        Unknown = 0,
        Prompt = 1,
        Command = 2,
        Output = 3,
    };

    enum class ShellContextProvenance : uint32_t
    {
        Unknown = 0,
        ShellReported = 1,
    };

    // TerminalCore assigns the sequence and owns the reported path.
    struct ShellContextReport
    {
        ShellContextPathState pathState{ ShellContextPathState::Unknown };
        ShellContextPhase phase{ ShellContextPhase::Unknown };
        ShellContextProvenance provenance{ ShellContextProvenance::ShellReported };
        std::wstring path;
        uint64_t sequence{ 0 };
    };

    struct ShellContext
    {
        GUID connectionId{};
        ExecutionEnvironment environment;
        ShellContextPathState pathState{ ShellContextPathState::Unknown };
        ShellContextPhase phase{ ShellContextPhase::Unknown };
        ShellContextProvenance provenance{ ShellContextProvenance::Unknown };
        std::wstring path;
        uint64_t sequence{ 0 };
    };
}
