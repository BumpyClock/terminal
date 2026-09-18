// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "GitHubDashboardData.h"
#include "GitHubDashboardLayout.h"
#include <memory>

namespace TerminalApp
{
    class GitHubDashboard : public std::enable_shared_from_this<GitHubDashboard>
    {
    public:
        static std::shared_ptr<GitHubDashboard> Create(GitHub::Update accountChanged = {});
        static void SetAvatar(const winrt::Windows::UI::Xaml::Controls::PersonPicture& picture, const std::string& url);
        void Show(const winrt::Windows::UI::Xaml::FrameworkElement& anchor,
                  const winrt::Windows::UI::Xaml::Controls::Grid& host);
        void Close();

    private:
        void _Initialize();
        void _AttachHost(const winrt::Windows::UI::Xaml::Controls::Grid& host);
        void _UpdatePlacement();
        void _Close(bool restoreFocus);
        safe_void_coroutine _Refresh(GitHub::RefreshReason reason);
        void _ApplySnapshot(GitHub::Snapshot snapshot);
        void _RenderHeader();
        void _RenderUsage();
        void _RenderSelected();
        void _RenderCalendar();
        void _SelectDay(size_t index);
        void _RenderItems(const GitHub::Section<std::vector<GitHub::Item>>& section,
                          const winrt::Windows::UI::Xaml::Controls::StackPanel& panel);

        GitHub::Snapshot _snapshot;
        GitHub::Update _accountChanged;
        std::shared_ptr<std::atomic<bool>> _cancelled{ std::make_shared<std::atomic<bool>>(false) };
        bool _busy{};
        bool _visible{};
        bool _cacheLoaded{};
        bool _unverified{};
        bool _cacheWarning{};
        bool _compactLayout{};
        int32_t _selectedSection{};
        std::filesystem::path _cachePath;
        double _cellSize{ 6 };
        double _listScrollOffset{};
        std::optional<size_t> _selectedDay;
        std::chrono::steady_clock::time_point _lastRefresh{};
        winrt::weak_ref<winrt::Windows::UI::Xaml::Controls::Grid> _host;
        winrt::weak_ref<winrt::Windows::UI::Xaml::FrameworkElement> _anchor;
        winrt::event_token _hostLayoutUpdated;
        winrt::event_token _hostPointerPressed;
        bool _hostAttached{};
        winrt::Windows::UI::Xaml::Controls::FlyoutPresenter _presenter;
        winrt::Windows::UI::Xaml::Controls::Grid _root;
        winrt::Windows::UI::Xaml::Controls::TextBlock _account;
        winrt::Windows::UI::Xaml::Controls::TextBlock _accountName;
        winrt::Windows::UI::Xaml::Controls::TextBlock _status;
        winrt::Windows::UI::Xaml::Controls::TextBlock _calendarSummary;
        winrt::Windows::UI::Xaml::Controls::Button _calendarButton;
        winrt::Windows::UI::Xaml::Controls::Grid _calendarGrid;
        winrt::Windows::UI::Xaml::Controls::TextBlock _dayDescription;
        winrt::Windows::UI::Xaml::Controls::StackPanel _usage;
        winrt::Windows::UI::Xaml::Controls::StackPanel _sectionStatus;
        winrt::Windows::UI::Xaml::Controls::ListBox _sections;
        winrt::Windows::UI::Xaml::Controls::Button _refreshButton;
        winrt::Windows::UI::Xaml::Controls::ScrollViewer _overflow;
        winrt::Windows::UI::Xaml::Controls::ScrollViewer _scroll;
        winrt::Windows::UI::Xaml::Controls::StackPanel _list;
        winrt::Windows::UI::Xaml::DispatcherTimer _timer;
    };
}
