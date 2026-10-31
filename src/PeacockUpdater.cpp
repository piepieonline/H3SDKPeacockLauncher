#include "PeacockUpdater.h"

#define NOMINMAX
#include <Windows.h>
#include <winhttp.h>

#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <vector>

#include <miniz.h>
#include <nlohmann/json.hpp>

#include <Logging.h>

namespace fs = std::filesystem;

namespace {
    constexpr const wchar_t* c_LatestReleaseUrl = L"https://api.github.com/repos/thepeacockproject/Peacock/releases/latest";
    constexpr const wchar_t* c_StagingDirName = L".peacock-update";

    const std::set<std::wstring> c_UserDataDirs = {
        L"userdata", L"contractSessions", L"contracts", L"plugins", L"images", L"logs"
    };
    const std::set<std::wstring> c_ReplacedDirs = {L"resources", L"webui", L"nodedist"};

    std::wstring Utf8ToWide(const std::string& p_String) {
        if (p_String.empty()) return {};
        const int s_Size = MultiByteToWideChar(CP_UTF8, 0, p_String.data(), (int)p_String.size(), nullptr, 0);
        std::wstring s_Result(s_Size, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, p_String.data(), (int)p_String.size(), s_Result.data(), s_Size);
        return s_Result;
    }

    struct WinHttpHandle {
        HINTERNET Handle = nullptr;
        ~WinHttpHandle() { if (Handle) WinHttpCloseHandle(Handle); }
    };

    std::optional<std::string> HttpGet(const std::wstring& p_Url) {
        URL_COMPONENTSW s_Components { sizeof(s_Components) };
        s_Components.dwHostNameLength = (DWORD)-1;
        s_Components.dwUrlPathLength = (DWORD)-1;
        s_Components.dwExtraInfoLength = (DWORD)-1;

        if (!WinHttpCrackUrl(p_Url.c_str(), 0, 0, &s_Components)) return std::nullopt;

        const std::wstring s_Host(s_Components.lpszHostName, s_Components.dwHostNameLength);
        const std::wstring s_Path = std::wstring(s_Components.lpszUrlPath, s_Components.dwUrlPathLength)
            + std::wstring(s_Components.lpszExtraInfo, s_Components.dwExtraInfoLength);

        WinHttpHandle s_Session { WinHttpOpen(L"PeacockLauncher", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0) };
        if (!s_Session.Handle) return std::nullopt;
        WinHttpSetTimeouts(s_Session.Handle, 10000, 10000, 15000, 30000);

        WinHttpHandle s_Connection { WinHttpConnect(s_Session.Handle, s_Host.c_str(), s_Components.nPort, 0) };
        if (!s_Connection.Handle) return std::nullopt;

        WinHttpHandle s_Request {
            WinHttpOpenRequest(
                s_Connection.Handle, L"GET", s_Path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                s_Components.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0
            )
        };
        if (!s_Request.Handle) return std::nullopt;

        if (!WinHttpSendRequest(s_Request.Handle, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
            || !WinHttpReceiveResponse(s_Request.Handle, nullptr))
            return std::nullopt;

        DWORD s_StatusCode = 0;
        DWORD s_StatusCodeSize = sizeof(s_StatusCode);
        WinHttpQueryHeaders(
            s_Request.Handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
            &s_StatusCode, &s_StatusCodeSize, WINHTTP_NO_HEADER_INDEX
        );

        if (s_StatusCode != 200) {
            Logger::Warn("[PeacockLauncher] GET {} returned HTTP {}.", std::string(p_Url.begin(), p_Url.end()), s_StatusCode);
            return std::nullopt;
        }

        std::string s_Body;
        DWORD s_Available = 0;

        while (WinHttpQueryDataAvailable(s_Request.Handle, &s_Available) && s_Available > 0) {
            const size_t s_Offset = s_Body.size();
            s_Body.resize(s_Offset + s_Available);

            DWORD s_Read = 0;
            if (!WinHttpReadData(s_Request.Handle, s_Body.data() + s_Offset, s_Available, &s_Read)) return std::nullopt;
            s_Body.resize(s_Offset + s_Read);
        }

        return s_Body;
    }

    std::vector<int> ParseVersion(const std::string& p_Version) {
        std::vector<int> s_Parts;
        std::stringstream s_Stream(p_Version.starts_with('v') ? p_Version.substr(1) : p_Version);
        std::string s_Part;

        while (std::getline(s_Stream, s_Part, '.')) {
            try { s_Parts.push_back(std::stoi(s_Part)); }
            catch (...) { s_Parts.push_back(0); }
        }

        return s_Parts;
    }

    // The windows build bundles node in nodedist, the linux build expects node to be installed
    bool IsLinuxBuild(const std::wstring& p_PeacockDir) {
        return !fs::is_directory(fs::path(p_PeacockDir) / L"nodedist");
    }

    bool IsSafeRelativePath(const fs::path& p_Path) {
        if (p_Path.empty() || p_Path.has_root_name() || p_Path.has_root_directory()) return false;
        for (const auto& s_Part : p_Path)
            if (s_Part == L"..") return false;
        return true;
    }

    bool ExtractToStaging(const std::string& p_ZipData, const fs::path& p_StagingDir) {
        mz_zip_archive s_Zip {};
        if (!mz_zip_reader_init_mem(&s_Zip, p_ZipData.data(), p_ZipData.size(), 0)) {
            Logger::Error("[PeacockLauncher] Downloaded update is not a valid zip.");
            return false;
        }

        const mz_uint s_FileCount = mz_zip_reader_get_num_files(&s_Zip);
        std::vector<std::string> s_Names(s_FileCount);

        for (mz_uint i = 0; i < s_FileCount; ++i) {
            mz_zip_archive_file_stat s_Stat;
            mz_zip_reader_file_stat(&s_Zip, i, &s_Stat);
            s_Names[i] = s_Stat.m_filename;
        }

        // Releases wrap everything in a "Peacock-vX.Y.Z" folder, which gets stripped
        std::string s_Prefix;
        if (!s_Names.empty()) {
            s_Prefix = s_Names[0].substr(0, s_Names[0].find('/') + 1);
            for (const auto& s_Name : s_Names)
                if (!s_Name.starts_with(s_Prefix)) s_Prefix.clear();
        }

        bool s_Success = true;

        for (mz_uint i = 0; i < s_FileCount && s_Success; ++i) {
            const std::string s_RelativeName = s_Names[i].substr(s_Prefix.size());
            if (s_RelativeName.empty()) continue;

            const fs::path s_RelativePath = fs::path(Utf8ToWide(s_RelativeName)).lexically_normal();
            if (!IsSafeRelativePath(s_RelativePath)) {
                Logger::Error("[PeacockLauncher] Refusing unsafe path '{}' in update zip.", s_Names[i]);
                s_Success = false;
                break;
            }

            const fs::path s_Target = p_StagingDir / s_RelativePath;

            if (mz_zip_reader_is_file_a_directory(&s_Zip, i)) {
                fs::create_directories(s_Target);
                continue;
            }

            fs::create_directories(s_Target.parent_path());

            size_t s_Size = 0;
            void* s_Data = mz_zip_reader_extract_to_heap(&s_Zip, i, &s_Size, 0);
            if (!s_Data) {
                Logger::Error("[PeacockLauncher] Failed to extract '{}' from update zip.", s_Names[i]);
                s_Success = false;
                break;
            }

            std::ofstream s_File(s_Target, std::ios::binary | std::ios::trunc);
            s_File.write(static_cast<const char*>(s_Data), s_Size);
            s_Success = s_File.good();
            mz_free(s_Data);

            if (!s_Success) Logger::Error("[PeacockLauncher] Failed to write '{}'.", s_Target.string());
        }

        mz_zip_reader_end(&s_Zip);
        return s_Success;
    }

    void MergeStagingIntoInstall(const fs::path& p_StagingDir, const fs::path& p_PeacockDir) {
        for (const auto& s_Entry : fs::directory_iterator(p_StagingDir)) {
            const std::wstring s_Name = s_Entry.path().filename().wstring();
            const fs::path s_Target = p_PeacockDir / s_Name;

            if (s_Entry.is_directory() && c_ReplacedDirs.contains(s_Name)) {
                fs::remove_all(s_Target);
                fs::rename(s_Entry.path(), s_Target);
            }
            else if (c_UserDataDirs.contains(s_Name) || (s_Name == L"options.ini" && fs::exists(s_Target))) {
                fs::copy(s_Entry.path(), s_Target, fs::copy_options::recursive | fs::copy_options::skip_existing);
            }
            else {
                fs::copy(s_Entry.path(), s_Target, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
            }
        }
    }
}

namespace PeacockUpdater {
    std::string GetInstalledVersion(const std::wstring& p_PeacockDir) {
        std::ifstream s_File(fs::path(p_PeacockDir) / L"chunk0.js", std::ios::binary);
        if (!s_File) return {};

        const std::string s_Source((std::istreambuf_iterator<char>(s_File)), std::istreambuf_iterator<char>());

        // The bundle declares the version right after the game version struct: _Revision:0},Gu="8.9.1"
        for (size_t s_Pos = s_Source.find("_Revision:"); s_Pos != std::string::npos; s_Pos = s_Source.find("_Revision:", s_Pos + 1)) {
            size_t i = s_Pos + 10;
            while (i < s_Source.size() && isdigit((unsigned char)s_Source[i])) ++i;
            if (s_Source.compare(i, 2, "},") != 0) continue;
            i += 2;

            while (i < s_Source.size() && (isalnum((unsigned char)s_Source[i]) || s_Source[i] == '_' || s_Source[i] == '$')) ++i;
            if (s_Source.compare(i, 2, "=\"") != 0) continue;
            i += 2;

            const size_t s_End = s_Source.find('"', i);
            if (s_End == std::string::npos || s_End - i > 32) continue;

            const std::string s_Version = s_Source.substr(i, s_End - i);
            if (!s_Version.empty() && s_Version.find_first_not_of("0123456789.") == std::string::npos)
                return s_Version;
        }

        return {};
    }

    std::optional<Release> GetLatestRelease(const std::wstring& p_PeacockDir) {
        const auto s_Body = HttpGet(c_LatestReleaseUrl);
        if (!s_Body) {
            Logger::Warn("[PeacockLauncher] Failed to fetch the latest Peacock release.");
            return std::nullopt;
        }

        try {
            const auto s_Json = nlohmann::json::parse(*s_Body);
            const bool s_WantLinux = IsLinuxBuild(p_PeacockDir);

            Release s_Release;
            s_Release.Version = s_Json.at("tag_name").get<std::string>();
            if (s_Release.Version.starts_with('v')) s_Release.Version.erase(0, 1);
            s_Release.PageUrl = s_Json.value("html_url", c_ReleasesPage);

            for (const auto& s_Asset : s_Json.at("assets")) {
                const auto s_Name = s_Asset.at("name").get<std::string>();
                if (!s_Name.ends_with(".zip") || s_Name.ends_with("-linux.zip") != s_WantLinux) continue;

                s_Release.AssetUrl = s_Asset.at("browser_download_url").get<std::string>();
                break;
            }

            if (s_Release.AssetUrl.empty())
                Logger::Warn("[PeacockLauncher] Release {} has no {} zip.", s_Release.Version, s_WantLinux ? "linux" : "windows");

            return s_Release;
        }
        catch (const std::exception& e) {
            Logger::Warn("[PeacockLauncher] Failed to parse the latest Peacock release: {}", e.what());
            return std::nullopt;
        }
    }

    bool IsNewerVersion(const std::string& p_Candidate, const std::string& p_Current) {
        auto s_Candidate = ParseVersion(p_Candidate);
        auto s_Current = ParseVersion(p_Current);

        const size_t s_Length = std::max(s_Candidate.size(), s_Current.size());
        s_Candidate.resize(s_Length);
        s_Current.resize(s_Length);

        return s_Candidate > s_Current;
    }

    bool InstallRelease(const Release& p_Release, const std::wstring& p_PeacockDir) {
        if (p_Release.AssetUrl.empty()) return false;

        Logger::Info("[PeacockLauncher] Downloading Peacock {} from {}.", p_Release.Version, p_Release.AssetUrl);

        const auto s_ZipData = HttpGet(Utf8ToWide(p_Release.AssetUrl));
        if (!s_ZipData) {
            Logger::Error("[PeacockLauncher] Failed to download Peacock {}.", p_Release.Version);
            return false;
        }

        const fs::path s_PeacockDir = p_PeacockDir;
        const fs::path s_StagingDir = s_PeacockDir / c_StagingDirName;

        try {
            fs::remove_all(s_StagingDir);
            fs::create_directories(s_StagingDir);

            const bool s_Extracted = ExtractToStaging(*s_ZipData, s_StagingDir);
            if (s_Extracted && !fs::exists(s_StagingDir / L"chunk0.js"))
                Logger::Error("[PeacockLauncher] Update zip has no chunk0.js, not installing it.");
            else if (s_Extracted)
                MergeStagingIntoInstall(s_StagingDir, s_PeacockDir);

            fs::remove_all(s_StagingDir);

            if (!s_Extracted || GetInstalledVersion(p_PeacockDir) != p_Release.Version) return false;
        }
        catch (const std::exception& e) {
            Logger::Error("[PeacockLauncher] Failed to install Peacock {}: {}", p_Release.Version, e.what());
            std::error_code s_Ignored;
            fs::remove_all(s_StagingDir, s_Ignored);
            return false;
        }

        Logger::Info("[PeacockLauncher] Updated Peacock to {}.", p_Release.Version);
        return true;
    }
}
