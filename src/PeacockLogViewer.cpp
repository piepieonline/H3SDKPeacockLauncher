#include "PeacockLogViewer.h"

#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <format>
#include <optional>
#include <vector>

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <IModSDK.h>
#include <IconsMaterialDesign.h>

namespace {
    constexpr size_t c_MaxLines = 5000;
    constexpr auto c_PollInterval = std::chrono::milliseconds(500);

    std::string StripAnsiCodes(const std::string& p_Text) {
        std::string s_Result;
        s_Result.reserve(p_Text.size());

        for (size_t i = 0; i < p_Text.size(); ++i) {
            if (p_Text[i] == '\x1b' && i + 1 < p_Text.size() && p_Text[i + 1] == '[') {
                i += 2;
                while (i < p_Text.size() && !(p_Text[i] >= 0x40 && p_Text[i] <= 0x7E)) ++i;
                continue;
            }

            s_Result += p_Text[i];
        }

        return s_Result;
    }

    std::wstring FindNewestLogFile(const std::wstring& p_PeacockDir) {
        const std::wstring s_LogsDir = p_PeacockDir + L"\\logs\\";

        WIN32_FIND_DATAW s_FindData;
        const HANDLE s_Find = FindFirstFileW((s_LogsDir + L"peacock-*.json").c_str(), &s_FindData);
        if (s_Find == INVALID_HANDLE_VALUE) return {};

        // Names embed a yyyyMMddHHmmss timestamp, so the greatest name is the newest
        std::wstring s_Newest;
        do {
            if (!(s_FindData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && s_FindData.cFileName > s_Newest)
                s_Newest = s_FindData.cFileName;
        }
        while (FindNextFileW(s_Find, &s_FindData));

        FindClose(s_Find);
        return s_Newest.empty() ? std::wstring {} : s_LogsDir + s_Newest;
    }

    std::string WideToUtf8(const std::wstring& p_String) {
        if (p_String.empty()) return {};
        const int s_Size = WideCharToMultiByte(CP_UTF8, 0, p_String.data(), (int)p_String.size(), nullptr, 0, nullptr, nullptr);
        std::string s_Result(s_Size, '\0');
        WideCharToMultiByte(CP_UTF8, 0, p_String.data(), (int)p_String.size(), s_Result.data(), s_Size, nullptr, nullptr);
        return s_Result;
    }

    bool ContainsCaseInsensitive(const std::string& p_Haystack, const std::string& p_Needle) {
        return std::search(
            p_Haystack.begin(), p_Haystack.end(), p_Needle.begin(), p_Needle.end(),
            [](char a, char b) { return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); }
        ) != p_Haystack.end();
    }
}

void PeacockLogViewer::Reset() {
    m_LogFilePath.clear();
    m_ReadOffset = 0;
    m_PartialLine.clear();
    m_Lines.clear();
}

void PeacockLogViewer::Poll(const std::wstring& p_PeacockDir) {
    if (p_PeacockDir != m_PeacockDir) {
        Reset();
        m_PeacockDir = p_PeacockDir;
    }

    if (m_PeacockDir.empty()) return;

    const std::wstring s_Newest = FindNewestLogFile(m_PeacockDir);
    if (s_Newest != m_LogFilePath) {
        Reset();
        m_LogFilePath = s_Newest;
    }

    if (!m_LogFilePath.empty()) ReadNewData();
}

void PeacockLogViewer::ReadNewData() {
    const HANDLE s_File = CreateFileW(
        m_LogFilePath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr
    );
    if (s_File == INVALID_HANDLE_VALUE) return;

    LARGE_INTEGER s_Size {};
    GetFileSizeEx(s_File, &s_Size);

    if (s_Size.QuadPart < m_ReadOffset) {
        m_ReadOffset = 0;
        m_PartialLine.clear();
        m_Lines.clear();
    }

    if (s_Size.QuadPart > m_ReadOffset) {
        std::vector<char> s_Buffer(static_cast<size_t>(s_Size.QuadPart - m_ReadOffset));

        LARGE_INTEGER s_Offset {};
        s_Offset.QuadPart = m_ReadOffset;
        SetFilePointerEx(s_File, s_Offset, nullptr, FILE_BEGIN);

        DWORD s_BytesRead = 0;
        if (ReadFile(s_File, s_Buffer.data(), (DWORD)s_Buffer.size(), &s_BytesRead, nullptr)) {
            m_ReadOffset += s_BytesRead;
            m_PartialLine.append(s_Buffer.data(), s_BytesRead);

            size_t s_LineStart = 0;
            for (size_t s_NewLine; (s_NewLine = m_PartialLine.find('\n', s_LineStart)) != std::string::npos;) {
                AddLine(m_PartialLine.substr(s_LineStart, s_NewLine - s_LineStart));
                s_LineStart = s_NewLine + 1;
            }

            m_PartialLine.erase(0, s_LineStart);
        }
    }

    CloseHandle(s_File);
}

void PeacockLogViewer::AddLine(const std::string& p_RawLine) {
    if (p_RawLine.empty() || p_RawLine == "\r") return;

    LogLine s_Line {ELevel::Info, {}, {}, {}};
    const auto s_Json = nlohmann::json::parse(p_RawLine, nullptr, false);

    if (s_Json.is_object()) {
        const std::string s_Level = s_Json.value("level", "");
        s_Line.Level = s_Level == "Debug" ? ELevel::Debug
            : s_Level == "Warn" ? ELevel::Warn
            : s_Level == "Error" ? ELevel::Error
            : ELevel::Info;
        s_Line.Timestamp = s_Json.value("timestamp", "");
        s_Line.Category = s_Json.value("category", "");
        s_Line.Message = StripAnsiCodes(s_Json.value("message", ""));
    }
    else {
        s_Line.Message = StripAnsiCodes(p_RawLine);
    }

    m_Lines.push_back(std::move(s_Line));
    if (m_Lines.size() > c_MaxLines) m_Lines.pop_front();
}

bool PeacockLogViewer::IsVisible(const LogLine& p_Line) const {
    if (static_cast<int>(p_Line.Level) < m_MinLevel) return false;
    if (m_TextFilter[0] == '\0') return true;

    return ContainsCaseInsensitive(p_Line.Message, m_TextFilter) || ContainsCaseInsensitive(p_Line.Category, m_TextFilter);
}

void PeacockLogViewer::CopyVisibleToClipboard() const {
    std::string s_Text;
    for (const auto& s_Line : m_Lines) {
        if (IsVisible(s_Line)) s_Text += std::format("[{}] [{}] {}\n", s_Line.Timestamp, s_Line.Category, s_Line.Message);
    }

    ImGui::SetClipboardText(s_Text.c_str());
}

void PeacockLogViewer::Draw(bool* p_Open, const std::function<std::wstring()>& p_GetPeacockDir) {
    const auto s_Now = std::chrono::steady_clock::now();
    if (s_Now >= m_NextPollTime) {
        Poll(p_GetPeacockDir());
        m_NextPollTime = s_Now + c_PollInterval;
    }

    ImGui::SetNextWindowSize(ImVec2(1000, 500), ImGuiCond_FirstUseEver);

    ImGui::PushFont(SDK()->GetImGuiBlackFont());
    const auto s_Showing = ImGui::Begin(ICON_MD_TERMINAL " Peacock Logs", p_Open);
    ImGui::PushFont(SDK()->GetImGuiRegularFont());

    if (s_Showing) {
        const float s_FooterHeight = ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeightWithSpacing();
        ImGui::BeginChild("ScrollingRegion", ImVec2(0, -s_FooterHeight), false);

        if (m_PeacockDir.empty()) {
            ImGui::TextDisabled("Peacock path is not set.");
        }
        else if (m_LogFilePath.empty()) {
            ImGui::TextDisabled("No logs found yet. Has Peacock started?");
        }
        else {
            ImGui::PushTextWrapPos();

            for (const auto& s_Line : m_Lines) {
                if (!IsVisible(s_Line)) continue;

                std::optional<ImVec4> s_Color;
                switch (s_Line.Level) {
                    case ELevel::Debug: s_Color = ImVec4(61.f / 255.f, 129.f / 255.f, 1.f, 1.f); break;
                    case ELevel::Warn: s_Color = ImVec4(1.f, 168.f / 255.f, 61.f / 255.f, 1.f); break;
                    case ELevel::Error: s_Color = ImVec4(1.f, 69.f / 255.f, 69.f / 255.f, 1.f); break;
                    case ELevel::Info: break;
                }

                if (s_Color) ImGui::PushStyleColor(ImGuiCol_Text, *s_Color);

                if (s_Line.Timestamp.empty()) {
                    ImGui::TextUnformatted(s_Line.Message.c_str(), s_Line.Message.c_str() + s_Line.Message.size());
                }
                else {
                    const std::string s_Text = std::format("[{}] [{}] {}", s_Line.Timestamp, s_Line.Category, s_Line.Message);
                    ImGui::TextUnformatted(s_Text.c_str(), s_Text.c_str() + s_Text.size());
                }

                if (s_Color) ImGui::PopStyleColor();
            }

            ImGui::PopTextWrapPos();

            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
                ImGui::SetScrollHereY(1.0f);
        }

        ImGui::EndChild();

        ImGui::Separator();

        const char* s_Levels[] = {"Debug and above", "Info and above", "Warn and above", "Errors only"};
        ImGui::SetNextItemWidth(180);
        ImGui::Combo("##minLevel", &m_MinLevel, s_Levels, IM_ARRAYSIZE(s_Levels));

        ImGui::SameLine();
        ImGui::SetNextItemWidth(250);
        ImGui::InputTextWithHint("##textFilter", "Filter", m_TextFilter, sizeof(m_TextFilter));

        ImGui::SameLine();
        if (ImGui::Button(ICON_MD_DELETE_SWEEP " Clear"))
            m_Lines.clear();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Hides the current lines. The log file is not changed.");

        ImGui::SameLine();
        if (ImGui::Button(ICON_MD_CONTENT_COPY " Copy"))
            CopyVisibleToClipboard();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Copies the lines matching the current filters.");

        if (!m_LogFilePath.empty()) {
            ImGui::SameLine();
            const size_t s_NameStart = m_LogFilePath.find_last_of(L'\\');
            ImGui::TextDisabled("%s", WideToUtf8(m_LogFilePath.substr(s_NameStart + 1)).c_str());
        }
    }

    ImGui::PopFont();
    ImGui::End();
    ImGui::PopFont();
}
