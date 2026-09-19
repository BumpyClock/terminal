// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "../inc/ShellContext.h"

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <stop_token>
#include <system_error>

namespace Microsoft::Terminal::StatusBar
{
    enum class GitBranchKind : uint32_t
    {
        Named = 0,
        Unborn = 1,
        Detached = 2,
    };

    struct GitTracking
    {
        std::wstring upstream;
        uint32_t ahead{ 0 };
        uint32_t behind{ 0 };
    };

    struct GitChanges
    {
        bool staged{ false };
        bool unstaged{ false };
        bool untracked{ false };
        bool conflicts{ false };
    };

    struct GitLineChanges
    {
        uint64_t added{ 0 };
        uint64_t deleted{ 0 };
        bool hasBinaryChanges{ false };
    };

    struct GitSnapshot
    {
        ExecutionEnvironment environment;
        std::wstring worktreeRoot;
        GitBranchKind branchKind{ GitBranchKind::Named };
        std::wstring branchName;
        std::wstring commit;
        std::optional<GitTracking> tracking;
        GitChanges changes;
        // Net tracked changes against HEAD (or the empty tree before the first commit).
        // Conflicts have no aggregate line totals; binary entries never contribute numbers.
        std::optional<GitLineChanges> lineChanges;
    };

    enum class GitQueryStatus : uint32_t
    {
        Ready = 0,
        NoRepository = 1,
        GitUnavailable = 2,
        UnsupportedEnvironment = 3,
        Cancelled = 4,
        TimedOut = 5,
        OutputLimitExceeded = 6,
        MalformedOutput = 7,
        OwnershipRefused = 8,
        Failed = 9,
    };

    struct GitQueryLimits
    {
        std::chrono::milliseconds deadline{ 5000 };
        size_t maximumOutputBytes{ 16 * 1024 * 1024 };
    };

    struct GitQueryRequest
    {
        ExecutionEnvironment environment;
        std::wstring directory;
        GitQueryLimits limits;
        std::stop_token cancellation;
    };

    struct GitQueryResult
    {
        GitQueryStatus status{ GitQueryStatus::Failed };
        std::optional<GitSnapshot> snapshot;
        std::error_code error;

        [[nodiscard]] bool IsValid() const noexcept
        {
            return (status == GitQueryStatus::Ready) == snapshot.has_value();
        }
    };

    using GitQueryCompletion = std::function<void(GitQueryResult)>;
    using GitQueryFunction = std::function<void(GitQueryRequest, GitQueryCompletion)>;

    // Empty Git content keeps the enabled row empty.
    struct StatusBarSnapshot
    {
        bool visible{ true };
        std::optional<GitSnapshot> git;
    };
}
