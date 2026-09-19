// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "StatusBarPresentation.h"

namespace
{
    template<typename... Args>
    std::wstring _format(const std::wstring& pattern, Args&&... args)
    {
        return fmt::format(fmt::runtime(pattern), std::forward<Args>(args)...);
    }

    void _appendSummaryPart(std::wstring& summary, const std::wstring& part, const std::wstring& separator)
    {
        if (part.empty())
        {
            return;
        }

        if (!summary.empty())
        {
            summary.append(separator);
        }
        summary.append(part);
    }

    std::wstring _branchName(const ::Microsoft::Terminal::StatusBar::GitSnapshot& snapshot,
                             const TerminalApp::StatusBarPresentation::Strings& strings)
    {
        return snapshot.branchName.empty() ? strings.unknownBranch : snapshot.branchName;
    }

    std::wstring _shortCommit(const ::Microsoft::Terminal::StatusBar::GitSnapshot& snapshot)
    {
        constexpr size_t shortCommitLength{ 7 };
        return snapshot.commit.substr(0, std::min(snapshot.commit.size(), shortCommitLength));
    }

    std::wstring _environmentDescription(const ::Microsoft::Terminal::StatusBar::GitSnapshot& snapshot,
                                         const TerminalApp::StatusBarPresentation::Strings& strings)
    {
        using ::Microsoft::Terminal::StatusBar::EnvironmentKind;

        switch (snapshot.environment.kind)
        {
        case EnvironmentKind::LocalWindows:
            return strings.windowsEnvironment;
        case EnvironmentKind::Wsl:
            return _format(strings.wslEnvironment, snapshot.environment.wslDistro);
        default:
            return strings.unsupportedEnvironment;
        }
    }
}

namespace TerminalApp::StatusBarPresentation
{
    std::wstring CompactCount(const uint32_t count)
    {
        return count > 999 ? L"999+" : std::to_wstring(count);
    }

    std::wstring CompactLineCount(const uint64_t count)
    {
        constexpr uint64_t maximumVisibleLineCount{ 9999 };
        return count > maximumVisibleLineCount ? L"9999+" : std::to_wstring(count);
    }

    View Build(const ::Microsoft::Terminal::StatusBar::StatusBarSnapshot& snapshot, const Strings& strings)
    {
        using ::Microsoft::Terminal::StatusBar::GitBranchKind;

        View result;
        if (!snapshot.git)
        {
            return result;
        }

        result.hasGitContent = true;
        const auto& git{ *snapshot.git };
        const auto branchName{ _branchName(git, strings) };

        std::wstring branchDescription;
        switch (git.branchKind)
        {
        case GitBranchKind::Unborn:
            result.branchText = _format(strings.unbornBranchCompact, branchName);
            branchDescription = _format(strings.unbornBranch, branchName);
            break;
        case GitBranchKind::Detached:
            result.identityIcon = View::IdentityIcon::Detached;
            if (git.commit.empty())
            {
                result.branchText = strings.detachedHeadCompactUnknown;
                branchDescription = strings.detachedHeadUnknown;
            }
            else
            {
                result.branchText = _format(strings.detachedHeadCompact, _shortCommit(git));
                branchDescription = _format(strings.detachedHead, git.commit);
            }
            break;
        default:
            result.branchText = branchName;
            branchDescription = _format(strings.namedBranch, branchName);
            break;
        }

        if (git.tracking)
        {
            result.ahead.visible = git.tracking->ahead != 0;
            result.ahead.text = CompactCount(git.tracking->ahead);
            result.ahead.tooltip = git.tracking->ahead == 1 ?
                                       strings.aheadOne :
                                       _format(strings.aheadMany, git.tracking->ahead);
            result.behind.visible = git.tracking->behind != 0;
            result.behind.text = CompactCount(git.tracking->behind);
            result.behind.tooltip = git.tracking->behind == 1 ?
                                        strings.behindOne :
                                        _format(strings.behindMany, git.tracking->behind);
        }

        result.staged = { git.changes.staged, {}, strings.staged };
        result.unstaged = { git.changes.unstaged, {}, strings.unstaged };
        result.untracked = { git.changes.untracked, {}, strings.untracked };
        result.conflicted = { git.changes.conflicts, {}, strings.conflicted };

        const auto conflictWins{ result.conflicted.visible };
        const auto hasBinaryChanges{ !conflictWins && git.lineChanges && git.lineChanges->hasBinaryChanges };
        const auto hasPositiveLineTotals{ !conflictWins &&
                                          git.lineChanges &&
                                          (git.lineChanges->added != 0 || git.lineChanges->deleted != 0) };
        result.clean = !result.staged.visible &&
                       !result.unstaged.visible &&
                       !result.untracked.visible &&
                       !result.conflicted.visible &&
                       !hasPositiveLineTotals &&
                       !hasBinaryChanges;

        const auto addedDescription = [&](const uint64_t count) {
            if (git.branchKind == GitBranchKind::Unborn)
            {
                return count == 1 ?
                           strings.lineAddedUnborn :
                           _format(strings.linesAddedUnborn, count);
            }
            return count == 1 ?
                       strings.lineAdded :
                       _format(strings.linesAdded, count);
        };
        const auto deletedDescription = [&](const uint64_t count) {
            if (git.branchKind == GitBranchKind::Unborn)
            {
                return count == 1 ?
                           strings.lineDeletedUnborn :
                           _format(strings.linesDeletedUnborn, count);
            }
            return count == 1 ?
                       strings.lineDeleted :
                       _format(strings.linesDeleted, count);
        };

        if (git.lineChanges && !conflictWins)
        {
            result.added.visible = git.lineChanges->added != 0;
            result.added.text = _format(strings.linesAddedCompact, CompactLineCount(git.lineChanges->added));
            result.added.tooltip = addedDescription(git.lineChanges->added);
            result.deleted.visible = git.lineChanges->deleted != 0;
            result.deleted.text = _format(strings.linesDeletedCompact, CompactLineCount(git.lineChanges->deleted));
            result.deleted.tooltip = deletedDescription(git.lineChanges->deleted);
        }

        auto& summary{ result.accessibleSummary };
        _appendSummaryPart(summary, strings.summaryPrefix, strings.summarySeparator);
        _appendSummaryPart(summary, branchDescription, strings.summarySeparator);
        _appendSummaryPart(summary, _environmentDescription(git, strings), strings.summarySeparator);
        if (!git.worktreeRoot.empty())
        {
            _appendSummaryPart(summary, _format(strings.repository, git.worktreeRoot), strings.summarySeparator);
        }
        if (git.tracking)
        {
            _appendSummaryPart(summary,
                               _format(strings.tracking,
                                       git.tracking->upstream,
                                       git.tracking->ahead,
                                       git.tracking->behind),
                               strings.summarySeparator);
        }
        const auto hasTrackedChanges{ result.staged.visible ||
                                      result.unstaged.visible ||
                                      result.conflicted.visible ||
                                      result.added.visible ||
                                      result.deleted.visible ||
                                      hasBinaryChanges };
        if (git.lineChanges && !conflictWins && hasTrackedChanges)
        {
            _appendSummaryPart(summary, addedDescription(git.lineChanges->added), strings.summarySeparator);
            _appendSummaryPart(summary, deletedDescription(git.lineChanges->deleted), strings.summarySeparator);
            if (git.lineChanges->hasBinaryChanges)
            {
                _appendSummaryPart(summary, strings.binaryChanges, strings.summarySeparator);
            }
        }
        else if (result.conflicted.visible)
        {
            _appendSummaryPart(summary, strings.conflictLinesUnavailable, strings.summarySeparator);
        }
        else if (result.staged.visible || result.unstaged.visible)
        {
            _appendSummaryPart(summary, strings.linesUnavailable, strings.summarySeparator);
        }
        if (result.clean)
        {
            _appendSummaryPart(summary, strings.clean, strings.summarySeparator);
        }
        if (result.conflicted.visible)
        {
            _appendSummaryPart(summary, strings.conflicted, strings.summarySeparator);
        }
        if (result.staged.visible)
        {
            _appendSummaryPart(summary, strings.staged, strings.summarySeparator);
        }
        if (result.unstaged.visible)
        {
            _appendSummaryPart(summary, strings.unstaged, strings.summarySeparator);
        }
        if (result.untracked.visible)
        {
            _appendSummaryPart(summary, strings.untracked, strings.summarySeparator);
        }

        if (hasBinaryChanges)
        {
            auto* binaryTooltip = result.conflicted.visible ? &result.conflicted.tooltip :
                                  result.staged.visible ? &result.staged.tooltip :
                                  result.unstaged.visible ? &result.unstaged.tooltip :
                                  nullptr;
            if (binaryTooltip)
            {
                _appendSummaryPart(*binaryTooltip, strings.binaryChanges, strings.summarySeparator);
            }
        }
        if (result.conflicted.visible)
        {
            _appendSummaryPart(result.conflicted.tooltip, strings.conflictLinesUnavailable, strings.summarySeparator);
        }

        return result;
    }
}
