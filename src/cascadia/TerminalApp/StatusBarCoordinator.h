// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "GitStatusProvider.h"

#include <chrono>
#include <memory>
#include <winrt/Windows.System.h>

namespace TerminalApp
{
    class IStatusBarTimer
    {
    public:
        using Clock = std::chrono::steady_clock;

        virtual ~IStatusBarTimer() = default;
        virtual void Arm(Clock::time_point due, std::function<void()> callback) = 0;
        virtual void Cancel() noexcept = 0;
    };

    class StatusBarCoordinator final
    {
    public:
        using Clock = IStatusBarTimer::Clock;
        using PublishFunction = std::function<void(::Microsoft::Terminal::StatusBar::StatusBarSnapshot)>;
        using DispatchFunction = std::function<bool(std::function<void()>)>;

        struct Dependencies
        {
            std::unique_ptr<IStatusBarTimer> timer;
            std::function<Clock::time_point()> now;
            ::Microsoft::Terminal::StatusBar::GitQueryFunction query;
            DispatchFunction dispatch;
            PublishFunction publish;
        };

        static std::shared_ptr<StatusBarCoordinator> CreateForDispatcher(
            const winrt::Windows::System::DispatcherQueue& dispatcher,
            GitStatusProvider provider,
            PublishFunction publish);

        explicit StatusBarCoordinator(Dependencies dependencies);
        ~StatusBarCoordinator();

        StatusBarCoordinator(const StatusBarCoordinator&) = delete;
        StatusBarCoordinator& operator=(const StatusBarCoordinator&) = delete;

        void UpdateRuntimeState(bool enabled, bool presentationVisible, bool windowVisible, bool foreground);
        void SetShellContext(std::optional<::Microsoft::Terminal::StatusBar::ShellContext> context);
        void Close() noexcept;

    private:
        struct Lifetime;
        struct InFlight
        {
            uint64_t requestId{ 0 };
            std::stop_source cancellation;
        };

        Dependencies _dependencies;
        std::shared_ptr<Lifetime> _lifetime;

        std::optional<::Microsoft::Terminal::StatusBar::ShellContext> _context;
        std::optional<::Microsoft::Terminal::StatusBar::GitSnapshot> _snapshot;
        std::optional<InFlight> _inFlight;

        bool _enabled{ false };
        bool _presentationVisible{ false };
        bool _windowVisible{ true };
        bool _foreground{ false };
        bool _refreshPending{ false };
        bool _closed{ false };

        uint64_t _nextRequestId{ 0 };
        uint64_t _timerToken{ 0 };
        size_t _failureCount{ 0 };

        [[nodiscard]] bool _CanQuery() const noexcept;
        [[nodiscard]] static bool _IsEligible(const ::Microsoft::Terminal::StatusBar::ShellContext& context) noexcept;
        [[nodiscard]] static bool _IsSameRepositoryContext(
            const ::Microsoft::Terminal::StatusBar::ShellContext& left,
            const ::Microsoft::Terminal::StatusBar::ShellContext& right) noexcept;
        void _Publish();
        void _Arm(std::chrono::milliseconds delay);
        void _CancelTimer() noexcept;
        void _OnTimer(uint64_t token);
        void _RequestQueryNow();
        void _OnQueryCompleted(uint64_t requestId, ::Microsoft::Terminal::StatusBar::GitQueryResult result);
        void _CancelInFlight() noexcept;
        void _ScheduleAfterCompletion(std::chrono::milliseconds delay);
        [[nodiscard]] std::chrono::milliseconds _NextFailureBackoff() noexcept;
    };
}
