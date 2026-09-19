// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "../inc/ShellContext.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <functional>
#include <optional>
#include <shellapi.h>
#include <span>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>
#include <wil/resource.h>

namespace Microsoft::Terminal::TerminalConnection::ShellIntegration
{
    struct ShellIntegrationLaunchPolicy
    {
        bool enabled{ false };
        Microsoft::Terminal::StatusBar::ExecutionEnvironment environment;
        std::wstring helperAssetRoot;
    };

    using WslDefaultShellResolver = std::function<std::optional<std::wstring>(std::wstring_view executable,
                                                                              std::wstring_view distro,
                                                                              std::wstring_view user,
                                                                              std::stop_token cancellation)>;

    enum class ShellKind
    {
        None,
        PowerShell,
        GitBash,
        WslBash,
        WslZsh,
    };

    struct EnvironmentEdit
    {
        std::wstring name;
        std::wstring value;
    };

    struct PreparedLaunch
    {
        std::wstring commandline;
        std::vector<EnvironmentEdit> environment;
        ShellKind shell{ ShellKind::None };
        bool addAssetRootToWslEnv{ false };
        Microsoft::Terminal::StatusBar::ExecutionEnvironment executionEnvironment;
        bool useBashLoginPromptBootstrap{ false };

        [[nodiscard]] bool Integrated() const noexcept
        {
            return shell != ShellKind::None;
        }
    };

    namespace details
    {
        inline std::wstring Lowercase(std::wstring_view value)
        {
            std::wstring result{ value };
            std::transform(result.begin(), result.end(), result.begin(), [](const wchar_t ch) {
                return static_cast<wchar_t>(std::towlower(ch));
            });
            return result;
        }

        inline bool Equals(const std::wstring_view lhs, const std::wstring_view rhs)
        {
            return _wcsicmp(std::wstring{ lhs }.c_str(), std::wstring{ rhs }.c_str()) == 0;
        }

        inline std::vector<std::wstring> ParseCommandLine(const std::wstring_view commandline)
        {
            int argc{};
            const std::wstring input{ commandline };
            wil::unique_hlocal_ptr<LPWSTR[]> argv{ CommandLineToArgvW(input.c_str(), &argc) };
            if (!argv || argc <= 0)
            {
                return {};
            }

            std::vector<std::wstring> result;
            result.reserve(static_cast<size_t>(argc));
            for (int i = 0; i < argc; ++i)
            {
                result.emplace_back(argv[i]);
            }
            return result;
        }

        inline std::wstring QuoteArgument(const std::wstring_view argument)
        {
            if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos)
            {
                return std::wstring{ argument };
            }

            std::wstring result(1, L'"');
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
                    result.append(backslashes * 2 + 1, L'\\');
                    result.push_back(ch);
                    backslashes = 0;
                    continue;
                }

                result.append(backslashes, L'\\');
                backslashes = 0;
                result.push_back(ch);
            }
            result.append(backslashes * 2, L'\\');
            result.push_back(L'"');
            return result;
        }

        inline std::wstring BuildCommandLine(const std::vector<std::wstring>& arguments)
        {
            std::wstring result;
            for (const auto& argument : arguments)
            {
                if (!result.empty())
                {
                    result.push_back(L' ');
                }
                result.append(QuoteArgument(argument));
            }
            return result;
        }

        inline std::wstring PowerShellLiteral(const std::filesystem::path& path)
        {
            auto result = path.wstring();
            for (size_t index = 0; (index = result.find(L'\'', index)) != std::wstring::npos; index += 2)
            {
                result.insert(index, 1, L'\'');
            }
            return result;
        }

        inline bool IsPowerShell(const std::filesystem::path& executable)
        {
            const auto name = Lowercase(executable.filename().wstring());
            return name == L"pwsh" || name == L"pwsh.exe" ||
                   name == L"powershell" || name == L"powershell.exe";
        }

        inline bool IsWsl(const std::filesystem::path& executable)
        {
            const auto name = Lowercase(executable.filename().wstring());
            return name == L"wsl" || name == L"wsl.exe";
        }

        inline bool IsGitForWindowsBash(const std::filesystem::path& executable)
        {
            std::error_code error;
            if (!executable.is_absolute() ||
                Lowercase(executable.filename().wstring()) != L"bash.exe" ||
                !std::filesystem::is_regular_file(executable, error))
            {
                return false;
            }

            auto root = executable.parent_path();
            if (Lowercase(root.filename().wstring()) != L"bin")
            {
                return false;
            }
            root = root.parent_path();
            if (Lowercase(root.filename().wstring()) == L"usr")
            {
                root = root.parent_path();
            }
            return std::filesystem::is_regular_file(root / L"cmd" / L"git.exe", error);
        }

        struct BashArguments
        {
            bool supported{ true };
            bool login{ false };
            bool skipProfile{ false };
            bool skipRc{ false };
        };

        inline BashArguments ParseBashArguments(const std::span<const std::wstring> arguments)
        {
            BashArguments result;
            for (const auto& argument : arguments)
            {
                if (argument == L"--login")
                {
                    result.login = true;
                }
                else if (argument == L"--noprofile")
                {
                    result.skipProfile = true;
                }
                else if (argument == L"--norc")
                {
                    result.skipRc = true;
                }
                else if (argument.starts_with(L"-") && !argument.starts_with(L"--"))
                {
                    for (const auto flag : argument.substr(1))
                    {
                        if (flag == L'l')
                        {
                            result.login = true;
                        }
                        else if (flag != L'i')
                        {
                            result.supported = false;
                        }
                    }
                }
                else
                {
                    result.supported = false;
                }
            }
            return result;
        }

        inline bool ArePowerShellArgumentsSupported(const std::span<const std::wstring> arguments, bool& hasNoExit)
        {
            for (size_t index = 0; index < arguments.size(); ++index)
            {
                auto argument = Lowercase(arguments[index]);
                if (argument == L"-noexit")
                {
                    hasNoExit = true;
                }
                else if (argument == L"-nologo" ||
                         argument == L"-noprofile" ||
                         argument == L"-sta" ||
                         argument == L"-mta" ||
                         argument == L"-login" ||
                         argument == L"-interactive" ||
                         argument == L"-noprofileloadtime")
                {
                }
                else if (argument == L"-workingdirectory" ||
                         argument == L"-executionpolicy" ||
                         argument == L"-inputformat" ||
                         argument == L"-outputformat" ||
                         argument == L"-windowstyle" ||
                         argument == L"-configurationname")
                {
                    if (++index >= arguments.size())
                    {
                        return false;
                    }
                }
                else
                {
                    return false;
                }
            }
            return true;
        }

        inline void AddBashEnvironment(std::vector<EnvironmentEdit>& environment, const BashArguments& arguments)
        {
            environment.emplace_back(L"WT_SHELL_INTEGRATION_BASH_LOGIN", arguments.login ? L"1" : L"0");
            environment.emplace_back(L"WT_SHELL_INTEGRATION_BASH_SKIP_PROFILE", arguments.skipProfile ? L"1" : L"0");
            environment.emplace_back(L"WT_SHELL_INTEGRATION_BASH_SKIP_RC", arguments.skipRc ? L"1" : L"0");
        }

        inline PreparedLaunch PreparePowerShell(const std::wstring_view commandline,
                                                const std::vector<std::wstring>& arguments,
                                                const std::filesystem::path& assetRoot)
        {
            const auto script = assetRoot / L"powershell" / L"windows-terminal.ps1";
            std::error_code error;
            bool hasNoExit{};
            if (!std::filesystem::is_regular_file(script, error) ||
                !ArePowerShellArgumentsSupported(std::span{ arguments }.subspan(1), hasNoExit))
            {
                return { std::wstring{ commandline } };
            }

            std::wstring result{ commandline };
            if (!result.empty() && !std::iswspace(result.back()))
            {
                result.push_back(L' ');
            }
            if (!hasNoExit)
            {
                result.append(L"-NoExit ");
            }
            result.append(L"-Command ");
            result.append(QuoteArgument(L"& { . '" + PowerShellLiteral(script) + L"' }"));
            PreparedLaunch prepared{ std::move(result), {}, ShellKind::PowerShell };
            prepared.executionEnvironment.kind = Microsoft::Terminal::StatusBar::EnvironmentKind::LocalWindows;
            return prepared;
        }

        inline PreparedLaunch PrepareGitBash(const std::wstring_view commandline,
                                             const std::vector<std::wstring>& arguments,
                                             const std::filesystem::path& assetRoot)
        {
            const auto helper = assetRoot / L"bash" / L"windows-terminal.bash";
            const auto bootstrap = assetRoot / L"bash" / L"windows-terminal-bootstrap.bash";
            std::error_code error;
            const auto bashArguments = ParseBashArguments(std::span{ arguments }.subspan(1));
            if (!bashArguments.supported ||
                !std::filesystem::is_regular_file(helper, error) ||
                (bashArguments.login && !std::filesystem::is_regular_file(bootstrap, error)))
            {
                return { std::wstring{ commandline } };
            }

            PreparedLaunch result;
            result.environment.emplace_back(L"WT_SHELL_INTEGRATION_ROOT", assetRoot.wstring());
            AddBashEnvironment(result.environment, bashArguments);
            result.shell = ShellKind::GitBash;
            result.executionEnvironment.kind = Microsoft::Terminal::StatusBar::EnvironmentKind::LocalWindows;
            if (bashArguments.login)
            {
                result.commandline = commandline;
                result.useBashLoginPromptBootstrap = true;
                return result;
            }

            static constexpr std::wstring_view wrapper{
                LR"(__wt_shell=$1; shift; __wt_root=$(cygpath -u "$WT_SHELL_INTEGRATION_ROOT" 2>/dev/null); __wt_helper="$__wt_root/bash/windows-terminal.bash"; if [ -n "$__wt_root" ] && [ -r "$__wt_helper" ]; then exec "$__wt_shell" --noprofile --rcfile "$__wt_helper" -i; fi; unset WT_SHELL_INTEGRATION_ROOT WT_SHELL_INTEGRATION_BASH_LOGIN WT_SHELL_INTEGRATION_BASH_SKIP_PROFILE WT_SHELL_INTEGRATION_BASH_SKIP_RC; exec "$__wt_shell" "$@")"
            };

            std::vector<std::wstring> rewritten{
                arguments[0],
                L"--noprofile",
                L"--norc",
                L"-c",
                std::wstring{ wrapper },
                L"wt-shell-integration",
            };
            rewritten.insert(rewritten.end(), arguments.begin(), arguments.end());

            result.commandline = BuildCommandLine(rewritten);
            return result;
        }

        inline bool IsZshArgumentSupported(const std::wstring_view argument)
        {
            return argument == L"-i" || argument == L"-l" || argument == L"--login" ||
                   argument == L"-il" || argument == L"-li";
        }

        inline PreparedLaunch PrepareWsl(const std::wstring_view commandline,
                                         const std::vector<std::wstring>& arguments,
                                         const ShellIntegrationLaunchPolicy& policy,
                                         const std::filesystem::path& assetRoot)
        {
            size_t execIndex = std::wstring::npos;
            std::wstring commandDistro;
            std::wstring commandUser;
            for (size_t index = 1; index < arguments.size(); ++index)
            {
                const auto option = Lowercase(arguments[index]);
                if (option == L"--exec" || option == L"-e")
                {
                    execIndex = index;
                    break;
                }
                if (option == L"--distribution" || option == L"-d" ||
                    option == L"--user" || option == L"-u" ||
                    option == L"--cd")
                {
                    if (++index >= arguments.size())
                    {
                        return { std::wstring{ commandline } };
                    }
                    if (option == L"--distribution" || option == L"-d")
                    {
                        commandDistro = arguments[index];
                    }
                    else if (option == L"--user" || option == L"-u")
                    {
                        commandUser = arguments[index];
                    }
                    continue;
                }
                return { std::wstring{ commandline } };
            }

            if (execIndex == std::wstring::npos || execIndex + 1 >= arguments.size() ||
                (!commandDistro.empty() && !Equals(commandDistro, policy.environment.wslDistro)) ||
                (!commandUser.empty() && !policy.environment.wslUser.empty() && !Equals(commandUser, policy.environment.wslUser)))
            {
                return { std::wstring{ commandline } };
            }

            const auto shellExecutable = arguments[execIndex + 1];
            const auto shellName = Lowercase(std::filesystem::path{ shellExecutable }.filename().wstring());
            const auto shellArguments = std::span{ arguments }.subspan(execIndex + 2);
            ShellKind shell{ ShellKind::None };
            BashArguments bashArguments;
            std::filesystem::path helper;
            if (shellName == L"bash")
            {
                bashArguments = ParseBashArguments(shellArguments);
                helper = assetRoot / L"bash" / L"windows-terminal.bash";
                if (!bashArguments.supported)
                {
                    return { std::wstring{ commandline } };
                }
                shell = ShellKind::WslBash;
            }
            else if (shellName == L"zsh")
            {
                if (!std::all_of(shellArguments.begin(), shellArguments.end(), IsZshArgumentSupported))
                {
                    return { std::wstring{ commandline } };
                }
                helper = assetRoot / L"zsh" / L".zshenv";
                shell = ShellKind::WslZsh;
            }
            else
            {
                return { std::wstring{ commandline } };
            }

            std::error_code error;
            const auto wslBootstrap = assetRoot / L"bash" / L"windows-terminal-wsl.sh";
            if (!std::filesystem::is_regular_file(helper, error) ||
                !std::filesystem::is_regular_file(wslBootstrap, error))
            {
                return { std::wstring{ commandline } };
            }

            static constexpr std::wstring_view wrapper{
                LR"(__wt_shell=$1; shift; __wt_root=$(wslpath -u "$WT_SHELL_INTEGRATION_ROOT" 2>/dev/null); __wt_bootstrap="$__wt_root/bash/windows-terminal-wsl.sh"; if [ -n "$__wt_root" ] && [ -r "$__wt_bootstrap" ]; then exec /bin/sh "$__wt_bootstrap" "$__wt_root" "$__wt_shell" "$@"; fi; unset WT_SHELL_INTEGRATION_ROOT WT_SHELL_INTEGRATION_BASH_LOGIN WT_SHELL_INTEGRATION_BASH_SKIP_PROFILE WT_SHELL_INTEGRATION_BASH_SKIP_RC; exec "$__wt_shell" "$@")"
            };

            std::vector<std::wstring> rewritten{ arguments.begin(), arguments.begin() + execIndex + 1 };
            rewritten.emplace_back(L"/bin/sh");
            rewritten.emplace_back(L"-c");
            rewritten.emplace_back(wrapper);
            rewritten.emplace_back(L"wt-shell-integration");
            rewritten.insert(rewritten.end(), arguments.begin() + execIndex + 1, arguments.end());

            PreparedLaunch result;
            result.commandline = BuildCommandLine(rewritten);
            result.environment.emplace_back(L"WT_SHELL_INTEGRATION_ROOT", assetRoot.wstring());
            if (shell == ShellKind::WslBash)
            {
                AddBashEnvironment(result.environment, bashArguments);
            }
            result.shell = shell;
            result.addAssetRootToWslEnv = true;
            result.executionEnvironment = policy.environment;
            if (!commandUser.empty())
            {
                result.executionEnvironment.wslUser = std::move(commandUser);
            }
            return result;
        }

        inline std::optional<Microsoft::Terminal::StatusBar::ExecutionEnvironment> ResolveAutomaticEnvironment(const std::vector<std::wstring>& arguments)
        {
            if (arguments.empty())
            {
                return std::nullopt;
            }

            const std::filesystem::path executable{ arguments.front() };
            if (IsPowerShell(executable) || IsGitForWindowsBash(executable))
            {
                return Microsoft::Terminal::StatusBar::ExecutionEnvironment{
                    .kind = Microsoft::Terminal::StatusBar::EnvironmentKind::LocalWindows
                };
            }
            if (!IsWsl(executable))
            {
                return std::nullopt;
            }

            std::wstring distro;
            std::wstring user;
            size_t execIndex = std::wstring::npos;
            for (size_t index = 1; index < arguments.size(); ++index)
            {
                const auto option = Lowercase(arguments[index]);
                if (option == L"--exec" || option == L"-e")
                {
                    execIndex = index;
                    break;
                }
                if (option == L"--distribution" || option == L"-d" ||
                    option == L"--user" || option == L"-u" ||
                    option == L"--cd")
                {
                    if (++index >= arguments.size())
                    {
                        return std::nullopt;
                    }
                    if (option == L"--distribution" || option == L"-d")
                    {
                        distro = arguments[index];
                    }
                    else if (option == L"--user" || option == L"-u")
                    {
                        user = arguments[index];
                    }
                    continue;
                }
                return std::nullopt;
            }

            if (distro.empty() || execIndex == std::wstring::npos || execIndex + 1 >= arguments.size())
            {
                return std::nullopt;
            }

            const auto shell = Lowercase(std::filesystem::path{ arguments[execIndex + 1] }.filename().wstring());
            if (shell != L"bash" && shell != L"zsh")
            {
                return std::nullopt;
            }
            return Microsoft::Terminal::StatusBar::ExecutionEnvironment{
                .kind = Microsoft::Terminal::StatusBar::EnvironmentKind::Wsl,
                .wslDistro = std::move(distro),
                .wslUser = std::move(user),
            };
        }

        struct NamedWslDefaultShell
        {
            Microsoft::Terminal::StatusBar::ExecutionEnvironment environment;
            std::vector<std::wstring> arguments;
        };

        inline bool IsSupportedResolvedShell(const std::wstring_view shell)
        {
            if (shell.empty() || shell.front() != L'/')
            {
                return false;
            }
            for (const auto ch : shell)
            {
                if (!(std::iswalnum(ch) || ch == L'/' || ch == L'_' || ch == L'-' || ch == L'.' || ch == L'+'))
                {
                    return false;
                }
            }
            const auto name = Lowercase(std::filesystem::path{ shell }.filename().wstring());
            return name == L"bash" || name == L"zsh";
        }

        inline std::optional<NamedWslDefaultShell> ResolveNamedWslDefaultShell(const std::vector<std::wstring>& arguments,
                                                                               const WslDefaultShellResolver& resolver,
                                                                               const std::stop_token cancellation)
        {
            if (cancellation.stop_requested() ||
                !resolver ||
                arguments.empty() ||
                !IsWsl(std::filesystem::path{ arguments.front() }))
            {
                return std::nullopt;
            }

            std::wstring distro;
            std::wstring user;
            for (size_t index = 1; index < arguments.size(); ++index)
            {
                const auto option = Lowercase(arguments[index]);
                if (option == L"--exec" || option == L"-e")
                {
                    return std::nullopt;
                }
                if (option == L"--distribution" || option == L"-d" ||
                    option == L"--user" || option == L"-u" ||
                    option == L"--cd")
                {
                    if (++index >= arguments.size())
                    {
                        return std::nullopt;
                    }
                    if (option == L"--distribution" || option == L"-d")
                    {
                        distro = arguments[index];
                    }
                    else if (option == L"--user" || option == L"-u")
                    {
                        user = arguments[index];
                    }
                    continue;
                }
                return std::nullopt;
            }
            if (distro.empty())
            {
                return std::nullopt;
            }

            const auto shell = resolver(arguments.front(), distro, user, cancellation);
            if (cancellation.stop_requested() || !shell || !IsSupportedResolvedShell(*shell))
            {
                return std::nullopt;
            }

            auto rewritten = arguments;
            rewritten.emplace_back(L"--exec");
            rewritten.emplace_back(*shell);
            rewritten.emplace_back(L"-l");
            return NamedWslDefaultShell{
                .environment = {
                    .kind = Microsoft::Terminal::StatusBar::EnvironmentKind::Wsl,
                    .wslDistro = std::move(distro),
                    .wslUser = std::move(user),
                },
                .arguments = std::move(rewritten),
            };
        }
    }

    inline PreparedLaunch PrepareLaunch(const std::wstring_view commandline,
                                        const ShellIntegrationLaunchPolicy& policy)
    {
        PreparedLaunch unchanged{ std::wstring{ commandline } };
        if (!policy.enabled || !policy.environment.IsEligible() || policy.helperAssetRoot.empty())
        {
            return unchanged;
        }

        const auto arguments = details::ParseCommandLine(commandline);
        if (arguments.empty())
        {
            return unchanged;
        }

        const std::filesystem::path executable{ arguments.front() };
        const std::filesystem::path assetRoot{ policy.helperAssetRoot };
        if (!assetRoot.is_absolute())
        {
            return unchanged;
        }
        if (policy.environment.kind == Microsoft::Terminal::StatusBar::EnvironmentKind::LocalWindows)
        {
            if (details::IsPowerShell(executable))
            {
                return details::PreparePowerShell(commandline, arguments, assetRoot);
            }
            if (details::IsGitForWindowsBash(executable))
            {
                return details::PrepareGitBash(commandline, arguments, assetRoot);
            }
        }
        else if (policy.environment.kind == Microsoft::Terminal::StatusBar::EnvironmentKind::Wsl &&
                 details::IsWsl(executable))
        {
            return details::PrepareWsl(commandline, arguments, policy, assetRoot);
        }
        return unchanged;
    }

    inline PreparedLaunch PrepareLaunchAutomatically(const std::wstring_view commandline,
                                                      const bool enabled,
                                                      const std::wstring_view helperAssetRoot,
                                                      const WslDefaultShellResolver& wslDefaultShellResolver = {},
                                                      const std::stop_token cancellation = {})
    {
        PreparedLaunch unchanged{ std::wstring{ commandline } };
        if (!enabled || helperAssetRoot.empty())
        {
            return unchanged;
        }

        const auto arguments = details::ParseCommandLine(commandline);
        if (const auto environment = details::ResolveAutomaticEnvironment(arguments))
        {
            ShellIntegrationLaunchPolicy policy;
            policy.enabled = true;
            policy.environment = *environment;
            policy.helperAssetRoot = helperAssetRoot;
            return PrepareLaunch(commandline, policy);
        }

        const auto namedWsl = details::ResolveNamedWslDefaultShell(arguments, wslDefaultShellResolver, cancellation);
        if (!namedWsl)
        {
            return unchanged;
        }
        ShellIntegrationLaunchPolicy policy;
        policy.enabled = true;
        policy.environment = namedWsl->environment;
        policy.helperAssetRoot = helperAssetRoot;
        return details::PrepareWsl(details::BuildCommandLine(namedWsl->arguments),
                                   namedWsl->arguments,
                                   policy,
                                   std::filesystem::path{ helperAssetRoot });
    }

    inline bool RequiresWslDefaultShellDiscovery(const std::wstring_view commandline)
    {
        const auto arguments = details::ParseCommandLine(commandline);
        if (arguments.empty() || !details::IsWsl(std::filesystem::path{ arguments.front() }))
        {
            return false;
        }

        bool hasDistro{};
        for (size_t index = 1; index < arguments.size(); ++index)
        {
            const auto option = details::Lowercase(arguments[index]);
            if (option == L"--exec" || option == L"-e")
            {
                return false;
            }
            if (option == L"--distribution" || option == L"-d" ||
                option == L"--user" || option == L"-u" ||
                option == L"--cd")
            {
                if (++index >= arguments.size())
                {
                    return false;
                }
                if (option == L"--distribution" || option == L"-d")
                {
                    hasDistro = true;
                }
                continue;
            }
            return false;
        }
        return hasDistro;
    }
}
