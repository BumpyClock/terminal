// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "StatusBarCoordinator.h"

#include <atomic>
#include <array>

using namespace ::Microsoft::Terminal::StatusBar;

namespace
{
    constexpr std::chrono::milliseconds EventDebounce{ 250 };
    constexpr std::chrono::seconds PollInterval{ 5 };
    constexpr std::array FailureBackoff{
        std::chrono::seconds{ 15 },
        std::chrono::seconds{ 30 },
        std::chrono::seconds{ 60 },
    };

    class DispatcherStatusBarTimer final : public ::TerminalApp::IStatusBarTimer
    {
    public:
        void Arm(const Clock::time_point due, std::function<void()> callback) override
        {
            Cancel();
            _callback = std::move(callback);

            const auto now{ Clock::now() };
            const auto remaining{ due > now ? due - now : Clock::duration::zero() };
            const auto interval{ std::max(std::chrono::duration_cast<std::chrono::milliseconds>(remaining),
                                         std::chrono::milliseconds{ 1 }) };

            _timer.Interval(interval);
            _timer.Tick([this](auto&&, auto&&) {
                _timer.Stop();
                auto callback{ std::move(_callback) };
                _callback = {};
                if (callback)
                {
                    callback();
                }
            });
            _timer.Start();
        }

        void Cancel() noexcept override
        {
            _timer.Stop();
            _callback = {};
        }

    private:
        SafeDispatcherTimer _timer;
        std::function<void()> _callback;
    };
}

namespace TerminalApp
{
    struct StatusBarCoordinator::Lifetime
    {
        std::atomic<StatusBarCoordinator*> owner{ nullptr };
    };

    std::shared_ptr<StatusBarCoordinator> StatusBarCoordinator::CreateForDispatcher(
        const winrt::Windows::System::DispatcherQueue& dispatcher,
        GitStatusProvider provider,
        PublishFunction publish)
    {
        Dependencies dependencies;
        dependencies.timer = std::make_unique<DispatcherStatusBarTimer>();
        dependencies.now = [] { return Clock::now(); };
        dependencies.query = [provider = std::move(provider)](GitQueryRequest request, GitQueryCompletion completion) {
            provider.QueryAsync(std::move(request), std::move(completion));
        };
        dependencies.dispatch = [dispatcher](std::function<void()> callback) {
            return dispatcher &&
                   dispatcher.TryEnqueue([callback = std::move(callback)]() mutable {
                       callback();
                   });
        };
        dependencies.publish = std::move(publish);

        return std::make_shared<StatusBarCoordinator>(std::move(dependencies));
    }

    StatusBarCoordinator::StatusBarCoordinator(Dependencies dependencies) :
        _dependencies{ std::move(dependencies) },
        _lifetime{ std::make_shared<Lifetime>() }
    {
        THROW_HR_IF(E_INVALIDARG,
                    !_dependencies.timer ||
                        !_dependencies.now ||
                        !_dependencies.query ||
                        !_dependencies.dispatch ||
                        !_dependencies.publish);
        _lifetime->owner.store(this, std::memory_order_release);
    }

    StatusBarCoordinator::~StatusBarCoordinator()
    {
        Close();
    }

    void StatusBarCoordinator::UpdateRuntimeState(
        const bool enabled,
        const bool presentationVisible,
        const bool windowVisible,
        const bool foreground)
    {
        if (_closed)
        {
            return;
        }

        const auto couldQuery{ _CanQuery() };
        const auto visibilityChanged{ _enabled != enabled || _presentationVisible != presentationVisible };

        _enabled = enabled;
        _presentationVisible = presentationVisible;
        _windowVisible = windowVisible;
        _foreground = foreground;

        const auto canQuery{ _CanQuery() };
        if (!_enabled || !_presentationVisible)
        {
            _CancelTimer();
            _CancelInFlight();
            _refreshPending = false;
            _snapshot.reset();
            _Publish();
            return;
        }

        if (!canQuery)
        {
            _CancelTimer();
            _CancelInFlight();
            _refreshPending = false;
            if (visibilityChanged || couldQuery)
            {
                _Publish();
            }
            return;
        }

        if (visibilityChanged)
        {
            _Publish();
        }

        if (!couldQuery)
        {
            _CancelTimer();
            _RequestQueryNow();
        }
    }

    void StatusBarCoordinator::SetShellContext(std::optional<ShellContext> context)
    {
        if (_closed)
        {
            return;
        }

        if (context && _context &&
            InlineIsEqualGUID(context->connectionId, _context->connectionId) &&
            context->sequence <= _context->sequence)
        {
            return;
        }

        const auto sameRepository{ context && _context && _IsSameRepositoryContext(*context, *_context) };

        _CancelTimer();
        _CancelInFlight();
        _refreshPending = false;
        _failureCount = 0;
        _context = std::move(context);

        if (!_context || !_IsEligible(*_context))
        {
            _snapshot.reset();
            _Publish();
            return;
        }

        if (!sameRepository)
        {
            _snapshot.reset();
        }
        _Publish();
        _Arm(_context->phase == ShellContextPhase::Prompt ? EventDebounce : PollInterval);
    }

    void StatusBarCoordinator::Close() noexcept
    {
        if (_closed)
        {
            return;
        }

        _closed = true;
        _CancelTimer();
        _CancelInFlight();
        _refreshPending = false;
        _dependencies.publish = {};
        _lifetime->owner.store(nullptr, std::memory_order_release);
    }

    bool StatusBarCoordinator::_CanQuery() const noexcept
    {
        return !_closed &&
               _enabled &&
               _presentationVisible &&
               _windowVisible &&
               _foreground &&
               _context &&
               _IsEligible(*_context);
    }

    bool StatusBarCoordinator::_IsEligible(const ShellContext& context) noexcept
    {
        return context.environment.IsEligible() &&
               context.pathState == ShellContextPathState::FileSystem &&
               context.phase != ShellContextPhase::Unknown &&
               context.provenance == ShellContextProvenance::ShellReported &&
               !context.path.empty();
    }

    bool StatusBarCoordinator::_IsSameRepositoryContext(const ShellContext& left, const ShellContext& right) noexcept
    {
        return InlineIsEqualGUID(left.connectionId, right.connectionId) &&
               left.environment == right.environment &&
               left.pathState == right.pathState &&
               left.path == right.path;
    }

    void StatusBarCoordinator::_Publish()
    {
        if (_closed || !_dependencies.publish)
        {
            return;
        }

        StatusBarSnapshot snapshot;
        snapshot.visible = _enabled && _presentationVisible;
        snapshot.git = _snapshot;
        _dependencies.publish(std::move(snapshot));
    }

    void StatusBarCoordinator::_Arm(const std::chrono::milliseconds delay)
    {
        if (!_CanQuery())
        {
            return;
        }

        _CancelTimer();
        const auto token{ ++_timerToken };
        const auto weakLifetime{ std::weak_ptr<Lifetime>{ _lifetime } };
        _dependencies.timer->Arm(_dependencies.now() + delay, [weakLifetime, token]() {
            if (const auto lifetime{ weakLifetime.lock() })
            {
                if (const auto owner{ lifetime->owner.load(std::memory_order_acquire) })
                {
                    owner->_OnTimer(token);
                }
            }
        });
    }

    void StatusBarCoordinator::_CancelTimer() noexcept
    {
        ++_timerToken;
        if (_dependencies.timer)
        {
            _dependencies.timer->Cancel();
        }
    }

    void StatusBarCoordinator::_OnTimer(const uint64_t token)
    {
        if (_closed || token != _timerToken)
        {
            return;
        }

        _RequestQueryNow();
    }

    void StatusBarCoordinator::_RequestQueryNow()
    {
        if (!_CanQuery())
        {
            return;
        }

        if (_inFlight)
        {
            _refreshPending = true;
            return;
        }

        _refreshPending = false;
        InFlight inFlight{
            .requestId = ++_nextRequestId,
        };
        const auto requestId{ inFlight.requestId };
        const auto cancellation{ inFlight.cancellation.get_token() };
        _inFlight.emplace(std::move(inFlight));

        GitQueryRequest request{
            .environment = _context->environment,
            .directory = _context->path,
            .cancellation = cancellation,
        };

        const auto weakLifetime{ std::weak_ptr<Lifetime>{ _lifetime } };
        const auto dispatch{ _dependencies.dispatch };
        auto completion = [weakLifetime, dispatch, requestId](GitQueryResult result) mutable {
            const auto dispatched = dispatch([weakLifetime, requestId, result = std::move(result)]() mutable {
                if (const auto lifetime{ weakLifetime.lock() })
                {
                    if (const auto owner{ lifetime->owner.load(std::memory_order_acquire) })
                    {
                        owner->_OnQueryCompleted(requestId, std::move(result));
                    }
                }
            });
            if (!dispatched)
            {
                LOG_HR_MSG(E_ABORT, "Could not enqueue Git status completion on the UI dispatcher.");
            }
        };

        try
        {
            _dependencies.query(std::move(request), std::move(completion));
        }
        catch (...)
        {
            const auto error{ wil::ResultFromCaughtException() };
            LOG_HR_MSG(error, "Git status provider threw before scheduling a query.");
            GitQueryResult result{
                .status = GitQueryStatus::Failed,
                .error = { static_cast<int>(error), std::system_category() },
            };
            _OnQueryCompleted(requestId, std::move(result));
        }
    }

    void StatusBarCoordinator::_OnQueryCompleted(const uint64_t requestId, GitQueryResult result)
    {
        if (_closed || !_inFlight || _inFlight->requestId != requestId)
        {
            return;
        }

        // Every context or eligibility change cancels this request. Keep its slot until
        // completion so cancellation cleanup cannot overlap a replacement query.
        const auto cancellationRequested{ _inFlight->cancellation.stop_requested() };
        _inFlight.reset();

        if (cancellationRequested || result.status == GitQueryStatus::Cancelled)
        {
            if (_refreshPending && _CanQuery())
            {
                _RequestQueryNow();
            }
            return;
        }

        if (result.status == GitQueryStatus::Ready && result.IsValid())
        {
            _snapshot = std::move(result.snapshot);
            _failureCount = 0;
            _Publish();
            _ScheduleAfterCompletion(PollInterval);
            return;
        }

        _snapshot.reset();
        _Publish();

        if (result.status == GitQueryStatus::NoRepository && result.IsValid())
        {
            _failureCount = 0;
            _ScheduleAfterCompletion(PollInterval);
        }
        else
        {
            _ScheduleAfterCompletion(_NextFailureBackoff());
        }
    }

    void StatusBarCoordinator::_CancelInFlight() noexcept
    {
        if (_inFlight)
        {
            _inFlight->cancellation.request_stop();
        }
    }

    void StatusBarCoordinator::_ScheduleAfterCompletion(const std::chrono::milliseconds delay)
    {
        if (_refreshPending && _CanQuery())
        {
            _RequestQueryNow();
        }
        else
        {
            _Arm(delay);
        }
    }

    std::chrono::milliseconds StatusBarCoordinator::_NextFailureBackoff() noexcept
    {
        const auto index{ std::min(_failureCount, FailureBackoff.size() - 1) };
        ++_failureCount;
        return FailureBackoff[index];
    }
}
