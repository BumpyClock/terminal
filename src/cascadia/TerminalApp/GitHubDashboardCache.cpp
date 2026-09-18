// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "GitHubDashboardData.h"
#include <til/io.h>

namespace TerminalApp::GitHub
{
    namespace
    {
        constexpr DWORD maximumBytes = 4 * 1024 * 1024;
        constexpr std::array textFields{
            std::pair{ "title", &Item::title },
            std::pair{ "repository", &Item::repository },
            std::pair{ "url", &Item::url },
            std::pair{ "updatedAt", &Item::updatedAt },
            std::pair{ "description", &Item::description },
            std::pair{ "activityKind", &Item::activityKind },
            std::pair{ "state", &Item::state },
            std::pair{ "author", &Item::author },
            std::pair{ "head", &Item::head },
            std::pair{ "base", &Item::base },
            std::pair{ "review", &Item::review }
        };
        constexpr std::array numberFields{
            std::pair{ "number", &Item::number },
            std::pair{ "comments", &Item::comments },
            std::pair{ "labelCount", &Item::labelCount },
            std::pair{ "checkCount", &Item::checkCount },
            std::pair{ "events", &Item::events }
        };

        void Require(const bool condition)
        {
            if (!condition)
            {
                throw Error{ Failure::InvalidResponse };
            }
        }

        std::string String(const Json::Value& value)
        {
            Require(value.isString() && value.asString().size() <= 16384);
            const auto text = value.asString();
            Require(text.find('\0') == std::string::npos);
            return text;
        }

        Json::Value Encode(const std::vector<Item>& items)
        {
            Json::Value array{ Json::arrayValue };
            for (const auto& item : items)
            {
                Json::Value value;
                for (const auto& [key, member] : textFields)
                    value[key] = item.*member;
                for (const auto& [key, member] : numberFields)
                    value[key] = item.*member;
                value["avatarUrl"] = item.avatarUrl;
                value["labels"] = Json::Value{ Json::arrayValue };
                for (const auto& label : item.labels)
                    value["labels"].append(label);
                value["checks"] = Json::Value{ Json::arrayValue };
                for (const auto& check : item.checks)
                {
                    Json::Value entry;
                    entry["name"] = check.name;
                    entry["state"] = check.state;
                    value["checks"].append(entry);
                }
                array.append(value);
            }
            return array;
        }

        void Decode(const Json::Value& array, std::vector<Item>& items)
        {
            Require(array.isArray() && array.size() <= 30);
            for (const auto& value : array)
            {
                Require(value.isObject());
                Item item;
                for (const auto& [key, member] : textFields)
                    item.*member = String(value[key]);
                for (const auto& [key, member] : numberFields)
                {
                    Require(value[key].isUInt());
                    item.*member = value[key].asUInt();
                }
                Require(IsGitHubUrl(item.url));
                // Older snapshots predate avatar support.
                if (!value["avatarUrl"].isNull())
                {
                    item.avatarUrl = String(value["avatarUrl"]);
                    Require(item.avatarUrl.empty() || IsGitHubAvatarUrl(item.avatarUrl));
                }
                Require(value["labels"].isArray() && value["labels"].size() <= 10);
                for (const auto& label : value["labels"])
                    item.labels.emplace_back(String(label));
                Require(value["checks"].isArray() && value["checks"].size() <= 100);
                for (const auto& check : value["checks"])
                {
                    item.checks.push_back({ String(check["name"]), String(check["state"]) });
                }
                Require(item.checks.size() <= item.checkCount && item.labels.size() <= item.labelCount);
                items.emplace_back(std::move(item));
            }
        }

        Json::Value Encode(const Calendar& calendar)
        {
            Json::Value value;
            value["totalContributions"] = calendar.total;
            value["weeks"] = Json::Value{ Json::arrayValue };
            constexpr std::array levels{ "NONE", "FIRST_QUARTILE", "SECOND_QUARTILE", "THIRD_QUARTILE", "FOURTH_QUARTILE" };
            for (const auto& day : calendar.days)
            {
                Require(day.week < 54 && day.level < levels.size());
                Json::Value entry;
                entry["date"] = day.date;
                entry["weekday"] = day.weekday;
                entry["contributionCount"] = day.count;
                entry["contributionLevel"] = levels[day.level];
                value["weeks"][day.week]["contributionDays"].append(entry);
            }
            return value;
        }

        void Decode(const Json::Value& value, Calendar& calendar)
        {
            calendar = ParseCalendar(value);
        }

        Json::Value Encode(const Usage& usage)
        {
            Json::Value value;
            value["plan"] = usage.plan;
            value["quotas"] = Json::Value{ Json::arrayValue };
            for (const auto& quota : usage.quotas)
            {
                Json::Value entry;
                entry["kind"] = quota.kind;
                entry["credits"] = quota.credits;
                entry["unlimited"] = quota.unlimited;
                entry["included"] = quota.included;
                entry["used"] = quota.used ? Json::Value{ *quota.used } : Json::Value{};
                entry["reset"] = quota.reset;
                value["quotas"].append(entry);
            }
            return value;
        }

        void Decode(const Json::Value& value, Usage& usage)
        {
            usage.plan = String(value["plan"]);
            const auto& quotas = value["quotas"];
            Require(quotas.isArray() && !quotas.empty() && quotas.size() <= 3);
            for (const auto& entry : quotas)
            {
                Quota quota;
                quota.kind = String(entry["kind"]);
                Require(quota.kind == "premium_interactions" || quota.kind == "chat" || quota.kind == "completions");
                Require(entry["credits"].isBool() && entry["unlimited"].isBool() && entry["included"].isBool());
                quota.credits = entry["credits"].asBool();
                quota.unlimited = entry["unlimited"].asBool();
                quota.included = entry["included"].asBool();
                if (!quota.unlimited && quota.included)
                {
                    Require(entry["used"].isNumeric());
                    const auto used = entry["used"].asDouble();
                    Require(used >= 0 && used <= 100);
                    quota.used = used;
                }
                else
                {
                    Require(entry["used"].isNull());
                }
                quota.reset = String(entry["reset"]);
                usage.quotas.emplace_back(std::move(quota));
            }
        }

        template<typename T>
        Json::Value EncodeSection(const Section<T>& section)
        {
            Json::Value value;
            if (section.value)
            {
                value["updated"] = static_cast<Json::Int64>(std::chrono::duration_cast<std::chrono::milliseconds>(section.updated.time_since_epoch()).count());
                value["value"] = Encode(*section.value);
                value["failed"] = section.failure != Failure::None;
            }
            return value;
        }

        template<typename T>
        void DecodeSection(const Json::Value& value, Section<T>& section)
        {
            if (value.isNull())
                return;
            Require(value.isObject() && value["updated"].isInt64() && value["failed"].isBool());
            const auto timestamp = value["updated"].asInt64();
            const auto now = std::chrono::system_clock::now();
            const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
            Require(timestamp >= 0 && timestamp <= nowMs);
            section.updated = std::chrono::system_clock::time_point{ std::chrono::milliseconds{ timestamp } };
            if (IsSectionExpired(section.updated, now))
                return;
            T data;
            Decode(value["value"], data);
            section.value = std::move(data);
            section.failure = value["failed"].asBool() ? Failure::Request : Failure::None;
        }
    }

    Json::Value SerializeCache(const Snapshot& snapshot)
    {
        Json::Value value;
        value["version"] = 1;
        value["host"] = "github.com";
        value["id"] = snapshot.id;
        value["login"] = snapshot.login;
        value["name"] = snapshot.name;
        value["avatarUrl"] = snapshot.avatarUrl;
        value["activity"] = EncodeSection(snapshot.activity);
        value["pulls"] = EncodeSection(snapshot.pulls);
        value["reviews"] = EncodeSection(snapshot.reviews);
        value["repos"] = EncodeSection(snapshot.repos);
        value["calendar"] = EncodeSection(snapshot.calendar);
        value["usage"] = EncodeSection(snapshot.usage);
        return value;
    }

    Snapshot DeserializeCache(const Json::Value& value)
    {
        try
        {
            Require(value.isObject() && value["version"].isInt() && value["version"].asInt() == 1 && value["host"] == "github.com");
            Snapshot snapshot;
            snapshot.id = String(value["id"]);
            snapshot.login = String(value["login"]);
            snapshot.name = String(value["name"]);
            if (!value["avatarUrl"].isNull())
            {
                snapshot.avatarUrl = String(value["avatarUrl"]);
                Require(snapshot.avatarUrl.empty() || IsGitHubAvatarUrl(snapshot.avatarUrl));
            }
            Require(!snapshot.id.empty() && snapshot.id.size() <= 20 &&
                    snapshot.id.find_first_not_of("0123456789") == std::string::npos &&
                    !snapshot.login.empty() && snapshot.login.size() <= 100 &&
                    snapshot.login.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-") == std::string::npos);
            DecodeSection(value["activity"], snapshot.activity);
            DecodeSection(value["pulls"], snapshot.pulls);
            DecodeSection(value["reviews"], snapshot.reviews);
            DecodeSection(value["repos"], snapshot.repos);
            DecodeSection(value["calendar"], snapshot.calendar);
            DecodeSection(value["usage"], snapshot.usage);
            return snapshot;
        }
        catch (const Json::Exception&)
        {
            throw Error{ Failure::InvalidResponse };
        }
    }

    std::filesystem::path CachePath()
    {
        return std::filesystem::path{ std::wstring_view{ winrt::Microsoft::Terminal::Settings::Model::CascadiaSettings::SettingsPath() } }.parent_path() / L"github-dashboard.json";
    }

    Snapshot ReadCache(const std::filesystem::path& path)
    {
        wil::unique_hfile file{ CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr) };
        if (!file)
        {
            const auto error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
                return {};
            THROW_WIN32(error);
        }
        LARGE_INTEGER size{};
        THROW_IF_WIN32_BOOL_FALSE(GetFileSizeEx(file.get(), &size));
        Require(size.QuadPart > 0 && size.QuadPart <= maximumBytes);
        std::string text(static_cast<size_t>(size.QuadPart), '\0');
        DWORD read{};
        THROW_IF_WIN32_BOOL_FALSE(ReadFile(file.get(), text.data(), static_cast<DWORD>(text.size()), &read, nullptr));
        Require(read == text.size());
        Json::CharReaderBuilder builder;
        builder["rejectDupKeys"] = true;
        const std::unique_ptr<Json::CharReader> reader{ builder.newCharReader() };
        Json::Value value;
        std::string error;
        Require(reader->parse(text.data(), text.data() + text.size(), &value, &error));
        return DeserializeCache(value);
    }

    void WriteCache(const std::filesystem::path& path, const Snapshot& snapshot)
    {
        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        const auto text = Json::writeString(builder, SerializeCache(snapshot));
        Require(text.size() <= maximumBytes);
        std::filesystem::create_directories(path.parent_path());
        GUID id{};
        THROW_IF_FAILED(CoCreateGuid(&id));
        auto staging = path;
        staging += std::wstring{ winrt::to_hstring(id) } + L".tmp";
        const auto cleanup = wil::scope_exit([&] {
            if (!DeleteFileW(staging.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND)
                LOG_LAST_ERROR();
        });
        til::io::write_utf8_string_to_file(staging, text, false);
        THROW_IF_WIN32_BOOL_FALSE(MoveFileExW(staging.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH));
    }
}
