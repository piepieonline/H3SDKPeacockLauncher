#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>

class PeacockLogViewer {
public:
    void Draw(bool* p_Open, const std::function<std::wstring()>& p_GetPeacockDir);

private:
    enum class ELevel {
        Debug,
        Info,
        Warn,
        Error,
    };

    struct LogLine {
        ELevel Level;
        std::string Timestamp;
        std::string Category;
        std::string Message;
    };

    void Poll(const std::wstring& p_PeacockDir);
    void ReadNewData();
    void AddLine(const std::string& p_RawLine);
    void Reset();
    bool IsVisible(const LogLine& p_Line) const;
    void CopyVisibleToClipboard() const;

    std::wstring m_PeacockDir;
    std::wstring m_LogFilePath;
    int64_t m_ReadOffset = 0;
    std::string m_PartialLine;
    std::deque<LogLine> m_Lines;

    std::chrono::steady_clock::time_point m_NextPollTime {};

    int m_MinLevel = static_cast<int>(ELevel::Debug);
    char m_TextFilter[256] {};
};
