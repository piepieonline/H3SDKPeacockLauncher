#include "PeacockLauncher.h"

#include <WS2tcpip.h>

#include <algorithm>
#include <chrono>
#include <format>
#include <thread>

#include <Logging.h>
#include <IconsMaterialDesign.h>
#include <Functions.h>

#include <Glacier/ZConfigCommand.h>

using wine_get_unix_file_name_t = char*(__cdecl*)(LPCWSTR);
using wine_get_dos_file_name_t = WCHAR*(__cdecl*)(LPCSTR);

#pragma region String Utils

static std::wstring Utf8ToWide(const std::string& p_String) {
    if (p_String.empty()) return {};
    const int s_Size = MultiByteToWideChar(CP_UTF8, 0, p_String.data(), (int)p_String.size(), nullptr, 0);
    std::wstring s_Result(s_Size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, p_String.data(), (int)p_String.size(), s_Result.data(), s_Size);
    return s_Result;
}

static std::string WideToUtf8(const std::wstring& p_String) {
    if (p_String.empty()) return {};
    const int s_Size = WideCharToMultiByte(CP_UTF8, 0, p_String.data(), (int)p_String.size(), nullptr, 0, nullptr, nullptr);
    std::string s_Result(s_Size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, p_String.data(), (int)p_String.size(), s_Result.data(), s_Size, nullptr, nullptr);
    return s_Result;
}

static std::string ShellQuote(const std::string& p_String) {
    std::string s_Result = "'";
    for (const char c : p_String) {
        if (c == '\'') s_Result += "'\\''";
        else s_Result += c;
    }
    return s_Result + "'";
}

// Follows the CommandLineToArgvW rules, which Wine also uses to build argv for unix processes.
static std::wstring WindowsQuoteArg(const std::wstring& p_Arg) {
    std::wstring s_Result = L"\"";
    size_t s_Backslashes = 0;

    for (const wchar_t c : p_Arg) {
        if (c == L'\\') {
            ++s_Backslashes;
            continue;
        }

        if (c == L'"') s_Result.append(s_Backslashes * 2 + 1, L'\\');
        else s_Result.append(s_Backslashes, L'\\');

        s_Backslashes = 0;
        s_Result += c;
    }

    s_Result.append(s_Backslashes * 2, L'\\');
    return s_Result + L"\"";
}

static bool FileExists(const std::wstring& p_Path) {
    const DWORD s_Attributes = GetFileAttributesW(p_Path.c_str());
    return s_Attributes != INVALID_FILE_ATTRIBUTES && !(s_Attributes & FILE_ATTRIBUTE_DIRECTORY);
}

static std::wstring TrimTrailingSeparators(std::wstring p_Path) {
    while (p_Path.size() > 1 && (p_Path.back() == L'\\' || p_Path.back() == L'/'))
        p_Path.pop_back();
    return p_Path;
}

#pragma endregion

#pragma region Wine Helpers

static wine_get_unix_file_name_t GetWineUnixFileNameFn() {
    return reinterpret_cast<wine_get_unix_file_name_t>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "wine_get_unix_file_name")
    );
}

static wine_get_dos_file_name_t GetWineDosFileNameFn() {
    return reinterpret_cast<wine_get_dos_file_name_t>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "wine_get_dos_file_name")
    );
}

static std::wstring UnixToDosPath(const std::string& p_UnixPath) {
    const auto s_Convert = GetWineDosFileNameFn();
    if (!s_Convert) return {};

    WCHAR* s_DosPath = s_Convert(p_UnixPath.c_str());
    if (!s_DosPath) return {};

    std::wstring s_Result = s_DosPath;
    HeapFree(GetProcessHeap(), 0, s_DosPath);
    return s_Result;
}

static std::string DosToUnixPath(const std::wstring& p_DosPath) {
    const auto s_Convert = GetWineUnixFileNameFn();
    if (!s_Convert) return {};

    char* s_UnixPath = s_Convert(p_DosPath.c_str());
    if (!s_UnixPath) return {};

    std::string s_Result = s_UnixPath;
    HeapFree(GetProcessHeap(), 0, s_UnixPath);
    return s_Result;
}

#pragma endregion

void PeacockLauncher::Init() {
    m_IsWine = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr;

    WSADATA s_WsaData;
    WSAStartup(MAKEWORD(2, 2), &s_WsaData);

    LoadSettings();

    if (m_AutoConnect) {
        PatchAuthHeaderChecks();
        Hooks::ZOnlineVersionConfig_GetConfigHost->AddDetour(this, &PeacockLauncher::GetConfigHost);
        Hooks::Check_SSL_Cert->AddDetour(this, &PeacockLauncher::Check_SSL_Cert);
    }

    // Launched here rather than OnEngineInitialized so Peacock has a head start before the game first connects
    m_IsLaunchPending = true;
    RunInBackground([this] {
        if (m_UpdateMode == EPeacockUpdateMode::Auto) {
            CheckForUpdate();
            InstallUpdate();
        }

        if (m_AutoLaunch) LaunchPeacock();
        m_IsLaunchPending = false;

        if (m_UpdateMode == EPeacockUpdateMode::Alert) CheckForUpdate();
    });
}

void PeacockLauncher::OnEngineInitialized() {
    if (m_AutoConnect) ConnectToPeacock();
}

PeacockLauncher::~PeacockLauncher() {
    {
        std::lock_guard s_Lock(m_BackgroundMutex);
        if (m_BackgroundThread.joinable()) m_BackgroundThread.join();
    }

    RestoreAuthHeaderChecks();
    StopPeacock();
    WSACleanup();
}

#pragma region Settings

void PeacockLauncher::LoadSettings() {
    m_PeacockPath = GetSetting("peacock", "path", "").c_str();
    m_NodePath = GetSetting("peacock", "node_path", "").c_str();
    m_Port = GetSettingInt("peacock", "port", 8123);
    m_StartupTimeoutSeconds = GetSettingInt("peacock", "startup_timeout_seconds", 30);
    m_AutoLaunch = GetSettingBool("peacock", "auto_launch", true);
    m_AutoConnect = GetSettingBool("peacock", "auto_connect", true);

    const std::string s_UpdateMode = GetSetting("peacock", "update_mode", "alert").c_str();
    m_UpdateMode = s_UpdateMode == "none" ? EPeacockUpdateMode::None
        : s_UpdateMode == "auto" ? EPeacockUpdateMode::Auto
        : EPeacockUpdateMode::Alert;
}

void PeacockLauncher::SaveSettings() {
    SetSetting("peacock", "path", m_PeacockPath);
    SetSetting("peacock", "node_path", m_NodePath);
    SetSettingInt("peacock", "port", m_Port);
    SetSettingInt("peacock", "startup_timeout_seconds", m_StartupTimeoutSeconds);
    SetSettingBool("peacock", "auto_launch", m_AutoLaunch);
    SetSettingBool("peacock", "auto_connect", m_AutoConnect);
    SetSetting(
        "peacock", "update_mode",
        std::string(m_UpdateMode == EPeacockUpdateMode::None ? "none" : m_UpdateMode == EPeacockUpdateMode::Auto ? "auto" : "alert")
    );
}

#pragma endregion

#pragma region Process Management

std::wstring PeacockLauncher::GetPeacockDir() {
    std::string s_PeacockPath;
    {
        std::lock_guard s_Lock(m_StateMutex);
        s_PeacockPath = m_PeacockPath;
    }

    if (s_PeacockPath.empty()) return {};

    const std::wstring s_PeacockDir = m_IsWine && s_PeacockPath.starts_with('/')
        ? UnixToDosPath(s_PeacockPath)
        : Utf8ToWide(s_PeacockPath);
    return TrimTrailingSeparators(s_PeacockDir);
}

bool PeacockLauncher::IsPeacockListening() const {
    const SOCKET s_Socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s_Socket == INVALID_SOCKET) return false;

    u_long s_NonBlocking = 1;
    ioctlsocket(s_Socket, FIONBIO, &s_NonBlocking);

    sockaddr_in s_Address {};
    s_Address.sin_family = AF_INET;
    s_Address.sin_port = htons(static_cast<u_short>(m_Port));
    inet_pton(AF_INET, "127.0.0.1", &s_Address.sin_addr);

    connect(s_Socket, reinterpret_cast<sockaddr*>(&s_Address), sizeof(s_Address));

    fd_set s_WriteSet, s_ErrorSet;
    FD_ZERO(&s_WriteSet);
    FD_ZERO(&s_ErrorSet);
    FD_SET(s_Socket, &s_WriteSet);
    FD_SET(s_Socket, &s_ErrorSet);
    timeval s_Timeout {0, 250000};

    const bool s_Connected = select(0, nullptr, &s_WriteSet, &s_ErrorSet, &s_Timeout) > 0
        && FD_ISSET(s_Socket, &s_WriteSet) && !FD_ISSET(s_Socket, &s_ErrorSet);

    closesocket(s_Socket);
    return s_Connected;
}

bool PeacockLauncher::WaitUntilListening(int64_t p_TimeoutSeconds) {
    const auto s_Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(p_TimeoutSeconds);

    while (std::chrono::steady_clock::now() < s_Deadline) {
        if (IsPeacockListening()) {
            Logger::Info("[PeacockLauncher] Peacock is listening on port {}.", m_Port);
            SetStatus(std::format("Running on port {}", m_Port));
            return true;
        }

        {
            std::lock_guard s_Lock(m_ProcessMutex);
            if (m_PeacockProcess && WaitForSingleObject(m_PeacockProcess, 0) == WAIT_OBJECT_0) {
                Logger::Error("[PeacockLauncher] Peacock exited before it started listening.");
                SetStatus("Peacock exited during startup");
                return false;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    Logger::Error("[PeacockLauncher] Timed out waiting for Peacock to listen on port {}.", m_Port);
    SetStatus("Timed out waiting for Peacock");
    return false;
}

void PeacockLauncher::WaitForPeacock() {
    if (m_HasWaitedForPeacock) return;
    m_HasWaitedForPeacock = true;

    // Covers an auto update, which has its own network timeouts
    while (m_IsLaunchPending)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    if (HasLaunchedPeacock()) WaitUntilListening(m_StartupTimeoutSeconds);
}

bool PeacockLauncher::HasLaunchedPeacock() {
    std::lock_guard s_Lock(m_ProcessMutex);
    return m_PeacockProcess || !m_WinePeacockDir.empty();
}

void PeacockLauncher::LaunchPeacock() {
    std::lock_guard s_Lock(m_ProcessMutex);

    if (HasLaunchedPeacock()) {
        SetStatus("Already launched");
        return;
    }

    if (IsPeacockListening()) {
        Logger::Info("[PeacockLauncher] Something is already listening on port {}, not launching Peacock.", m_Port);
        SetStatus(std::format("Already running on port {}", m_Port));
        return;
    }

    const std::wstring s_PeacockDir = GetPeacockDir();

    if (s_PeacockDir.empty()) {
        Logger::Warn("[PeacockLauncher] Peacock path is not set.");
        SetStatus("Peacock path is not set");
        return;
    }

    if (!FileExists(s_PeacockDir + L"\\chunk0.js")) {
        Logger::Error("[PeacockLauncher] chunk0.js not found in '{}'.", WideToUtf8(s_PeacockDir));
        SetStatus("chunk0.js not found in Peacock path");
        return;
    }

    const bool s_UseWindowsNode = !m_IsWine || m_NodePath.ends_with(".exe");
    const bool s_Launched = s_UseWindowsNode ? LaunchPeacockWindows(s_PeacockDir) : LaunchPeacockWine(s_PeacockDir);

    if (s_Launched) {
        m_HasWaitedForPeacock = false;
        SetStatus("Starting...");
    }
}

bool PeacockLauncher::LaunchPeacockWindows(const std::wstring& p_PeacockDir) {
    std::wstring s_NodePath = Utf8ToWide(m_NodePath);

    if (s_NodePath.empty() && FileExists(p_PeacockDir + L"\\nodedist\\node.exe"))
        s_NodePath = p_PeacockDir + L"\\nodedist\\node.exe";

    if (s_NodePath.empty()) {
        wchar_t s_FoundPath[MAX_PATH] {};
        if (SearchPathW(nullptr, L"node.exe", nullptr, MAX_PATH, s_FoundPath, nullptr))
            s_NodePath = s_FoundPath;
    }

    if (s_NodePath.empty()) {
        Logger::Error("[PeacockLauncher] Could not find node.exe. Set node_path in the settings.");
        SetStatus("node.exe not found");
        return false;
    }

    std::wstring s_CommandLine = WindowsQuoteArg(s_NodePath) + L" --enable-source-maps chunk0.js";

    wchar_t s_OldPort[32] {};
    const bool s_HadPort = GetEnvironmentVariableW(L"PORT", s_OldPort, 32) > 0;
    SetEnvironmentVariableW(L"PORT", std::to_wstring(m_Port).c_str());

    STARTUPINFOW s_StartupInfo { sizeof(s_StartupInfo) };
    PROCESS_INFORMATION s_ProcessInfo {};

    const BOOL s_Created = CreateProcessW(
        s_NodePath.c_str(), s_CommandLine.data(), nullptr, nullptr, FALSE,
        CREATE_NEW_CONSOLE | CREATE_SUSPENDED, nullptr, p_PeacockDir.c_str(), &s_StartupInfo, &s_ProcessInfo
    );

    SetEnvironmentVariableW(L"PORT", s_HadPort ? s_OldPort : nullptr);

    if (!s_Created) {
        Logger::Error("[PeacockLauncher] Failed to start '{}' (error {}).", WideToUtf8(s_NodePath), GetLastError());
        SetStatus("Failed to start node");
        return false;
    }

    m_PeacockJob = CreateJobObjectW(nullptr, nullptr);
    if (m_PeacockJob) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION s_Limits {};
        s_Limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(m_PeacockJob, JobObjectExtendedLimitInformation, &s_Limits, sizeof(s_Limits));
        AssignProcessToJobObject(m_PeacockJob, s_ProcessInfo.hProcess);
    }

    ResumeThread(s_ProcessInfo.hThread);
    CloseHandle(s_ProcessInfo.hThread);
    m_PeacockProcess = s_ProcessInfo.hProcess;

    Logger::Info("[PeacockLauncher] Launched Peacock with '{}'.", WideToUtf8(s_NodePath));
    return true;
}

// Wine runs non-PE executables as native unix processes, which get no Windows process handle back
static bool RunUnixShell(const std::string& p_Script, const std::wstring& p_DosCwd) {
    const std::wstring s_ShellPath = UnixToDosPath("/bin/sh");
    if (s_ShellPath.empty()) return false;

    std::wstring s_CommandLine = L"/bin/sh -c " + WindowsQuoteArg(Utf8ToWide(p_Script));
    STARTUPINFOW s_StartupInfo { sizeof(s_StartupInfo) };
    PROCESS_INFORMATION s_ProcessInfo {};

    const BOOL s_Created = CreateProcessW(
        s_ShellPath.c_str(), s_CommandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, p_DosCwd.c_str(),
        &s_StartupInfo, &s_ProcessInfo
    );

    if (s_ProcessInfo.hThread) CloseHandle(s_ProcessInfo.hThread);
    if (s_ProcessInfo.hProcess) CloseHandle(s_ProcessInfo.hProcess);
    return s_Created;
}

// Files are opened by wineserver, so /proc/self is wineserver rather than the game. It exits shortly after the game does.
static std::string GetWineserverPid() {
    const HANDLE s_File = CreateFileW(
        UnixToDosPath("/proc/self/stat").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr
    );
    if (s_File == INVALID_HANDLE_VALUE) return {};

    char s_Buffer[32] {};
    DWORD s_BytesRead = 0;
    ReadFile(s_File, s_Buffer, sizeof(s_Buffer) - 1, &s_BytesRead, nullptr);
    CloseHandle(s_File);

    std::string s_Pid;
    for (const char* c = s_Buffer; *c >= '0' && *c <= '9'; ++c) s_Pid += *c;
    return s_Pid;
}

bool PeacockLauncher::LaunchPeacockWine(const std::wstring& p_PeacockDir) {
    const std::string s_UnixPeacockDir = DosToUnixPath(p_PeacockDir);

    if (s_UnixPeacockDir.empty()) {
        Logger::Error("[PeacockLauncher] Failed to convert '{}' to a unix path.", WideToUtf8(p_PeacockDir));
        SetStatus("Wine path conversion failed");
        return false;
    }

    const std::string s_WineserverPid = GetWineserverPid();
    if (s_WineserverPid.empty())
        Logger::Warn("[PeacockLauncher] Could not find the wineserver PID, Peacock will keep running after the game exits.");

    const std::string s_Script = std::format(
        "cd {} || exit 1\n"
        "NODE={}\n"
        "[ -z \"$NODE\" ] && NODE=$(command -v node || ls -d \"$HOME\"/.nvm/versions/node/*/bin/node 2>/dev/null | sort -V | tail -n 1)\n"
        "unset LD_PRELOAD LD_LIBRARY_PATH\n"
        "PORT={} \"$NODE\" --enable-source-maps chunk0.js </dev/null >peacock-launcher.log 2>&1 &\n"
        "PEACOCK_PID=$!\n"
        "echo $PEACOCK_PID >peacock-launcher.pid\n"
        "WATCH_PID={}\n"
        "[ -z \"$WATCH_PID\" ] && exit 0\n"
        "while kill -0 $WATCH_PID 2>/dev/null && kill -0 $PEACOCK_PID 2>/dev/null; do sleep 2; done\n"
        "kill $PEACOCK_PID 2>/dev/null\n"
        "rm -f peacock-launcher.pid\n",
        ShellQuote(s_UnixPeacockDir), ShellQuote(m_NodePath), m_Port, s_WineserverPid
    );

    if (!RunUnixShell(s_Script, p_PeacockDir)) {
        Logger::Error("[PeacockLauncher] Failed to start /bin/sh (error {}).", GetLastError());
        SetStatus("Failed to start /bin/sh");
        return false;
    }

    m_WinePeacockDir = p_PeacockDir;

    Logger::Info("[PeacockLauncher] Launched native Peacock in '{}', output in peacock-launcher.log.", s_UnixPeacockDir);
    return true;
}

void PeacockLauncher::StopPeacock() {
    std::lock_guard s_Lock(m_ProcessMutex);

    if (m_PeacockJob) {
        TerminateJobObject(m_PeacockJob, 0);
        CloseHandle(m_PeacockJob);
        m_PeacockJob = nullptr;
    }
    else if (m_PeacockProcess) {
        TerminateProcess(m_PeacockProcess, 0);
    }

    if (m_PeacockProcess) {
        CloseHandle(m_PeacockProcess);
        m_PeacockProcess = nullptr;
    }

    if (!m_WinePeacockDir.empty()) {
        RunUnixShell("[ -f peacock-launcher.pid ] && kill \"$(cat peacock-launcher.pid)\"; rm -f peacock-launcher.pid", m_WinePeacockDir);
        m_WinePeacockDir.clear();
    }

    SetStatus("Stopped");
}

void PeacockLauncher::ConnectToPeacock() {
    const std::string s_Domain = std::format("localhost:{}", m_Port);

    Functions::ZConfigCommand_ExecuteCommand->Call("OnlineResources_Disable", "0");
    Functions::ZConfigCommand_ExecuteCommand->Call("OnlineResources_ForceOfflineOnFailure", "0");
    Functions::ZConfigCommand_ExecuteCommand->Call("online_VersionConfigDomain", s_Domain.c_str());

    Logger::Info("[PeacockLauncher] Set online domain to '{}'.", s_Domain);
}

#pragma endregion

#pragma region Updates

void PeacockLauncher::RunInBackground(std::function<void()> p_Task) {
    std::lock_guard s_Lock(m_BackgroundMutex);
    if (m_IsBackgroundBusy) return;
    if (m_BackgroundThread.joinable()) m_BackgroundThread.join();

    m_IsBackgroundBusy = true;
    m_BackgroundThread = std::thread([this, p_Task = std::move(p_Task)] {
        p_Task();
        m_IsBackgroundBusy = false;
    });
}

void PeacockLauncher::CheckForUpdate() {
    const std::wstring s_PeacockDir = GetPeacockDir();
    if (s_PeacockDir.empty()) return;

    const std::string s_InstalledVersion = PeacockUpdater::GetInstalledVersion(s_PeacockDir);
    {
        std::lock_guard s_Lock(m_StateMutex);
        m_InstalledVersion = s_InstalledVersion;
    }

    if (s_InstalledVersion.empty()) {
        Logger::Warn("[PeacockLauncher] Could not read the installed Peacock version, skipping update check.");
        return;
    }

    const auto s_Latest = PeacockUpdater::GetLatestRelease(s_PeacockDir);
    if (!s_Latest) return;

    if (!PeacockUpdater::IsNewerVersion(s_Latest->Version, s_InstalledVersion)) {
        Logger::Info("[PeacockLauncher] Peacock {} is up to date.", s_InstalledVersion);
        return;
    }

    Logger::Warn("[PeacockLauncher] Peacock {} is available, installed version is {}.", s_Latest->Version, s_InstalledVersion);

    std::lock_guard s_Lock(m_StateMutex);
    m_AvailableUpdate = s_Latest;
    m_ShowUpdatePopup = true;
}

void PeacockLauncher::InstallUpdate() {
    std::optional<PeacockUpdater::Release> s_Release;
    {
        std::lock_guard s_Lock(m_StateMutex);
        s_Release = m_AvailableUpdate;
    }

    const std::wstring s_PeacockDir = GetPeacockDir();
    if (!s_Release || s_PeacockDir.empty()) return;

    const bool s_WasRunning = HasLaunchedPeacock();

    if (s_WasRunning) {
        StopPeacock();

        for (int i = 0; i < 40 && IsPeacockListening(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    if (IsPeacockListening()) {
        Logger::Warn("[PeacockLauncher] Peacock is running outside of the launcher, not updating it.");
        SetStatus("Close the running Peacock to update");
        return;
    }

    SetStatus(std::format("Updating to {}...", s_Release->Version));

    if (PeacockUpdater::InstallRelease(*s_Release, s_PeacockDir)) {
        std::lock_guard s_Lock(m_StateMutex);
        m_InstalledVersion = s_Release->Version;
        m_AvailableUpdate.reset();
        m_ShowUpdatePopup = false;
        m_Status = std::format("Updated to {}", s_Release->Version);
    }
    else {
        SetStatus("Update failed, see the log");
    }

    if (s_WasRunning) {
        LaunchPeacock();
        WaitUntilListening(m_StartupTimeoutSeconds);
    }
}

#pragma endregion

#pragma region Patching Helpers

// Same patches as OnlineTools: lets the Authorization header be sent to non-HTTPS and untrusted servers.
void PeacockLauncher::PatchAuthHeaderChecks() {
    uint8_t nop_2[2] = {0x90, 0x90};
    uint8_t nop_6[6] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90};

    if (m_OldAuthPatch1Code == nullptr) m_OldAuthPatch1Code = malloc(2);
    if (m_OldAuthPatch2Code == nullptr) m_OldAuthPatch2Code = malloc(6);

    if (!SDK()->PatchCodeStoreOriginal(
        "\x75\x00\x48\x83\xF9\x00\x75\x00\xEB\x00\x49\x8B\xCF", "x?xxx?x?x?xxx", nop_2, sizeof(nop_2), 0,
        m_OldAuthPatch1Code
    )) {
        Logger::Error("[PeacockLauncher] Failed to patch protocol check in ZOnlineManager::HttpRequest!");
        free(m_OldAuthPatch1Code);
        m_OldAuthPatch1Code = nullptr;
    }

    if (!SDK()->PatchCodeStoreOriginal(
        "\x0F\x84\x00\x00\x00\x00\x45\x84\xED\x0F\x85\x00\x00\x00\x00\x48\x8D\x55", "xx????xxxxx????xxx", nop_6,
        sizeof(nop_6), 0, m_OldAuthPatch2Code
    )) {
        Logger::Error("[PeacockLauncher] Failed to patch trusted domain check in ZOnlineManager::HttpRequest!");
        free(m_OldAuthPatch2Code);
        m_OldAuthPatch2Code = nullptr;
    }
}

void PeacockLauncher::RestoreAuthHeaderChecks() {
    if (m_OldAuthPatch1Code) {
        if (!SDK()->PatchCode(
            "\x90\x90\x48\x83\xF9\x00\x75\x00\xEB\x00\x49\x8B\xCF", "xxxxx?x?x?xxx", m_OldAuthPatch1Code, 2, 0
        ))
            Logger::Error("[PeacockLauncher] Failed to restore protocol check in ZOnlineManager::HttpRequest!");

        free(m_OldAuthPatch1Code);
        m_OldAuthPatch1Code = nullptr;
    }

    if (m_OldAuthPatch2Code) {
        if (!SDK()->PatchCode(
            "\x90\x90\x90\x90\x90\x90\x45\x84\xED\x0F\x85\x00\x00\x00\x00\x48\x8D\x55", "xxxxxxxxxxx????xxx",
            m_OldAuthPatch2Code, 6, 0
        ))
            Logger::Error("[PeacockLauncher] Failed to restore trusted domain check in ZOnlineManager::HttpRequest!");

        free(m_OldAuthPatch2Code);
        m_OldAuthPatch2Code = nullptr;
    }
}

#pragma endregion

#pragma region Hooks

DEFINE_PLUGIN_DETOUR(PeacockLauncher, ZString*, GetConfigHost, ZOnlineVersionConfig* th, ZString* out) {
    if (!m_AutoConnect) {
        return {HookAction::Continue()};
    }

    // Blocks the first config request until Peacock is up, otherwise the game falls back to offline mode
    WaitForPeacock();

    const auto s_OriginalUrl = p_Hook->CallOriginal(th, out);

    if (s_OriginalUrl->StartsWith("https://")) {
        *out = s_OriginalUrl->Replace("https://", "http://");
    }

    return {HookAction::Return(), out};
}

DEFINE_PLUGIN_DETOUR(PeacockLauncher, bool, Check_SSL_Cert, void* unk1, void* unk2) {
    if (!m_AutoConnect) {
        return {HookAction::Continue()};
    }

    return {HookAction::Return(), true};
}

#pragma endregion

#pragma region UI

void PeacockLauncher::SetStatus(const std::string& p_Status) {
    std::lock_guard s_Lock(m_StateMutex);
    m_Status = p_Status;
}

std::string PeacockLauncher::GetStatus() {
    std::lock_guard s_Lock(m_StateMutex);
    return m_Status;
}

void PeacockLauncher::OnDrawMenu() {
    if (ImGui::Button(ICON_MD_ROCKET_LAUNCH " PEACOCK LAUNCHER"))
        m_ShowSettings = !m_ShowSettings;
}

void PeacockLauncher::DrawSettingsWindow() {
    ImGui::SetNextWindowSize(ImVec2(520, 0), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(ICON_MD_ROCKET_LAUNCH " Peacock Launcher", &m_ShowSettings)) {
        ImGui::Text("Status: %s", GetStatus().c_str());
        ImGui::Text("Platform: %s", m_IsWine ? "Wine / Proton" : "Windows");

        ImGui::SeparatorText("Settings");

        char s_PeacockPath[512] {};
        strncpy_s(s_PeacockPath, m_PeacockPath.c_str(), sizeof(s_PeacockPath) - 1);
        ImGui::InputText("Peacock folder", s_PeacockPath, sizeof(s_PeacockPath));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Folder containing chunk0.js. Under Proton this can be a Linux path, e.g. /home/me/Peacock.");
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            {
                std::lock_guard s_Lock(m_StateMutex);
                m_PeacockPath = s_PeacockPath;
            }
            SaveSettings();
        }

        char s_NodePath[512] {};
        strncpy_s(s_NodePath, m_NodePath.c_str(), sizeof(s_NodePath) - 1);
        ImGui::InputText("Node path", s_NodePath, sizeof(s_NodePath));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Optional. Leave empty to use Peacock's bundled nodedist\\node.exe, node on PATH, or the newest nvm install.\n"
                "Under Proton, a path ending in .exe runs that node.exe through Wine instead of native node."
            );
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            m_NodePath = s_NodePath;
            SaveSettings();
        }

        int s_Port = static_cast<int>(m_Port);
        ImGui::InputInt("Port", &s_Port);
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            m_Port = std::clamp(s_Port, 1, 65535);
            SaveSettings();
        }

        int s_Timeout = static_cast<int>(m_StartupTimeoutSeconds);
        ImGui::InputInt("Startup timeout (s)", &s_Timeout);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("How long the game waits for Peacock to start before its first online request.");
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            m_StartupTimeoutSeconds = std::max(s_Timeout, 0);
            SaveSettings();
        }

        if (ImGui::Checkbox("Launch Peacock on game start", &m_AutoLaunch))
            SaveSettings();

        if (ImGui::Checkbox("Connect to Peacock on game start", &m_AutoConnect))
            SaveSettings();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Applies the OnlineTools settings needed for Peacock. Takes effect on next game start.\n"
                "Don't also set a default domain in OnlineTools."
            );

        const char* s_UpdateModes[] = {"Do nothing", "Alert me", "Update automatically"};
        int s_UpdateMode = static_cast<int>(m_UpdateMode);
        if (ImGui::Combo("New Peacock versions", &s_UpdateMode, s_UpdateModes, IM_ARRAYSIZE(s_UpdateModes))) {
            m_UpdateMode = static_cast<EPeacockUpdateMode>(s_UpdateMode);
            SaveSettings();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Checked against GitHub on game start. Automatic updates happen before Peacock launches.\n"
                "userdata, contractSessions, contracts, plugins, images, logs and options.ini are kept."
            );

        ImGui::SeparatorText("Actions");

        const bool s_IsBusy = m_IsBackgroundBusy;
        ImGui::BeginDisabled(s_IsBusy);

        if (ImGui::Button(ICON_MD_PLAY_ARROW " Launch"))
            RunInBackground([this] {
                LaunchPeacock();
                if (HasLaunchedPeacock()) WaitUntilListening(m_StartupTimeoutSeconds);
            });

        ImGui::SameLine();
        if (ImGui::Button(ICON_MD_STOP " Stop"))
            StopPeacock();

        ImGui::SameLine();
        if (ImGui::Button(ICON_MD_PUBLIC " Set domain now"))
            ConnectToPeacock();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Point the game at localhost:%d. You may need to go back to the title screen to reconnect.", (int)m_Port);

        if (ImGui::Button(ICON_MD_SYSTEM_UPDATE " Check for updates"))
            RunInBackground([this] { CheckForUpdate(); });

        ImGui::EndDisabled();

        std::string s_InstalledVersion;
        bool s_HasUpdate = false;
        {
            std::lock_guard s_Lock(m_StateMutex);
            s_InstalledVersion = m_InstalledVersion;
            s_HasUpdate = m_AvailableUpdate.has_value();
        }

        ImGui::SameLine();
        ImGui::Text(
            "Installed: %s%s", s_InstalledVersion.empty() ? "unknown" : s_InstalledVersion.c_str(),
            s_HasUpdate ? " (update available)" : ""
        );

        if (s_HasUpdate) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Show")) {
                std::lock_guard s_Lock(m_StateMutex);
                m_ShowUpdatePopup = true;
            }
        }
    }

    ImGui::End();
}

void PeacockLauncher::DrawUpdatePopup() {
    std::optional<PeacockUpdater::Release> s_Release;
    std::string s_InstalledVersion;
    {
        std::lock_guard s_Lock(m_StateMutex);
        if (!m_ShowUpdatePopup || !m_AvailableUpdate) return;
        s_Release = m_AvailableUpdate;
        s_InstalledVersion = m_InstalledVersion;
    }

    ImGui::PushFont(SDK()->GetImGuiBlackFont());
    const auto s_Expanded = ImGui::Begin(ICON_MD_SYSTEM_UPDATE " Peacock Update Available");
    ImGui::PushFont(SDK()->GetImGuiRegularFont());

    if (s_Expanded) {
        ImGui::Text("Peacock %s is available, you have %s.", s_Release->Version.c_str(), s_InstalledVersion.c_str());

        if (m_IsBackgroundBusy) {
            ImGui::Text("%s", GetStatus().c_str());
        }
        else {
            ImGui::BeginDisabled(s_Release->AssetUrl.empty());
            if (ImGui::Button(ICON_MD_DOWNLOAD " Update now"))
                RunInBackground([this] { InstallUpdate(); });
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip(
                    s_Release->AssetUrl.empty()
                        ? "This release has no zip for your Peacock build."
                        : "Peacock restarts if it is running. Best done from the main menu."
                );

            ImGui::SameLine();
            if (ImGui::Button(ICON_MD_OPEN_IN_NEW " Release page"))
                ShellExecuteA(nullptr, "open", s_Release->PageUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);

            ImGui::SameLine();
            if (ImGui::Button("Dismiss")) {
                std::lock_guard s_Lock(m_StateMutex);
                m_ShowUpdatePopup = false;
            }
        }
    }

    ImGui::PopFont();
    ImGui::End();
    ImGui::PopFont();
}

void PeacockLauncher::OnDrawUI(bool p_HasFocus) {
    DrawUpdatePopup();
    if (p_HasFocus && m_ShowSettings) DrawSettingsWindow();
}

#pragma endregion

DECLARE_ZHM_PLUGIN(PeacockLauncher);
