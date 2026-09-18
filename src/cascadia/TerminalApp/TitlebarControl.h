// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "TitlebarControl.g.h"
#include <atomic>

namespace TerminalApp
{
    class GitHubDashboard;
}

namespace winrt::TerminalApp::implementation
{
    struct TitlebarControl : TitlebarControlT<TitlebarControl>
    {
        TitlebarControl(uint64_t handle);

        void HoverButton(CaptionButton button);
        void PressButton(CaptionButton button);
        safe_void_coroutine ClickButton(CaptionButton button);
        void ReleaseButtons();
        float CaptionButtonWidth();

        bool Focused();
        void Focused(bool focused);

        IInspectable Content();
        void Content(IInspectable content);

        void SetWindowVisualState(WindowVisualState visualState);
        void Root_SizeChanged(const IInspectable& sender, const Windows::UI::Xaml::SizeChangedEventArgs& e);
        void FullscreenChanged(const bool fullscreen);

        void Minimize_Click(const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::UI::Xaml::RoutedEventArgs& e);
        void Maximize_Click(const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::UI::Xaml::RoutedEventArgs& e);
        void Close_Click(const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::UI::Xaml::RoutedEventArgs& e);
        void DragBar_DoubleTapped(const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::UI::Xaml::Input::DoubleTappedRoutedEventArgs& e);
        void GitHub_Click(const IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& e);

    private:
        std::shared_ptr<::TerminalApp::GitHubDashboard> _githubDashboard;
        std::shared_ptr<std::atomic<bool>> _avatarRequest{ std::make_shared<std::atomic<bool>>(false) };
        std::string _avatarUrl;
        safe_void_coroutine _LoadGitHubAvatar();
        void _SetGitHubAvatar(const std::string& login, const std::string& url);
        void _OnMaximizeOrRestore(byte flag);
        HWND _window{ nullptr }; // non-owning handle; should not be freed in the dtor.

        void _backgroundChanged(winrt::Windows::UI::Xaml::Media::Brush brush);
    };
}

namespace winrt::TerminalApp::factory_implementation
{
    BASIC_FACTORY(TitlebarControl);
}
