// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "StatusBarControl.g.h"
#include "StatusBarPresentation.h"

namespace TerminalAppLocalTests
{
    class TabTests;
}

namespace winrt::TerminalApp::implementation
{
    struct StatusBarControl : StatusBarControlT<StatusBarControl>
    {
        StatusBarControl();

        void ApplySnapshot(const ::Microsoft::Terminal::StatusBar::StatusBarSnapshot& snapshot);
        void _OnSizeChanged(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::SizeChangedEventArgs& args);

    private:
        ::TerminalApp::StatusBarPresentation::Strings _strings;
        ::TerminalApp::StatusBarPresentation::View _presentation;
        std::wstring _lastAccessibleSummary;

        void _ApplyPresentation(const ::TerminalApp::StatusBarPresentation::View& presentation);
        void _UpdateResponsiveVisibility(double availableWidth);

        friend class ::TerminalAppLocalTests::TabTests;
    };
}

namespace winrt::TerminalApp::factory_implementation
{
    BASIC_FACTORY(StatusBarControl);
}
