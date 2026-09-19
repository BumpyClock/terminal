// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "StatusBarControl.h"

#include "StatusBarControl.g.cpp"

using namespace winrt;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Automation;
using namespace winrt::Windows::UI::Xaml::Controls;

namespace
{
    constexpr double secondaryStatesMinimumWidth{ 300.0 };
    constexpr double maximumBranchWidth{ 300.0 };
    constexpr double preferredBranchWidth{ 96.0 };
    constexpr double minimumBranchWidth{ 24.0 };
    constexpr double horizontalPadding{ 20.0 };
    constexpr double identityIconAndGap{ 18.0 };
    constexpr double railSpacing{ 10.0 };

    std::wstring _resource(const hstring& value)
    {
        return { value.c_str(), value.size() };
    }

    ::TerminalApp::StatusBarPresentation::Strings _loadStrings()
    {
        return {
            .summaryPrefix = _resource(RS_(L"StatusBar_GitSummaryPrefix")),
            .summarySeparator = _resource(RS_(L"StatusBar_GitSummarySeparator")),
            .namedBranch = _resource(RS_(L"StatusBar_GitBranchNamed")),
            .unbornBranch = _resource(RS_(L"StatusBar_GitBranchUnborn")),
            .unbornBranchCompact = _resource(RS_(L"StatusBar_GitBranchUnbornCompact")),
            .detachedHead = _resource(RS_(L"StatusBar_GitDetachedHead")),
            .detachedHeadUnknown = _resource(RS_(L"StatusBar_GitDetachedHeadUnknown")),
            .detachedHeadCompact = _resource(RS_(L"StatusBar_GitDetachedHeadCompact")),
            .detachedHeadCompactUnknown = _resource(RS_(L"StatusBar_GitDetachedHeadCompactUnknown")),
            .unknownBranch = _resource(RS_(L"StatusBar_GitUnknownBranch")),
            .windowsEnvironment = _resource(RS_(L"StatusBar_GitEnvironmentWindows")),
            .wslEnvironment = _resource(RS_(L"StatusBar_GitEnvironmentWsl")),
            .unsupportedEnvironment = _resource(RS_(L"StatusBar_GitEnvironmentUnsupported")),
            .repository = _resource(RS_(L"StatusBar_GitRepository")),
            .tracking = _resource(RS_(L"StatusBar_GitTracking")),
            .aheadOne = _resource(RS_(L"StatusBar_GitAheadOne")),
            .aheadMany = _resource(RS_(L"StatusBar_GitAheadMany")),
            .behindOne = _resource(RS_(L"StatusBar_GitBehindOne")),
            .behindMany = _resource(RS_(L"StatusBar_GitBehindMany")),
            .clean = _resource(RS_(L"StatusBar_GitClean")),
            .staged = _resource(RS_(L"StatusBar_GitStaged")),
            .unstaged = _resource(RS_(L"StatusBar_GitUnstaged")),
            .untracked = _resource(RS_(L"StatusBar_GitUntracked")),
            .conflicted = _resource(RS_(L"StatusBar_GitConflicted")),
            .linesAddedCompact = _resource(RS_(L"StatusBar_GitLinesAddedCompact")),
            .linesDeletedCompact = _resource(RS_(L"StatusBar_GitLinesDeletedCompact")),
            .lineAdded = _resource(RS_(L"StatusBar_GitLineAdded")),
            .linesAdded = _resource(RS_(L"StatusBar_GitLinesAdded")),
            .lineDeleted = _resource(RS_(L"StatusBar_GitLineDeleted")),
            .linesDeleted = _resource(RS_(L"StatusBar_GitLinesDeleted")),
            .lineAddedUnborn = _resource(RS_(L"StatusBar_GitLineAddedUnborn")),
            .linesAddedUnborn = _resource(RS_(L"StatusBar_GitLinesAddedUnborn")),
            .lineDeletedUnborn = _resource(RS_(L"StatusBar_GitLineDeletedUnborn")),
            .linesDeletedUnborn = _resource(RS_(L"StatusBar_GitLinesDeletedUnborn")),
            .binaryChanges = _resource(RS_(L"StatusBar_GitBinaryChanges")),
            .conflictLinesUnavailable = _resource(RS_(L"StatusBar_GitConflictLinesUnavailable")),
            .linesUnavailable = _resource(RS_(L"StatusBar_GitLinesUnavailable")),
        };
    }

    void _setVisibility(const UIElement& element, const bool visible)
    {
        element.Visibility(visible ? Visibility::Visible : Visibility::Collapsed);
    }

    void _setToolTip(const DependencyObject& element, const std::wstring& text)
    {
        if (text.empty())
        {
            ToolTipService::SetToolTip(element, nullptr);
        }
        else
        {
            ToolTipService::SetToolTip(element, box_value(hstring{ text }));
        }
    }

    double _desiredWidth(const FrameworkElement& element)
    {
        if (element.Visibility() != Visibility::Visible)
        {
            return 0;
        }

        element.Measure({
            std::numeric_limits<float>::infinity(),
            gsl::narrow_cast<float>(::TerminalApp::StatusBarPresentation::MinimumRowHeight),
        });
        return element.DesiredSize().Width;
    }
}

namespace winrt::TerminalApp::implementation
{
    StatusBarControl::StatusBarControl()
    {
        InitializeComponent();
        MinHeight(::TerminalApp::StatusBarPresentation::MinimumRowHeight);
        _strings = _loadStrings();
    }

    void StatusBarControl::ApplySnapshot(const ::Microsoft::Terminal::StatusBar::StatusBarSnapshot& snapshot)
    {
        Visibility(snapshot.visible ? Visibility::Visible : Visibility::Collapsed);
        _presentation = ::TerminalApp::StatusBarPresentation::Build(snapshot, _strings);
        _ApplyPresentation(_presentation);
    }

    void StatusBarControl::_OnSizeChanged(const IInspectable&, const SizeChangedEventArgs& args)
    {
        Media::RectangleGeometry clip;
        clip.Rect({ 0, 0, args.NewSize().Width, args.NewSize().Height });
        Clip(clip);
        _UpdateResponsiveVisibility(args.NewSize().Width);
    }

    void StatusBarControl::_ApplyPresentation(const ::TerminalApp::StatusBarPresentation::View& presentation)
    {
        _setVisibility(GitContent(), presentation.hasGitContent);
        if (!presentation.hasGitContent)
        {
            if (!_lastAccessibleSummary.empty())
            {
                _lastAccessibleSummary.clear();
                AutomationProperties::SetName(*this, L"");
                ToolTipService::SetToolTip(*this, nullptr);
            }
            AutomationProperties::SetAccessibilityView(*this, winrt::Windows::UI::Xaml::Automation::Peers::AccessibilityView::Raw);
            return;
        }

        BranchText().Text(hstring{ presentation.branchText });
        _setVisibility(BranchIcon(), presentation.identityIcon == ::TerminalApp::StatusBarPresentation::View::IdentityIcon::Branch);
        _setVisibility(DetachedIcon(), presentation.identityIcon == ::TerminalApp::StatusBarPresentation::View::IdentityIcon::Detached);

        AheadText().Text(hstring{ presentation.ahead.text });
        BehindText().Text(hstring{ presentation.behind.text });
        AddedItem().Text(hstring{ presentation.added.text });
        DeletedItem().Text(hstring{ presentation.deleted.text });

        _setToolTip(AheadItem(), presentation.ahead.tooltip);
        _setToolTip(BehindItem(), presentation.behind.tooltip);
        _setToolTip(AddedItem(), presentation.added.tooltip);
        _setToolTip(DeletedItem(), presentation.deleted.tooltip);
        _setToolTip(StagedItem(), presentation.staged.tooltip);
        _setToolTip(UnstagedItem(), presentation.unstaged.tooltip);
        _setToolTip(UntrackedItem(), presentation.untracked.tooltip);
        _setToolTip(ConflictedItem(), presentation.conflicted.tooltip);
        _UpdateResponsiveVisibility(ActualWidth());

        AutomationProperties::SetAccessibilityView(*this, winrt::Windows::UI::Xaml::Automation::Peers::AccessibilityView::Control);
        if (_lastAccessibleSummary != presentation.accessibleSummary)
        {
            _lastAccessibleSummary = presentation.accessibleSummary;
            const hstring accessibleSummary{ _lastAccessibleSummary };
            AutomationProperties::SetName(*this, accessibleSummary);
            ToolTipService::SetToolTip(*this, box_value(accessibleSummary));
        }
    }

    void StatusBarControl::_UpdateResponsiveVisibility(const double availableWidth)
    {
        if (!_presentation.hasGitContent)
        {
            return;
        }

        _setVisibility(AddedItem(), _presentation.added.visible);
        _setVisibility(DeletedItem(), _presentation.deleted.visible);

        const auto linesVisible{ _presentation.added.visible || _presentation.deleted.visible };
        const auto showSecondaryStates{ availableWidth >= secondaryStatesMinimumWidth || !linesVisible };
        _setVisibility(ConflictedItem(), _presentation.conflicted.visible);
        _setVisibility(StagedItem(), _presentation.staged.visible && showSecondaryStates);
        _setVisibility(UnstagedItem(), _presentation.unstaged.visible && showSecondaryStates);
        _setVisibility(UntrackedItem(), _presentation.untracked.visible && showSecondaryStates);

        const auto statesVisible{ ConflictedItem().Visibility() == Visibility::Visible ||
                                  StagedItem().Visibility() == Visibility::Visible ||
                                  UnstagedItem().Visibility() == Visibility::Visible ||
                                  UntrackedItem().Visibility() == Visibility::Visible };
        _setVisibility(LinesGroup(), linesVisible);
        _setVisibility(LineSeparator(), linesVisible);
        _setVisibility(StatesGroup(), statesVisible);
        _setVisibility(StateSeparator(), statesVisible);

        const auto trackingAvailable{ _presentation.ahead.visible || _presentation.behind.visible };
        _setVisibility(AheadItem(), _presentation.ahead.visible);
        _setVisibility(BehindItem(), _presentation.behind.visible);
        _setVisibility(TrackingGroup(), trackingAvailable);

        const auto branchBudget = [&]() {
            double reserved{ horizontalPadding + identityIconAndGap };
            uint32_t visibleGroups{ 0 };
            for (const auto& element : {
                     TrackingGroup().as<FrameworkElement>(),
                     LineSeparator().as<FrameworkElement>(),
                     LinesGroup().as<FrameworkElement>(),
                     StateSeparator().as<FrameworkElement>(),
                     StatesGroup().as<FrameworkElement>(),
                 })
            {
                if (element.Visibility() == Visibility::Visible)
                {
                    reserved += _desiredWidth(element);
                    ++visibleGroups;
                }
            }
            reserved += visibleGroups * railSpacing;
            return availableWidth - reserved;
        };

        auto availableBranchWidth{ branchBudget() };
        if (trackingAvailable && availableBranchWidth < preferredBranchWidth)
        {
            _setVisibility(AheadItem(), false);
            _setVisibility(BehindItem(), false);
            _setVisibility(TrackingGroup(), false);
            availableBranchWidth = branchBudget();
        }

        BranchText().MaxWidth(std::clamp(
            availableBranchWidth,
            minimumBranchWidth,
            maximumBranchWidth));
    }
}
