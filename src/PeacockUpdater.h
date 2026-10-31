#pragma once

#include <optional>
#include <string>

namespace PeacockUpdater {
    struct Release {
        std::string Version;
        std::string AssetUrl;
        std::string PageUrl;
    };

    constexpr const char* c_ReleasesPage = "https://github.com/thepeacockproject/Peacock/releases";

    std::string GetInstalledVersion(const std::wstring& p_PeacockDir);
    std::optional<Release> GetLatestRelease(const std::wstring& p_PeacockDir);
    bool IsNewerVersion(const std::string& p_Candidate, const std::string& p_Current);
    bool InstallRelease(const Release& p_Release, const std::wstring& p_PeacockDir);
}
