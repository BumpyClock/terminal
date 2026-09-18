// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "GitHubDashboard.h"
#include <array>
#include <format>
#include <cmath>

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::UI;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Controls::Primitives;
using namespace winrt::Windows::UI::Xaml::Input;
using namespace winrt::Windows::UI::Xaml::Media;
using namespace winrt::Windows::UI::Xaml::Automation;
using namespace std::chrono_literals;

namespace TerminalApp
{
    namespace Windows = winrt::Windows;
    using IInspectable = winrt::Windows::Foundation::IInspectable;
    using winrt::Windows::UI::Xaml::Automation::Peers::AutomationLiveSetting;

    namespace
    {
        TextBlock Text(const hstring& value, const double size = 14)
        {
            TextBlock text;
            text.Text(value);
            text.FontSize(size);
            text.TextWrapping(TextWrapping::Wrap);
            text.IsTextSelectionEnabled(true);
            return text;
        }

        hstring Label(const wchar_t* key)
        {
            return GetLibraryResourceString(key);
        }

        hstring ErrorText(const GitHub::Failure failure)
        {
            switch (failure)
            {
            case GitHub::Failure::MissingCli:
                return Label(L"GitHubMissingCli");
            case GitHub::Failure::Timeout:
                return Label(L"GitHubTimeout");
            case GitHub::Failure::AccountChanged:
                return Label(L"GitHubAccountChanged");
            case GitHub::Failure::InvalidResponse:
                return Label(L"GitHubInvalidResponse");
            default:
                return Label(L"GitHubRequestFailed");
            }
        }

        void Separator(const StackPanel& parent)
        {
            const auto line = winrt::Windows::UI::Xaml::Markup::XamlReader::Load(
                LR"(<Border xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
                    Height="1" Margin="0,8,0,8" Background="{ThemeResource SystemControlForegroundBaseLowBrush}" />)");
            parent.Children().Append(line.as<Border>());
        }

        HyperlinkButton Link(const std::string& title, const std::string& url)
        {
            HyperlinkButton link;
            auto text = Text(to_hstring(title), 14);
            text.MaxLines(2);
            text.TextTrimming(TextTrimming::CharacterEllipsis);
            text.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
            link.Content(text);
            ToolTipService::SetToolTip(link, box_value(to_hstring(title)));
            link.Padding({ 0, 4, 0, 4 });
            link.HorizontalContentAlignment(HorizontalAlignment::Left);
            link.HorizontalAlignment(HorizontalAlignment::Stretch);
            if (GitHub::IsGitHubUrl(url))
            {
                link.NavigateUri(Uri{ to_hstring(url) });
            }
            return link;
        }

        Grid Pair(const UIElement& left, const UIElement& right)
        {
            Grid grid;
            grid.ColumnSpacing(8);
            ColumnDefinition first;
            first.Width({ 1, GridUnitType::Star });
            grid.ColumnDefinitions().Append(first);
            ColumnDefinition second;
            second.Width({ 1, GridUnitType::Auto });
            grid.ColumnDefinitions().Append(second);
            grid.Children().Append(left);
            Grid::SetColumn(right.as<FrameworkElement>(), 1);
            grid.Children().Append(right);
            return grid;
        }

        template<typename T>
        bool SectionChanged(const GitHub::Section<T>& before, const GitHub::Section<T>& after)
        {
            // A successful fetch replaces the value and advances its timestamp.
            return before.updated != after.updated || before.failure != after.failure ||
                   before.value.has_value() != after.value.has_value();
        }

        constexpr std::array listSections{ &GitHub::Snapshot::activity, &GitHub::Snapshot::pulls, &GitHub::Snapshot::reviews, &GitHub::Snapshot::repos };

        template<typename T>
        void SectionStatus(const GitHub::Section<T>& section, const StackPanel& parent)
        {
            if (section.failure != GitHub::Failure::None)
            {
                parent.Children().Append(Text((section.value ? Label(L"GitHubStale") + L" " : hstring{}) + ErrorText(section.failure)));
            }
            if (section.value)
            {
                parent.Children().Append(Text(Label(L"GitHubUpdated") + L" " +
                                                  to_hstring(std::format("{:%F %R} UTC", std::chrono::floor<std::chrono::seconds>(section.updated))),
                                              12));
            }
        }

        hstring QuotaName(const GitHub::Quota& quota)
        {
            if (quota.kind == "premium_interactions")
            {
                return Label(quota.credits ? L"GitHubAiCredits" : L"GitHubPremiumRequests");
            }
            return Label(quota.kind == "chat" ? L"GitHubChat" : L"GitHubCompletions");
        }

        hstring StateLabel(const std::string& value)
        {
            if (value == "OPEN")
                return Label(L"GitHubOpen");
            if (value == "CLOSED")
                return Label(L"GitHubClosed");
            if (value == "MERGED")
                return Label(L"GitHubMerged");
            if (value == "DRAFT")
                return Label(L"GitHubDraft");
            if (value == "APPROVED")
                return Label(L"GitHubApproved");
            if (value == "CHANGES_REQUESTED")
                return Label(L"GitHubChangesRequested");
            if (value == "REVIEW_REQUIRED")
                return Label(L"GitHubReviewRequired");
            return to_hstring(value);
        }

        hstring ActivityLabel(const std::string& value)
        {
            if (value == "PushEvent")
                return Label(L"GitHubPushed");
            if (value == "DeleteEvent")
                return Label(L"GitHubDeletedRef");
            if (value == "CreateEvent")
                return Label(L"GitHubCreatedRef");
            if (value == "PullRequestEvent")
                return Label(L"GitHubPullActivity");
            if (value == "PullRequestReviewEvent")
                return Label(L"GitHubReviewActivity");
            if (value == "PullRequestReviewCommentEvent" || value == "IssueCommentEvent")
                return Label(L"GitHubCommentActivity");
            if (value == "IssuesEvent")
                return Label(L"GitHubIssueActivity");
            if (value == "ReleaseEvent")
                return Label(L"GitHubReleaseActivity");
            return to_hstring(value);
        }
    }

    std::shared_ptr<GitHubDashboard> GitHubDashboard::Create(GitHub::Update accountChanged)
    {
        auto dashboard = std::make_shared<GitHubDashboard>();
        dashboard->_accountChanged = std::move(accountChanged);
        dashboard->_Initialize();
        return dashboard;
    }

    void GitHubDashboard::_Initialize()
    {
        const auto weak = weak_from_this();
        _cachePath = GitHub::CachePath();
        _root.Width(402);
        for (const auto unit : { GridUnitType::Auto, GridUnitType::Auto, GridUnitType::Auto, GridUnitType::Auto, GridUnitType::Star, GridUnitType::Auto })
        {
            RowDefinition row;
            row.Height({ 1, unit });
            _root.RowDefinitions().Append(row);
        }
        Grid header;
        header.Margin({ 8, 4, 8, 8 });
        ColumnDefinition accountColumn;
        accountColumn.Width({ 1, GridUnitType::Star });
        header.ColumnDefinitions().Append(accountColumn);
        ColumnDefinition summaryColumn;
        summaryColumn.Width({ 1, GridUnitType::Auto });
        header.ColumnDefinitions().Append(summaryColumn);
        StackPanel identity;
        identity.Spacing(2);
        _account.FontSize(14);
        _account.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
        _account.TextTrimming(TextTrimming::CharacterEllipsis);
        identity.Children().Append(_account);
        _accountName.FontSize(12);
        _accountName.Opacity(0.7);
        _accountName.TextTrimming(TextTrimming::CharacterEllipsis);
        identity.Children().Append(_accountName);
        header.Children().Append(identity);
        _calendarSummary.FontSize(12);
        _calendarSummary.Opacity(0.7);
        _calendarSummary.VerticalAlignment(VerticalAlignment::Center);
        Grid::SetColumn(_calendarSummary, 1);
        header.Children().Append(_calendarSummary);
        _root.Children().Append(header);
        _status.FontSize(12);
        _status.MaxLines(2);
        _status.TextTrimming(TextTrimming::CharacterEllipsis);
        _status.TextWrapping(TextWrapping::Wrap);
        _status.Margin({ 8, 0, 8, 8 });
        AutomationProperties::SetLiveSetting(_status, AutomationLiveSetting::Polite);
        Grid::SetRow(_status, 1);
        _root.Children().Append(_status);
        StackPanel overview;
        overview.Spacing(8);
        overview.Margin({ 8, 0, 8, 8 });
        Grid::SetRow(overview, 2);
        _root.Children().Append(overview);

        ScrollViewer calendarScroll;
        calendarScroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Auto);
        calendarScroll.HorizontalScrollMode(ScrollMode::Enabled);
        calendarScroll.VerticalScrollBarVisibility(ScrollBarVisibility::Disabled);
        calendarScroll.VerticalScrollMode(ScrollMode::Disabled);
        _calendarButton.Content(_calendarGrid);
        _calendarButton.Padding({ 2, 2, 2, 2 });
        _calendarButton.HorizontalAlignment(HorizontalAlignment::Left);
        _calendarButton.Background(SolidColorBrush{ Colors::Transparent() });
        _calendarButton.KeyDown([weak](const auto&, const KeyRoutedEventArgs& args) {
            if (auto self = weak.lock(); self && self->_snapshot.calendar.value)
            {
                auto index = static_cast<int64_t>(self->_selectedDay.value_or(0));
                switch (args.Key())
                {
                case Windows::System::VirtualKey::Left:
                    index -= 7;
                    break;
                case Windows::System::VirtualKey::Right:
                    index += 7;
                    break;
                case Windows::System::VirtualKey::Up:
                    --index;
                    break;
                case Windows::System::VirtualKey::Down:
                    ++index;
                    break;
                case Windows::System::VirtualKey::Home:
                    index = 0;
                    break;
                case Windows::System::VirtualKey::End:
                    index = static_cast<int64_t>(self->_snapshot.calendar.value->days.size()) - 1;
                    break;
                default:
                    return;
                }
                self->_SelectDay(static_cast<size_t>(std::clamp<int64_t>(index, 0, static_cast<int64_t>(self->_snapshot.calendar.value->days.size()) - 1)));
                args.Handled(true);
            }
        });
        calendarScroll.Content(_calendarButton);
        overview.Children().Append(calendarScroll);
        _dayDescription.FontSize(12);
        _dayDescription.TextWrapping(TextWrapping::Wrap);
        AutomationProperties::SetLiveSetting(_dayDescription, AutomationLiveSetting::Polite);
        _dayDescription.Visibility(Visibility::Collapsed);
        _usage.Spacing(6);
        overview.Children().Append(_usage);

        ListBox sections;
        sections.FontSize(14);
        sections.BorderThickness({ 0, 0, 0, 0 });
        sections.Background(SolidColorBrush{ Colors::Transparent() });
        sections.Padding({ 0, 0, 0, 0 });
        sections.ItemsPanel(Windows::UI::Xaml::Markup::XamlReader::Load(
                                LR"(<ItemsPanelTemplate xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation">
                    <StackPanel Orientation="Horizontal" />
                </ItemsPanelTemplate>)")
                                .as<ItemsPanelTemplate>());
        ScrollViewer::SetHorizontalScrollBarVisibility(sections, ScrollBarVisibility::Disabled);
        ScrollViewer::SetVerticalScrollBarVisibility(sections, ScrollBarVisibility::Disabled);
        AutomationProperties::SetName(sections, Label(L"GitHubDashboard"));
        const std::array names{ L"GitHubActivity", L"GitHubMyPrs", L"GitHubReviews", L"GitHubRepos" };
        for (size_t i = 0; i < names.size(); ++i)
        {
            ListBoxItem item;
            item.Content(box_value(Label(names[i])));
            item.FontSize(14);
            item.MinHeight(32);
            item.Padding({ 10, 4, 10, 4 });
            sections.Items().Append(item);
        }
        sections.SelectedIndex(0);
        sections.SelectionChanged([weak](const IInspectable& sender, const auto&) {
            if (const auto self = weak.lock())
            {
                self->_selectedSection = sender.as<ListBox>().SelectedIndex();
                self->_RenderSelected();
            }
        });
        overview.Children().Append(sections);
        _scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
        _scroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
        _scroll.HorizontalScrollMode(ScrollMode::Disabled);
        _scroll.Margin({ 8, 0, 8, 0 });
        _scroll.Content(_list);
        _sectionStatus.Margin({ 8, 0, 8, 4 });
        Grid::SetRow(_sectionStatus, 3);
        _root.Children().Append(_sectionStatus);
        Grid::SetRow(_scroll, 4);
        _root.Children().Append(_scroll);

        StackPanel footer;
        footer.Orientation(Orientation::Horizontal);
        footer.Spacing(12);
        footer.Margin({ 8, 8, 8, 0 });
        Button preferences;
        preferences.Content(box_value(Label(L"GitHubPreferences")));
        Flyout preferencesFlyout;
        StackPanel preferencesPanel;
        preferencesPanel.Spacing(12);
        preferencesPanel.MaxWidth(360);
        preferencesPanel.Children().Append(Text(Label(L"GitHubRefreshInterval")));
        ComboBox interval;
        for (const auto minutes : { 1, 5, 15, 30, 60 })
        {
            interval.Items().Append(box_value(to_hstring(minutes)));
        }
        interval.SelectedIndex(1);
        AutomationProperties::SetName(interval, Label(L"GitHubRefreshInterval"));
        interval.SelectionChanged([weak](const IInspectable& sender, const auto&) {
            if (auto self = weak.lock())
            {
                const auto interval = sender.as<ComboBox>();
                constexpr std::array values{ 1, 5, 15, 30, 60 };
                const auto index = interval.SelectedIndex();
                if (index >= 0 && index < static_cast<int32_t>(values.size()))
                {
                    self->_timer.Interval(std::chrono::minutes{ values[index] });
                }
            }
        });
        preferencesPanel.Children().Append(interval);
        preferencesPanel.Children().Append(Text(Label(L"GitHubCellSize")));
        ComboBox size;
        for (const auto key : { L"GitHubSmall", L"GitHubMedium", L"GitHubLarge" })
        {
            size.Items().Append(box_value(Label(key)));
        }
        size.SelectedIndex(0);
        AutomationProperties::SetName(size, Label(L"GitHubCellSize"));
        size.SelectionChanged([weak](const IInspectable& sender, const auto&) {
            const auto size = sender.as<ComboBox>();
            if (auto self = weak.lock(); self && size.SelectedIndex() >= 0)
            {
                self->_cellSize = 6 + 2 * size.SelectedIndex();
                self->_RenderCalendar();
            }
        });
        preferencesPanel.Children().Append(size);
        preferencesPanel.Children().Append(Text(Label(L"GitHubPrivacy"), 12));
        preferencesFlyout.Content(preferencesPanel);
        preferences.Flyout(preferencesFlyout);
        footer.Children().Append(preferences);
        _refreshButton.Content(box_value(Label(L"GitHubRefresh")));
        _refreshButton.Click([weak](const auto&, const auto&) {
            if (auto self = weak.lock())
            {
                self->_Refresh(GitHub::RefreshReason::Manual);
            }
        });
        footer.Children().Append(_refreshButton);
        Button close;
        close.Content(box_value(Label(L"GitHubClose")));
        close.Click([weak](const auto&, const auto&) {
            if (auto self = weak.lock())
            {
                self->_flyout.Hide();
            }
        });
        footer.Children().Append(close);
        Grid::SetRow(footer, 5);
        _root.Children().Append(footer);
        _flyout.Content(_root);
        _flyout.FlyoutPresenterStyle(Windows::UI::Xaml::Markup::XamlReader::Load(
                                         LR"(<Style xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" TargetType="FlyoutPresenter">
                <Setter Property="Padding" Value="8" />
                <Setter Property="MinWidth" Value="0" />
                <Setter Property="MaxWidth" Value="420" />
                <Setter Property="MaxHeight" Value="720" />
                <Setter Property="BorderThickness" Value="1" />
                <Setter Property="ScrollViewer.HorizontalScrollBarVisibility" Value="Disabled" />
                <Setter Property="ScrollViewer.HorizontalScrollMode" Value="Disabled" />
                <Setter Property="ScrollViewer.VerticalScrollBarVisibility" Value="Disabled" />
                <Setter Property="ScrollViewer.VerticalScrollMode" Value="Disabled" />
            </Style>)")
                                         .as<Style>());
        _flyout.Placement(FlyoutPlacementMode::BottomEdgeAlignedRight);
        _flyout.Closed([weak](const auto&, const auto&) {
            if (auto self = weak.lock())
            {
                self->_visible = false;
                self->_timer.Stop();
                self->_cancelled->store(true);
            }
        });
        _root.ActualThemeChanged([weak](const auto&, const auto&) {
            if (auto self = weak.lock())
            {
                self->_RenderCalendar();
            }
        });
        _root.KeyDown([weak](const auto&, const KeyRoutedEventArgs& args) {
            if (args.Key() == Windows::System::VirtualKey::F5)
            {
                if (auto self = weak.lock())
                {
                    self->_Refresh(GitHub::RefreshReason::Manual);
                }
                args.Handled(true);
            }
        });
        _timer.Interval(5min);
        _timer.Tick([weak](const auto&, const auto&) {
            if (auto self = weak.lock())
            {
                self->_Refresh(GitHub::RefreshReason::Automatic);
            }
        });
        _RenderHeader();
        _RenderCalendar();
        _RenderSelected();
    }

    void GitHubDashboard::Show(const FrameworkElement& anchor)
    {
        const auto size = anchor.XamlRoot().Size();
        _root.RequestedTheme(anchor.ActualTheme());
        _root.Width(std::max(240.0, std::min(402.0, static_cast<double>(size.Width) - 48)));
        _root.Height(std::max(120.0, std::min(702.0, static_cast<double>(size.Height) - 80)));
        _ApplySnapshot(_snapshot);
        _RenderCalendar();
        _visible = true;
        _flyout.ShowAt(anchor);
        _timer.Start();
        if (_snapshot.login.empty() || std::chrono::steady_clock::now() - _lastRefresh >= 5min)
        {
            _Refresh(GitHub::RefreshReason::Automatic);
        }
    }

    void GitHubDashboard::Close()
    {
        _cancelled->store(true);
        _timer.Stop();
        _flyout.Hide();
    }

    safe_void_coroutine GitHubDashboard::_Refresh(const GitHub::RefreshReason reason)
    {
        if (_busy || !_visible)
        {
            co_return;
        }
        auto lifetime = shared_from_this();
        _ApplySnapshot(_snapshot);
        _busy = true;
        _refreshButton.IsEnabled(false);
        _status.Visibility(Visibility::Visible);
        _status.Text(Label(L"GitHubLoading"));
        const auto dispatcher = _root.Dispatcher();
        auto previous = _snapshot;
        _cancelled = std::make_shared<std::atomic<bool>>(false);
        auto cancelled = _cancelled;
        const auto loadCache = !_cacheLoaded;
        const auto path = _cachePath;
        co_await winrt::resume_background();
        if (loadCache)
        {
            auto cacheWarning = false;
            try
            {
                previous = GitHub::ReadCache(path);
            }
            catch (...)
            {
                LOG_CAUGHT_EXCEPTION();
                cacheWarning = true;
            }
            co_await wil::resume_foreground(dispatcher);
            _cacheLoaded = true;
            _cacheWarning = cacheWarning;
            if (!previous.login.empty())
            {
                _unverified = true;
                _ApplySnapshot(previous);
            }
            co_await winrt::resume_background();
        }
        std::optional<GitHub::Snapshot> snapshot;
        auto failure = GitHub::Failure::None;
        auto writeWarning = false;
        try
        {
            snapshot = GitHub::Refresh([&](const auto& endpoint, const auto& query) { return GitHub::ReadApi(endpoint, query, *cancelled); },
                                       previous,
                                       reason,
                                       [lifetime, dispatcher, cancelled](const GitHub::Snapshot& partial) {
                                           dispatcher.RunAsync(Windows::UI::Core::CoreDispatcherPriority::Normal, [lifetime, cancelled, partial] {
                                               if (lifetime->_busy && lifetime->_visible && lifetime->_cancelled == cancelled && !cancelled->load())
                                               {
                                                   lifetime->_unverified = true;
                                                   lifetime->_ApplySnapshot(partial);
                                               }
                                           });
                                       });
            if (!cancelled->load())
            {
                try
                {
                    GitHub::WriteCache(path, *snapshot);
                }
                catch (...)
                {
                    LOG_CAUGHT_EXCEPTION();
                    writeWarning = true;
                }
            }
        }
        catch (const GitHub::Error& error)
        {
            failure = error.failure;
        }
        catch (...)
        {
            LOG_CAUGHT_EXCEPTION();
            failure = GitHub::Failure::Request;
        }
        co_await wil::resume_foreground(dispatcher);
        _busy = false;
        _refreshButton.IsEnabled(true);
        if (cancelled->load() || failure == GitHub::Failure::Cancelled)
        {
            _status.Text(Label(L"GitHubRefreshCancelled"));
            if (_visible)
            {
                _Refresh(reason);
            }
            co_return;
        }
        if (snapshot)
        {
            _unverified = false;
            _cacheWarning = writeWarning;
            _lastRefresh = std::chrono::steady_clock::now();
            _ApplySnapshot(std::move(*snapshot));
        }
        else
        {
            _unverified = failure != GitHub::Failure::AccountChanged && !_snapshot.login.empty();
            _ApplySnapshot(failure == GitHub::Failure::AccountChanged ? GitHub::Snapshot{} : _snapshot);
            _status.Visibility(Visibility::Visible);
            _status.Text((_unverified ? Label(L"GitHubSavedUnverified") + L" " : hstring{}) + ErrorText(failure));
        }
    }

    void GitHubDashboard::_ApplySnapshot(GitHub::Snapshot snapshot)
    {
        GitHub::ExpireSections(snapshot);
        const auto accountChanged = _snapshot.id != snapshot.id || _snapshot.login != snapshot.login;
        const auto identityChanged = accountChanged || _snapshot.name != snapshot.name || _snapshot.avatarUrl != snapshot.avatarUrl;
        const auto calendarChanged = accountChanged || SectionChanged(_snapshot.calendar, snapshot.calendar);
        const auto usageChanged = accountChanged || SectionChanged(_snapshot.usage, snapshot.usage);
        const auto listChanged = _selectedSection >= 0 && _selectedSection < static_cast<int32_t>(listSections.size()) &&
                                 (accountChanged || SectionChanged(_snapshot.*listSections[_selectedSection], snapshot.*listSections[_selectedSection]));
        std::string selectedDate;
        if (calendarChanged && !accountChanged && _snapshot.calendar.value && _selectedDay && *_selectedDay < _snapshot.calendar.value->days.size())
        {
            selectedDate = _snapshot.calendar.value->days[*_selectedDay].date;
        }
        _snapshot = std::move(snapshot);
        if (identityChanged && _accountChanged)
        {
            _accountChanged(_snapshot);
        }
        _RenderHeader();
        if (calendarChanged)
        {
            _selectedDay.reset();
            if (_snapshot.calendar.value)
            {
                const auto& days = _snapshot.calendar.value->days;
                const auto found = std::find_if(days.begin(), days.end(), [&](const auto& day) { return day.date == selectedDate; });
                if (found != days.end())
                {
                    _selectedDay = static_cast<size_t>(found - days.begin());
                }
            }
            _RenderCalendar();
        }
        if (usageChanged)
            _RenderUsage();
        if (listChanged)
            _RenderSelected();
    }

    void GitHubDashboard::_RenderHeader()
    {
        _account.Text(_snapshot.login.empty() ? Label(L"GitHubDashboard") : to_hstring("@" + _snapshot.login));
        _accountName.Text(to_hstring(_snapshot.name));
        _status.Text(_unverified             ? Label(L"GitHubSavedUnverified") :
                     _cacheWarning           ? Label(L"GitHubCacheWarning") :
                     _snapshot.login.empty() ? Label(L"GitHubSignIn") :
                                               hstring{});
        _status.Visibility(_status.Text().empty() ? Visibility::Collapsed : Visibility::Visible);
    }

    void GitHubDashboard::_RenderUsage()
    {
        _usage.Children().Clear();
        if (_snapshot.usage.failure != GitHub::Failure::None)
        {
            SectionStatus(_snapshot.usage, _usage);
        }
        if (_snapshot.usage.value)
        {
            const auto& usage = *_snapshot.usage.value;
            for (const auto& quota : usage.quotas)
            {
                if (quota.unlimited && quota.kind != "premium_interactions")
                {
                    continue;
                }
                const auto name = Text(QuotaName(quota));
                name.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
                const auto plan = Text(to_hstring(usage.plan), 12);
                plan.Opacity(0.7);
                _usage.Children().Append(Pair(name, plan));
                if (quota.used)
                {
                    ProgressBar bar;
                    bar.Minimum(0);
                    bar.Maximum(100);
                    bar.Value(*quota.used);
                    AutomationProperties::SetName(bar, QuotaName(quota));
                    _usage.Children().Append(bar);
                    _usage.Children().Append(Pair(
                        Text(to_hstring(std::format("{:.1f}%", *quota.used)) + L" " + Label(L"GitHubUsed"), 12),
                        Text(quota.reset.empty() ? hstring{} : Label(L"GitHubResets") + L" " + to_hstring(quota.reset.substr(0, 10)), 12)));
                }
                else
                {
                    _usage.Children().Append(Text(Label(quota.unlimited ? L"GitHubUnlimited" : L"GitHubNotIncluded")));
                }
            }
        }
    }

    void GitHubDashboard::_RenderSelected()
    {
        if (_selectedSection < 0 || _selectedSection >= static_cast<int32_t>(listSections.size()))
            return;
        _sectionStatus.Children().Clear();
        const auto& section = _snapshot.*listSections[_selectedSection];
        SectionStatus(section, _sectionStatus);
        _RenderItems(section, _list);
    }

    void GitHubDashboard::_RenderCalendar()
    {
        _calendarGrid.Children().Clear();
        _calendarGrid.ColumnDefinitions().Clear();
        _calendarGrid.RowDefinitions().Clear();
        GitHub::Calendar loading;
        if (!_snapshot.calendar.value)
        {
            for (uint32_t week = 0; week < 53; ++week)
            {
                for (uint32_t day = 0; day < 7; ++day)
                    loading.days.push_back({ {}, day, 0, 0, week });
            }
            _calendarSummary.Text(Label(L"GitHubNotLoaded"));
            _dayDescription.Text({});
            AutomationProperties::SetName(_calendarButton, Label(L"GitHubLoading"));
        }
        const auto& calendar = _snapshot.calendar.value ? *_snapshot.calendar.value : loading;
        _calendarButton.IsEnabled(_snapshot.calendar.value.has_value());
        if (_snapshot.calendar.value)
        {
            _calendarSummary.Text(to_hstring(calendar.total) + L" - " + Label(L"GitHubTwelveMonths") +
                                  (_snapshot.calendar.failure == GitHub::Failure::None ? hstring{} : L" - " + Label(L"GitHubStale")));
        }
        ToolTipService::SetToolTip(_calendarButton, box_value(_snapshot.calendar.failure == GitHub::Failure::None ? Label(L"GitHubCalendarKeys") : ErrorText(_snapshot.calendar.failure)));
        const auto cellSize = _cellSize == 6 ? std::max(2.0, std::floor((_root.Width() - 24) / (calendar.days.back().week + 1)) - 2) : _cellSize;
        for (uint32_t i = 0; i <= calendar.days.back().week; ++i)
        {
            ColumnDefinition column;
            column.Width({ cellSize + 2, GridUnitType::Pixel });
            _calendarGrid.ColumnDefinitions().Append(column);
        }
        for (uint32_t i = 0; i < 7; ++i)
        {
            RowDefinition row;
            row.Height({ cellSize + 2, GridUnitType::Pixel });
            _calendarGrid.RowDefinitions().Append(row);
        }
        const auto highContrast = Windows::UI::ViewManagement::AccessibilitySettings{}.HighContrast();
        const auto cellStyle = Windows::UI::Xaml::Markup::XamlReader::Load(
                                   LR"(<Style xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" TargetType="Border">
                <Setter Property="BorderBrush" Value="{ThemeResource SystemControlForegroundBaseHighBrush}" />
                <Setter Property="Background" Value="{ThemeResource SystemControlBackgroundBaseLowBrush}" />
            </Style>)")
                                   .as<Style>();
        const auto activeCellStyle = Windows::UI::Xaml::Markup::XamlReader::Load(
                                         LR"(<Style xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" TargetType="Border">
                <Setter Property="BorderBrush" Value="{ThemeResource SystemControlForegroundBaseHighBrush}" />
                <Setter Property="Background" Value="{ThemeResource SystemControlHighlightAccentBrush}" />
            </Style>)")
                                         .as<Style>();
        constexpr std::array<Color, 5> colors{ Color{ 255, 55, 65, 60 }, Color{ 255, 24, 78, 42 }, Color{ 255, 40, 120, 65 }, Color{ 255, 64, 170, 85 }, Color{ 255, 120, 220, 115 } };
        const auto weak = weak_from_this();
        for (size_t i = 0; i < calendar.days.size(); ++i)
        {
            const auto& day = calendar.days[i];
            Border cell;
            cell.Style(highContrast && day.count ? activeCellStyle : cellStyle);
            cell.Width(cellSize);
            cell.Height(cellSize);
            Grid::SetColumn(cell, static_cast<int32_t>(day.week));
            Grid::SetRow(cell, static_cast<int32_t>(day.weekday));
            if (!highContrast)
            {
                cell.Background(SolidColorBrush{ colors[day.level] });
            }
            const auto description = to_hstring(day.date + ": " + std::to_string(day.count)) + L" " + Label(L"GitHubContributions");
            if (_snapshot.calendar.value)
                ToolTipService::SetToolTip(cell, box_value(description));
            cell.PointerEntered([weak, i](const auto&, const auto&) {
                if (auto self = weak.lock())
                {
                    self->_SelectDay(i);
                }
            });
            _calendarGrid.Children().Append(cell);
        }
        _SelectDay(_selectedDay && *_selectedDay < calendar.days.size() ? *_selectedDay : calendar.days.size() - 1);
    }

    void GitHubDashboard::_SelectDay(const size_t index)
    {
        if (!_snapshot.calendar.value || index >= _snapshot.calendar.value->days.size())
        {
            return;
        }
        if (_selectedDay && *_selectedDay < _calendarGrid.Children().Size())
        {
            _calendarGrid.Children().GetAt(static_cast<uint32_t>(*_selectedDay)).as<Border>().BorderThickness({ 0, 0, 0, 0 });
        }
        _selectedDay = index;
        const auto cell = _calendarGrid.Children().GetAt(static_cast<uint32_t>(index)).as<Border>();
        cell.BorderThickness({ 1, 1, 1, 1 });
        cell.StartBringIntoView();
        const auto& day = _snapshot.calendar.value->days[index];
        const auto description = to_hstring(day.date + ": " + std::to_string(day.count)) + L" " + Label(L"GitHubContributions");
        _dayDescription.Text(description);
        AutomationProperties::SetName(_calendarButton, description + L". " + Label(L"GitHubCalendarKeys"));
    }

    void GitHubDashboard::SetAvatar(const PersonPicture& picture, const std::string& url)
    {
        const auto previous = picture.ProfilePicture().try_as<Imaging::BitmapImage>();
        picture.ProfilePicture(nullptr);
        if (previous)
        {
            previous.UriSource(nullptr);
        }
        ToolTipService::SetToolTip(picture, nullptr);
        if (url.empty())
        {
            return;
        }
        THROW_HR_IF(E_INVALIDARG, !GitHub::IsGitHubAvatarUrl(url));
        Imaging::BitmapImage image;
        image.DecodePixelWidth(96);
        image.ImageFailed([weak = winrt::make_weak(picture)](const IInspectable& failedImage, const auto&) {
            if (const auto target = weak.get(); target && target.ProfilePicture() == failedImage)
            {
                LOG_HR_MSG(E_FAIL, "GitHub avatar image failed to load");
                target.ProfilePicture(nullptr);
                ToolTipService::SetToolTip(target, box_value(Label(L"GitHubAvatarUnavailable")));
            }
        });
        picture.ProfilePicture(image);
        image.UriSource(Uri{ to_hstring(url) });
    }

    void GitHubDashboard::_RenderItems(const GitHub::Section<std::vector<GitHub::Item>>& section, const StackPanel& panel)
    {
        panel.Children().Clear();
        if (!section.value)
        {
            return;
        }
        if (section.value->empty())
        {
            panel.Children().Append(Text(Label(L"GitHubEmpty")));
        }
        for (const auto& item : *section.value)
        {
            StackPanel card;
            card.Spacing(4);
            card.Margin({ 32, 8, 0, 8 });
            Grid container;
            if (!item.author.empty())
            {
                PersonPicture author;
                author.Width(24);
                author.Height(24);
                author.DisplayName(to_hstring(item.author));
                if (!item.avatarUrl.empty())
                {
                    author.Loaded([url = item.avatarUrl](const IInspectable& sender, const auto&) {
                        const auto picture = sender.as<PersonPicture>();
                        SetAvatar(picture, url);
                    });
                    author.Unloaded([](const IInspectable& sender, const auto&) {
                        const auto picture = sender.as<PersonPicture>();
                        SetAvatar(picture, {});
                    });
                }
                author.VerticalAlignment(VerticalAlignment::Top);
                author.HorizontalAlignment(HorizontalAlignment::Left);
                author.Margin({ 0, 12, 8, 0 });
                container.Children().Append(author);
            }
            else
            {
                card.Margin({ 0, 8, 0, 8 });
            }
            container.Children().Append(card);
            panel.Children().Append(container);
            const auto state = Text(StateLabel(item.state), 12);
            state.VerticalAlignment(VerticalAlignment::Center);
            card.Children().Append(Pair(Link(item.title, item.url), state));
            if (!item.repository.empty() && (item.repository != item.title || item.number || !item.author.empty()))
            {
                card.Children().Append(Text(to_hstring(item.repository + (item.number ? "  #" + std::to_string(item.number) : "") +
                                                       (item.author.empty() ? "" : "  @" + item.author)),
                                            12));
            }
            if (!item.activityKind.empty())
            {
                card.Children().Append(Text(ActivityLabel(item.activityKind), 12));
            }
            if (!item.description.empty())
            {
                card.Children().Append(Text(to_hstring(item.description), 12));
            }
            if (item.events)
            {
                card.Children().Append(Text(to_hstring(item.events) + L" " + Label(L"GitHubEvents"), 12));
            }
            if (!item.head.empty())
            {
                const auto branches = Text(to_hstring(item.head + " -> " + item.base), 11);
                branches.FontFamily(FontFamily{ L"Cascadia Mono, Consolas" });
                branches.Opacity(0.7);
                card.Children().Append(branches);
                if (!item.review.empty())
                {
                    card.Children().Append(Text(StateLabel(item.review), 12));
                }
                std::string labels;
                for (const auto& label : item.labels)
                {
                    labels += (labels.empty() ? "" : ", ") + label;
                }
                if (!labels.empty())
                {
                    card.Children().Append(Text(to_hstring(labels), 12));
                }
                if (item.labelCount > item.labels.size())
                {
                    card.Children().Append(Text(Label(L"GitHubLabelsLimited") + L" " + to_hstring(item.labelCount), 12));
                }
                Button checks;
                const auto passed = std::count_if(item.checks.begin(), item.checks.end(), [](const auto& check) { return check.state == "SUCCESS"; });
                checks.Content(box_value(Label(L"GitHubChecks") + L" " + to_hstring(passed) + L"/" + to_hstring(item.checkCount)));
                checks.HorizontalAlignment(HorizontalAlignment::Stretch);
                checks.HorizontalContentAlignment(HorizontalAlignment::Left);
                Flyout details;
                details.Opening([item](const IInspectable& sender, const auto&) {
                    StackPanel contents;
                    contents.Spacing(6);
                    contents.MaxWidth(400);
                    if (item.checks.empty())
                    {
                        contents.Children().Append(Text(Label(L"GitHubNoChecks")));
                    }
                    for (const auto& check : item.checks)
                    {
                        contents.Children().Append(Text(to_hstring(check.state + " - " + check.name), 12));
                    }
                    if (item.checkCount > item.checks.size())
                    {
                        contents.Children().Append(Text(Label(L"GitHubChecksLimited") + L" " + to_hstring(item.checkCount)));
                    }
                    contents.Children().Append(Link("GitHub", item.url + "/checks"));
                    ScrollViewer scroll;
                    scroll.MaxHeight(300);
                    scroll.Content(contents);
                    sender.as<Flyout>().Content(scroll);
                });
                checks.Flyout(details);
                card.Children().Append(checks);
                card.Children().Append(Text(to_hstring(item.comments) + L" " + Label(L"GitHubComments"), 12));
            }
            card.Children().Append(Text(to_hstring(item.updatedAt), 12));
            Separator(panel);
        }
    }
}
