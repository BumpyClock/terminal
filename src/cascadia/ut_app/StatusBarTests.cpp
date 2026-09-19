// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "precomp.h"

#include "../TerminalApp/StatusBarCoordinator.h"
#include "../TerminalApp/StatusBarPresentation.h"

using namespace Microsoft::Terminal::StatusBar;
using namespace TerminalApp::StatusBarPresentation;
using namespace WEX::Common;
using namespace WEX::Logging;
using namespace WEX::TestExecution;

namespace
{
    using CoordinatorClock = TerminalApp::IStatusBarTimer::Clock;

    struct FakeClock
    {
        CoordinatorClock::time_point now{};
    };

    class FakeTimer final : public TerminalApp::IStatusBarTimer
    {
    public:
        explicit FakeTimer(FakeClock& clock) :
            _clock{ clock }
        {
        }

        void Arm(const Clock::time_point due, std::function<void()> callback) override
        {
            _due = due;
            _callback = std::move(callback);
        }

        void Cancel() noexcept override
        {
            _due.reset();
            _callback = {};
        }

        void Advance(const std::chrono::milliseconds amount)
        {
            _clock.now += amount;
            while (_due && *_due <= _clock.now)
            {
                _due.reset();
                auto callback{ std::move(_callback) };
                _callback = {};
                callback();
            }
        }

        std::chrono::milliseconds Remaining() const
        {
            VERIFY_IS_TRUE(_due.has_value());
            return std::chrono::duration_cast<std::chrono::milliseconds>(*_due - _clock.now);
        }

        bool IsArmed() const noexcept
        {
            return _due.has_value();
        }

    private:
        FakeClock& _clock;
        std::optional<Clock::time_point> _due;
        std::function<void()> _callback;
    };

    struct PendingQuery
    {
        GitQueryRequest request;
        GitQueryCompletion completion;
    };

    struct CoordinatorHarness
    {
        FakeClock clock;
        FakeTimer* timer{ nullptr };
        bool throwSynchronously{ false };
        bool dispatchSucceeds{ true };
        std::vector<PendingQuery> pending;
        std::vector<StatusBarSnapshot> published;
        std::shared_ptr<TerminalApp::StatusBarCoordinator> coordinator;

        CoordinatorHarness()
        {
            auto fakeTimer{ std::make_unique<FakeTimer>(clock) };
            timer = fakeTimer.get();

            TerminalApp::StatusBarCoordinator::Dependencies dependencies;
            dependencies.timer = std::move(fakeTimer);
            dependencies.now = [this] { return clock.now; };
            dependencies.query = [this](GitQueryRequest request, GitQueryCompletion completion) {
                if (throwSynchronously)
                {
                    throw winrt::hresult_error{ E_FAIL };
                }
                pending.emplace_back(PendingQuery{
                    .request = std::move(request),
                    .completion = std::move(completion),
                });
            };
            dependencies.dispatch = [this](std::function<void()> callback) {
                if (!dispatchSucceeds)
                {
                    return false;
                }
                callback();
                return true;
            };
            dependencies.publish = [this](StatusBarSnapshot snapshot) {
                published.emplace_back(std::move(snapshot));
            };

            coordinator = std::make_shared<TerminalApp::StatusBarCoordinator>(std::move(dependencies));
        }

        ~CoordinatorHarness()
        {
            if (coordinator)
            {
                coordinator->Close();
            }
        }

        static ShellContext Context(
            const uint64_t sequence,
            std::wstring path = L"C:\\src\\terminal",
            const ShellContextPhase phase = ShellContextPhase::Prompt,
            const uint32_t connection = 1)
        {
            ShellContext context;
            context.connectionId.Data1 = connection;
            context.environment.kind = EnvironmentKind::LocalWindows;
            context.pathState = ShellContextPathState::FileSystem;
            context.phase = phase;
            context.provenance = ShellContextProvenance::ShellReported;
            context.path = std::move(path);
            context.sequence = sequence;
            return context;
        }

        void Activate()
        {
            coordinator->UpdateRuntimeState(true, true, true, true);
        }

        void CompleteFront(const GitQueryStatus status, std::optional<GitSnapshot> snapshot = std::nullopt)
        {
            VERIFY_IS_FALSE(pending.empty());
            auto query{ std::move(pending.front()) };
            pending.erase(pending.begin());
            query.completion(GitQueryResult{
                .status = status,
                .snapshot = std::move(snapshot),
            });
        }

        GitSnapshot ReadySnapshot(const std::wstring& branch = L"main") const
        {
            VERIFY_IS_FALSE(pending.empty());
            GitSnapshot snapshot;
            snapshot.environment = pending.front().request.environment;
            snapshot.worktreeRoot = L"C:\\src\\terminal";
            snapshot.branchName = branch;
            return snapshot;
        }
    };

    Strings _strings()
    {
        return {
            .summaryPrefix = L"Git status",
            .summarySeparator = L"; ",
            .namedBranch = L"Git branch {0}",
            .unbornBranch = L"Git branch {0}, no commits yet",
            .unbornBranchCompact = L"{0} · no commits yet",
            .detachedHead = L"Git detached at {0}",
            .detachedHeadUnknown = L"Git detached HEAD",
            .detachedHeadCompact = L"detached · {0}",
            .detachedHeadCompactUnknown = L"detached",
            .unknownBranch = L"unknown branch",
            .windowsEnvironment = L"Windows",
            .wslEnvironment = L"WSL ({0})",
            .unsupportedEnvironment = L"unsupported environment",
            .repository = L"repository {0}",
            .tracking = L"tracking {0}: {1} ahead, {2} behind",
            .aheadOne = L"1 commit ahead of upstream",
            .aheadMany = L"{0} commits ahead of upstream",
            .behindOne = L"1 commit behind upstream",
            .behindMany = L"{0} commits behind upstream",
            .clean = L"clean working tree",
            .staged = L"Staged changes present",
            .unstaged = L"Unstaged changes present in the worktree",
            .untracked = L"Untracked files present; new paths Git is not tracking",
            .conflicted = L"Merge conflicts present; resolve them to continue",
            .linesAddedCompact = L"+{0}",
            .linesDeletedCompact = L"−{0}",
            .lineAdded = L"1 tracked line added compared with HEAD",
            .linesAdded = L"{0} tracked lines added compared with HEAD",
            .lineDeleted = L"1 tracked line deleted compared with HEAD",
            .linesDeleted = L"{0} tracked lines deleted compared with HEAD",
            .lineAddedUnborn = L"1 tracked line added compared with the empty tree before the first commit",
            .linesAddedUnborn = L"{0} tracked lines added compared with the empty tree before the first commit",
            .lineDeletedUnborn = L"1 tracked line deleted compared with the empty tree before the first commit",
            .linesDeletedUnborn = L"{0} tracked lines deleted compared with the empty tree before the first commit",
            .binaryChanges = L"Binary tracked changes present; binary files do not contribute line totals",
            .conflictLinesUnavailable = L"Tracked line totals are unavailable while merge conflicts are present",
            .linesUnavailable = L"Tracked line totals are unavailable",
        };
    }

    GitSnapshot _snapshot()
    {
        GitSnapshot snapshot;
        snapshot.environment.kind = EnvironmentKind::LocalWindows;
        snapshot.worktreeRoot = L"C:\\src\\terminal";
        snapshot.branchKind = GitBranchKind::Named;
        snapshot.branchName = L"main";
        return snapshot;
    }
}

namespace TerminalAppUnitTests
{
    class StatusBarTests
    {
        TEST_CLASS(StatusBarTests);

        TEST_METHOD(EmptySnapshotHasNoGitPresentation);
        TEST_METHOD(LineChangesAndStateIconsMatchApprovedRail);
        TEST_METHOD(CleanUnbornAndDetachedStatesAreDistinct);
        TEST_METHOD(ZeroTotalsBinaryAndConflictRemainAccurate);
        TEST_METHOD(LargeLineTotalsAreBoundedButAccessibleExactly);
        TEST_METHOD(PresentationHasNoFreshnessMetadata);
        TEST_METHOD(SchedulerDebouncesPollsAndRefreshesSameDirectory);
        TEST_METHOD(SchedulerRejectsStaleContextsAndClosesWithoutWaiting);
        TEST_METHOD(SchedulerSuppressesWorkAndAppliesFailureBackoff);
        TEST_METHOD(SchedulerTreatsWslUserAsContextIdentity);
        TEST_METHOD(SchedulerContainsProviderAndDispatcherFailures);
        TEST_METHOD(SchedulerPollsLastReportedContextDuringCommand);
        TEST_METHOD(SchedulerRejectsOutOfOrderReportsAndDuplicateCompletions);
        TEST_METHOD(SchedulerCancelsHiddenAndReboundRequests);
        TEST_METHOD(SchedulerRejectsUnavailableContexts);
    };

    void StatusBarTests::EmptySnapshotHasNoGitPresentation()
    {
        StatusBarSnapshot snapshot;
        const auto view{ Build(snapshot, _strings()) };

        VERIFY_IS_FALSE(view.hasGitContent);
        VERIFY_IS_TRUE(view.accessibleSummary.empty());
        VERIFY_ARE_EQUAL(std::wstring{ L"999" }, CompactCount(999));
        VERIFY_ARE_EQUAL(std::wstring{ L"999+" }, CompactCount(1000));
        VERIFY_ARE_EQUAL(std::wstring{ L"999+" }, CompactCount(UINT32_MAX));
        VERIFY_ARE_EQUAL(std::wstring{ L"9999" }, CompactLineCount(9999));
        VERIFY_ARE_EQUAL(std::wstring{ L"9999+" }, CompactLineCount(10000));
        VERIFY_ARE_EQUAL(std::wstring{ L"9999+" }, CompactLineCount(UINT64_MAX));
    }

    void StatusBarTests::LineChangesAndStateIconsMatchApprovedRail()
    {
        auto git{ _snapshot() };
        git.tracking = GitTracking{ .upstream = L"origin/main", .ahead = 1000, .behind = 2 };
        git.changes = GitChanges{ .staged = true, .unstaged = true, .untracked = true };
        git.lineChanges = GitLineChanges{ .added = 134, .deleted = 42 };

        StatusBarSnapshot snapshot{ .visible = true, .git = git };
        const auto view{ Build(snapshot, _strings()) };

        VERIFY_IS_TRUE(view.hasGitContent);
        VERIFY_ARE_EQUAL(std::wstring{ L"main" }, view.branchText);
        VERIFY_ARE_EQUAL(std::wstring{ L"999+" }, view.ahead.text);
        VERIFY_ARE_EQUAL(std::wstring{ L"2" }, view.behind.text);
        VERIFY_ARE_EQUAL(std::wstring{ L"+134" }, view.added.text);
        VERIFY_ARE_EQUAL(std::wstring{ L"−42" }, view.deleted.text);
        VERIFY_IS_FALSE(view.conflicted.visible);
        VERIFY_IS_TRUE(view.staged.visible);
        VERIFY_IS_TRUE(view.unstaged.visible);
        VERIFY_IS_TRUE(view.untracked.visible);
        VERIFY_IS_TRUE(view.staged.text.empty());
        VERIFY_IS_TRUE(view.untracked.text.empty());
        VERIFY_IS_FALSE(view.clean);
        VERIFY_ARE_EQUAL(
            std::wstring{ L"Git status; Git branch main; Windows; repository C:\\src\\terminal; tracking origin/main: 1000 ahead, 2 behind; 134 tracked lines added compared with HEAD; 42 tracked lines deleted compared with HEAD; Staged changes present; Unstaged changes present in the worktree; Untracked files present; new paths Git is not tracking" },
            view.accessibleSummary);
    }

    void StatusBarTests::CleanUnbornAndDetachedStatesAreDistinct()
    {
        auto git{ _snapshot() };
        git.branchKind = GitBranchKind::Unborn;
        git.branchName = L"feature/initial";
        git.changes = GitChanges{ .staged = true, .untracked = true };
        git.lineChanges = GitLineChanges{ .added = 212 };

        StatusBarSnapshot snapshot{ .visible = true, .git = git };
        auto view{ Build(snapshot, _strings()) };

        VERIFY_ARE_EQUAL(std::wstring{ L"feature/initial · no commits yet" }, view.branchText);
        VERIFY_ARE_EQUAL(std::wstring{ L"+212" }, view.added.text);
        VERIFY_IS_FALSE(view.clean);
        VERIFY_ARE_EQUAL(
            std::wstring{ L"Git status; Git branch feature/initial, no commits yet; Windows; repository C:\\src\\terminal; 212 tracked lines added compared with the empty tree before the first commit; 0 tracked lines deleted compared with the empty tree before the first commit; Staged changes present; Untracked files present; new paths Git is not tracking" },
            view.accessibleSummary);

        git.branchKind = GitBranchKind::Detached;
        git.branchName.clear();
        git.commit = L"0123456789abcdef";
        git.changes = {};
        git.lineChanges.reset();
        git.environment.kind = EnvironmentKind::Wsl;
        git.environment.wslDistro = L"Ubuntu";
        git.environment.wslUser = L"developer";
        snapshot.git = git;
        view = Build(snapshot, _strings());

        VERIFY_ARE_EQUAL(View::IdentityIcon::Detached, view.identityIcon);
        VERIFY_ARE_EQUAL(std::wstring{ L"detached · 0123456" }, view.branchText);
        VERIFY_ARE_EQUAL(
            std::wstring{ L"Git status; Git detached at 0123456789abcdef; WSL (Ubuntu); repository C:\\src\\terminal; clean working tree" },
            view.accessibleSummary);
    }

    void StatusBarTests::ZeroTotalsBinaryAndConflictRemainAccurate()
    {
        auto git{ _snapshot() };
        git.changes.staged = true;
        git.lineChanges = GitLineChanges{};

        StatusBarSnapshot snapshot{ .visible = true, .git = git };
        auto view{ Build(snapshot, _strings()) };
        VERIFY_IS_FALSE(view.added.visible);
        VERIFY_IS_FALSE(view.deleted.visible);
        VERIFY_IS_TRUE(view.staged.visible);
        VERIFY_IS_FALSE(view.clean);
        VERIFY_IS_TRUE(view.accessibleSummary.find(L"0 tracked lines added") != std::wstring::npos);
        VERIFY_IS_TRUE(view.accessibleSummary.find(L"0 tracked lines deleted") != std::wstring::npos);

        git.changes = {};
        git.lineChanges = GitLineChanges{ .added = 7 };
        snapshot.git = git;
        view = Build(snapshot, _strings());
        VERIFY_IS_TRUE(view.added.visible);
        VERIFY_IS_FALSE(view.clean);
        VERIFY_IS_TRUE(view.accessibleSummary.find(L"clean working tree") == std::wstring::npos);

        git.changes.staged = true;
        git.lineChanges = GitLineChanges{ .added = 1, .deleted = 1 };
        snapshot.git = git;
        view = Build(snapshot, _strings());
        VERIFY_ARE_EQUAL(std::wstring{ L"1 tracked line added compared with HEAD" }, view.added.tooltip);
        VERIFY_ARE_EQUAL(std::wstring{ L"1 tracked line deleted compared with HEAD" }, view.deleted.tooltip);

        git.lineChanges = GitLineChanges{ .deleted = 96 };
        snapshot.git = git;
        view = Build(snapshot, _strings());
        VERIFY_IS_FALSE(view.added.visible);
        VERIFY_ARE_EQUAL(std::wstring{ L"−96" }, view.deleted.text);

        git.changes = GitChanges{ .unstaged = true };
        git.lineChanges = GitLineChanges{ .hasBinaryChanges = true };
        snapshot.git = git;
        view = Build(snapshot, _strings());
        VERIFY_IS_FALSE(view.added.visible);
        VERIFY_IS_FALSE(view.deleted.visible);
        VERIFY_IS_TRUE(view.unstaged.visible);
        VERIFY_IS_FALSE(view.clean);
        VERIFY_IS_TRUE(view.unstaged.tooltip.find(L"Binary tracked changes present") != std::wstring::npos);
        VERIFY_IS_TRUE(view.accessibleSummary.find(L"Binary tracked changes present") != std::wstring::npos);

        git.lineChanges = GitLineChanges{ .added = 5, .deleted = 2, .hasBinaryChanges = true };
        snapshot.git = git;
        view = Build(snapshot, _strings());
        VERIFY_ARE_EQUAL(std::wstring{ L"+5" }, view.added.text);
        VERIFY_ARE_EQUAL(std::wstring{ L"−2" }, view.deleted.text);
        VERIFY_IS_TRUE(view.accessibleSummary.find(L"Binary tracked changes present") != std::wstring::npos);

        git.changes = GitChanges{ .staged = true, .unstaged = true, .conflicts = true };
        git.lineChanges = GitLineChanges{ .added = 50, .deleted = 25, .hasBinaryChanges = true };
        snapshot.git = git;
        view = Build(snapshot, _strings());
        VERIFY_IS_TRUE(view.conflicted.visible);
        VERIFY_IS_FALSE(view.added.visible);
        VERIFY_IS_FALSE(view.deleted.visible);
        VERIFY_IS_TRUE(view.conflicted.tooltip.find(L"line totals are unavailable") != std::wstring::npos);
        VERIFY_IS_TRUE(view.accessibleSummary.find(L"Merge conflicts present") != std::wstring::npos);
        VERIFY_IS_TRUE(view.accessibleSummary.find(L"50 tracked lines added") == std::wstring::npos);
        VERIFY_IS_TRUE(view.accessibleSummary.find(L"Binary tracked changes") == std::wstring::npos);

        git.changes = GitChanges{ .untracked = true };
        git.lineChanges = GitLineChanges{};
        snapshot.git = git;
        view = Build(snapshot, _strings());
        VERIFY_IS_TRUE(view.untracked.visible);
        VERIFY_IS_FALSE(view.added.visible);
        VERIFY_IS_FALSE(view.deleted.visible);
        VERIFY_IS_FALSE(view.clean);
        VERIFY_IS_TRUE(view.accessibleSummary.find(L"tracked lines added") == std::wstring::npos);
    }

    void StatusBarTests::LargeLineTotalsAreBoundedButAccessibleExactly()
    {
        auto git{ _snapshot() };
        git.changes.unstaged = true;
        git.lineChanges = GitLineChanges{ .added = UINT64_MAX, .deleted = UINT64_MAX - 1 };

        const auto view{ Build(StatusBarSnapshot{ .visible = true, .git = git }, _strings()) };
        VERIFY_ARE_EQUAL(std::wstring{ L"+9999+" }, view.added.text);
        VERIFY_ARE_EQUAL(std::wstring{ L"−9999+" }, view.deleted.text);
        VERIFY_IS_TRUE(view.added.tooltip.find(std::to_wstring(UINT64_MAX)) != std::wstring::npos);
        VERIFY_IS_TRUE(view.deleted.tooltip.find(std::to_wstring(UINT64_MAX - 1)) != std::wstring::npos);
        VERIFY_IS_TRUE(view.accessibleSummary.find(std::to_wstring(UINT64_MAX)) != std::wstring::npos);
    }

    void StatusBarTests::PresentationHasNoFreshnessMetadata()
    {
        auto git{ _snapshot() };
        git.changes.unstaged = true;
        git.lineChanges = GitLineChanges{ .added = 5, .deleted = 1 };
        StatusBarSnapshot snapshot{ .visible = true, .git = git };
        const auto view{ Build(snapshot, _strings()) };
        VERIFY_ARE_EQUAL(std::wstring{ L"5 tracked lines added compared with HEAD" }, view.added.tooltip);
        VERIFY_ARE_EQUAL(std::wstring{ L"1 tracked line deleted compared with HEAD" }, view.deleted.tooltip);
        VERIFY_IS_TRUE(view.accessibleSummary.find(L"Last reported") == std::wstring::npos);
        VERIFY_IS_TRUE(view.accessibleSummary.find(L"updated") == std::wstring::npos);
    }

    void StatusBarTests::SchedulerDebouncesPollsAndRefreshesSameDirectory()
    {
        CoordinatorHarness harness;
        harness.Activate();
        harness.coordinator->SetShellContext(CoordinatorHarness::Context(1));

        VERIFY_IS_TRUE(harness.timer->IsArmed());
        VERIFY_ARE_EQUAL(int64_t{ 250 }, harness.timer->Remaining().count());
        harness.timer->Advance(std::chrono::milliseconds{ 100 });
        harness.coordinator->SetShellContext(CoordinatorHarness::Context(2));
        VERIFY_ARE_EQUAL(int64_t{ 250 }, harness.timer->Remaining().count());
        harness.timer->Advance(std::chrono::milliseconds{ 249 });
        VERIFY_ARE_EQUAL(size_t{ 0 }, harness.pending.size());
        harness.timer->Advance(std::chrono::milliseconds{ 1 });
        VERIFY_ARE_EQUAL(size_t{ 1 }, harness.pending.size());
        VERIFY_ARE_EQUAL(std::wstring{ L"C:\\src\\terminal" }, harness.pending.front().request.directory);
        VERIFY_ARE_EQUAL(int64_t{ 5000 }, harness.pending.front().request.limits.deadline.count());
        VERIFY_ARE_EQUAL(size_t{ 16 * 1024 * 1024 }, harness.pending.front().request.limits.maximumOutputBytes);

        auto ready{ harness.ReadySnapshot() };
        harness.CompleteFront(GitQueryStatus::Ready, std::move(ready));
        VERIFY_IS_TRUE(harness.published.back().git.has_value());
        VERIFY_ARE_EQUAL(int64_t{ 5000 }, harness.timer->Remaining().count());

        harness.timer->Advance(std::chrono::milliseconds{ 5000 });
        VERIFY_ARE_EQUAL(size_t{ 1 }, harness.pending.size());

        harness.coordinator->SetShellContext(CoordinatorHarness::Context(3, L"C:\\src\\terminal", ShellContextPhase::Command));
        VERIFY_IS_TRUE(harness.pending.front().request.cancellation.stop_requested());
        VERIFY_IS_TRUE(harness.published.back().git.has_value());
        VERIFY_ARE_EQUAL(std::wstring{ L"main" }, harness.published.back().git->branchName);

        harness.coordinator->SetShellContext(CoordinatorHarness::Context(4));
        harness.timer->Advance(std::chrono::milliseconds{ 250 });
        VERIFY_ARE_EQUAL(size_t{ 1 }, harness.pending.size());

        harness.CompleteFront(GitQueryStatus::Cancelled);
        VERIFY_ARE_EQUAL(size_t{ 1 }, harness.pending.size());
        VERIFY_IS_FALSE(harness.pending.front().request.cancellation.stop_requested());
        harness.CompleteFront(GitQueryStatus::Failed);
        VERIFY_IS_FALSE(harness.published.back().git.has_value());
        VERIFY_ARE_EQUAL(int64_t{ 15000 }, harness.timer->Remaining().count());
    }

    void StatusBarTests::SchedulerRejectsStaleContextsAndClosesWithoutWaiting()
    {
        CoordinatorHarness harness;
        harness.Activate();
        harness.coordinator->SetShellContext(CoordinatorHarness::Context(1, L"C:\\src\\one"));
        harness.timer->Advance(std::chrono::milliseconds{ 250 });
        VERIFY_ARE_EQUAL(size_t{ 1 }, harness.pending.size());

        const auto staleSnapshot{ harness.ReadySnapshot(L"stale") };
        harness.coordinator->SetShellContext(CoordinatorHarness::Context(2, L"C:\\src\\two", ShellContextPhase::Prompt, 2));
        VERIFY_IS_TRUE(harness.pending.front().request.cancellation.stop_requested());
        VERIFY_IS_FALSE(harness.published.back().git.has_value());

        harness.timer->Advance(std::chrono::milliseconds{ 250 });
        VERIFY_ARE_EQUAL(size_t{ 1 }, harness.pending.size());
        harness.CompleteFront(GitQueryStatus::Ready, staleSnapshot);

        VERIFY_ARE_EQUAL(size_t{ 1 }, harness.pending.size());
        VERIFY_ARE_EQUAL(std::wstring{ L"C:\\src\\two" }, harness.pending.front().request.directory);
        VERIFY_IS_FALSE(harness.pending.front().request.cancellation.stop_requested());
        VERIFY_IS_FALSE(harness.published.back().git.has_value());

        auto currentSnapshot{ harness.ReadySnapshot(L"current") };
        harness.CompleteFront(GitQueryStatus::Ready, std::move(currentSnapshot));
        VERIFY_ARE_EQUAL(std::wstring{ L"current" }, harness.published.back().git->branchName);

        harness.coordinator->SetShellContext(std::nullopt);
        VERIFY_IS_FALSE(harness.published.back().git.has_value());
        VERIFY_IS_FALSE(harness.timer->IsArmed());

        harness.coordinator->SetShellContext(CoordinatorHarness::Context(3));
        harness.timer->Advance(std::chrono::milliseconds{ 250 });
        VERIFY_ARE_EQUAL(size_t{ 1 }, harness.pending.size());
        const auto publicationsBeforeClose{ harness.published.size() };
        harness.coordinator->Close();
        VERIFY_IS_TRUE(harness.pending.front().request.cancellation.stop_requested());
        harness.CompleteFront(GitQueryStatus::Ready, harness.ReadySnapshot(L"late"));
        VERIFY_ARE_EQUAL(publicationsBeforeClose, harness.published.size());
    }

    void StatusBarTests::SchedulerSuppressesWorkAndAppliesFailureBackoff()
    {
        CoordinatorHarness harness;
        harness.coordinator->SetShellContext(CoordinatorHarness::Context(1));
        VERIFY_IS_FALSE(harness.timer->IsArmed());
        VERIFY_ARE_EQUAL(size_t{ 0 }, harness.pending.size());

        harness.coordinator->UpdateRuntimeState(true, true, true, false);
        VERIFY_IS_FALSE(harness.timer->IsArmed());
        VERIFY_IS_TRUE(harness.published.back().visible);

        harness.Activate();
        VERIFY_ARE_EQUAL(size_t{ 1 }, harness.pending.size());
        harness.CompleteFront(GitQueryStatus::Failed);
        VERIFY_ARE_EQUAL(int64_t{ 15000 }, harness.timer->Remaining().count());

        harness.timer->Advance(std::chrono::milliseconds{ 15000 });
        VERIFY_ARE_EQUAL(size_t{ 1 }, harness.pending.size());
        harness.CompleteFront(GitQueryStatus::GitUnavailable);
        VERIFY_ARE_EQUAL(int64_t{ 30000 }, harness.timer->Remaining().count());

        harness.timer->Advance(std::chrono::milliseconds{ 30000 });
        VERIFY_ARE_EQUAL(size_t{ 1 }, harness.pending.size());
        harness.CompleteFront(GitQueryStatus::TimedOut);
        VERIFY_ARE_EQUAL(int64_t{ 60000 }, harness.timer->Remaining().count());

        harness.coordinator->UpdateRuntimeState(true, true, true, false);
        VERIFY_IS_FALSE(harness.timer->IsArmed());
        harness.timer->Advance(std::chrono::milliseconds{ 60000 });
        VERIFY_ARE_EQUAL(size_t{ 0 }, harness.pending.size());

        harness.Activate();
        VERIFY_ARE_EQUAL(size_t{ 1 }, harness.pending.size());
        harness.CompleteFront(GitQueryStatus::NoRepository);
        VERIFY_IS_FALSE(harness.published.back().git.has_value());
        VERIFY_ARE_EQUAL(int64_t{ 5000 }, harness.timer->Remaining().count());

        harness.coordinator->UpdateRuntimeState(true, false, true, true);
        VERIFY_IS_FALSE(harness.published.back().visible);
        VERIFY_IS_FALSE(harness.timer->IsArmed());
        harness.timer->Advance(std::chrono::milliseconds{ 60000 });
        VERIFY_ARE_EQUAL(size_t{ 0 }, harness.pending.size());

        harness.coordinator->UpdateRuntimeState(false, true, true, true);
        VERIFY_IS_FALSE(harness.published.back().visible);
        VERIFY_IS_FALSE(harness.timer->IsArmed());

        harness.coordinator->UpdateRuntimeState(true, true, false, true);
        VERIFY_IS_TRUE(harness.published.back().visible);
        VERIFY_IS_FALSE(harness.timer->IsArmed());

        harness.coordinator->UpdateRuntimeState(true, true, true, true);
        VERIFY_ARE_EQUAL(size_t{ 1 }, harness.pending.size());
    }

    void StatusBarTests::SchedulerTreatsWslUserAsContextIdentity()
    {
        CoordinatorHarness harness;
        harness.Activate();

        auto alice{ CoordinatorHarness::Context(1, L"/src/terminal") };
        alice.environment.kind = EnvironmentKind::Wsl;
        alice.environment.wslDistro = L"Ubuntu";
        alice.environment.wslUser = L"alice";
        harness.coordinator->SetShellContext(alice);
        harness.timer->Advance(std::chrono::milliseconds{ 250 });

        auto ready{ harness.ReadySnapshot() };
        ready.worktreeRoot = L"/src/terminal";
        harness.CompleteFront(GitQueryStatus::Ready, std::move(ready));
        VERIFY_IS_TRUE(harness.published.back().git.has_value());

        auto bob{ alice };
        bob.sequence = 2;
        bob.environment.wslUser = L"bob";
        harness.coordinator->SetShellContext(bob);
        VERIFY_IS_FALSE(harness.published.back().git.has_value());
        VERIFY_ARE_EQUAL(int64_t{ 250 }, harness.timer->Remaining().count());

        harness.timer->Advance(std::chrono::milliseconds{ 250 });
        VERIFY_ARE_EQUAL(size_t{ 1 }, harness.pending.size());
        VERIFY_ARE_EQUAL(std::wstring{ L"bob" }, harness.pending.front().request.environment.wslUser);
    }

    void StatusBarTests::SchedulerContainsProviderAndDispatcherFailures()
    {
        CoordinatorHarness synchronousFailure;
        synchronousFailure.throwSynchronously = true;
        synchronousFailure.coordinator->SetShellContext(CoordinatorHarness::Context(1));
        synchronousFailure.Activate();

        VERIFY_ARE_EQUAL(size_t{ 0 }, synchronousFailure.pending.size());
        VERIFY_IS_FALSE(synchronousFailure.published.back().git.has_value());
        VERIFY_ARE_EQUAL(int64_t{ 15000 }, synchronousFailure.timer->Remaining().count());

        CoordinatorHarness invalidResult;
        invalidResult.coordinator->SetShellContext(CoordinatorHarness::Context(1));
        invalidResult.Activate();
        invalidResult.CompleteFront(GitQueryStatus::Ready);
        VERIFY_IS_FALSE(invalidResult.published.back().git.has_value());
        VERIFY_ARE_EQUAL(int64_t{ 15000 }, invalidResult.timer->Remaining().count());
        invalidResult.timer->Advance(std::chrono::milliseconds{ 15000 });
        invalidResult.CompleteFront(GitQueryStatus::NoRepository, invalidResult.ReadySnapshot());
        VERIFY_IS_FALSE(invalidResult.published.back().git.has_value());
        VERIFY_ARE_EQUAL(int64_t{ 30000 }, invalidResult.timer->Remaining().count());

        CoordinatorHarness dispatchFailure;
        dispatchFailure.Activate();
        dispatchFailure.coordinator->SetShellContext(CoordinatorHarness::Context(1));
        dispatchFailure.timer->Advance(std::chrono::milliseconds{ 250 });
        VERIFY_ARE_EQUAL(size_t{ 1 }, dispatchFailure.pending.size());

        const auto publicationsBeforeCompletion{ dispatchFailure.published.size() };
        dispatchFailure.dispatchSucceeds = false;
        auto ready{ dispatchFailure.ReadySnapshot() };
        dispatchFailure.CompleteFront(GitQueryStatus::Ready, std::move(ready));

        VERIFY_ARE_EQUAL(publicationsBeforeCompletion, dispatchFailure.published.size());
        dispatchFailure.coordinator->Close();
    }

    void StatusBarTests::SchedulerPollsLastReportedContextDuringCommand()
    {
        CoordinatorHarness harness;
        harness.Activate();
        harness.coordinator->SetShellContext(CoordinatorHarness::Context(1));
        harness.timer->Advance(std::chrono::milliseconds{ 250 });

        auto initial{ harness.ReadySnapshot(L"initial") };
        harness.CompleteFront(GitQueryStatus::Ready, std::move(initial));
        VERIFY_ARE_EQUAL(std::wstring{ L"initial" }, harness.published.back().git->branchName);

        harness.coordinator->SetShellContext(
            CoordinatorHarness::Context(2, L"C:\\src\\terminal", ShellContextPhase::Command));
        VERIFY_ARE_EQUAL(std::wstring{ L"initial" }, harness.published.back().git->branchName);
        VERIFY_ARE_EQUAL(int64_t{ 5000 }, harness.timer->Remaining().count());

        harness.timer->Advance(std::chrono::milliseconds{ 5000 });
        VERIFY_ARE_EQUAL(size_t{ 1 }, harness.pending.size());
        VERIFY_ARE_EQUAL(std::wstring{ L"C:\\src\\terminal" }, harness.pending.front().request.directory);

        auto externalEdit{ harness.ReadySnapshot(L"external-edit") };
        externalEdit.changes.unstaged = true;
        harness.CompleteFront(GitQueryStatus::Ready, std::move(externalEdit));
        VERIFY_ARE_EQUAL(std::wstring{ L"external-edit" }, harness.published.back().git->branchName);
        VERIFY_IS_TRUE(harness.published.back().git->changes.unstaged);
        VERIFY_ARE_EQUAL(int64_t{ 5000 }, harness.timer->Remaining().count());

        harness.coordinator->SetShellContext(CoordinatorHarness::Context(3));
        VERIFY_ARE_EQUAL(int64_t{ 250 }, harness.timer->Remaining().count());
        harness.timer->Advance(std::chrono::milliseconds{ 250 });
        VERIFY_ARE_EQUAL(size_t{ 1 }, harness.pending.size());

        auto promptRefresh{ harness.ReadySnapshot(L"prompt-refresh") };
        harness.CompleteFront(GitQueryStatus::Ready, std::move(promptRefresh));
        VERIFY_ARE_EQUAL(std::wstring{ L"prompt-refresh" }, harness.published.back().git->branchName);
    }

    void StatusBarTests::SchedulerRejectsOutOfOrderReportsAndDuplicateCompletions()
    {
        CoordinatorHarness harness;
        harness.Activate();
        harness.coordinator->SetShellContext(CoordinatorHarness::Context(3));
        harness.timer->Advance(std::chrono::milliseconds{ 250 });

        harness.coordinator->SetShellContext(CoordinatorHarness::Context(2, L"C:\\stale"));
        harness.coordinator->SetShellContext(CoordinatorHarness::Context(3, L"C:\\duplicate"));
        VERIFY_IS_FALSE(harness.pending.front().request.cancellation.stop_requested());
        VERIFY_IS_FALSE(harness.timer->IsArmed());

        const auto firstCompletion{ harness.pending.front().completion };
        const auto firstSnapshot{ harness.ReadySnapshot(L"first") };
        harness.CompleteFront(GitQueryStatus::Ready, firstSnapshot);
        harness.timer->Advance(std::chrono::milliseconds{ 5000 });
        VERIFY_ARE_EQUAL(size_t{ 1 }, harness.pending.size());
        VERIFY_ARE_EQUAL(std::wstring{ L"C:\\src\\terminal" }, harness.pending.front().request.directory);

        const auto publications{ harness.published.size() };
        firstCompletion({ .status = GitQueryStatus::Ready, .snapshot = firstSnapshot });
        VERIFY_ARE_EQUAL(publications, harness.published.size());
        VERIFY_ARE_EQUAL(size_t{ 1 }, harness.pending.size());
        VERIFY_IS_FALSE(harness.pending.front().request.cancellation.stop_requested());

        harness.CompleteFront(GitQueryStatus::Ready, harness.ReadySnapshot(L"second"));
        VERIFY_ARE_EQUAL(std::wstring{ L"second" }, harness.published.back().git->branchName);
    }

    void StatusBarTests::SchedulerCancelsHiddenAndReboundRequests()
    {
        for (size_t hiddenState = 0; hiddenState < 4; ++hiddenState)
        {
            CoordinatorHarness harness;
            harness.Activate();
            harness.coordinator->SetShellContext(CoordinatorHarness::Context(1));
            harness.timer->Advance(std::chrono::milliseconds{ 250 });
            const auto oldSnapshot{ harness.ReadySnapshot(L"old-pane") };

            harness.coordinator->UpdateRuntimeState(hiddenState != 0, hiddenState != 1, hiddenState != 2, hiddenState != 3);
            VERIFY_IS_TRUE(harness.pending.front().request.cancellation.stop_requested());
            VERIFY_IS_FALSE(harness.timer->IsArmed());
            harness.Activate();

            // Rebinding to the same connection and path must still reject the old request.
            harness.coordinator->SetShellContext(std::nullopt);
            harness.coordinator->SetShellContext(CoordinatorHarness::Context(1));
            harness.timer->Advance(std::chrono::milliseconds{ 250 });
            VERIFY_ARE_EQUAL(size_t{ 1 }, harness.pending.size());
            harness.CompleteFront(GitQueryStatus::Ready, oldSnapshot);
            VERIFY_IS_FALSE(harness.published.back().git.has_value());
            VERIFY_ARE_EQUAL(size_t{ 1 }, harness.pending.size());
            VERIFY_IS_FALSE(harness.pending.front().request.cancellation.stop_requested());

            const auto lateSnapshot{ harness.ReadySnapshot(L"after-destruction") };
            const auto publications{ harness.published.size() };
            harness.coordinator.reset();
            VERIFY_IS_TRUE(harness.pending.front().request.cancellation.stop_requested());
            harness.CompleteFront(GitQueryStatus::Ready, lateSnapshot);
            VERIFY_ARE_EQUAL(publications, harness.published.size());
        }
    }

    void StatusBarTests::SchedulerRejectsUnavailableContexts()
    {
        for (size_t unavailableState = 0; unavailableState < 7; ++unavailableState)
        {
            CoordinatorHarness harness;
            harness.Activate();
            harness.coordinator->SetShellContext(CoordinatorHarness::Context(1));
            harness.timer->Advance(std::chrono::milliseconds{ 250 });
            harness.CompleteFront(GitQueryStatus::Ready, harness.ReadySnapshot());

            auto unavailable{ CoordinatorHarness::Context(2) };
            switch (unavailableState)
            {
            case 0: unavailable.environment.kind = EnvironmentKind::Unsupported; break;
            case 1: unavailable.environment.kind = EnvironmentKind::Wsl; break;
            case 2: unavailable.pathState = ShellContextPathState::Unknown; break;
            case 3: unavailable.pathState = ShellContextPathState::NonFileSystem; break;
            case 4: unavailable.phase = ShellContextPhase::Unknown; break;
            case 5: unavailable.provenance = ShellContextProvenance::Unknown; break;
            case 6: unavailable.path.clear(); break;
            }
            harness.coordinator->SetShellContext(unavailable);
            VERIFY_IS_FALSE(harness.published.back().git.has_value());
            VERIFY_IS_FALSE(harness.timer->IsArmed());
            harness.timer->Advance(std::chrono::milliseconds{ 60000 });
            VERIFY_IS_TRUE(harness.pending.empty());
        }
    }
}
