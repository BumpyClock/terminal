// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

namespace TerminalApp
{
    struct GitHubDashboardPlacement
    {
        double x;
        double y;
        double width;
        double height;
    };

    GitHubDashboardPlacement CalculateGitHubDashboardPlacement(
        const winrt::Windows::Foundation::Size& hostSize,
        const winrt::Windows::Foundation::Rect& anchorBounds) noexcept;

    bool ShouldUseCompactGitHubDashboardLayout(
        double availableHeight,
        double fixedContentHeight) noexcept;
}
