// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "GitHubDashboardData.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <format>
#include <future>
#include <mutex>
#include <map>
#include <set>

namespace TerminalApp::GitHub
{
    namespace
    {
        constexpr auto sectionLifetime = std::chrono::hours{ 24 * 7 };

        void Require(const bool condition)
        {
            if (!condition)
            {
                throw Error{ Failure::InvalidResponse };
            }
        }

        std::string Text(const Json::Value& value, const char* key, const bool optional = false)
        {
            Require(value.isObject());
            const auto& field = value[key];
            if (optional && field.isNull())
            {
                return {};
            }
            Require(field.isString());
            auto text = field.asString();
            Require(text.size() <= 16384 && (optional || !text.empty()));
            Require(text.find('\0') == std::string::npos);
            return text;
        }

        uint32_t Count(const Json::Value& value, const char* key)
        {
            Require(value.isObject());
            Require(value[key].isUInt());
            return value[key].asUInt();
        }

        bool Boolean(const Json::Value& value, const char* key, const bool fallback)
        {
            Require(value.isObject());
            if (value[key].isNull())
            {
                return fallback;
            }
            Require(value[key].isBool());
            return value[key].asBool();
        }

        void Array(const Json::Value& value, const uint32_t maximum)
        {
            Require(value.isArray() && value.size() <= maximum);
        }

        void Account(const std::string& actual, const std::string& expected)
        {
            if (actual != expected)
            {
                throw Error{ Failure::AccountChanged };
            }
        }

        const Json::Value& Graph(const Json::Value& value, const std::string& login)
        {
            if (!value["data"]["viewer"]["login"].isNull())
            {
                Account(Text(value["data"]["viewer"], "login"), login);
            }
            Require(value["errors"].isNull() || (value["errors"].isArray() && value["errors"].empty()));
            Account(Text(value["data"]["viewer"], "login"), login);
            return value["data"];
        }

        std::string Url(const Json::Value& value, const char* key)
        {
            auto url = Text(value, key);
            Require(IsGitHubUrl(url));
            return url;
        }

        constexpr auto pullFields =
            "number title url updatedAt state isDraft headRefName baseRefName reviewDecision "
            "repository { nameWithOwner } author { login avatarUrl(size:96) } comments { totalCount } "
            "labels(first:10) { totalCount nodes { name } } "
            "commits(last:1) { nodes { commit { statusCheckRollup { contexts(first:100) { "
            "totalCount nodes { __typename ... on CheckRun { name status conclusion } "
            "... on StatusContext { context state } } } } } } } ";

        template<typename T>
        Section<T> Failed(const Section<T>& previous, const Failure failure)
        {
            auto result = previous;
            // A failure never renews the age of retained private data.
            if (IsSectionExpired(result.updated))
            {
                result.value.reset();
            }
            result.failure = failure;
            return result;
        }

        template<typename T, typename F>
        Section<T> Load(const Section<T>& previous, F&& fetch)
        {
            try
            {
                return { fetch(), std::chrono::system_clock::now(), Failure::None };
            }
            catch (const Error& error)
            {
                if (error.failure == Failure::AccountChanged || error.failure == Failure::Cancelled)
                {
                    throw;
                }
                return Failed(previous, error.failure);
            }
            catch (const Json::Exception&)
            {
                return Failed(previous, Failure::InvalidResponse);
            }
        }

        std::vector<Item> Pulls(const Request& request, const std::string& login, const bool reviews)
        {
            const auto filter = reviews ? "is:pr is:open review-requested:" : "is:pr author:";
            const auto query = std::string{ "query { viewer { login } search(type:ISSUE,first:30,query:\"" } +
                               filter + login + " sort:updated-desc\") { nodes { ... on PullRequest { " + pullFields + " } } } }";
            const auto response = request("graphql", query);
            const auto& nodes = Graph(response, login)["search"]["nodes"];
            Array(nodes, 30);
            std::vector<Item> items;
            for (const auto& node : nodes)
            {
                items.emplace_back(ParsePullRequest(node));
            }
            return items;
        }

        bool Identifier(const std::string& text)
        {
            return !text.empty() && text.size() <= 100 && std::all_of(text.begin(), text.end(), [](const char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
            });
        }

        std::vector<Item> Activity(const Request& request, const std::string& login)
        {
            const auto events = request("users/" + login + "/events?per_page=30", {});
            Array(events, 30);
            std::vector<Item> items;
            std::map<std::pair<std::string, uint32_t>, size_t> pulls;
            for (const auto& event : events)
            {
                Item item;
                item.repository = Text(event["repo"], "name");
                const auto slash = item.repository.find('/');
                Require(slash != std::string::npos && Identifier(item.repository.substr(0, slash)) && Identifier(item.repository.substr(slash + 1)));
                item.url = "https://github.com/" + item.repository;
                item.title = item.repository;
                item.updatedAt = Text(event, "created_at");
                const auto type = Text(event, "type");
                const auto& payload = event["payload"];
                item.activityKind = type;
                if (type == "PullRequestEvent" || type == "PullRequestReviewEvent" || type == "PullRequestReviewCommentEvent")
                {
                    item.number = payload["pull_request"]["number"].isUInt() ?
                                      Count(payload["pull_request"], "number") :
                                      Count(payload, "number");
                    Require(item.number > 0);
                    const auto key = std::pair{ item.repository, item.number };
                    if (const auto found = pulls.find(key); found != pulls.end())
                    {
                        ++items[found->second].events;
                        continue;
                    }
                    item.events = 1;
                    item.url += "/pull/" + std::to_string(item.number);
                    item.description = Text(payload, "action", true);
                    pulls.emplace(key, items.size());
                }
                else if (type == "PushEvent" || type == "CreateEvent" || type == "DeleteEvent")
                {
                    item.description = Text(payload, "ref", true);
                }
                else if (type == "IssuesEvent" || type == "IssueCommentEvent")
                {
                    item.title = Text(payload["issue"], "title");
                    item.url = Url(payload["issue"], "html_url");
                    item.description = Text(payload, "action", true);
                }
                items.emplace_back(std::move(item));
            }
            if (!pulls.empty())
            {
                std::string query{ "query { viewer { login } " };
                for (const auto& [key, index] : pulls)
                {
                    const auto& [repository, number] = key;
                    const auto slash = repository.find('/');
                    query += "r" + std::to_string(index) + ":repository(owner:\"" + repository.substr(0, slash) +
                             "\",name:\"" + repository.substr(slash + 1) + "\") { pullRequest(number:" + std::to_string(number) +
                             ") { " + pullFields + " } } ";
                }
                const auto response = request("graphql", query + "}");
                const auto& data = Graph(response, login);
                for (const auto& [key, index] : pulls)
                {
                    const auto& pull = data["r" + std::to_string(index)]["pullRequest"];
                    if (!pull.isNull())
                    {
                        auto hydrated = ParsePullRequest(pull);
                        hydrated.events = items[index].events;
                        hydrated.updatedAt = items[index].updatedAt;
                        hydrated.description = items[index].description;
                        hydrated.activityKind = items[index].activityKind;
                        items[index] = std::move(hydrated);
                    }
                }
            }
            return items;
        }
    }

    bool IsGitHubUrl(const std::string& url) noexcept
    {
        return url.starts_with("https://github.com/") &&
               std::none_of(url.begin(), url.end(), [](const unsigned char c) { return c <= 0x20 || c == '\\' || c == 0x7f; });
    }

    bool IsGitHubAvatarUrl(const std::string& url) noexcept
    {
        return url.size() <= 2048 &&
               (url.starts_with("https://avatars.githubusercontent.com/") || url.starts_with("https://avatars.githubusercontent.com:443/")) &&
               std::none_of(url.begin(), url.end(), [](const unsigned char c) { return c <= 0x20 || c == '\\' || c == 0x7f; });
    }

    Item ParsePullRequest(const Json::Value& value)
    {
        Item item;
        item.title = Text(value, "title");
        item.repository = Text(value["repository"], "nameWithOwner");
        item.url = Url(value, "url");
        item.number = Count(value, "number");
        Require(item.number > 0 && item.url == "https://github.com/" + item.repository + "/pull/" + std::to_string(item.number));
        item.updatedAt = Text(value, "updatedAt");
        item.state = Text(value, "state");
        Require(item.state == "OPEN" || item.state == "CLOSED" || item.state == "MERGED");
        Require(value["isDraft"].isBool());
        if (item.state == "OPEN" && Boolean(value, "isDraft", false))
        {
            item.state = "DRAFT";
        }
        item.author = value["author"].isNull() ? std::string{} : Text(value["author"], "login");
        if (!value["author"].isNull())
        {
            item.avatarUrl = Text(value["author"], "avatarUrl", true);
            Require(item.avatarUrl.empty() || IsGitHubAvatarUrl(item.avatarUrl));
        }
        item.head = Text(value, "headRefName");
        item.base = Text(value, "baseRefName");
        item.review = Text(value, "reviewDecision", true);
        item.comments = Count(value["comments"], "totalCount");
        const auto& labels = value["labels"];
        item.labelCount = Count(labels, "totalCount");
        Array(labels["nodes"], 10);
        Require(labels["nodes"].size() == std::min(item.labelCount, 10u));
        for (const auto& label : labels["nodes"])
        {
            item.labels.emplace_back(Text(label, "name"));
        }
        const auto& commits = value["commits"]["nodes"];
        Array(commits, 1);
        if (!commits.empty() && !commits[0]["commit"]["statusCheckRollup"].isNull())
        {
            const auto& contexts = commits[0]["commit"]["statusCheckRollup"]["contexts"];
            item.checkCount = Count(contexts, "totalCount");
            Array(contexts["nodes"], 100);
            Require(contexts["nodes"].size() == std::min(item.checkCount, 100u));
            for (const auto& check : contexts["nodes"])
            {
                const auto type = Text(check, "__typename");
                if (type == "CheckRun")
                {
                    auto state = Text(check, "status");
                    if (state == "COMPLETED")
                    {
                        state = Text(check, "conclusion", true);
                    }
                    item.checks.push_back({ Text(check, "name"), state.empty() ? "UNKNOWN" : state });
                }
                else
                {
                    Require(type == "StatusContext");
                    item.checks.push_back({ Text(check, "context"), Text(check, "state") });
                }
            }
        }
        return item;
    }

    Calendar ParseCalendar(const Json::Value& value)
    {
        Calendar calendar;
        calendar.total = Count(value, "totalContributions");
        const auto& weeks = value["weeks"];
        Array(weeks, 54);
        Require(!weeks.empty());
        uint64_t sum{};
        uint32_t weekIndex{};
        std::optional<std::chrono::sys_days> previous;
        for (const auto& week : weeks)
        {
            Array(week["contributionDays"], 7);
            Require(!week["contributionDays"].empty());
            Require(weekIndex == 0 || Count(week["contributionDays"][0], "weekday") == 0);
            for (const auto& valueDay : week["contributionDays"])
            {
                Day day;
                day.date = Text(valueDay, "date");
                day.weekday = Count(valueDay, "weekday");
                day.count = Count(valueDay, "contributionCount");
                day.week = weekIndex;
                const auto level = Text(valueDay, "contributionLevel");
                constexpr std::array levels{ "NONE", "FIRST_QUARTILE", "SECOND_QUARTILE", "THIRD_QUARTILE", "FOURTH_QUARTILE" };
                const auto found = std::find(levels.begin(), levels.end(), level);
                Require(found != levels.end());
                day.level = static_cast<uint32_t>(found - levels.begin());
                Require(day.weekday < 7 && day.date.size() == 10 && day.date[4] == '-' && day.date[7] == '-' &&
                        (day.count == 0) == (day.level == 0));
                int year{};
                unsigned month{}, date{};
                const auto parseNumber = [&](const size_t offset, const size_t length, auto& number) {
                    const auto first = day.date.data() + offset;
                    const auto result = std::from_chars(first, first + length, number);
                    Require(result.ec == std::errc{} && result.ptr == first + length);
                };
                parseNumber(0, 4, year);
                parseNumber(5, 2, month);
                parseNumber(8, 2, date);
                const std::chrono::year_month_day ymd{ std::chrono::year{ year }, std::chrono::month{ month }, std::chrono::day{ date } };
                Require(ymd.ok());
                const std::chrono::sys_days current{ ymd };
                Require(std::chrono::weekday{ current }.c_encoding() == day.weekday &&
                        (!previous || current == *previous + std::chrono::days{ 1 }) &&
                        current <= std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now()));
                previous = current;
                sum += day.count;
                calendar.days.emplace_back(std::move(day));
            }
            Require(weekIndex + 1 == weeks.size() || calendar.days.back().weekday == 6);
            ++weekIndex;
        }
        Require(sum == calendar.total);
        return calendar;
    }

    Usage ParseUsage(const Json::Value& value, const std::string& login)
    {
        Account(Text(value, "login"), login);
        Usage usage;
        usage.plan = Text(value, "copilot_plan");
        for (const auto kind : { "premium_interactions", "chat", "completions" })
        {
            const auto& entry = value["quota_snapshots"][kind];
            if (entry.isNull())
            {
                continue;
            }
            Quota quota;
            quota.kind = kind;
            Require(entry["unlimited"].isBool());
            quota.unlimited = Boolean(entry, "unlimited", false);
            quota.included = Boolean(entry, "has_quota", true);
            quota.credits = Boolean(entry, "token_based_billing", Boolean(value, "token_based_billing", false));
            if (quota.included && !quota.unlimited)
            {
                Require(entry["percent_remaining"].isNumeric());
                const auto remaining = entry["percent_remaining"].asDouble();
                Require(std::isfinite(remaining) && remaining >= 0 && remaining <= 100);
                quota.used = 100.0 - remaining;
            }
            quota.reset = Text(value, "quota_reset_date_utc", true);
            if (quota.reset.empty())
            {
                quota.reset = Text(value, "quota_reset_date", true);
            }
            if (!entry["quota_reset_at"].isNull())
            {
                Require(entry["quota_reset_at"].isInt64() && entry["quota_reset_at"].asInt64() >= 0 &&
                        entry["quota_reset_at"].asInt64() <= 253402300799LL);
                const auto seconds = entry["quota_reset_at"].asInt64();
                if (seconds > 0)
                {
                    const auto date = std::chrono::sys_seconds{ std::chrono::seconds{ seconds } };
                    quota.reset = std::format("{:%FT%TZ}", date);
                }
            }
            usage.quotas.emplace_back(std::move(quota));
        }
        Require(!usage.quotas.empty());
        return usage;
    }

    Snapshot ParseIdentity(const Json::Value& user)
    {
        Require(user.isObject());
        Require(user["id"].isUInt64());
        Snapshot result;
        result.id = std::to_string(user["id"].asUInt64());
        result.login = Text(user, "login");
        Require(Identifier(result.login));
        result.name = Text(user, "name", true);
        result.avatarUrl = Text(user, "avatar_url", true);
        Require(result.avatarUrl.empty() || IsGitHubAvatarUrl(result.avatarUrl));
        return result;
    }

    bool IsSectionExpired(const std::chrono::system_clock::time_point updated,
                          const std::chrono::system_clock::time_point now) noexcept
    {
        return updated > now || now - updated >= sectionLifetime;
    }

    namespace
    {
        template<typename T>
        void ExpireSection(Section<T>& section, const std::chrono::system_clock::time_point now)
        {
            if (section.value && IsSectionExpired(section.updated, now))
            {
                section.value.reset();
            }
        }
    }

    void ExpireSections(Snapshot& snapshot, const std::chrono::system_clock::time_point now)
    {
        ExpireSection(snapshot.activity, now);
        ExpireSection(snapshot.pulls, now);
        ExpireSection(snapshot.reviews, now);
        ExpireSection(snapshot.repos, now);
        ExpireSection(snapshot.calendar, now);
        ExpireSection(snapshot.usage, now);
    }

    Snapshot Refresh(const Request& request, const Snapshot& previous, const RefreshReason reason, const Update& update)
    {
        auto result = ParseIdentity(request("user", {}));
        auto old = previous.id == result.id && previous.login == result.login ? previous : Snapshot{};
        ExpireSections(old);
        result.activity = old.activity;
        result.pulls = old.pulls;
        result.reviews = old.reviews;
        result.repos = old.repos;
        result.calendar = old.calendar;
        result.usage = old.usage;
        if (update)
            update(result);
        std::mutex mutex;
        const auto fetch = [&](auto member, const std::chrono::minutes freshness, auto load) {
            return std::async(std::launch::async, [&, section = old.*member, member, freshness, load] {
                const auto age = std::chrono::system_clock::now() - section.updated;
                const auto fresh = reason == RefreshReason::Automatic && section.value && section.failure == Failure::None &&
                                   age >= std::chrono::seconds::zero() && age < freshness;
                auto loaded = fresh ? section : Load(section, load);
                {
                    std::scoped_lock lock{ mutex };
                    result.*member = std::move(loaded);
                    if (update)
                        update(result);
                }
            });
        };
        // All workers are joined before returning, including on cancellation or account mismatch.
        auto activity = fetch(&Snapshot::activity, std::chrono::minutes{ 5 }, [&] { return Activity(request, result.login); });
        auto pulls = fetch(&Snapshot::pulls, std::chrono::minutes{ 5 }, [&] { return Pulls(request, result.login, false); });
        auto reviews = fetch(&Snapshot::reviews, std::chrono::minutes{ 5 }, [&] { return Pulls(request, result.login, true); });
        auto repos = fetch(&Snapshot::repos, std::chrono::minutes{ 15 }, [&] {
            const auto repos = request("user/repos?sort=pushed&direction=desc&per_page=30&affiliation=owner,collaborator,organization_member", {});
            Array(repos, 30);
            std::vector<Item> items;
            for (const auto& repo : repos)
            {
                Item item;
                item.title = Text(repo, "full_name");
                item.url = Url(repo, "html_url");
                item.description = Text(repo, "description", true);
                item.updatedAt = Text(repo, "pushed_at", true);
                items.emplace_back(std::move(item));
            }
            return items;
        });
        auto calendar = fetch(&Snapshot::calendar, std::chrono::minutes{ 15 }, [&] {
            const auto response = request("graphql", "query { viewer { login contributionsCollection { contributionCalendar { totalContributions weeks { contributionDays { date weekday contributionCount contributionLevel } } } } } }");
            return ParseCalendar(Graph(response, result.login)["viewer"]["contributionsCollection"]["contributionCalendar"]);
        });
        auto usage = fetch(&Snapshot::usage, std::chrono::minutes{ 5 }, [&] { return ParseUsage(request("copilot_internal/user", {}), result.login); });
        activity.get();
        pulls.get();
        reviews.get();
        repos.get();
        calendar.get();
        usage.get();
        const auto finalUser = request("user", {});
        Require(finalUser.isObject());
        Require(finalUser["id"].isUInt64());
        Account(std::to_string(finalUser["id"].asUInt64()), result.id);
        Account(Text(finalUser, "login"), result.login);
        return result;
    }
}
