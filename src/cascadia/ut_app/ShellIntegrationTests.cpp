// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "precomp.h"
#include "../TerminalConnection/ShellIntegrationLaunch.h"

#include <fstream>
#include <winrt/Microsoft.Terminal.TerminalConnection.h>

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::StatusBar;
using namespace Microsoft::Terminal::TerminalConnection::ShellIntegration;

namespace
{
    winrt::Microsoft::Terminal::TerminalConnection::ConptyConnection ActivateConptyConnectionForTest()
    {
        static const auto module = [] {
            // Resolve beside this test DLL, not beside the TAEF executable.
            HMODULE testModule{};
            VERIFY_WIN32_BOOL_SUCCEEDED(GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&ActivateConptyConnectionForTest),
                &testModule));
            std::filesystem::path path{ wil::GetModuleFileNameW<std::wstring>(testModule) };
            path.replace_filename(L"TerminalConnection.dll");
            wil::unique_hmodule loaded{ LoadLibraryExW(
                path.c_str(),
                nullptr,
                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS) };
            VERIFY_WIN32_BOOL_SUCCEEDED(loaded.get() != nullptr);
            return loaded;
        }();
        VERIFY_IS_NOT_NULL(module.get());

        using GetActivationFactory = HRESULT(WINAPI*)(HSTRING, ::IActivationFactory**);
        const auto getActivationFactory = reinterpret_cast<GetActivationFactory>(GetProcAddress(module.get(), "DllGetActivationFactory"));
        VERIFY_IS_NOT_NULL(getActivationFactory);

        const winrt::hstring className{ L"Microsoft.Terminal.TerminalConnection.ConptyConnection" };
        winrt::com_ptr<::IActivationFactory> factory;
        const auto factoryResult = getActivationFactory(static_cast<HSTRING>(winrt::get_abi(className)), factory.put());
        VERIFY_SUCCEEDED(factoryResult);

        winrt::com_ptr<::IInspectable> instance;
        const auto activationResult = factory->ActivateInstance(instance.put());
        VERIFY_SUCCEEDED(activationResult);

        winrt::Windows::Foundation::IInspectable inspectable{ instance.detach(), winrt::take_ownership_from_abi };
        return inspectable.as<winrt::Microsoft::Terminal::TerminalConnection::ConptyConnection>();
    }

    struct TemporaryAssets
    {
        TemporaryAssets()
        {
            GUID id{};
            VERIFY_SUCCEEDED(CoCreateGuid(&id));
            root = std::filesystem::temp_directory_path() /
                   L"WindowsTerminalShellIntegrationTests" /
                   (std::wstring{ winrt::to_hstring(id) } + L" path & O'Brien");
            gitRoot = root / L"Git";

            Touch(root / L"powershell" / L"windows-terminal.ps1");
            Touch(root / L"bash" / L"windows-terminal.bash");
            Touch(root / L"bash" / L"windows-terminal-bootstrap.bash");
            Touch(root / L"bash" / L"windows-terminal-wsl.sh");
            Touch(root / L"zsh" / L".zshenv");
            Touch(gitRoot / L"bin" / L"bash.exe");
            Touch(gitRoot / L"cmd" / L"git.exe");
        }

        ~TemporaryAssets()
        {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }

        void Touch(const std::filesystem::path& path)
        {
            std::filesystem::create_directories(path.parent_path());
            std::ofstream{ path };
        }

        std::filesystem::path root;
        std::filesystem::path gitRoot;
    };

    ShellIntegrationLaunchPolicy WindowsPolicy(const TemporaryAssets& assets)
    {
        return {
            .enabled = true,
            .environment = { .kind = EnvironmentKind::LocalWindows },
            .helperAssetRoot = assets.root.wstring(),
        };
    }

    ShellIntegrationLaunchPolicy WslPolicy(const TemporaryAssets& assets)
    {
        return {
            .enabled = true,
            .environment = { .kind = EnvironmentKind::Wsl, .wslDistro = L"Ubuntu" },
            .helperAssetRoot = assets.root.wstring(),
        };
    }
}

class ShellIntegrationTests
{
    TEST_CLASS(ShellIntegrationTests);

    TEST_METHOD(WindowsCommandLineQuotingRoundTrips)
    {
        const std::vector<std::wstring> arguments{
            L"C:\\Program Files\\shell.exe",
            L"plain",
            L"space and trailing slash\\",
            L"quote\"inside",
            L"",
        };
        const auto parsed = details::ParseCommandLine(details::BuildCommandLine(arguments));
        VERIFY_ARE_EQUAL(arguments.size(), parsed.size());
        for (size_t index = 0; index < arguments.size(); ++index)
        {
            VERIFY_ARE_EQUAL(arguments[index], parsed[index]);
        }
    }

    TEST_METHOD(ClosedConnectionDoesNotStart)
    {
        using winrt::Microsoft::Terminal::TerminalConnection::ConnectionState;
        const auto connection = ActivateConptyConnectionForTest();
        winrt::Windows::Foundation::Collections::ValueSet settings;
        settings.Insert(L"commandline", winrt::box_value(L"must-not-launch.exe"));
        connection.Initialize(settings);
        connection.Close();
        connection.Start();
        VERIFY_ARE_EQUAL(static_cast<int>(ConnectionState::Closed), static_cast<int>(connection.State()));
    }

    TEST_METHOD(PowerShellRecognizesOnlyInteractiveLaunches)
    {
        TemporaryAssets assets;
        const auto policy = WindowsPolicy(assets);

        const auto integrated = PrepareLaunch(L"pwsh.exe -NoProfile", policy);
        VERIFY_ARE_EQUAL(static_cast<int>(ShellKind::PowerShell), static_cast<int>(integrated.shell));
        VERIFY_IS_TRUE(integrated.commandline.find(L"-NoExit") != std::wstring::npos);
        VERIFY_IS_TRUE(integrated.commandline.find(L"windows-terminal.ps1") != std::wstring::npos);
        VERIFY_IS_TRUE(integrated.commandline.find(L"O''Brien") != std::wstring::npos);

        const auto payload = PrepareLaunch(L"pwsh.exe -File script.ps1", policy);
        VERIFY_IS_FALSE(payload.Integrated());
        VERIFY_ARE_EQUAL(std::wstring{ L"pwsh.exe -File script.ps1" }, payload.commandline);

        const auto nonInteractive = PrepareLaunch(L"powershell.exe -NonInteractive", policy);
        VERIFY_IS_FALSE(nonInteractive.Integrated());
    }

    TEST_METHOD(AutomaticConfigurationOwnsEnvironmentRecognition)
    {
        TemporaryAssets assets;

        const auto powershell = PrepareLaunchAutomatically(L"pwsh.exe -NoProfile", true, assets.root.wstring());
        VERIFY_ARE_EQUAL(static_cast<int>(ShellKind::PowerShell), static_cast<int>(powershell.shell));
        VERIFY_ARE_EQUAL(static_cast<int>(EnvironmentKind::LocalWindows), static_cast<int>(powershell.executionEnvironment.kind));

        const auto gitBashCommand = L"\"" + (assets.gitRoot / L"bin" / L"bash.exe").wstring() + L"\" -i";
        const auto gitBash = PrepareLaunchAutomatically(gitBashCommand, true, assets.root.wstring());
        VERIFY_ARE_EQUAL(static_cast<int>(ShellKind::GitBash), static_cast<int>(gitBash.shell));
        VERIFY_ARE_EQUAL(static_cast<int>(EnvironmentKind::LocalWindows), static_cast<int>(gitBash.executionEnvironment.kind));

        const auto wsl = PrepareLaunchAutomatically(L"wsl.exe -d Ubuntu --user alice -e /bin/zsh -i", true, assets.root.wstring());
        VERIFY_ARE_EQUAL(static_cast<int>(ShellKind::WslZsh), static_cast<int>(wsl.shell));
        VERIFY_ARE_EQUAL(static_cast<int>(EnvironmentKind::Wsl), static_cast<int>(wsl.executionEnvironment.kind));
        VERIFY_ARE_EQUAL(std::wstring{ L"Ubuntu" }, wsl.executionEnvironment.wslDistro);
        VERIFY_ARE_EQUAL(std::wstring{ L"alice" }, wsl.executionEnvironment.wslUser);

        VERIFY_IS_FALSE(PrepareLaunchAutomatically(L"wsl.exe -e bash", true, assets.root.wstring()).Integrated());
        VERIFY_IS_FALSE(PrepareLaunchAutomatically(L"cmd.exe", true, assets.root.wstring()).Integrated());
        VERIFY_IS_FALSE(PrepareLaunchAutomatically(L"pwsh.exe", false, assets.root.wstring()).Integrated());
    }

    TEST_METHOD(NamedWslDefaultShellUsesBoundedResolver)
    {
        TemporaryAssets assets;
        std::wstring observedExecutable;
        std::wstring observedDistro;
        std::wstring observedUser;
        const WslDefaultShellResolver resolver = [&](const auto executable, const auto distro, const auto user, const std::stop_token cancellation) {
            VERIFY_IS_FALSE(cancellation.stop_requested());
            observedExecutable = executable;
            observedDistro = distro;
            observedUser = user;
            return std::optional<std::wstring>{ L"/usr/bin/zsh" };
        };

        const auto resolved = PrepareLaunchAutomatically(
            L"wsl.exe -d Ubuntu --user alice --cd /work",
            true,
            assets.root.wstring(),
            resolver);
        VERIFY_ARE_EQUAL(static_cast<int>(ShellKind::WslZsh), static_cast<int>(resolved.shell));
        VERIFY_ARE_EQUAL(std::wstring{ L"wsl.exe" }, observedExecutable);
        VERIFY_ARE_EQUAL(std::wstring{ L"Ubuntu" }, observedDistro);
        VERIFY_ARE_EQUAL(std::wstring{ L"alice" }, observedUser);
        VERIFY_IS_TRUE(resolved.commandline.find(L"--cd /work") != std::wstring::npos);
        VERIFY_IS_TRUE(resolved.commandline.find(L"/usr/bin/zsh") != std::wstring::npos);
        VERIFY_ARE_EQUAL(std::wstring{ L"Ubuntu" }, resolved.executionEnvironment.wslDistro);
        VERIFY_ARE_EQUAL(std::wstring{ L"alice" }, resolved.executionEnvironment.wslUser);

        const WslDefaultShellResolver unsupported = [](auto&&...) {
            return std::optional<std::wstring>{ L"/usr/bin/fish" };
        };
        VERIFY_IS_FALSE(PrepareLaunchAutomatically(
                            L"wsl.exe -d Ubuntu",
                            true,
                            assets.root.wstring(),
                            unsupported)
                            .Integrated());
        const WslDefaultShellResolver payload = [](auto&&...) {
            return std::optional<std::wstring>{ L"/bin/bash -c payload" };
        };
        VERIFY_IS_FALSE(PrepareLaunchAutomatically(
                            L"wsl.exe -d Ubuntu",
                            true,
                            assets.root.wstring(),
                            payload)
                            .Integrated());
        VERIFY_IS_FALSE(PrepareLaunchAutomatically(
                            L"wsl.exe -d Ubuntu",
                            true,
                            assets.root.wstring())
                            .Integrated());

        std::stop_source cancelled;
        cancelled.request_stop();
        bool resolverCalled{};
        const WslDefaultShellResolver cancelledResolver = [&](auto&&...) {
            resolverCalled = true;
            return std::optional<std::wstring>{ L"/bin/bash" };
        };
        VERIFY_IS_FALSE(PrepareLaunchAutomatically(
                            L"wsl.exe -d Ubuntu",
                            true,
                            assets.root.wstring(),
                            cancelledResolver,
                            cancelled.get_token())
                            .Integrated());
        VERIFY_IS_FALSE(resolverCalled);

        std::stop_source cancelledDuringResolution;
        const WslDefaultShellResolver cancellingResolver = [&](auto&&...) {
            cancelledDuringResolution.request_stop();
            return std::optional<std::wstring>{ L"/bin/bash" };
        };
        VERIFY_IS_FALSE(PrepareLaunchAutomatically(
                            L"wsl.exe -d Ubuntu",
                            true,
                            assets.root.wstring(),
                            cancellingResolver,
                            cancelledDuringResolution.get_token())
                            .Integrated());
        VERIFY_IS_TRUE(RequiresWslDefaultShellDiscovery(L"wsl.exe -d Ubuntu --user alice --cd /work"));
        VERIFY_IS_FALSE(RequiresWslDefaultShellDiscovery(L"wsl.exe -d Ubuntu -e bash"));
    }

    TEST_METHOD(GitBashRequiresAValidatedGitForWindowsLayout)
    {
        TemporaryAssets assets;
        const auto policy = WindowsPolicy(assets);
        const auto commandline = L"\"" + (assets.gitRoot / L"bin" / L"bash.exe").wstring() + L"\" -li";

        const auto integrated = PrepareLaunch(commandline, policy);
        VERIFY_ARE_EQUAL(static_cast<int>(ShellKind::GitBash), static_cast<int>(integrated.shell));
        VERIFY_ARE_EQUAL(commandline, integrated.commandline);
        VERIFY_IS_TRUE(integrated.useBashLoginPromptBootstrap);
        VERIFY_ARE_EQUAL(static_cast<size_t>(4), integrated.environment.size());

        const auto nonLogin = PrepareLaunch(L"\"" + (assets.gitRoot / L"bin" / L"bash.exe").wstring() + L"\" -i", policy);
        VERIFY_IS_TRUE(nonLogin.commandline.find(L"cygpath") != std::wstring::npos);
        VERIFY_IS_FALSE(nonLogin.useBashLoginPromptBootstrap);

        const auto opaquePayload = PrepareLaunch(commandline + L" -c \"echo no\"", policy);
        VERIFY_IS_FALSE(opaquePayload.Integrated());
    }

    TEST_METHOD(WslRequiresAnExplicitSupportedShellAndMatchingDistro)
    {
        TemporaryAssets assets;
        const auto policy = WslPolicy(assets);

        const auto bash = PrepareLaunch(L"wsl.exe -d Ubuntu --exec /bin/bash -li", policy);
        VERIFY_ARE_EQUAL(static_cast<int>(ShellKind::WslBash), static_cast<int>(bash.shell));
        VERIFY_IS_TRUE(bash.addAssetRootToWslEnv);
        VERIFY_IS_TRUE(bash.commandline.find(L"wslpath") != std::wstring::npos);

        const auto zsh = PrepareLaunch(L"wsl.exe --distribution Ubuntu -e /bin/zsh -li", policy);
        VERIFY_ARE_EQUAL(static_cast<int>(ShellKind::WslZsh), static_cast<int>(zsh.shell));

        auto userPolicy = policy;
        userPolicy.environment.wslUser = L"alice";
        const auto user = PrepareLaunch(L"wsl.exe -d Ubuntu --user alice -e /bin/bash -i", userPolicy);
        VERIFY_ARE_EQUAL(std::wstring{ L"alice" }, user.executionEnvironment.wslUser);
        VERIFY_IS_FALSE(PrepareLaunch(L"wsl.exe -d Ubuntu --user bob -e /bin/bash -i", userPolicy).Integrated());

        const auto defaultShell = PrepareLaunch(L"wsl.exe -d Ubuntu", policy);
        VERIFY_IS_FALSE(defaultShell.Integrated());

        const auto wrongDistro = PrepareLaunch(L"wsl.exe -d Debian -e bash", policy);
        VERIFY_IS_FALSE(wrongDistro.Integrated());

        const auto opaquePayload = PrepareLaunch(L"wsl.exe -d Ubuntu -e bash -c pwd", policy);
        VERIFY_IS_FALSE(opaquePayload.Integrated());
    }
};
