// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "GitStatusParser.h"

#include <charconv>
#include <limits>

using namespace Microsoft::Terminal::StatusBar;

namespace
{
    std::optional<std::wstring> Utf8ToWide(const std::string_view value)
    {
        if (value.empty())
        {
            return std::wstring{};
        }
        if (value.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
        {
            return std::nullopt;
        }
        const auto length = MultiByteToWideChar(CP_UTF8,
                                                MB_ERR_INVALID_CHARS,
                                                value.data(),
                                                static_cast<int>(value.size()),
                                                nullptr,
                                                0);
        if (length == 0)
        {
            return std::nullopt;
        }
        std::wstring result(static_cast<size_t>(length), L'\0');
        if (MultiByteToWideChar(CP_UTF8,
                                MB_ERR_INVALID_CHARS,
                                value.data(),
                                static_cast<int>(value.size()),
                                result.data(),
                                length) != length)
        {
            return std::nullopt;
        }
        return result;
    }

    std::optional<uint32_t> Unsigned(const std::string_view value) noexcept
    {
        uint32_t result{};
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
        if (error != std::errc{} || end != value.data() + value.size())
        {
            return std::nullopt;
        }
        return result;
    }

    std::optional<uint64_t> Unsigned64(const std::string_view value) noexcept
    {
        uint64_t result{};
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
        if (error != std::errc{} || end != value.data() + value.size())
        {
            return std::nullopt;
        }
        return result;
    }

    bool AddChecked(uint64_t& total, const uint64_t value) noexcept
    {
        if (value > std::numeric_limits<uint64_t>::max() - total)
        {
            return false;
        }
        total += value;
        return true;
    }

    std::optional<std::string_view> RemainderAfterFields(const std::string_view record, const size_t fields) noexcept
    {
        size_t position{};
        for (size_t i = 0; i < fields; ++i)
        {
            position = record.find(' ', position);
            if (position == std::string_view::npos)
            {
                return std::nullopt;
            }
            ++position;
        }
        if (position >= record.size())
        {
            return std::nullopt;
        }
        return record.substr(position);
    }

    bool ValidXY(const std::string_view record) noexcept
    {
        constexpr std::string_view states{ ".MTADRCU" };
        return record.size() >= 4 &&
               record[1] == ' ' &&
               states.find(record[2]) != std::string_view::npos &&
               states.find(record[3]) != std::string_view::npos;
    }

    bool ValidObjectId(const std::string_view value) noexcept
    {
        if (value.size() != 40 && value.size() != 64)
        {
            return false;
        }
        return std::all_of(value.begin(), value.end(), [](const char ch) {
            return (ch >= '0' && ch <= '9') ||
                   (ch >= 'a' && ch <= 'f') ||
                   (ch >= 'A' && ch <= 'F');
        });
    }

    std::string LowerAscii(std::string_view value)
    {
        std::string result;
        result.reserve(value.size());
        for (const auto ch : value)
        {
            result.push_back(ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch);
        }
        return result;
    }

    constexpr std::string_view WslProcessGroupPrefix{ "WTGITPGID:" };
    constexpr std::string_view WslGitUnavailableRecord{ "WTGITERROR:GIT_UNAVAILABLE\n" };

    std::optional<uint32_t> FindProcessGroup(const std::string_view control) noexcept
    {
        size_t begin{};
        while ((begin = control.find(WslProcessGroupPrefix, begin)) != std::string_view::npos)
        {
            const auto digits = begin + WslProcessGroupPrefix.size();
            const auto end = control.find('\n', digits);
            if (end == std::string_view::npos)
            {
                return std::nullopt;
            }
            uint32_t processGroup{};
            const auto [parsed, error] = std::from_chars(control.data() + digits,
                                                         control.data() + end,
                                                         processGroup);
            if (error == std::errc{} && parsed == control.data() + end && processGroup != 0)
            {
                return processGroup;
            }
            begin = digits;
        }
        return std::nullopt;
    }
}

GitQueryResult TerminalApp::GitStatus::ParsePorcelainV2(const std::string_view output)
{
    GitQueryResult result;
    result.status = GitQueryStatus::MalformedOutput;

    if (output.empty() || output.back() != '\0')
    {
        return result;
    }

    GitSnapshot snapshot;

    bool sawOid{};
    bool sawHead{};
    bool sawUpstream{};
    bool sawAheadBehind{};
    std::wstring upstream;
    uint32_t ahead{};
    uint32_t behind{};

    size_t position{};
    while (position < output.size())
    {
        const auto terminator = output.find('\0', position);
        if (terminator == std::string_view::npos || terminator == position)
        {
            return result;
        }
        const auto record = output.substr(position, terminator - position);
        position = terminator + 1;

        if (record.starts_with("# branch.oid "))
        {
            if (sawOid)
            {
                return result;
            }
            sawOid = true;
            const auto oid = record.substr(13);
            if (oid == "(initial)")
            {
                snapshot.branchKind = GitBranchKind::Unborn;
                snapshot.commit.clear();
            }
            else
            {
                if (!ValidObjectId(oid))
                {
                    return result;
                }
                const auto converted = Utf8ToWide(oid);
                if (!converted || converted->empty())
                {
                    return result;
                }
                snapshot.commit = std::move(*converted);
            }
        }
        else if (record.starts_with("# branch.head "))
        {
            if (sawHead)
            {
                return result;
            }
            sawHead = true;
            const auto head = record.substr(14);
            if (head == "(detached)")
            {
                snapshot.branchKind = GitBranchKind::Detached;
                snapshot.branchName.clear();
            }
            else
            {
                const auto converted = Utf8ToWide(head);
                if (!converted || converted->empty())
                {
                    return result;
                }
                snapshot.branchName = std::move(*converted);
            }
        }
        else if (record.starts_with("# branch.upstream "))
        {
            if (sawUpstream)
            {
                return result;
            }
            sawUpstream = true;
            const auto converted = Utf8ToWide(record.substr(18));
            if (!converted || converted->empty())
            {
                return result;
            }
            upstream = std::move(*converted);
        }
        else if (record.starts_with("# branch.ab "))
        {
            if (sawAheadBehind)
            {
                return result;
            }
            sawAheadBehind = true;
            const auto counts = record.substr(12);
            const auto separator = counts.find(' ');
            if (separator == std::string_view::npos ||
                counts.empty() ||
                counts.front() != '+' ||
                separator + 2 > counts.size() ||
                counts[separator + 1] != '-')
            {
                return result;
            }
            const auto parsedAhead = Unsigned(counts.substr(1, separator - 1));
            const auto parsedBehind = Unsigned(counts.substr(separator + 2));
            if (!parsedAhead || !parsedBehind)
            {
                return result;
            }
            ahead = *parsedAhead;
            behind = *parsedBehind;
        }
        else if (record.starts_with("# "))
        {
            continue;
        }
        else if (record.front() == '1' || record.front() == '2')
        {
            const bool renamed = record.front() == '2';
            if (!ValidXY(record) || !RemainderAfterFields(record, renamed ? 9 : 8))
            {
                return result;
            }
            snapshot.changes.staged |= record[2] != '.';
            snapshot.changes.unstaged |= record[3] != '.';
            if (renamed)
            {
                const auto originalTerminator = output.find('\0', position);
                if (originalTerminator == std::string_view::npos || originalTerminator == position)
                {
                    return result;
                }
                position = originalTerminator + 1;
            }
        }
        else if (record.front() == 'u')
        {
            if (!ValidXY(record) || !RemainderAfterFields(record, 10))
            {
                return result;
            }
            snapshot.changes.conflicts = true;
        }
        else if (record.starts_with("? "))
        {
            if (record.size() == 2)
            {
                return result;
            }
            snapshot.changes.untracked = true;
        }
        else if (record.starts_with("! "))
        {
            if (record.size() == 2)
            {
                return result;
            }
        }
        else
        {
            return result;
        }
    }

    if (!sawOid || !sawHead || (sawAheadBehind && !sawUpstream))
    {
        return result;
    }
    if (snapshot.branchKind == GitBranchKind::Unborn)
    {
        if (!snapshot.commit.empty() || snapshot.branchName.empty())
        {
            return result;
        }
    }
    else if (snapshot.branchKind == GitBranchKind::Detached)
    {
        if (snapshot.commit.empty() || !snapshot.branchName.empty() || sawUpstream)
        {
            return result;
        }
    }
    else if (snapshot.commit.empty() || snapshot.branchName.empty())
    {
        return result;
    }

    if (sawUpstream && sawAheadBehind)
    {
        snapshot.tracking = GitTracking{ std::move(upstream), ahead, behind };
    }
    result.status = GitQueryStatus::Ready;
    result.snapshot = std::move(snapshot);
    return result;
}

GitQueryStatus TerminalApp::GitStatus::ClassifyGitFailure(
    const std::string_view diagnostic,
    const bool executableUnavailable) noexcept
{
    if (executableUnavailable)
    {
        return GitQueryStatus::GitUnavailable;
    }

    try
    {
        const auto text = LowerAscii(diagnostic);
        if (text.find("detected dubious ownership") != std::string::npos ||
            text.find("safe.directory") != std::string::npos)
        {
            return GitQueryStatus::OwnershipRefused;
        }
        if (text.find("not a git repository") != std::string::npos ||
            text.find("outside repository") != std::string::npos)
        {
            return GitQueryStatus::NoRepository;
        }
        if ((text.find("/usr/bin/git") != std::string::npos ||
             (text.find("/usr/bin/env:") != std::string::npos &&
              (text.find("'git'") != std::string::npos || text.find(" git:") != std::string::npos))) &&
            (text.find("no such file") != std::string::npos ||
             text.find("not found") != std::string::npos))
        {
            return GitQueryStatus::GitUnavailable;
        }
        if (text.find("there is no distribution with the supplied name") != std::string::npos ||
            text.find("wsl_e_distro_not_found") != std::string::npos ||
            text.find("windows subsystem for linux has no installed distributions") != std::string::npos ||
            text.find("windows subsystem for linux has not been installed") != std::string::npos)
        {
            return GitQueryStatus::UnsupportedEnvironment;
        }
    }
    catch (...)
    {
        return GitQueryStatus::Failed;
    }
    return GitQueryStatus::Failed;
}

std::optional<GitLineChanges> TerminalApp::GitStatus::ParseNumStat(const std::string_view output) noexcept
{
    GitLineChanges result;
    size_t position{};
    while (position < output.size())
    {
        const auto terminator = output.find('\0', position);
        if (terminator == std::string_view::npos || terminator == position)
        {
            return std::nullopt;
        }
        const auto record = output.substr(position, terminator - position);
        position = terminator + 1;

        const auto firstTab = record.find('\t');
        const auto secondTab = firstTab == std::string_view::npos ?
                                   std::string_view::npos :
                                   record.find('\t', firstTab + 1);
        if (firstTab == std::string_view::npos ||
            secondTab == std::string_view::npos ||
            firstTab == 0 ||
            secondTab == firstTab + 1)
        {
            return std::nullopt;
        }

        const auto addedField = record.substr(0, firstTab);
        const auto deletedField = record.substr(firstTab + 1, secondTab - firstTab - 1);
        if (addedField == "-" || deletedField == "-")
        {
            if (addedField != "-" || deletedField != "-")
            {
                return std::nullopt;
            }
            result.hasBinaryChanges = true;
        }
        else
        {
            const auto added = Unsigned64(addedField);
            const auto deleted = Unsigned64(deletedField);
            if (!added || !deleted ||
                !AddChecked(result.added, *added) ||
                !AddChecked(result.deleted, *deleted))
            {
                return std::nullopt;
            }
        }

        if (secondTab + 1 == record.size())
        {
            for (size_t i = 0; i < 2; ++i)
            {
                const auto pathTerminator = output.find('\0', position);
                if (pathTerminator == std::string_view::npos || pathTerminator == position)
                {
                    return std::nullopt;
                }
                position = pathTerminator + 1;
            }
        }
    }
    return result;
}

void TerminalApp::GitStatus::details::WslControlParser::Observe(const std::string_view chunk)
{
    constexpr size_t maximumSuffixBytes{ 64 };
    std::string control;
    control.reserve(_suffix.size() + chunk.size());
    control.append(_suffix);
    control.append(chunk);

    if (!_processGroup)
    {
        _processGroup = FindProcessGroup(control);
    }
    if (!_gitUnavailable && control.find(WslGitUnavailableRecord) != std::string::npos)
    {
        _gitUnavailable = true;
    }

    const auto retained = std::min(control.size(), maximumSuffixBytes);
    _suffix.assign(control.end() - retained, control.end());
}

std::optional<uint32_t> TerminalApp::GitStatus::details::WslControlParser::ProcessGroup() const noexcept
{
    return _processGroup;
}

bool TerminalApp::GitStatus::details::WslControlParser::GitUnavailable() const noexcept
{
    return _gitUnavailable;
}
