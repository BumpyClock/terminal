// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "GitHubDashboardData.h"
#include <WtExeUtils.h>
#include <filesystem>

namespace TerminalApp::GitHub
{
    Json::Value ReadApi(const std::string& endpoint, const std::string& query, const std::atomic<bool>& cancelled)
    {
        if (cancelled.load())
        {
            throw Error{ Failure::Cancelled };
        }
        // An explicit PATH search avoids executing gh.exe from the terminal's working directory.
        const auto environmentPath = wil::GetEnvironmentVariableW<std::wstring>(L"PATH");
        std::wstring path;
        for (size_t begin = 0; begin < environmentPath.size();)
        {
            const auto end = environmentPath.find(L';', begin);
            auto entry = environmentPath.substr(begin, end == std::wstring::npos ? end : end - begin);
            if (entry.size() >= 2 && entry.front() == L'"' && entry.back() == L'"')
            {
                entry = entry.substr(1, entry.size() - 2);
            }
            if (std::filesystem::path{ entry }.is_absolute())
            {
                if (!path.empty())
                {
                    path += L';';
                }
                path += entry;
            }
            if (end == std::wstring::npos)
            {
                break;
            }
            begin = end + 1;
        }
        if (path.empty())
        {
            throw Error{ Failure::MissingCli };
        }
        std::wstring executable(32768, L'\0');
        const auto length = SearchPathW(path.c_str(), L"gh.exe", nullptr, static_cast<DWORD>(executable.size()), executable.data(), nullptr);
        if (length == 0 || length >= executable.size())
        {
            throw Error{ Failure::MissingCli };
        }
        executable.resize(length);
        std::wstring command = QuoteAndEscapeCommandlineArg(executable) + L" api --hostname github.com ";
        command += QuoteAndEscapeCommandlineArg(winrt::to_hstring(endpoint));
        if (!query.empty())
        {
            command += L" -f ";
            command += QuoteAndEscapeCommandlineArg(winrt::to_hstring("query=" + query));
        }

        SECURITY_ATTRIBUTES security{ sizeof(security), nullptr, TRUE };
        wil::unique_handle read;
        wil::unique_handle write;
        THROW_IF_WIN32_BOOL_FALSE(CreatePipe(read.put(), write.put(), &security, 0));
        THROW_IF_WIN32_BOOL_FALSE(SetHandleInformation(read.get(), HANDLE_FLAG_INHERIT, 0));
        wil::unique_handle null{ CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr) };
        THROW_LAST_ERROR_IF(!null);

        SIZE_T attributeBytes{};
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
        std::vector<std::byte> storage(attributeBytes);
        auto attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        THROW_IF_WIN32_BOOL_FALSE(InitializeProcThreadAttributeList(attributes, 1, 0, &attributeBytes));
        const auto deleteAttributes = wil::scope_exit([&] { DeleteProcThreadAttributeList(attributes); });
        HANDLE handles[]{ write.get(), null.get() };
        THROW_IF_WIN32_BOOL_FALSE(UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, sizeof(handles), nullptr, nullptr));

        wil::unique_handle job{ CreateJobObjectW(nullptr, nullptr) };
        THROW_LAST_ERROR_IF(!job);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        THROW_IF_WIN32_BOOL_FALSE(SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)));
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = null.get();
        startup.StartupInfo.hStdOutput = write.get();
        startup.StartupInfo.hStdError = null.get();
        startup.lpAttributeList = attributes;
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &startup.StartupInfo, &process))
        {
            LOG_LAST_ERROR();
            throw Error{ Failure::Request };
        }
        wil::unique_handle child{ process.hProcess };
        wil::unique_handle thread{ process.hThread };
        // Also cover assignment failures: a suspended process must never be orphaned.
        const auto terminate = wil::scope_exit([&] { TerminateProcess(child.get(), ERROR_CANCELLED); });
        THROW_IF_WIN32_BOOL_FALSE(AssignProcessToJobObject(job.get(), child.get()));
        THROW_LAST_ERROR_IF(ResumeThread(thread.get()) == static_cast<DWORD>(-1));
        write.reset();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{ 45 };
        std::string output;
        std::array<char, 8192> buffer;
        for (;;)
        {
            if (cancelled.load())
            {
                throw Error{ Failure::Cancelled };
            }
            if (std::chrono::steady_clock::now() >= deadline)
            {
                throw Error{ Failure::Timeout };
            }
            DWORD available{};
            if (!PeekNamedPipe(read.get(), nullptr, 0, nullptr, &available, nullptr))
            {
                if (GetLastError() != ERROR_BROKEN_PIPE)
                {
                    THROW_LAST_ERROR();
                }
            }
            if (available)
            {
                DWORD received{};
                THROW_IF_WIN32_BOOL_FALSE(ReadFile(read.get(), buffer.data(), std::min<DWORD>(available, static_cast<DWORD>(buffer.size())), &received, nullptr));
                output.append(buffer.data(), received);
                if (output.size() > 4 * 1024 * 1024)
                {
                    throw Error{ Failure::InvalidResponse };
                }
            }
            else if (WaitForSingleObject(child.get(), 25) == WAIT_OBJECT_0)
            {
                // The process may have written between the previous peek and the wait.
                if (!PeekNamedPipe(read.get(), nullptr, 0, nullptr, &available, nullptr) || available == 0)
                {
                    break;
                }
            }
        }
        DWORD exitCode{};
        THROW_IF_WIN32_BOOL_FALSE(GetExitCodeProcess(child.get(), &exitCode));
        if (exitCode != 0)
        {
            // Do not display stderr: credential helpers and gh errors can contain secrets.
            throw Error{ Failure::Request };
        }
        Json::CharReaderBuilder builder;
        builder["rejectDupKeys"] = true;
        const std::unique_ptr<Json::CharReader> reader{ builder.newCharReader() };
        Json::Value value;
        std::string errors;
        if (!reader->parse(output.data(), output.data() + output.size(), &value, &errors))
        {
            throw Error{ Failure::InvalidResponse };
        }
        return value;
    }
}
