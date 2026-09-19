// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "GitStatusProvider.h"
#include "GitStatusParser.h"

#include <til/env.h>

#include <array>
#include <filesystem>
#include <ShlObj_core.h>
#include <thread>

using namespace Microsoft::Terminal::StatusBar;
using TerminalApp::GitStatusProviderOptions;

namespace
{
    enum class ProcessState
    {
        Completed,
        Cancelled,
        TimedOut,
        OutputLimitExceeded,
        Unavailable,
        Failed,
    };

    struct ProcessOutput
    {
        ProcessState state{ ProcessState::Failed };
        DWORD exitCode{};
        std::string standardOutput;
        std::string standardError;
        std::error_code error;
        size_t capturedBytes{};
        size_t maximumOutputBytes{};
        TerminalApp::GitStatus::details::WslControlParser wslControl;
    };

    struct ProcessInvocation
    {
        std::wstring executable;
        std::vector<std::wstring> arguments;
        std::wstring environment;
        std::chrono::steady_clock::time_point deadline;
        size_t maximumOutputBytes{};
        std::stop_token cancellation;
        ExecutionEnvironment wslEnvironment;
    };

    constexpr std::wstring_view WslGitScript{
        LR"(child=; cleanup(){ if [ -n "$child" ]; then /usr/bin/kill -CONT -- "$child" 2>/dev/null || true; /usr/bin/kill -TERM -- "-$child" 2>/dev/null || true; sleep 0.1; /usr/bin/kill -KILL -- "-$child" 2>/dev/null || true; fi; }; trap cleanup EXIT HUP INT TERM; /usr/bin/setsid /bin/sh -c '/usr/bin/kill -STOP "$$"; git=$1; shift; PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin; export PATH; case "$git" in git) command -v git >/dev/null 2>&1 || { printf "WTGITERROR:GIT_UNAVAILABLE\n" >&2; exit 127; };; /*) { [ -f "$git" ] && [ -x "$git" ]; } || { printf "WTGITERROR:GIT_UNAVAILABLE\n" >&2; exit 127; };; *) printf "WTGITERROR:GIT_UNAVAILABLE\n" >&2; exit 127;; esac; exec /usr/bin/env -i "HOME=${HOME-}" "USER=${USER-}" "LOGNAME=${LOGNAME-}" "XDG_CONFIG_HOME=${XDG_CONFIG_HOME-}" "PATH=$PATH" LC_ALL=C GIT_CONFIG_COUNT=0 GIT_OPTIONAL_LOCKS=0 GIT_TERMINAL_PROMPT=0 GCM_INTERACTIVE=Never "$git" "$@"' wt-git-child "$@" & child=$!; printf 'WTGITPGID:%s\n' "$child" >&2; /usr/bin/kill -CONT -- "$child"; wait "$child"; status=$?; trap - EXIT HUP INT TERM; exit "$status")"
    };

    std::error_code LastErrorCode() noexcept
    {
        return { static_cast<int>(GetLastError()), std::system_category() };
    }

    bool IsRegularFile(const std::filesystem::path& path) noexcept
    {
        std::error_code error;
        return path.is_absolute() && std::filesystem::is_regular_file(path, error);
    }

    bool ValidWslIdentityArgument(const std::wstring_view value) noexcept
    {
        return value.find(L'\0') == std::wstring_view::npos &&
               value.find(L'\r') == std::wstring_view::npos &&
               value.find(L'\n') == std::wstring_view::npos;
    }

    std::optional<std::filesystem::path> KnownFolder(const KNOWNFOLDERID& id)
    {
        PWSTR value{};
        if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &value)))
        {
            return std::nullopt;
        }
        const auto freeValue = wil::scope_exit([&] { CoTaskMemFree(value); });
        return std::filesystem::path{ value };
    }

    std::optional<std::wstring> ResolveWindowsGit(const std::wstring& configured)
    {
        if (!configured.empty())
        {
            return IsRegularFile(configured) ? std::optional{ configured } : std::nullopt;
        }

        std::vector<std::filesystem::path> candidates;
        if (const auto programFiles = KnownFolder(FOLDERID_ProgramFiles))
        {
            candidates.emplace_back(*programFiles / L"Git" / L"cmd" / L"git.exe");
        }
        if (const auto programFilesX86 = KnownFolder(FOLDERID_ProgramFilesX86))
        {
            candidates.emplace_back(*programFilesX86 / L"Git" / L"cmd" / L"git.exe");
        }
        if (const auto localAppData = KnownFolder(FOLDERID_LocalAppData))
        {
            candidates.emplace_back(*localAppData / L"Programs" / L"Git" / L"cmd" / L"git.exe");
        }
        for (const auto& candidate : candidates)
        {
            if (IsRegularFile(candidate))
            {
                return candidate.wstring();
            }
        }
        return std::nullopt;
    }

    std::optional<std::wstring> ResolveWsl(const std::wstring& configured)
    {
        if (!configured.empty())
        {
            return IsRegularFile(configured) ? std::optional{ configured } : std::nullopt;
        }
        std::wstring systemDirectory;
        if (FAILED(wil::GetSystemDirectoryW<std::wstring>(systemDirectory)))
        {
            return std::nullopt;
        }
        const auto candidate = std::filesystem::path{ systemDirectory } / L"wsl.exe";
        return IsRegularFile(candidate) ? std::optional{ candidate.wstring() } : std::nullopt;
    }

    void AppendQuotedArgument(const std::wstring_view argument, std::wstring& commandLine)
    {
        commandLine.push_back(L'"');
        size_t backslashes{};
        for (const auto ch : argument)
        {
            if (ch == L'\\')
            {
                ++backslashes;
                continue;
            }
            if (ch == L'"')
            {
                commandLine.append(backslashes * 2 + 1, L'\\');
                commandLine.push_back(ch);
            }
            else
            {
                commandLine.append(backslashes, L'\\');
                commandLine.push_back(ch);
            }
            backslashes = 0;
        }
        commandLine.append(backslashes * 2, L'\\');
        commandLine.push_back(L'"');
    }

    std::optional<std::wstring> BuildCommandLineImpl(const std::wstring& executable, const std::vector<std::wstring>& arguments)
    {
        if (executable.find(L'\0') != std::wstring::npos)
        {
            return std::nullopt;
        }
        std::wstring commandLine;
        AppendQuotedArgument(executable, commandLine);
        for (const auto& argument : arguments)
        {
            if (argument.find(L'\0') != std::wstring::npos)
            {
                return std::nullopt;
            }
            commandLine.push_back(L' ');
            AppendQuotedArgument(argument, commandLine);
        }
        return commandLine;
    }

    std::wstring SanitizedEnvironment()
    {
        auto environment = til::env::from_current_environment();
        auto& values = environment.as_map();
        for (auto iterator = values.begin(); iterator != values.end();)
        {
            const auto& name = iterator->first;
            if (name.size() >= 4 && _wcsnicmp(name.c_str(), L"GIT_", 4) == 0)
            {
                iterator = values.erase(iterator);
            }
            else
            {
                ++iterator;
            }
        }
        values[L"GIT_OPTIONAL_LOCKS"] = L"0";
        values[L"GIT_TERMINAL_PROMPT"] = L"0";
        values[L"GCM_INTERACTIVE"] = L"Never";
        values[L"LC_ALL"] = L"C";
        return environment.to_string();
    }

    void RemoveWslControlRecords(std::string& diagnostic) noexcept
    {
        for (const auto prefix : { std::string_view{ "WTGITPGID:" },
                                   std::string_view{ "WTGITERROR:" } })
        {
            for (;;)
            {
                const auto begin = diagnostic.find(prefix);
                if (begin == std::string::npos)
                {
                    break;
                }
                const auto end = diagnostic.find('\n', begin + prefix.size());
                if (end == std::string::npos)
                {
                    break;
                }
                diagnostic.erase(begin, end + 1 - begin);
            }
        }
    }

    bool DrainPipe(HANDLE pipe, std::string& destination, ProcessOutput& output, const bool linuxControl)
    {
        std::array<char, 64 * 1024> buffer{};
        for (;;)
        {
            DWORD available{};
            if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr))
            {
                if (GetLastError() == ERROR_BROKEN_PIPE)
                {
                    return true;
                }
                output.error = LastErrorCode();
                output.state = ProcessState::Failed;
                return false;
            }
            if (available == 0)
            {
                return true;
            }
            DWORD received{};
            const auto requested = std::min<DWORD>(available, static_cast<DWORD>(buffer.size()));
            if (!ReadFile(pipe, buffer.data(), requested, &received, nullptr))
            {
                if (GetLastError() == ERROR_BROKEN_PIPE)
                {
                    return true;
                }
                output.error = LastErrorCode();
                output.state = ProcessState::Failed;
                return false;
            }
            if (linuxControl)
            {
                output.wslControl.Observe(std::string_view{ buffer.data(), received });
            }
            if (received > output.maximumOutputBytes - std::min(output.capturedBytes, output.maximumOutputBytes))
            {
                output.capturedBytes = output.maximumOutputBytes;
                output.state = ProcessState::OutputLimitExceeded;
                return false;
            }
            output.capturedBytes += received;
            destination.append(buffer.data(), received);
        }
    }

    void RunWslKill(const std::wstring& wsl,
                    const ExecutionEnvironment& environment,
                    const DWORD processGroup,
                    const wchar_t* signal) noexcept
    {
        try
        {
            const auto arguments = TerminalApp::GitStatus::details::BuildWslCleanupArguments(
                environment,
                processGroup,
                signal);
            auto commandLine = TerminalApp::GitStatus::details::BuildCommandLine(wsl, arguments);
            if (!commandLine)
            {
                return;
            }
            STARTUPINFOW startup{ sizeof(startup) };
            startup.dwFlags = STARTF_USESHOWWINDOW;
            startup.wShowWindow = SW_HIDE;
            PROCESS_INFORMATION process{};
            if (!CreateProcessW(wsl.c_str(),
                                commandLine->data(),
                                nullptr,
                                nullptr,
                                FALSE,
                                CREATE_NO_WINDOW,
                                nullptr,
                                nullptr,
                                &startup,
                                &process))
            {
                LOG_WIN32_MSG(GetLastError(), "Could not start WSL Git cleanup.");
                return;
            }
            wil::unique_handle child{ process.hProcess };
            wil::unique_handle thread{ process.hThread };
            if (WaitForSingleObject(child.get(), 500) == WAIT_TIMEOUT)
            {
                LOG_IF_WIN32_BOOL_FALSE(TerminateProcess(child.get(), ERROR_TIMEOUT));
            }
        }
        catch (...)
        {
            LOG_CAUGHT_EXCEPTION();
        }
    }

    ProcessOutput RunProcess(ProcessInvocation invocation)
    {
        ProcessOutput output;
        output.maximumOutputBytes = invocation.maximumOutputBytes;
        if (invocation.maximumOutputBytes == 0)
        {
            output.state = ProcessState::OutputLimitExceeded;
            return output;
        }
        if (invocation.cancellation.stop_requested())
        {
            output.state = ProcessState::Cancelled;
            return output;
        }
        if (std::chrono::steady_clock::now() >= invocation.deadline)
        {
            output.state = ProcessState::TimedOut;
            return output;
        }
        const auto commandLine = TerminalApp::GitStatus::details::BuildCommandLine(invocation.executable, invocation.arguments);
        if (!commandLine || !IsRegularFile(invocation.executable))
        {
            output.state = ProcessState::Unavailable;
            output.error = { ERROR_FILE_NOT_FOUND, std::system_category() };
            return output;
        }

        SECURITY_ATTRIBUTES security{ sizeof(security), nullptr, TRUE };
        wil::unique_handle stdoutRead;
        wil::unique_handle stdoutWrite;
        wil::unique_handle stderrRead;
        wil::unique_handle stderrWrite;
        if (!CreatePipe(stdoutRead.put(), stdoutWrite.put(), &security, 0) ||
            !SetHandleInformation(stdoutRead.get(), HANDLE_FLAG_INHERIT, 0) ||
            !CreatePipe(stderrRead.put(), stderrWrite.put(), &security, 0) ||
            !SetHandleInformation(stderrRead.get(), HANDLE_FLAG_INHERIT, 0))
        {
            output.error = LastErrorCode();
            return output;
        }
        wil::unique_hfile null{ CreateFileW(L"NUL",
                                            GENERIC_READ,
                                            FILE_SHARE_READ | FILE_SHARE_WRITE,
                                            &security,
                                            OPEN_EXISTING,
                                            0,
                                            nullptr) };
        if (!null)
        {
            output.error = LastErrorCode();
            return output;
        }

        SIZE_T attributeBytes{};
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
        std::vector<std::byte> attributeStorage(attributeBytes);
        const auto attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
        if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attributeBytes))
        {
            output.error = LastErrorCode();
            return output;
        }
        const auto deleteAttributes = wil::scope_exit([&] { DeleteProcThreadAttributeList(attributes); });
        HANDLE inheritedHandles[]{ stdoutWrite.get(), stderrWrite.get(), null.get() };
        if (!UpdateProcThreadAttribute(attributes,
                                       0,
                                       PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                       inheritedHandles,
                                       sizeof(inheritedHandles),
                                       nullptr,
                                       nullptr))
        {
            output.error = LastErrorCode();
            return output;
        }

        wil::unique_handle job{ CreateJobObjectW(nullptr, nullptr) };
        if (!job)
        {
            output.error = LastErrorCode();
            return output;
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobLimits{};
        jobLimits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &jobLimits, sizeof(jobLimits)))
        {
            output.error = LastErrorCode();
            return output;
        }

        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        startup.StartupInfo.wShowWindow = SW_HIDE;
        startup.StartupInfo.hStdInput = null.get();
        startup.StartupInfo.hStdOutput = stdoutWrite.get();
        startup.StartupInfo.hStdError = stderrWrite.get();
        startup.lpAttributeList = attributes;
        PROCESS_INFORMATION rawProcess{};
        auto mutableCommandLine = *commandLine;
        if (!CreateProcessW(invocation.executable.c_str(),
                            mutableCommandLine.data(),
                            nullptr,
                            nullptr,
                            TRUE,
                            CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
                            invocation.environment.data(),
                            nullptr,
                            &startup.StartupInfo,
                            &rawProcess))
        {
            const auto error = GetLastError();
            output.error = { static_cast<int>(error), std::system_category() };
            output.state = error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ?
                               ProcessState::Unavailable :
                               ProcessState::Failed;
            return output;
        }

        wil::unique_handle child{ rawProcess.hProcess };
        wil::unique_handle thread{ rawProcess.hThread };
        auto terminate = wil::scope_exit([&] { TerminateProcess(child.get(), ERROR_CANCELLED); });
        if (!AssignProcessToJobObject(job.get(), child.get()) ||
            ResumeThread(thread.get()) == static_cast<DWORD>(-1))
        {
            output.error = LastErrorCode();
            return output;
        }
        stdoutWrite.reset();
        stderrWrite.reset();

        bool processExited{};
        for (;;)
        {
            if (!DrainPipe(stderrRead.get(), output.standardError, output, true) ||
                !DrainPipe(stdoutRead.get(), output.standardOutput, output, false))
            {
                break;
            }
            RemoveWslControlRecords(output.standardError);
            if (invocation.cancellation.stop_requested())
            {
                output.state = ProcessState::Cancelled;
                break;
            }
            if (std::chrono::steady_clock::now() >= invocation.deadline)
            {
                output.state = ProcessState::TimedOut;
                break;
            }

            const auto wait = WaitForSingleObject(child.get(), 15);
            if (wait == WAIT_OBJECT_0)
            {
                processExited = true;
                if (!DrainPipe(stderrRead.get(), output.standardError, output, true) ||
                    !DrainPipe(stdoutRead.get(), output.standardOutput, output, false))
                {
                    break;
                }
                RemoveWslControlRecords(output.standardError);
                output.state = ProcessState::Completed;
                break;
            }
            if (wait != WAIT_TIMEOUT)
            {
                output.error = LastErrorCode();
                output.state = ProcessState::Failed;
                break;
            }
        }

        if (!processExited)
        {
            if (invocation.wslEnvironment.kind == EnvironmentKind::Wsl && !output.wslControl.ProcessGroup())
            {
                const auto reason = output.state;
                const auto cleanupDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{ 250 };
                while (!output.wslControl.ProcessGroup() && std::chrono::steady_clock::now() < cleanupDeadline)
                {
                    DrainPipe(stderrRead.get(), output.standardError, output, true);
                    RemoveWslControlRecords(output.standardError);
                    output.state = reason;
                    if (WaitForSingleObject(child.get(), 5) == WAIT_OBJECT_0)
                    {
                        break;
                    }
                }
            }
            if (invocation.wslEnvironment.kind == EnvironmentKind::Wsl && output.wslControl.ProcessGroup())
            {
                const auto processGroup = *output.wslControl.ProcessGroup();
                RunWslKill(invocation.executable, invocation.wslEnvironment, processGroup, L"-TERM");
                RunWslKill(invocation.executable, invocation.wslEnvironment, processGroup, L"-KILL");
            }
            LOG_IF_WIN32_BOOL_FALSE(TerminateJobObject(job.get(), ERROR_CANCELLED));
            WaitForSingleObject(child.get(), 1000);
        }
        else
        {
            terminate.release();
        }

        if (output.state == ProcessState::Completed &&
            !GetExitCodeProcess(child.get(), &output.exitCode))
        {
            output.error = LastErrorCode();
            output.state = ProcessState::Failed;
        }
        return output;
    }

    std::optional<std::wstring> ParseRoot(const std::string& output, const EnvironmentKind environment)
    {
        auto root = std::string_view{ output };
        if (root.ends_with('\n'))
        {
            root.remove_suffix(1);
            if (root.ends_with('\r'))
            {
                root.remove_suffix(1);
            }
        }
        if (root.empty() || root.find('\0') != std::string_view::npos ||
            root.find('\n') != std::string_view::npos || root.find('\r') != std::string_view::npos)
        {
            return std::nullopt;
        }
        const auto converted = [&]() -> std::optional<std::wstring> {
            const auto length = MultiByteToWideChar(CP_UTF8,
                                                    MB_ERR_INVALID_CHARS,
                                                    root.data(),
                                                    gsl::narrow<int>(root.size()),
                                                    nullptr,
                                                    0);
            if (length == 0)
            {
                return std::nullopt;
            }
            std::wstring value(static_cast<size_t>(length), L'\0');
            if (MultiByteToWideChar(CP_UTF8,
                                    MB_ERR_INVALID_CHARS,
                                    root.data(),
                                    gsl::narrow<int>(root.size()),
                                    value.data(),
                                    length) != length)
            {
                return std::nullopt;
            }
            return value;
        }();
        if (!converted)
        {
            return std::nullopt;
        }
        if (environment == EnvironmentKind::LocalWindows)
        {
            return std::filesystem::path{ *converted }.is_absolute() ? converted : std::nullopt;
        }
        return converted->starts_with(L"/") ? converted : std::nullopt;
    }

    std::optional<std::wstring> ParseObjectId(const std::string& output)
    {
        auto value = std::string_view{ output };
        if (value.ends_with('\n'))
        {
            value.remove_suffix(1);
            if (value.ends_with('\r'))
            {
                value.remove_suffix(1);
            }
        }
        if (value.size() != 40 && value.size() != 64)
        {
            return std::nullopt;
        }
        if (!std::all_of(value.begin(), value.end(), [](const char ch) {
                return (ch >= '0' && ch <= '9') ||
                       (ch >= 'a' && ch <= 'f') ||
                       (ch >= 'A' && ch <= 'F');
            }))
        {
            return std::nullopt;
        }
        return std::wstring{ value.begin(), value.end() };
    }

    ProcessInvocation GitInvocation(const GitStatusProviderOptions& options,
                                    const GitQueryRequest& request,
                                    const std::wstring& executable,
                                    std::vector<std::wstring> gitArguments,
                                    const std::chrono::steady_clock::time_point deadline,
                                    const size_t maximumOutputBytes,
                                    const std::wstring& environment)
    {
        ProcessInvocation invocation{
            executable,
            {},
            environment,
            deadline,
            maximumOutputBytes,
            request.cancellation,
            {}
        };
        if (request.environment.kind == EnvironmentKind::Wsl)
        {
            // The fixed wrapper reports the setsid process group before waiting. Cancellation
            // uses a second, distro-qualified WSL invocation to terminate that Linux group.
            invocation.wslEnvironment = request.environment;
            invocation.arguments = TerminalApp::GitStatus::details::BuildWslGitArguments(
                invocation.wslEnvironment,
                options.wslGitPath,
                std::move(gitArguments));
        }
        else
        {
            invocation.arguments = std::move(gitArguments);
        }
        return invocation;
    }

    GitQueryResult StatusFromProcessFailure(const ProcessOutput& output)
    {
        GitQueryResult result;
        result.error = output.error;
        switch (output.state)
        {
        case ProcessState::Cancelled:
            result.status = GitQueryStatus::Cancelled;
            break;
        case ProcessState::TimedOut:
            result.status = GitQueryStatus::TimedOut;
            break;
        case ProcessState::OutputLimitExceeded:
            result.status = GitQueryStatus::OutputLimitExceeded;
            break;
        case ProcessState::Unavailable:
            result.status = GitQueryStatus::GitUnavailable;
            break;
        default:
            result.status = GitQueryStatus::Failed;
            break;
        }
        return result;
    }

    GitQueryResult Query(const GitStatusProviderOptions& options, const GitQueryRequest& request)
    {
        GitQueryResult result;
        result.status = GitQueryStatus::UnsupportedEnvironment;

        const auto& environment = request.environment;
        if (request.directory.empty() || !environment.IsEligible())
        {
            return result;
        }
        if (environment.kind == EnvironmentKind::LocalWindows &&
            !std::filesystem::path{ request.directory }.is_absolute())
        {
            return result;
        }
        if (environment.kind == EnvironmentKind::Wsl && !request.directory.starts_with(L"/"))
        {
            return result;
        }
        if (environment.kind == EnvironmentKind::Wsl &&
            (!ValidWslIdentityArgument(environment.wslDistro) ||
             !ValidWslIdentityArgument(environment.wslUser)))
        {
            return result;
        }
        if (environment.kind == EnvironmentKind::Wsl &&
            (options.wslGitPath.empty() ||
             (options.wslGitPath != L"git" && !options.wslGitPath.starts_with(L"/"))))
        {
            result.status = GitQueryStatus::GitUnavailable;
            return result;
        }

        const auto executable = environment.kind == EnvironmentKind::LocalWindows ?
                                    ResolveWindowsGit(options.windowsGitPath) :
                                    ResolveWsl(options.wslExecutablePath);
        if (!executable)
        {
            result.status = GitQueryStatus::GitUnavailable;
            result.error = { ERROR_FILE_NOT_FOUND, std::system_category() };
            return result;
        }
        const auto deadline = std::chrono::steady_clock::now() + request.limits.deadline;
        const auto processEnvironment = SanitizedEnvironment();
        size_t remaining = request.limits.maximumOutputBytes;

        // All commands share one deadline, output budget, read-only policy, and failure path.
        const auto runGit = [&](const std::wstring& directory, std::vector<std::wstring> arguments) -> std::optional<std::string> {
            arguments.insert(arguments.begin(), {
                L"--no-optional-locks",
                L"--no-lazy-fetch",
                L"-c", L"core.fsmonitor=false",
                L"-C", directory,
            });
            auto output = RunProcess(GitInvocation(options,
                                                   request,
                                                   *executable,
                                                   std::move(arguments),
                                                   deadline,
                                                   remaining,
                                                   processEnvironment));
            if (output.state != ProcessState::Completed)
            {
                result = StatusFromProcessFailure(output);
                return std::nullopt;
            }
            if (output.exitCode != 0)
            {
                result.status = output.wslControl.GitUnavailable() ?
                                    GitQueryStatus::GitUnavailable :
                                    environment.kind == EnvironmentKind::Wsl && !output.wslControl.ProcessGroup() ?
                                        GitQueryStatus::UnsupportedEnvironment :
                                        TerminalApp::GitStatus::ClassifyGitFailure(output.standardError);
                result.error = { static_cast<int>(output.exitCode), std::system_category() };
                return std::nullopt;
            }
            if (output.capturedBytes > remaining)
            {
                result.status = GitQueryStatus::OutputLimitExceeded;
                return std::nullopt;
            }
            remaining -= output.capturedBytes;
            return std::move(output.standardOutput);
        };

        const auto discovery = runGit(request.directory, { L"rev-parse", L"--path-format=absolute", L"--show-toplevel" });
        if (!discovery)
        {
            return result;
        }
        const auto root = ParseRoot(*discovery, environment.kind);
        if (!root)
        {
            result.status = GitQueryStatus::MalformedOutput;
            return result;
        }

        const auto status = runGit(*root, {
            L"status", L"--porcelain=v2", L"--branch", L"--ahead-behind",
            L"-z", L"--untracked-files=all", L"--ignore-submodules=none"
        });
        if (!status)
        {
            return result;
        }

        auto parsed = TerminalApp::GitStatus::ParsePorcelainV2(*status);
        if (parsed.status != GitQueryStatus::Ready)
        {
            return parsed;
        }
        parsed.snapshot->environment = environment;
        parsed.snapshot->worktreeRoot = *root;
        if (parsed.snapshot->changes.conflicts)
        {
            return parsed;
        }

        std::wstring baseObject = parsed.snapshot->commit;
        if (parsed.snapshot->branchKind == GitBranchKind::Unborn)
        {
            const auto emptyTree = runGit(*root, { L"hash-object", L"-t", L"tree", L"--stdin" });
            if (!emptyTree)
            {
                return result;
            }
            const auto emptyTreeId = ParseObjectId(*emptyTree);
            if (!emptyTreeId)
            {
                result.status = GitQueryStatus::MalformedOutput;
                return result;
            }
            baseObject = std::move(*emptyTreeId);
        }

        const auto diff = runGit(*root, {
            L"diff", L"--no-ext-diff", L"--no-textconv",
            L"--numstat", L"-z", L"--find-renames",
            baseObject, L"--"
        });
        if (!diff)
        {
            return result;
        }
        const auto lineChanges = TerminalApp::GitStatus::ParseNumStat(*diff);
        if (!lineChanges)
        {
            result.status = GitQueryStatus::MalformedOutput;
            return result;
        }
        parsed.snapshot->lineChanges = *lineChanges;
        return parsed;
    }
}

std::optional<std::wstring> TerminalApp::GitStatus::details::BuildCommandLine(
    const std::wstring& executable,
    const std::vector<std::wstring>& arguments)
{
    return BuildCommandLineImpl(executable, arguments);
}

std::vector<std::wstring> TerminalApp::GitStatus::details::BuildWslGitArguments(
    const ExecutionEnvironment& environment,
    const std::wstring& git,
    std::vector<std::wstring> gitArguments)
{
    std::vector<std::wstring> arguments{
        L"--distribution", environment.wslDistro
    };
    if (!environment.wslUser.empty())
    {
        arguments.emplace_back(L"--user");
        arguments.emplace_back(environment.wslUser);
    }
    arguments.insert(arguments.end(), { L"--exec", L"/bin/sh", L"-c", std::wstring{ WslGitScript }, L"wt-git", git });
    arguments.insert(arguments.end(),
                     std::make_move_iterator(gitArguments.begin()),
                     std::make_move_iterator(gitArguments.end()));
    return arguments;
}

std::vector<std::wstring> TerminalApp::GitStatus::details::BuildWslCleanupArguments(
    const ExecutionEnvironment& environment,
    const uint32_t processGroup,
    const std::wstring_view signal)
{
    std::vector<std::wstring> arguments{
        L"--distribution", environment.wslDistro
    };
    if (!environment.wslUser.empty())
    {
        arguments.emplace_back(L"--user");
        arguments.emplace_back(environment.wslUser);
    }
    arguments.insert(arguments.end(), {
                                          L"--exec",
                                          L"/usr/bin/kill",
                                          std::wstring{ signal },
                                          L"--",
                                          L"-" + std::to_wstring(processGroup),
                                      });
    return arguments;
}

TerminalApp::GitStatusProvider::GitStatusProvider() = default;

TerminalApp::GitStatusProvider::GitStatusProvider(GitStatusProviderOptions options) :
    _options{ std::move(options) }
{
}

void TerminalApp::GitStatusProvider::QueryAsync(
    GitQueryRequest request,
    GitQueryCompletion completion) const
{
    if (!completion)
    {
        LOG_HR_MSG(E_INVALIDARG, "Git status query requires a completion callback.");
        return;
    }

    const auto options = _options;
    try
    {
        auto queuedRequest = request;
        auto queuedCompletion = completion;
        std::thread{
            [options, request = std::move(queuedRequest), completion = std::move(queuedCompletion)]() mutable {
                GitQueryResult result;
                result.status = GitQueryStatus::Failed;
                try
                {
                    result = Query(options, request);
                    if (result.status != GitQueryStatus::Ready &&
                        result.status != GitQueryStatus::Cancelled &&
                        result.status != GitQueryStatus::NoRepository &&
                        result.status != GitQueryStatus::UnsupportedEnvironment)
                    {
                        LOG_HR_MSG(E_FAIL,
                                   "Git status query completed with status %u.",
                                   static_cast<uint32_t>(result.status));
                    }
                }
                catch (...)
                {
                    const auto error = wil::ResultFromCaughtException();
                    LOG_HR_MSG(error, "Git status query failed.");
                    result.error = { static_cast<int>(error), std::system_category() };
                }

                try
                {
                    completion(std::move(result));
                }
                catch (...)
                {
                    LOG_CAUGHT_EXCEPTION();
                }
            }
        }.detach();
    }
    catch (...)
    {
        const auto error = wil::ResultFromCaughtException();
        LOG_HR_MSG(error, "Could not schedule Git status query.");
        GitQueryResult result;
        result.status = GitQueryStatus::Failed;
        result.error = { static_cast<int>(error), std::system_category() };
        try
        {
            completion(std::move(result));
        }
        catch (...)
        {
            LOG_CAUGHT_EXCEPTION();
        }
    }
}
