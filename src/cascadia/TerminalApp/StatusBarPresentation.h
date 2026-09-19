// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "GitStatus.h"

#include <string>

namespace TerminalApp::StatusBarPresentation
{
    inline constexpr double MinimumRowHeight{ 26.0 };

    struct Strings
    {
        std::wstring summaryPrefix;
        std::wstring summarySeparator;
        std::wstring namedBranch;
        std::wstring unbornBranch;
        std::wstring unbornBranchCompact;
        std::wstring detachedHead;
        std::wstring detachedHeadUnknown;
        std::wstring detachedHeadCompact;
        std::wstring detachedHeadCompactUnknown;
        std::wstring unknownBranch;
        std::wstring windowsEnvironment;
        std::wstring wslEnvironment;
        std::wstring unsupportedEnvironment;
        std::wstring repository;
        std::wstring tracking;
        std::wstring aheadOne;
        std::wstring aheadMany;
        std::wstring behindOne;
        std::wstring behindMany;
        std::wstring clean;
        std::wstring staged;
        std::wstring unstaged;
        std::wstring untracked;
        std::wstring conflicted;
        std::wstring linesAddedCompact;
        std::wstring linesDeletedCompact;
        std::wstring lineAdded;
        std::wstring linesAdded;
        std::wstring lineDeleted;
        std::wstring linesDeleted;
        std::wstring lineAddedUnborn;
        std::wstring linesAddedUnborn;
        std::wstring lineDeletedUnborn;
        std::wstring linesDeletedUnborn;
        std::wstring binaryChanges;
        std::wstring conflictLinesUnavailable;
        std::wstring linesUnavailable;
    };

    struct View
    {
        enum class IdentityIcon
        {
            Branch,
            Detached,
        };

        struct Item
        {
            bool visible{ false };
            std::wstring text;
            std::wstring tooltip;
        };

        bool hasGitContent{ false };
        IdentityIcon identityIcon{ IdentityIcon::Branch };
        std::wstring branchText;
        Item ahead;
        Item behind;
        Item added;
        Item deleted;
        Item staged;
        Item unstaged;
        Item untracked;
        Item conflicted;
        bool clean{ false };
        std::wstring accessibleSummary;
    };

    [[nodiscard]] std::wstring CompactCount(uint32_t count);
    [[nodiscard]] std::wstring CompactLineCount(uint64_t count);
    [[nodiscard]] View Build(const ::Microsoft::Terminal::StatusBar::StatusBarSnapshot& snapshot, const Strings& strings);
}
