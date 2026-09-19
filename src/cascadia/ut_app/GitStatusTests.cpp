// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "precomp.h"
#include "../TerminalApp/GitStatusParser.h"
#include "../TerminalApp/GitStatusProvider.h"

#include <filesystem>
#include <future>
#include <shellapi.h>
#include <thread>

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::StatusBar;
using namespace TerminalApp;

namespace
{
    void AppendQuotedArgument(const std::wstring_view argument, std::wstring& commandLine)
    {
        commandLine.push_back(L'"');
        size_t backslashes{};
        for (const auto ch : argument)
        {
            if (ch == L'\\')
            {
                ++backslashes;
            }
            else
            {
                commandLine.append(ch == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
                commandLine.push_back(ch);
                backslashes = 0;
            }
        }
        commandLine.append(backslashes * 2, L'\\');
        commandLine.push_back(L'"');
    }

    std::wstring CommandLine(const std::wstring& executable, const std::vector<std::wstring>& arguments)
    {
        std::wstring result;
        AppendQuotedArgument(executable, result);
        for (const auto& argument : arguments)
        {
            result.push_back(L' ');
            AppendQuotedArgument(argument, result);
        }
        return result;
    }

    DWORD Run(const std::wstring& executable, const std::vector<std::wstring>& arguments)
    {
        auto commandLine = CommandLine(executable, arguments);
        STARTUPINFOW startup{ sizeof(startup) };
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION process{};
        VERIFY_IS_TRUE(CreateProcessW(executable.c_str(),
                                      commandLine.data(),
                                      nullptr,
                                      nullptr,
                                      FALSE,
                                      CREATE_NO_WINDOW,
                                      nullptr,
                                      nullptr,
                                      &startup,
                                      &process) != FALSE);
        wil::unique_handle child{ process.hProcess };
        wil::unique_handle thread{ process.hThread };
        VERIFY_ARE_EQUAL(static_cast<DWORD>(WAIT_OBJECT_0), WaitForSingleObject(child.get(), 30000));
        DWORD exitCode{};
        VERIFY_IS_TRUE(GetExitCodeProcess(child.get(), &exitCode) != FALSE);
        return exitCode;
    }

    std::wstring FindGit()
    {
        std::wstring programFiles(32768, L'\0');
        const auto length = GetEnvironmentVariableW(L"ProgramFiles", programFiles.data(), static_cast<DWORD>(programFiles.size()));
        VERIFY_IS_TRUE(length != 0 && length < programFiles.size());
        programFiles.resize(length);
        const auto git = std::filesystem::path{ programFiles } / L"Git" / L"cmd" / L"git.exe";
        VERIFY_IS_TRUE(std::filesystem::is_regular_file(git));
        return git.wstring();
    }

    struct TemporaryRepository
    {
        explicit TemporaryRepository(const bool sha256 = false)
        {
            GUID id{};
            VERIFY_ARE_EQUAL(S_OK, CoCreateGuid(&id));
            wchar_t identifier[40]{};
            VERIFY_IS_TRUE(StringFromGUID2(id, identifier, ARRAYSIZE(identifier)) != 0);
            base = std::filesystem::temp_directory_path() /
                   L"WindowsTerminalGitStatusTests" /
                   (std::wstring{ identifier } + L" path & \u03a9");
            repository = base / L"repository";
            git = FindGit();
            std::filesystem::create_directories(repository);
            std::vector<std::wstring> arguments{ L"init", L"-q", L"--initial-branch=main" };
            if (sha256)
            {
                arguments.emplace_back(L"--object-format=sha256");
            }
            Git(std::move(arguments));
        }

        ~TemporaryRepository()
        {
            std::error_code error;
            std::filesystem::remove_all(base, error);
        }

        DWORD Git(std::vector<std::wstring> arguments, const DWORD expected = 0) const
        {
            arguments.insert(arguments.begin(), { L"-C", repository.wstring() });
            const auto result = Run(git, arguments);
            VERIFY_ARE_EQUAL(expected, result);
            return result;
        }

        void Write(const std::filesystem::path& relative, const std::string_view contents) const
        {
            const auto path = repository / relative;
            std::filesystem::create_directories(path.parent_path());
            wil::unique_hfile file{ CreateFileW(path.c_str(),
                                                GENERIC_WRITE,
                                                FILE_SHARE_READ,
                                                nullptr,
                                                CREATE_ALWAYS,
                                                FILE_ATTRIBUTE_NORMAL,
                                                nullptr) };
            VERIFY_IS_TRUE(static_cast<bool>(file));
            DWORD written{};
            VERIFY_IS_TRUE(WriteFile(file.get(),
                                     contents.data(),
                                     gsl::narrow<DWORD>(contents.size()),
                                     &written,
                                     nullptr) != FALSE);
            VERIFY_ARE_EQUAL(gsl::narrow<DWORD>(contents.size()), written);
        }

        void Commit(const wchar_t* message = L"fixture") const
        {
            Git({ L"-c", L"user.name=Fixture",
                  L"-c", L"user.email=fixture@example.invalid",
                  L"commit", L"-q", L"-m", message });
        }

        GitQueryResult Query(const std::filesystem::path& path,
                             GitQueryLimits limits = {},
                             std::stop_token cancellation = {}) const
        {
            GitStatusProvider provider{ GitStatusProviderOptions{ git } };
            GitQueryRequest request;
            request.environment.kind = EnvironmentKind::LocalWindows;
            request.directory = path.wstring();
            request.limits = limits;
            request.cancellation = cancellation;

            std::promise<GitQueryResult> promise;
            auto future = promise.get_future();
            provider.QueryAsync(std::move(request), [&promise](GitQueryResult result) mutable {
                promise.set_value(std::move(result));
            });
            VERIFY_IS_TRUE(future.wait_for(std::chrono::seconds{ 10 }) == std::future_status::ready);
            return future.get();
        }

        std::filesystem::path base;
        std::filesystem::path repository;
        std::wstring git;
    };

    void VerifyStatus(const GitQueryResult& result, const GitQueryStatus status)
    {
        VERIFY_ARE_EQUAL(static_cast<int>(status), static_cast<int>(result.status));
        VERIFY_IS_TRUE(result.IsValid());
    }

    void VerifyChanges(const GitQueryResult& result,
                       const bool staged,
                       const bool unstaged,
                       const bool untracked,
                       const bool conflicts)
    {
        VerifyStatus(result, GitQueryStatus::Ready);
        VERIFY_ARE_EQUAL(staged, result.snapshot->changes.staged);
        VERIFY_ARE_EQUAL(unstaged, result.snapshot->changes.unstaged);
        VERIFY_ARE_EQUAL(untracked, result.snapshot->changes.untracked);
        VERIFY_ARE_EQUAL(conflicts, result.snapshot->changes.conflicts);
    }

    void VerifyLines(const GitQueryResult& result,
                     const uint64_t added,
                     const uint64_t deleted,
                     const bool hasBinaryChanges = false)
    {
        VerifyStatus(result, GitQueryStatus::Ready);
        VERIFY_IS_TRUE(result.snapshot->lineChanges.has_value());
        VERIFY_ARE_EQUAL(added, result.snapshot->lineChanges->added);
        VERIFY_ARE_EQUAL(deleted, result.snapshot->lineChanges->deleted);
        VERIFY_ARE_EQUAL(hasBinaryChanges, result.snapshot->lineChanges->hasBinaryChanges);
    }

    struct EnvironmentRestore
    {
        explicit EnvironmentRestore(const wchar_t* variable) :
            name{ variable }
        {
            std::wstring buffer(32768, L'\0');
            SetLastError(ERROR_SUCCESS);
            const auto length = GetEnvironmentVariableW(name.c_str(), buffer.data(), static_cast<DWORD>(buffer.size()));
            if (length != 0 && length < buffer.size())
            {
                buffer.resize(length);
                value = std::move(buffer);
            }
        }

        ~EnvironmentRestore()
        {
            SetEnvironmentVariableW(name.c_str(), value ? value->c_str() : nullptr);
        }

        std::wstring name;
        std::optional<std::wstring> value;
    };
}

class GitStatusTests
{
    TEST_CLASS(GitStatusTests);

    TEST_METHOD(ParserDetectsOverlappingAndRenameRecords)
    {
        using namespace std::string_literals;
        const auto output =
            "# branch.oid 0123456789012345678901234567890123456789\0"
            "# branch.head main\0"
            "# branch.upstream origin/main\0"
            "# branch.ab +2 -3\0"
            "1 MM N... 100644 100644 100644 a a path with spaces\0"
            "2 R. N... 100644 100644 100644 a b R100 renamed\0original\0"
            "? untracked\0"
            "u UU N... 100644 100644 100644 100644 a b c conflict\0"s;
        const auto result = TerminalApp::GitStatus::ParsePorcelainV2(output);
        VerifyChanges(result, true, true, true, true);
        VERIFY_ARE_EQUAL(static_cast<int>(GitBranchKind::Named), static_cast<int>(result.snapshot->branchKind));
        VERIFY_ARE_EQUAL(std::wstring{ L"main" }, result.snapshot->branchName);
        VERIFY_IS_TRUE(result.snapshot->tracking.has_value());
        VERIFY_ARE_EQUAL(std::wstring{ L"origin/main" }, result.snapshot->tracking->upstream);
        VERIFY_ARE_EQUAL(2u, result.snapshot->tracking->ahead);
        VERIFY_ARE_EQUAL(3u, result.snapshot->tracking->behind);
    }

    TEST_METHOD(ParserRejectsTruncationAndMalformedRecords)
    {
        using namespace std::string_literals;
        for (const auto& output : {
                 "# branch.oid (initial)\0# branch.head main"s,
                 "# branch.oid 0123456789012345678901234567890123456789\0# branch.head main\0? \0"s,
                 "# branch.oid invalid\0# branch.head main\0"s,
                 "# branch.oid 0123456789012345678901234567890123456789\0# branch.head main\0" "2 R. N... 1 1 1 a b R100 new\0"s })
        {
            VerifyStatus(TerminalApp::GitStatus::ParsePorcelainV2(output),
                         GitQueryStatus::MalformedOutput);
        }
    }

    TEST_METHOD(ParserOmitsUnknownTrackingCountsButRejectsCountsWithoutUpstream)
    {
        using namespace std::string_literals;
        const auto upstreamOnly =
            "# branch.oid 0123456789012345678901234567890123456789\0"
            "# branch.head main\0"
            "# branch.upstream origin/missing\0"s;
        const auto result = TerminalApp::GitStatus::ParsePorcelainV2(upstreamOnly);
        VerifyChanges(result, false, false, false, false);
        VERIFY_IS_FALSE(result.snapshot->tracking.has_value());

        const auto countsOnly =
            "# branch.oid 0123456789012345678901234567890123456789\0"
            "# branch.head main\0"
            "# branch.ab +1 -2\0"s;
        VerifyStatus(TerminalApp::GitStatus::ParsePorcelainV2(countsOnly),
                     GitQueryStatus::MalformedOutput);
    }

    TEST_METHOD(NumStatParserHandlesTextBinaryRenamesAndUnusualPaths)
    {
        using namespace std::string_literals;
        const auto output =
            "2\t3\tpath\twith\ttabs\nand-newline\0"
            "-\t-\tbinary.dat\0"
            "4\t5\t\0old\tname\n\0new\tname\n\0"s;
        const auto changes = TerminalApp::GitStatus::ParseNumStat(output);
        VERIFY_IS_TRUE(changes.has_value());
        VERIFY_ARE_EQUAL(6ull, changes->added);
        VERIFY_ARE_EQUAL(8ull, changes->deleted);
        VERIFY_IS_TRUE(changes->hasBinaryChanges);

        const auto clean = TerminalApp::GitStatus::ParseNumStat({});
        VERIFY_IS_TRUE(clean.has_value());
        VERIFY_ARE_EQUAL(0ull, clean->added);
        VERIFY_ARE_EQUAL(0ull, clean->deleted);
        VERIFY_IS_FALSE(clean->hasBinaryChanges);
    }

    TEST_METHOD(NumStatParserRejectsMalformedTruncatedAndOverflowingRecords)
    {
        using namespace std::string_literals;
        for (const auto& output : {
                 "1\t2\tmissing-nul"s,
                 "1\t-\tpartial-binary\0"s,
                 "-\t2\tpartial-binary\0"s,
                 "x\t2\tbad-number\0"s,
                 "1\t2\t\0old-only\0"s,
                 "1\t2\t\0\0new\0"s,
                 "18446744073709551615\t0\tone\0"
                 "1\t0\ttwo\0"s })
        {
            VERIFY_IS_FALSE(TerminalApp::GitStatus::ParseNumStat(output).has_value());
        }
    }

    TEST_METHOD(FailureClassificationKeepsUserVisibleStatesDistinct)
    {
        using TerminalApp::GitStatus::ClassifyGitFailure;
        VERIFY_ARE_EQUAL(static_cast<int>(GitQueryStatus::OwnershipRefused),
                         static_cast<int>(ClassifyGitFailure("fatal: detected dubious ownership; add safe.directory")));
        VERIFY_ARE_EQUAL(static_cast<int>(GitQueryStatus::NoRepository),
                         static_cast<int>(ClassifyGitFailure("fatal: not a git repository")));
        VERIFY_ARE_EQUAL(static_cast<int>(GitQueryStatus::GitUnavailable),
                         static_cast<int>(ClassifyGitFailure("/usr/bin/env: '/usr/bin/git': No such file")));
        VERIFY_ARE_EQUAL(static_cast<int>(GitQueryStatus::UnsupportedEnvironment),
                         static_cast<int>(ClassifyGitFailure("There is no distribution with the supplied name.")));
        VERIFY_ARE_EQUAL(static_cast<int>(GitQueryStatus::GitUnavailable),
                         static_cast<int>(ClassifyGitFailure({}, true)));
        VERIFY_ARE_EQUAL(static_cast<int>(GitQueryStatus::Failed),
                         static_cast<int>(ClassifyGitFailure("fatal: unexpected failure")));
    }

    TEST_METHOD(WslControlParserScansLargeAndSplitRecords)
    {
        TerminalApp::GitStatus::details::WslControlParser largeChunk;
        std::string output{ "warning\nWTGITPGID:4242\n" };
        output.resize(64 * 1024, 'x');
        largeChunk.Observe(output);
        VERIFY_IS_TRUE(largeChunk.ProcessGroup().has_value());
        VERIFY_ARE_EQUAL(4242u, *largeChunk.ProcessGroup());

        TerminalApp::GitStatus::details::WslControlParser splitRecord;
        splitRecord.Observe("launcher warning\nWTGITP");
        VERIFY_IS_FALSE(splitRecord.ProcessGroup().has_value());
        splitRecord.Observe("GID:8675309\nremaining diagnostic");
        VERIFY_IS_TRUE(splitRecord.ProcessGroup().has_value());
        VERIFY_ARE_EQUAL(8675309u, *splitRecord.ProcessGroup());
    }

    TEST_METHOD(WslControlParserTypesMissingArbitraryGitPath)
    {
        TerminalApp::GitStatus::details::WslControlParser control;
        constexpr std::string_view diagnostic{
            "WTGITPGID:73\n"
            "/usr/bin/env: '/opt/custom tools/git': No such file or directory\n"
            "WTGITERROR:GIT_UNAVAILABLE\n"
        };
        control.Observe(diagnostic);
        VERIFY_IS_TRUE(control.ProcessGroup().has_value());
        VERIFY_IS_TRUE(control.GitUnavailable());
        VERIFY_ARE_EQUAL(
            static_cast<int>(GitQueryStatus::GitUnavailable),
            static_cast<int>(TerminalApp::GitStatus::ClassifyGitFailure(diagnostic, control.GitUnavailable())));
        VERIFY_ARE_EQUAL(
            static_cast<int>(GitQueryStatus::Failed),
            static_cast<int>(TerminalApp::GitStatus::ClassifyGitFailure(
                "/opt/custom tools/unrelated-helper returned 127",
                false)));
    }

    TEST_METHOD(WslArgumentsPreserveDistroUserAndPathsAsDistinctValues)
    {
        ExecutionEnvironment environment;
        environment.kind = EnvironmentKind::Wsl;
        environment.wslDistro = L"Distro & Name";
        environment.wslUser = L"user;$(id) \"quoted\"";
        const std::wstring git{ L"/opt/custom tools/git" };
        const std::vector<std::wstring> gitArguments{
            L"-C", L"/repo;$(touch nope)", L"status"
        };

        const auto arguments = TerminalApp::GitStatus::details::BuildWslGitArguments(
            environment,
            git,
            gitArguments);
        VERIFY_ARE_EQUAL(std::wstring{ L"--distribution" }, arguments.at(0));
        VERIFY_ARE_EQUAL(environment.wslDistro, arguments.at(1));
        VERIFY_ARE_EQUAL(std::wstring{ L"--user" }, arguments.at(2));
        VERIFY_ARE_EQUAL(environment.wslUser, arguments.at(3));
        VERIFY_ARE_EQUAL(std::wstring{ L"--exec" }, arguments.at(4));
        VERIFY_ARE_EQUAL(git, arguments.at(arguments.size() - gitArguments.size() - 1));
        VERIFY_ARE_EQUAL(gitArguments.at(1), arguments.at(arguments.size() - 2));

        const auto commandLine = TerminalApp::GitStatus::details::BuildCommandLine(
            L"C:\\Windows\\System32\\wsl.exe",
            arguments);
        VERIFY_IS_TRUE(commandLine.has_value());
        int parsedCount{};
        wil::unique_hlocal_ptr<PWSTR[]> parsed{
            CommandLineToArgvW(commandLine->c_str(), &parsedCount)
        };
        VERIFY_IS_TRUE(static_cast<bool>(parsed));
        VERIFY_ARE_EQUAL(gsl::narrow<int>(arguments.size() + 1), parsedCount);
        VERIFY_ARE_EQUAL(std::wstring{ L"C:\\Windows\\System32\\wsl.exe" }, std::wstring{ parsed[0] });
        for (size_t i = 0; i < arguments.size(); ++i)
        {
            VERIFY_ARE_EQUAL(arguments[i], std::wstring{ parsed[i + 1] });
        }

        const auto cleanup = TerminalApp::GitStatus::details::BuildWslCleanupArguments(
            environment,
            4242,
            L"-TERM");
        VERIFY_ARE_EQUAL(std::wstring{ L"--distribution" }, cleanup.at(0));
        VERIFY_ARE_EQUAL(environment.wslDistro, cleanup.at(1));
        VERIFY_ARE_EQUAL(std::wstring{ L"--user" }, cleanup.at(2));
        VERIFY_ARE_EQUAL(environment.wslUser, cleanup.at(3));
        VERIFY_ARE_EQUAL(std::wstring{ L"--exec" }, cleanup.at(4));
        VERIFY_ARE_EQUAL(std::wstring{ L"-4242" }, cleanup.back());

        environment.wslUser.clear();
        const auto defaultUser = TerminalApp::GitStatus::details::BuildWslCleanupArguments(
            environment,
            7,
            L"-KILL");
        VERIFY_ARE_EQUAL(std::wstring{ L"--exec" }, defaultUser.at(2));
        VERIFY_IS_TRUE(std::find(defaultUser.begin(), defaultUser.end(), L"--user") == defaultUser.end());

        environment.wslUser.assign(L"bad\0user", 8);
        const auto invalidUserArguments = TerminalApp::GitStatus::details::BuildWslCleanupArguments(
            environment,
            7,
            L"-KILL");
        VERIFY_IS_FALSE(TerminalApp::GitStatus::details::BuildCommandLine(
                            L"C:\\Windows\\System32\\wsl.exe",
                            invalidUserArguments)
                            .has_value());
    }

    TEST_METHOD(RealRepositoryReportsUnbornCleanAndDetachedBranches)
    {
        TemporaryRepository fixture;
        auto result = fixture.Query(fixture.repository);
        VerifyChanges(result, false, false, false, false);
        VerifyLines(result, 0, 0);
        VERIFY_ARE_EQUAL(EnvironmentKind::LocalWindows, result.snapshot->environment.kind);
        VERIFY_IS_TRUE(std::filesystem::equivalent(
            std::filesystem::path{ result.snapshot->worktreeRoot }, fixture.repository));
        VERIFY_ARE_EQUAL(static_cast<int>(GitBranchKind::Unborn), static_cast<int>(result.snapshot->branchKind));
        VERIFY_ARE_EQUAL(std::wstring{ L"main" }, result.snapshot->branchName);
        VERIFY_IS_TRUE(result.snapshot->commit.empty());

        fixture.Write(L"tracked.txt", "base");
        fixture.Git({ L"add", L"--", L"tracked.txt" });
        fixture.Commit();
        result = fixture.Query(fixture.repository);
        VerifyChanges(result, false, false, false, false);
        VerifyLines(result, 0, 0);
        VERIFY_ARE_EQUAL(static_cast<int>(GitBranchKind::Named), static_cast<int>(result.snapshot->branchKind));

        fixture.Git({ L"checkout", L"-q", L"--detach", L"HEAD" });
        result = fixture.Query(fixture.repository);
        VerifyChanges(result, false, false, false, false);
        VerifyLines(result, 0, 0);
        VERIFY_ARE_EQUAL(static_cast<int>(GitBranchKind::Detached), static_cast<int>(result.snapshot->branchKind));
        VERIFY_IS_TRUE(result.snapshot->branchName.empty());
        VERIFY_IS_FALSE(result.snapshot->commit.empty());
    }

    TEST_METHOD(RealRepositoryReportsStagedUnstagedBothAndUntrackedPaths)
    {
        TemporaryRepository fixture;
        fixture.Write(L"staged.txt", "base");
        fixture.Write(L"unstaged.txt", "base");
        fixture.Write(L"both.txt", "base");
        fixture.Git({ L"add", L"--", L"." });
        fixture.Commit();

        fixture.Write(L"staged.txt", "staged");
        fixture.Git({ L"add", L"--", L"staged.txt" });
        fixture.Write(L"unstaged.txt", "unstaged");
        fixture.Write(L"both.txt", "index");
        fixture.Git({ L"add", L"--", L"both.txt" });
        fixture.Write(L"both.txt", "working tree");
        fixture.Write(L"untracked directory\\one.txt", "one");
        fixture.Write(L"untracked directory\\two & three.txt", "two");

        const auto result = fixture.Query(fixture.repository);
        VerifyChanges(result, true, true, true, false);
        VerifyLines(result, 3, 3);
    }

    TEST_METHOD(RealRepositoryUsesNetTrackedLinesAgainstHead)
    {
        TemporaryRepository fixture;
        fixture.Write(L"tracked.txt", "base\n");
        fixture.Git({ L"add", L"--", L"tracked.txt" });
        fixture.Commit();

        fixture.Write(L"tracked.txt", "changed\n");
        fixture.Git({ L"add", L"--", L"tracked.txt" });
        fixture.Write(L"tracked.txt", "base\n");
        const auto result = fixture.Query(fixture.repository);
        VerifyChanges(result, true, true, false, false);
        VerifyLines(result, 0, 0);
    }

    TEST_METHOD(UnbornRepositoryUsesEmptyTreeWithoutDoubleCountingIndexAndWorktree)
    {
        TemporaryRepository fixture;
        fixture.Write(L"initial.txt", "one\ntwo\n");
        fixture.Git({ L"add", L"--", L"initial.txt" });
        fixture.Write(L"initial.txt", "current\n");

        const auto result = fixture.Query(fixture.repository);
        VerifyChanges(result, true, true, false, false);
        VERIFY_ARE_EQUAL(static_cast<int>(GitBranchKind::Unborn), static_cast<int>(result.snapshot->branchKind));
        VerifyLines(result, 1, 0);
    }

    TEST_METHOD(UntrackedOnlyContentDoesNotContributeLines)
    {
        TemporaryRepository fixture;
        fixture.Write(L"untracked.txt", "one\ntwo\nthree\n");
        const auto result = fixture.Query(fixture.repository);
        VerifyChanges(result, false, false, true, false);
        VerifyLines(result, 0, 0);
    }

    TEST_METHOD(BinaryAndMixedChangesPreserveNumericTextTotals)
    {
        TemporaryRepository binaryFixture;
        binaryFixture.Write(L"binary.dat", std::string_view{ "\0base", 5 });
        binaryFixture.Git({ L"add", L"--", L"binary.dat" });
        binaryFixture.Commit();
        binaryFixture.Write(L"binary.dat", std::string_view{ "\0changed", 8 });
        auto result = binaryFixture.Query(binaryFixture.repository);
        VerifyChanges(result, false, true, false, false);
        VerifyLines(result, 0, 0, true);

        TemporaryRepository mixedFixture;
        mixedFixture.Write(L"text.txt", "one\n");
        mixedFixture.Write(L"delete.txt", "gone\n");
        mixedFixture.Write(L"binary.dat", std::string_view{ "\0base", 5 });
        mixedFixture.Git({ L"add", L"--", L"." });
        mixedFixture.Commit();
        mixedFixture.Write(L"text.txt", "one\ntwo\n");
        mixedFixture.Write(L"binary.dat", std::string_view{ "\0changed", 8 });
        VERIFY_IS_TRUE(std::filesystem::remove(mixedFixture.repository / L"delete.txt"));
        result = mixedFixture.Query(mixedFixture.repository);
        VerifyChanges(result, false, true, false, false);
        VerifyLines(result, 1, 1, true);
    }

    TEST_METHOD(RealRenameWithUnusualNamesHasZeroNetLines)
    {
        TemporaryRepository fixture;
        const std::filesystem::path oldName{ L"old name & [\u03a9].txt" };
        const std::filesystem::path newName{ L"new name ' & [\u03a9].txt" };
        fixture.Write(oldName, "same\n");
        fixture.Git({ L"add", L"--", oldName.wstring() });
        fixture.Commit();
        fixture.Git({ L"mv", L"--", oldName.wstring(), newName.wstring() });

        const auto result = fixture.Query(fixture.repository);
        VerifyChanges(result, true, false, false, false);
        VerifyLines(result, 0, 0);
    }

    TEST_METHOD(Sha256UnbornRepositoryUsesItsOwnEmptyTree)
    {
        TemporaryRepository fixture{ true };
        fixture.Write(L"initial.txt", "one\ntwo\n");
        fixture.Git({ L"add", L"--", L"initial.txt" });
        fixture.Write(L"initial.txt", "current\n");

        const auto result = fixture.Query(fixture.repository);
        VerifyChanges(result, true, true, false, false);
        VERIFY_ARE_EQUAL(static_cast<int>(GitBranchKind::Unborn), static_cast<int>(result.snapshot->branchKind));
        VerifyLines(result, 1, 0);
    }

    TEST_METHOD(RealRepositoryCountsRenameAndConflictSeparately)
    {
        TemporaryRepository fixture;
        fixture.Write(L"rename old.txt", "rename");
        fixture.Write(L"conflict.txt", "base");
        fixture.Git({ L"add", L"--", L"." });
        fixture.Commit();
        fixture.Git({ L"mv", L"--", L"rename old.txt", L"rename new.txt" });
        auto result = fixture.Query(fixture.repository);
        VerifyChanges(result, true, false, false, false);
        VerifyLines(result, 0, 0);
        fixture.Commit(L"rename");

        fixture.Git({ L"checkout", L"-q", L"-b", L"other" });
        fixture.Write(L"conflict.txt", "other");
        fixture.Git({ L"add", L"--", L"conflict.txt" });
        fixture.Commit(L"other");
        fixture.Git({ L"checkout", L"-q", L"main" });
        fixture.Write(L"conflict.txt", "main");
        fixture.Git({ L"add", L"--", L"conflict.txt" });
        fixture.Commit(L"main");
        fixture.Git({ L"merge", L"--no-edit", L"other" }, 1);
        result = fixture.Query(fixture.repository);
        VerifyChanges(result, false, false, false, true);
        VERIFY_IS_FALSE(result.snapshot->lineChanges.has_value());
    }

    TEST_METHOD(RealRepositoryReportsLocalUpstreamAheadAndBehind)
    {
        TemporaryRepository fixture;
        fixture.Write(L"base.txt", "base");
        fixture.Git({ L"add", L"--", L"." });
        fixture.Commit(L"base");
        fixture.Git({ L"branch", L"upstream" });

        fixture.Write(L"main.txt", "main");
        fixture.Git({ L"add", L"--", L"main.txt" });
        fixture.Commit(L"main");
        fixture.Git({ L"checkout", L"-q", L"upstream" });
        fixture.Write(L"upstream.txt", "upstream");
        fixture.Git({ L"add", L"--", L"upstream.txt" });
        fixture.Commit(L"upstream");
        fixture.Git({ L"checkout", L"-q", L"main" });
        fixture.Git({ L"branch", L"--set-upstream-to=upstream", L"main" });

        const auto result = fixture.Query(fixture.repository);
        VerifyChanges(result, false, false, false, false);
        VerifyLines(result, 0, 0);
        VERIFY_IS_TRUE(result.snapshot->tracking.has_value());
        VERIFY_ARE_EQUAL(std::wstring{ L"upstream" }, result.snapshot->tracking->upstream);
        VERIFY_ARE_EQUAL(1u, result.snapshot->tracking->ahead);
        VERIFY_ARE_EQUAL(1u, result.snapshot->tracking->behind);

        fixture.Git({ L"branch", L"-D", L"upstream" });
        const auto missingUpstream = fixture.Query(fixture.repository);
        VerifyChanges(missingUpstream, false, false, false, false);
        VerifyLines(missingUpstream, 0, 0);
        VERIFY_IS_FALSE(missingUpstream.snapshot->tracking.has_value());
    }

    TEST_METHOD(RealRepositoryDiscoversLinkedWorktreeFromNestedDirectory)
    {
        TemporaryRepository fixture;
        fixture.Write(L"tracked.txt", "base");
        fixture.Write(L".gitattributes", "tracked.txt diff=evil\n");
        fixture.Git({ L"add", L"--", L"." });
        fixture.Commit();
        const auto worktree = fixture.base / L"linked worktree [x]";
        fixture.Git({ L"worktree", L"add", L"-q", L"-b", L"linked", worktree.wstring() });
        const auto nested = worktree / L"nested";
        std::filesystem::create_directories(nested);

        const auto result = fixture.Query(nested);
        VerifyChanges(result, false, false, false, false);
        VerifyLines(result, 0, 0);
        VERIFY_IS_TRUE(std::filesystem::equivalent(std::filesystem::path{ result.snapshot->worktreeRoot }, worktree));
    }

    TEST_METHOD(RealRepositoryReportsModifiedSubmodulePath)
    {
        TemporaryRepository fixture;
        const auto source = fixture.base / L"submodule source";
        std::filesystem::create_directories(source);
        VERIFY_ARE_EQUAL(0u, Run(fixture.git, { L"-C", source.wstring(), L"init", L"-q", L"--initial-branch=main" }));
        const auto sourceFile = source / L"tracked.txt";
        {
            wil::unique_hfile file{ CreateFileW(sourceFile.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr) };
            DWORD written{};
            VERIFY_IS_TRUE(WriteFile(file.get(), "base", 4, &written, nullptr) != FALSE);
        }
        VERIFY_ARE_EQUAL(0u, Run(fixture.git, { L"-C", source.wstring(), L"add", L"--", L"tracked.txt" }));
        VERIFY_ARE_EQUAL(0u, Run(fixture.git, { L"-C", source.wstring(),
                                               L"-c", L"user.name=Fixture",
                                               L"-c", L"user.email=fixture@example.invalid",
                                               L"commit", L"-q", L"-m", L"base" }));

        fixture.Git({ L"-c", L"protocol.file.allow=always",
                      L"submodule", L"add", L"-q", source.wstring(), L"modules/sub" });
        fixture.Git({ L"add", L"--", L"." });
        fixture.Commit(L"submodule");
        fixture.Write(L"modules\\sub\\tracked.txt", "changed");
        VerifyChanges(fixture.Query(fixture.repository), false, true, false, false);
        const auto submoduleResult = fixture.Query(fixture.repository / L"modules" / L"sub");
        VerifyChanges(submoduleResult, false, true, false, false);
        VERIFY_IS_TRUE(std::filesystem::equivalent(
            std::filesystem::path{ submoduleResult.snapshot->worktreeRoot },
            fixture.repository / L"modules" / L"sub"));
    }

    TEST_METHOD(ProviderBoundsAndFailureStatesAreNotReady)
    {
        TemporaryRepository fixture;
        fixture.Write(L"tracked.txt", "base\n");
        fixture.Git({ L"add", L"--", L"tracked.txt" });
        fixture.Commit();
        fixture.Write(L"tracked.txt", "index\n");
        fixture.Git({ L"add", L"--", L"tracked.txt" });
        fixture.Write(L"tracked.txt", "worktree\n");

        GitQueryLimits timeout;
        timeout.deadline = std::chrono::milliseconds{ 0 };
        VerifyStatus(fixture.Query(fixture.repository, timeout), GitQueryStatus::TimedOut);

        GitQueryLimits outputLimit;
        outputLimit.maximumOutputBytes = 1;
        VerifyStatus(fixture.Query(fixture.repository, outputLimit), GitQueryStatus::OutputLimitExceeded);

        std::stop_source cancellation;
        cancellation.request_stop();
        VerifyStatus(fixture.Query(fixture.repository, {}, cancellation.get_token()), GitQueryStatus::Cancelled);

        const auto nonRepository = fixture.base / L"not a repository";
        // Stop discovery here even when the test's fixture directory is inside a checkout.
        fixture.Write(L"..\\not a repository\\.git", "gitdir: missing\n");
        VerifyStatus(fixture.Query(nonRepository), GitQueryStatus::NoRepository);

        GitStatusProvider missing{ GitStatusProviderOptions{ (fixture.base / L"missing git.exe").wstring() } };
        GitQueryRequest request;
        request.environment.kind = EnvironmentKind::LocalWindows;
        request.directory = fixture.repository.wstring();
        std::promise<GitQueryResult> promise;
        auto future = promise.get_future();
        missing.QueryAsync(request, [&promise](GitQueryResult result) mutable { promise.set_value(std::move(result)); });
        VERIFY_IS_TRUE(future.wait_for(std::chrono::seconds{ 5 }) == std::future_status::ready);
        VerifyStatus(future.get(), GitQueryStatus::GitUnavailable);
    }

    TEST_METHOD(ProviderSharesOutputBudgetAcrossCommands)
    {
        TemporaryRepository fixture;
        fixture.Write(L"tracked.txt", "base\n");
        fixture.Git({ L"add", L"--", L"tracked.txt" });
        fixture.Commit();
        VerifyChanges(fixture.Query(fixture.repository), false, false, false, false);

        // Either root discovery or this clean porcelain response fits on its own,
        // but their combined output must exceed the request's shared budget.
        constexpr size_t statusBytes = sizeof(
            "# branch.oid 0123456789012345678901234567890123456789\0"
            "# branch.head main\0") - 1;
        GitQueryLimits limits;
        limits.maximumOutputBytes = std::max(fixture.repository.u8string().size() + 2, statusBytes);
        VerifyStatus(fixture.Query(fixture.repository, limits), GitQueryStatus::OutputLimitExceeded);
    }

    TEST_METHOD(ProviderIgnoresInheritedGitSelectionAndRepositoryFsmonitor)
    {
        TemporaryRepository fixture;
        fixture.Write(L"tracked.txt", "base");
        fixture.Git({ L"add", L"--", L"." });
        fixture.Commit();

        const auto marker = fixture.base / L"fsmonitor-ran.txt";
        const auto hook = fixture.base / L"fsmonitor hook.cmd";
        {
            constexpr std::string_view command{ "@echo ran>\"%~dp0fsmonitor-ran.txt\"\r\n" };
            wil::unique_hfile file{ CreateFileW(hook.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr) };
            DWORD written{};
            VERIFY_IS_TRUE(WriteFile(file.get(), command.data(), gsl::narrow<DWORD>(command.size()), &written, nullptr) != FALSE);
        }
        fixture.Git({ L"config", L"core.fsmonitor", hook.wstring() });
        fixture.Git({ L"config", L"diff.external", hook.wstring() });
        fixture.Git({ L"config", L"diff.evil.textconv", hook.wstring() });
        fixture.Write(L"tracked.txt", "changed");

        EnvironmentRestore gitDirectory{ L"GIT_DIR" };
        EnvironmentRestore gitWorkTree{ L"GIT_WORK_TREE" };
        VERIFY_IS_TRUE(SetEnvironmentVariableW(L"GIT_DIR", L"Z:\\redirected\\.git") != FALSE);
        VERIFY_IS_TRUE(SetEnvironmentVariableW(L"GIT_WORK_TREE", L"Z:\\redirected") != FALSE);
        const auto nested = fixture.repository / L"nested & quoted ' directory";
        std::filesystem::create_directories(nested);
        const auto result = fixture.Query(nested);
        VerifyChanges(result, false, true, false, false);
        VerifyLines(result, 1, 1);
        VERIFY_IS_TRUE(std::filesystem::equivalent(std::filesystem::path{ result.snapshot->worktreeRoot }, fixture.repository));
        VERIFY_IS_FALSE(std::filesystem::exists(marker));
    }

    TEST_METHOD(CompletionRunsExactlyOnceOffTheCallingThread)
    {
        TemporaryRepository fixture;
        GitStatusProvider provider{ GitStatusProviderOptions{ fixture.git } };
        GitQueryRequest request;
        request.environment.kind = EnvironmentKind::LocalWindows;
        request.directory = fixture.repository.wstring();
        const auto caller = std::this_thread::get_id();
        std::promise<std::thread::id> promise;
        auto future = promise.get_future();
        std::atomic<uint32_t> calls{};
        provider.QueryAsync(request, [&](GitQueryResult result) {
            VERIFY_IS_TRUE(result.IsValid());
            ++calls;
            promise.set_value(std::this_thread::get_id());
        });
        VERIFY_IS_TRUE(future.wait_for(std::chrono::seconds{ 5 }) == std::future_status::ready);
        VERIFY_IS_TRUE(future.get() != caller);
        std::this_thread::sleep_for(std::chrono::milliseconds{ 50 });
        VERIFY_ARE_EQUAL(1u, calls.load());
    }

    TEST_METHOD(WslRuntimeFixtureIsExplicitlyGated)
    {
        wchar_t distro[256]{};
        wchar_t user[256]{};
        wchar_t repository[1024]{};
        const auto distroLength = GetEnvironmentVariableW(L"WT_TEST_WSL_DISTRO", distro, ARRAYSIZE(distro));
        const auto userLength = GetEnvironmentVariableW(L"WT_TEST_WSL_USER", user, ARRAYSIZE(user));
        const auto repositoryLength = GetEnvironmentVariableW(L"WT_TEST_WSL_GIT_REPOSITORY", repository, ARRAYSIZE(repository));
        if (distroLength == 0 || distroLength >= ARRAYSIZE(distro) ||
            userLength >= ARRAYSIZE(user) ||
            repositoryLength == 0 || repositoryLength >= ARRAYSIZE(repository))
        {
            WEX::Logging::Log::Comment(L"Blocked: set WT_TEST_WSL_DISTRO and WT_TEST_WSL_GIT_REPOSITORY to run the real distro-local provider fixture.");
            WEX::Logging::Log::Result(WEX::Logging::TestResults::Skipped);
            return;
        }

        GitStatusProvider provider;
        GitQueryRequest request;
        request.environment.kind = EnvironmentKind::Wsl;
        request.environment.wslDistro.assign(distro, distroLength);
        request.environment.wslUser.assign(user, userLength);
        request.directory.assign(repository, repositoryLength);
        std::promise<GitQueryResult> promise;
        auto future = promise.get_future();
        provider.QueryAsync(request, [&promise](GitQueryResult result) mutable { promise.set_value(std::move(result)); });
        VERIFY_IS_TRUE(future.wait_for(std::chrono::seconds{ 10 }) == std::future_status::ready);
        const auto result = future.get();
        VerifyStatus(result, GitQueryStatus::Ready);
        const std::wstring expectedRepository{ repository, repositoryLength };
        const std::wstring expectedDistro{ distro, distroLength };
        const std::wstring expectedUser{ user, userLength };
        VERIFY_ARE_EQUAL(expectedRepository, result.snapshot->worktreeRoot);
        VERIFY_ARE_EQUAL(expectedDistro, result.snapshot->environment.wslDistro);
        VERIFY_ARE_EQUAL(expectedUser, result.snapshot->environment.wslUser);
    }
};
