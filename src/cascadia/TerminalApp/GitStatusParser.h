// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "GitStatus.h"

#include <optional>
#include <string>
#include <string_view>

namespace TerminalApp::GitStatus
{
    Microsoft::Terminal::StatusBar::GitQueryResult ParsePorcelainV2(std::string_view output);

    std::optional<Microsoft::Terminal::StatusBar::GitLineChanges> ParseNumStat(
        std::string_view output) noexcept;

    Microsoft::Terminal::StatusBar::GitQueryStatus ClassifyGitFailure(
        std::string_view diagnostic,
        bool executableUnavailable = false) noexcept;

    namespace details
    {
        class WslControlParser
        {
        public:
            void Observe(std::string_view chunk);

            [[nodiscard]] std::optional<uint32_t> ProcessGroup() const noexcept;
            [[nodiscard]] bool GitUnavailable() const noexcept;

        private:
            std::string _suffix;
            std::optional<uint32_t> _processGroup;
            bool _gitUnavailable{};
        };
    }
}
