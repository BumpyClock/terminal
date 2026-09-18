// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "precomp.h"
#include "../TerminalApp/GitHubDashboardData.h"
#include "../TerminalApp/GitHubDashboardLayout.h"
#include <condition_variable>
#include <mutex>

using namespace WEX::TestExecution;
using namespace TerminalApp::GitHub;

namespace
{
    Json::Value Parse(const char* text)
    {
        Json::CharReaderBuilder builder;
        const std::unique_ptr<Json::CharReader> reader{ builder.newCharReader() };
        Json::Value value;
        std::string errors;
        VERIFY_IS_TRUE(reader->parse(text, text + strlen(text), &value, &errors));
        return value;
    }

    Json::Value Pull()
    {
        return Parse(R"({
            "number":42,"title":"Test pull","url":"https://github.com/example/repo/pull/42",
            "repository":{"nameWithOwner":"example/repo"},"updatedAt":"2026-01-01T00:00:00Z",
            "state":"MERGED","isDraft":false,"headRefName":"feature","baseRefName":"main",
            "author":{"login":"example","avatarUrl":"https://avatars.githubusercontent.com/u/1?s=96&v=4"},"reviewDecision":"APPROVED","comments":{"totalCount":2},
            "labels":{"totalCount":0,"nodes":[]},"commits":{"nodes":[{"commit":{"statusCheckRollup":{
                "contexts":{"totalCount":3,"nodes":[
                    {"__typename":"CheckRun","name":"build","status":"COMPLETED","conclusion":"SUCCESS"},
                    {"__typename":"CheckRun","name":"lint","status":"COMPLETED","conclusion":"SKIPPED"},
                    {"__typename":"StatusContext","context":"external","state":"PENDING"}
                ]}
            }}}]}
        })");
    }

    Json::Value User()
    {
        return Parse(R"({"id":1,"login":"example","name":"Example","avatar_url":"https://avatars.githubusercontent.com/u/1?v=4"})");
    }

    Json::Value CalendarValue()
    {
        return Parse(R"({"totalContributions":2,"weeks":[{"contributionDays":[
            {"date":"2026-01-01","weekday":4,"contributionCount":2,"contributionLevel":"FIRST_QUARTILE"},
            {"date":"2026-01-02","weekday":5,"contributionCount":0,"contributionLevel":"NONE"}
        ]}]})");
    }

    Json::Value Api(const std::string& endpoint, const std::string& query)
    {
        if (endpoint == "user")
        {
            return User();
        }
        if (endpoint == "copilot_internal/user")
        {
            return Parse(R"({"login":"example","copilot_plan":"individual","token_based_billing":true,
                "quota_snapshots":{"premium_interactions":{"unlimited":false,"percent_remaining":50.4}}})");
        }
        if (endpoint == "graphql")
        {
            Json::Value result;
            result["data"]["viewer"]["login"] = "example";
            if (query.find("contributionsCollection") != std::string::npos)
            {
                result["data"]["viewer"]["contributionsCollection"]["contributionCalendar"] = CalendarValue();
            }
            else
            {
                result["data"]["search"]["nodes"] = Json::Value{ Json::arrayValue };
            }
            return result;
        }
        return Json::Value{ Json::arrayValue };
    }
}

class GitHubDashboardTests
{
    TEST_CLASS(GitHubDashboardTests);

    TEST_METHOD(DashboardPlacementTracksAnchorAndClampsToHost)
    {
        const auto placement = TerminalApp::CalculateGitHubDashboardPlacement(
            { 1000, 800 },
            { 900, 0, 40, 32 });
        VERIFY_ARE_EQUAL(520.0, placement.x);
        VERIFY_ARE_EQUAL(36.0, placement.y);
        VERIFY_ARE_EQUAL(420.0, placement.width);
        VERIFY_ARE_EQUAL(720.0, placement.height);

        const auto narrow = TerminalApp::CalculateGitHubDashboardPlacement(
            { 300, 250 },
            { 240, 0, 40, 32 });
        VERIFY_ARE_EQUAL(8.0, narrow.x);
        VERIFY_ARE_EQUAL(36.0, narrow.y);
        VERIFY_ARE_EQUAL(284.0, narrow.width);
        VERIFY_ARE_EQUAL(206.0, narrow.height);
    }

    TEST_METHOD(DashboardPlacementStaysInVerySmallHost)
    {
        const auto shortHost = TerminalApp::CalculateGitHubDashboardPlacement(
            { 300, 150 },
            { 240, 60, 40, 32 });
        VERIFY_ARE_EQUAL(96.0, shortHost.y);
        VERIFY_ARE_EQUAL(46.0, shortHost.height);
        VERIFY_IS_TRUE(shortHost.y + shortHost.height <= 150);

        const auto anchorBelowHost = TerminalApp::CalculateGitHubDashboardPlacement(
            { 300, 80 },
            { 240, 60, 40, 32 });
        VERIFY_ARE_EQUAL(80.0, anchorBelowHost.y);
        VERIFY_ARE_EQUAL(0.0, anchorBelowHost.height);

        const auto tinyHost = TerminalApp::CalculateGitHubDashboardPlacement(
            { 10, 10 },
            { 240, 60, 40, 32 });
        VERIFY_IS_TRUE(tinyHost.x >= 0);
        VERIFY_IS_TRUE(tinyHost.y >= 0);
        VERIFY_IS_TRUE(tinyHost.width >= 0);
        VERIFY_IS_TRUE(tinyHost.height >= 0);
        VERIFY_IS_TRUE(tinyHost.x + tinyHost.width <= 10);
        VERIFY_IS_TRUE(tinyHost.y + tinyHost.height <= 10);
    }

    TEST_METHOD(DashboardCompactLayoutKeepsAListViewport)
    {
        VERIFY_IS_FALSE(TerminalApp::ShouldUseCompactGitHubDashboardLayout(300, 200));
        VERIFY_IS_TRUE(TerminalApp::ShouldUseCompactGitHubDashboardLayout(220, 200));
        VERIFY_IS_TRUE(TerminalApp::ShouldUseCompactGitHubDashboardLayout(0, 0));
    }

    TEST_METHOD(PullRequestStatesRemainIndependent)
    {
        const auto pull = ParsePullRequest(Pull());
        VERIFY_ARE_EQUAL(std::string{ "MERGED" }, pull.state);
        VERIFY_ARE_EQUAL(std::string{ "APPROVED" }, pull.review);
        VERIFY_ARE_EQUAL(std::string{ "SUCCESS" }, pull.checks[0].state);
        VERIFY_ARE_EQUAL(std::string{ "SKIPPED" }, pull.checks[1].state);
        VERIFY_ARE_EQUAL(std::string{ "PENDING" }, pull.checks[2].state);
    }

    TEST_METHOD(RejectMismatchedPullUrl)
    {
        auto pull = Pull();
        pull["url"] = "https://github.com/example/other/pull/42";
        VERIFY_THROWS(ParsePullRequest(pull), Error);
        VERIFY_IS_FALSE(IsGitHubUrl("https://github.com.evil.example/repo"));
        VERIFY_IS_FALSE(IsGitHubUrl("https://github.com\\@evil.example/"));
        VERIFY_IS_FALSE(IsGitHubUrl("file:///C:/Windows"));
    }

    TEST_METHOD(PullRequestAvatarsAreOptional)
    {
        auto value = Pull();
        VERIFY_ARE_EQUAL(std::string{ "https://avatars.githubusercontent.com/u/1?s=96&v=4" }, ParsePullRequest(value).avatarUrl);
        value["author"].removeMember("avatarUrl");
        VERIFY_IS_TRUE(ParsePullRequest(value).avatarUrl.empty());
        value["author"] = Json::Value{};
        VERIFY_IS_TRUE(ParsePullRequest(value).avatarUrl.empty());
        VERIFY_IS_TRUE(ParsePullRequest(value).author.empty());
    }

    TEST_METHOD(AccountAvatarIsValidatedAndCached)
    {
        const auto account = ParseIdentity(User());
        VERIFY_ARE_EQUAL(std::string{ "https://avatars.githubusercontent.com/u/1?v=4" }, account.avatarUrl);
        auto encoded = SerializeCache(account);
        VERIFY_ARE_EQUAL(account.avatarUrl, DeserializeCache(encoded).avatarUrl);
        encoded.removeMember("avatarUrl");
        VERIFY_IS_TRUE(DeserializeCache(encoded).avatarUrl.empty());
        encoded["avatarUrl"] = "file:///C:/avatar.png";
        VERIFY_THROWS(DeserializeCache(encoded), Error);
        auto user = User();
        user["avatar_url"] = "https://example.com/avatar.png";
        VERIFY_THROWS(ParseIdentity(user), Error);
        user.removeMember("avatar_url");
        VERIFY_IS_TRUE(ParseIdentity(user).avatarUrl.empty());
    }

    TEST_METHOD(AvatarUrlsRequireGitHubHttpsHost)
    {
        VERIFY_IS_TRUE(IsGitHubAvatarUrl("https://avatars.githubusercontent.com:443/u/1?v=4"));
        for (const auto url : {
                 "http://avatars.githubusercontent.com/u/1",
                 "https://avatars.githubusercontent.com.evil.example/u/1",
                 "https://avatars.githubusercontent.com@evil.example/u/1",
                 "https://avatars.githubusercontent.com:8443/u/1",
                 "https://avatars.githubusercontent.com\\@evil.example/u/1",
                 "https://avatars.githubusercontent.com/u/\n1",
                 "file:///C:/avatar.png" })
        {
            VERIFY_IS_FALSE(IsGitHubAvatarUrl(url));
            auto value = Pull();
            value["author"]["avatarUrl"] = url;
            VERIFY_THROWS(ParsePullRequest(value), Error);
        }
    }

    TEST_METHOD(CachePreservesAvatarsAndReadsOlderSnapshots)
    {
        auto snapshot = Refresh(Api, {});
        snapshot.pulls.value->push_back(ParsePullRequest(Pull()));
        auto encoded = SerializeCache(snapshot);
        VERIFY_ARE_EQUAL(snapshot.pulls.value->front().avatarUrl, DeserializeCache(encoded).pulls.value->front().avatarUrl);
        encoded["pulls"]["value"][0].removeMember("avatarUrl");
        VERIFY_IS_TRUE(DeserializeCache(encoded).pulls.value->front().avatarUrl.empty());
        encoded["pulls"]["value"][0]["avatarUrl"] = "https://example.com/avatar";
        VERIFY_THROWS(DeserializeCache(encoded), Error);
    }

    TEST_METHOD(PullQueriesRequestAvatars)
    {
        std::atomic<uint32_t> pullQueries{};
        const auto result = Refresh([&](const auto& endpoint, const auto& query) {
            if (query.find("... on PullRequest") != std::string::npos)
            {
                if (query.find("avatarUrl(size:96)") != std::string::npos)
                    ++pullQueries;
                auto response = Api(endpoint, query);
                response["data"]["search"]["nodes"].append(Pull());
                return response;
            }
            return Api(endpoint, query);
        },
                                    {});
        VERIFY_ARE_EQUAL(2u, pullQueries.load());
        VERIFY_IS_FALSE(result.pulls.value->front().avatarUrl.empty());
        VERIFY_IS_FALSE(result.reviews.value->front().avatarUrl.empty());
    }

    TEST_METHOD(MissingChecksAreNotPassing)
    {
        auto pull = Pull();
        pull["commits"]["nodes"][0]["commit"]["statusCheckRollup"] = Json::Value{};
        const auto parsed = ParsePullRequest(pull);
        VERIFY_ARE_EQUAL(0u, parsed.checkCount);
        VERIFY_IS_TRUE(parsed.checks.empty());
    }

    TEST_METHOD(CalendarValidatesTotalsAndDates)
    {
        auto calendar = CalendarValue();
        VERIFY_ARE_EQUAL(2u, ParseCalendar(calendar).total);
        calendar["totalContributions"] = 3;
        VERIFY_THROWS(ParseCalendar(calendar), Error);
        calendar = CalendarValue();
        calendar["weeks"][0]["contributionDays"][1]["date"] = "2026-01-04";
        VERIFY_THROWS(ParseCalendar(calendar), Error);
    }

    TEST_METHOD(QuotaUsesRemainingPercentage)
    {
        const auto value = Api("copilot_internal/user", {});
        const auto usage = ParseUsage(value, "example");
        VERIFY_IS_TRUE(usage.quotas[0].credits);
        VERIFY_ARE_EQUAL(49.6, *usage.quotas[0].used);
        VERIFY_THROWS(ParseUsage(value, "different"), Error);
        auto invalid = value;
        invalid["quota_snapshots"]["premium_interactions"]["percent_remaining"] = 101;
        VERIFY_THROWS(ParseUsage(invalid, "example"), Error);
    }

    TEST_METHOD(SectionFailureRetainsOriginalSuccessTime)
    {
        const auto previous = Refresh(Api, {});
        const auto result = Refresh([](const auto& endpoint, const auto& query) {
            if (endpoint == "copilot_internal/user")
            {
                throw Error{ Failure::Request };
            }
            return Api(endpoint, query);
        },
                                    previous);
        VERIFY_IS_TRUE(result.usage.value.has_value());
        VERIFY_IS_TRUE(result.usage.updated == previous.usage.updated);
        VERIFY_IS_TRUE(result.usage.failure == Failure::Request);
        VERIFY_IS_TRUE(result.pulls.failure == Failure::None);
    }

    TEST_METHOD(ChangedAccountNeverRetainsPrivateData)
    {
        auto previous = Refresh(Api, {});
        previous.id = "2";
        const auto result = Refresh([](const auto& endpoint, const auto& query) {
            if (endpoint == "copilot_internal/user")
            {
                throw Error{ Failure::Request };
            }
            return Api(endpoint, query);
        },
                                    previous);
        VERIFY_IS_FALSE(result.usage.value.has_value());
    }

    TEST_METHOD(FinalIdentityChangeRejectsSnapshot)
    {
        uint32_t userCalls{};
        VERIFY_THROWS(Refresh([&](const auto& endpoint, const auto& query) {
                          auto value = Api(endpoint, query);
                          if (endpoint == "user" && ++userCalls == 2)
                          {
                              value["id"] = 2;
                          }
                          return value;
                      },
                              {}),
                      Error);
    }

    TEST_METHOD(CancellationDoesNotPublishPartialSuccess)
    {
        VERIFY_THROWS(Refresh([](const auto&, const auto&) -> Json::Value {
                          throw Error{ Failure::Cancelled };
                      },
                              {}),
                      Error);
    }

    TEST_METHOD(SuccessfulEmptyResponseClearsOldItems)
    {
        auto previous = Refresh(Api, {});
        previous.pulls.value->push_back(ParsePullRequest(Pull()));
        const auto result = Refresh(Api, previous);
        VERIFY_IS_TRUE(result.pulls.value->empty());
    }

    TEST_METHOD(ExpiredDataIsNotRetained)
    {
        auto previous = Refresh(Api, {});
        previous.usage.updated -= std::chrono::hours{ 24 * 7 };
        const auto result = Refresh([](const auto& endpoint, const auto& query) {
            if (endpoint == "copilot_internal/user")
            {
                throw Error{ Failure::Timeout };
            }
            return Api(endpoint, query);
        },
                                    previous);
        VERIFY_IS_FALSE(result.usage.value.has_value());
        VERIFY_IS_TRUE(result.usage.failure == Failure::Timeout);
    }

    TEST_METHOD(ExpireSectionsRemovesOldBoundaryAndFutureData)
    {
        auto snapshot = Refresh(Api, {});
        const auto now = std::chrono::system_clock::now();
        snapshot.activity.updated = now - std::chrono::hours{ 24 * 7 } - std::chrono::seconds{ 1 };
        snapshot.activity.failure = Failure::Request;
        snapshot.pulls.updated = now - std::chrono::hours{ 24 * 7 };
        snapshot.reviews.updated = now - std::chrono::hours{ 24 * 7 } + std::chrono::seconds{ 1 };
        snapshot.repos.updated = now + std::chrono::seconds{ 1 };

        ExpireSections(snapshot, now);

        VERIFY_IS_FALSE(snapshot.activity.value.has_value());
        VERIFY_IS_TRUE(snapshot.activity.failure == Failure::Request);
        VERIFY_IS_FALSE(snapshot.pulls.value.has_value());
        VERIFY_IS_TRUE(snapshot.reviews.value.has_value());
        VERIFY_IS_FALSE(snapshot.repos.value.has_value());
        VERIFY_IS_TRUE(snapshot.calendar.value.has_value());
        VERIFY_IS_TRUE(snapshot.usage.value.has_value());
    }

    TEST_METHOD(RefreshDoesNotPublishExpiredSections)
    {
        auto previous = Refresh(Api, {});
        previous.pulls.value->push_back(ParsePullRequest(Pull()));
        previous.pulls.updated -= std::chrono::hours{ 24 * 7 };
        bool publishedExpiredPulls = false;
        Refresh(Api, previous, RefreshReason::Automatic, [&](const Snapshot& partial) {
            if (partial.pulls.value && !partial.pulls.value->empty())
            {
                publishedExpiredPulls = true;
            }
        });
        VERIFY_IS_FALSE(publishedExpiredPulls);
    }

    TEST_METHOD(MalformedSectionDoesNotHideOtherSections)
    {
        const auto result = Refresh([](const auto& endpoint, const auto& query) {
            if (endpoint == "copilot_internal/user")
            {
                return Json::Value{ Json::arrayValue };
            }
            return Api(endpoint, query);
        },
                                    {});
        VERIFY_IS_FALSE(result.usage.value.has_value());
        VERIFY_IS_TRUE(result.usage.failure == Failure::InvalidResponse);
        VERIFY_IS_TRUE(result.calendar.value.has_value());
        VERIFY_IS_TRUE(result.pulls.value.has_value());
    }

    TEST_METHOD(UnlimitedQuotaHasNoPercentage)
    {
        auto value = Api("copilot_internal/user", {});
        value["quota_snapshots"]["premium_interactions"]["unlimited"] = true;
        value["quota_snapshots"]["premium_interactions"]["percent_remaining"] = Json::Value{};
        const auto usage = ParseUsage(value, "example");
        VERIFY_IS_TRUE(usage.quotas[0].unlimited);
        VERIFY_IS_FALSE(usage.quotas[0].used.has_value());
    }

    TEST_METHOD(CacheRoundTripPreservesAllSections)
    {
        auto snapshot = Refresh(Api, {});
        auto pull = ParsePullRequest(Pull());
        pull.labels = { "bug", "windows" };
        pull.labelCount = 2;
        pull.description = "Description";
        pull.activityKind = "PullRequestEvent";
        pull.events = 3;
        snapshot.pulls.value->push_back(pull);
        snapshot.activity = snapshot.pulls;
        snapshot.reviews = snapshot.pulls;
        snapshot.repos = snapshot.pulls;
        snapshot.usage.failure = Failure::Timeout;
        const auto encoded = SerializeCache(snapshot);
        const auto cached = DeserializeCache(encoded);
        VERIFY_IS_TRUE(encoded == SerializeCache(cached));
        VERIFY_IS_TRUE(cached.usage.failure == Failure::Request);
        VERIFY_ARE_EQUAL(std::string{ "bug" }, cached.pulls.value->front().labels.front());
    }

    TEST_METHOD(CacheRejectsCorruptionAndUnsafeUrls)
    {
        auto snapshot = Refresh(Api, {});
        snapshot.pulls.value->push_back(ParsePullRequest(Pull()));
        auto encoded = SerializeCache(snapshot);
        encoded["version"] = 2;
        VERIFY_THROWS(DeserializeCache(encoded), Error);
        encoded = SerializeCache(snapshot);
        encoded["pulls"]["value"][0]["url"] = "https://github.com.evil.example/private";
        VERIFY_THROWS(DeserializeCache(encoded), Error);
        encoded = SerializeCache(snapshot);
        encoded["id"] = "../other";
        VERIFY_THROWS(DeserializeCache(encoded), Error);
        encoded = SerializeCache(snapshot);
        encoded["usage"]["value"]["quotas"][0]["used"] = 101;
        VERIFY_THROWS(DeserializeCache(encoded), Error);
    }

    TEST_METHOD(CacheExpiresSectionsAndRejectsFutureTimestamps)
    {
        auto snapshot = Refresh(Api, {});
        snapshot.pulls.updated -= std::chrono::hours{ 24 * 8 };
        auto cached = DeserializeCache(SerializeCache(snapshot));
        VERIFY_IS_FALSE(cached.pulls.value.has_value());
        VERIFY_IS_TRUE(cached.calendar.value.has_value());
        snapshot.pulls.updated = std::chrono::system_clock::now() + std::chrono::hours{ 1 };
        VERIFY_THROWS(DeserializeCache(SerializeCache(snapshot)), Error);
    }

    TEST_METHOD(CachePersistsAndReplacesOnDisk)
    {
        GUID id{};
        VERIFY_SUCCEEDED(CoCreateGuid(&id));
        const auto path = std::filesystem::temp_directory_path() / (std::wstring{ winrt::to_hstring(id) } + L".json");
        const auto cleanup = wil::scope_exit([&] { DeleteFileW(path.c_str()); });
        VERIFY_IS_TRUE(ReadCache(path).login.empty());
        auto snapshot = Refresh(Api, {});
        WriteCache(path, snapshot);
        VERIFY_IS_TRUE(SerializeCache(snapshot) == SerializeCache(ReadCache(path)));
        snapshot.pulls.value->push_back(ParsePullRequest(Pull()));
        WriteCache(path, snapshot);
        VERIFY_ARE_EQUAL(size_t{ 1 }, ReadCache(path).pulls.value->size());
    }

    TEST_METHOD(AutomaticRefreshReusesFreshDataAndManualBypassesCache)
    {
        const auto previous = DeserializeCache(SerializeCache(Refresh(Api, {})));
        std::atomic<uint32_t> sectionCalls{};
        const auto request = [&](const auto& endpoint, const auto& query) {
            if (endpoint != "user")
                ++sectionCalls;
            return Api(endpoint, query);
        };
        const auto cached = Refresh(request, previous, RefreshReason::Automatic);
        VERIFY_ARE_EQUAL(0u, sectionCalls.load());
        VERIFY_IS_TRUE(cached.pulls.updated == previous.pulls.updated);
        Refresh(request, previous, RefreshReason::Manual);
        VERIFY_ARE_EQUAL(6u, sectionCalls.load());
    }

    TEST_METHOD(AutomaticRefreshRetriesFailedSections)
    {
        auto previous = Refresh(Api, {});
        previous.usage.failure = Failure::Request;
        std::atomic<uint32_t> sectionCalls{};
        const auto result = Refresh([&](const auto& endpoint, const auto& query) {
            if (endpoint != "user")
                ++sectionCalls;
            return Api(endpoint, query);
        },
                                    previous,
                                    RefreshReason::Automatic);
        VERIFY_ARE_EQUAL(1u, sectionCalls.load());
        VERIFY_IS_TRUE(result.usage.failure == Failure::None);
    }

    TEST_METHOD(CachedAccountChangeForcesFreshSections)
    {
        auto previous = DeserializeCache(SerializeCache(Refresh(Api, {})));
        previous.id = "2";
        std::atomic<uint32_t> sectionCalls{};
        Refresh([&](const auto& endpoint, const auto& query) {
            if (endpoint != "user")
                ++sectionCalls;
            return Api(endpoint, query);
        },
                previous,
                RefreshReason::Automatic);
        VERIFY_ARE_EQUAL(6u, sectionCalls.load());
    }

    TEST_METHOD(IndependentSectionsRunConcurrently)
    {
        std::mutex mutex;
        std::condition_variable condition;
        uint32_t entered{};
        bool timedOut{};
        Refresh([&](const auto& endpoint, const auto& query) {
            if (endpoint != "user")
            {
                std::unique_lock lock{ mutex };
                ++entered;
                condition.notify_all();
                if (!condition.wait_for(lock, std::chrono::seconds{ 5 }, [&] { return entered == 6; }))
                {
                    timedOut = true;
                }
            }
            return Api(endpoint, query);
        },
                {});
        VERIFY_ARE_EQUAL(6u, entered);
        VERIFY_IS_FALSE(timedOut);
    }

    TEST_METHOD(PartialUpdatesArriveBeforeSlowSectionCompletes)
    {
        std::mutex mutex;
        std::condition_variable condition;
        bool listPublished{};
        bool timedOut{};
        Refresh([&](const auto& endpoint, const auto& query) {
            if (endpoint == "copilot_internal/user")
            {
                std::unique_lock lock{ mutex };
                timedOut = !condition.wait_for(lock, std::chrono::seconds{ 5 }, [&] { return listPublished; });
            }
            return Api(endpoint, query); }, {}, RefreshReason::Manual, [&](const Snapshot& partial) {
            std::scoped_lock lock{ mutex };
            if (partial.pulls.value && !partial.usage.value)
            {
                listPublished = true;
                condition.notify_all();
            } });
        VERIFY_IS_TRUE(listPublished);
        VERIFY_IS_FALSE(timedOut);
    }
};
