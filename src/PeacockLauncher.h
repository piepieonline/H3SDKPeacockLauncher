#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#define NOMINMAX
#include <WinSock2.h>

#include <IPluginInterface.h>

#include "PeacockUpdater.h"

enum class EPeacockUpdateMode {
    None,
    Alert,
    Auto,
};

class PeacockLauncher : public IPluginInterface {
public:
    void Init() override;
    void OnEngineInitialized() override;
    ~PeacockLauncher() override;
    void OnDrawMenu() override;
    void OnDrawUI(bool p_HasFocus) override;

private:
    void LoadSettings();
    void SaveSettings();

    std::wstring GetPeacockDir();
    bool IsPeacockListening() const;
    bool WaitUntilListening(int64_t p_TimeoutSeconds);
    void WaitForPeacock();
    void LaunchPeacock();
    bool LaunchPeacockWindows(const std::wstring& p_PeacockDir);
    bool LaunchPeacockWine(const std::wstring& p_PeacockDir);
    bool HasLaunchedPeacock();
    void StopPeacock();
    void ConnectToPeacock();

    void RunInBackground(std::function<void()> p_Task);
    void CheckForUpdate();
    void InstallUpdate();

    void PatchAuthHeaderChecks();
    void RestoreAuthHeaderChecks();

    void SetStatus(const std::string& p_Status);
    std::string GetStatus();
    void DrawSettingsWindow();
    void DrawUpdatePopup();

    DECLARE_PLUGIN_DETOUR(PeacockLauncher, ZString*, GetConfigHost, ZOnlineVersionConfig* th, ZString* out);
    DECLARE_PLUGIN_DETOUR(PeacockLauncher, bool, Check_SSL_Cert, void*, void*);

private:
    std::string m_PeacockPath;
    std::string m_NodePath;
    int64_t m_Port = 8123;
    int64_t m_StartupTimeoutSeconds = 30;
    bool m_AutoLaunch = true;
    bool m_AutoConnect = true;
    EPeacockUpdateMode m_UpdateMode = EPeacockUpdateMode::Alert;

    bool m_IsWine = false;
    bool m_HasWaitedForPeacock = false;
    std::atomic<bool> m_IsLaunchPending = false;

    std::recursive_mutex m_ProcessMutex;
    HANDLE m_PeacockProcess = nullptr;
    HANDLE m_PeacockJob = nullptr;
    std::wstring m_WinePeacockDir;

    std::mutex m_BackgroundMutex;
    std::thread m_BackgroundThread;
    std::atomic<bool> m_IsBackgroundBusy = false;

    void* m_OldAuthPatch1Code = nullptr;
    void* m_OldAuthPatch2Code = nullptr;

    std::mutex m_StateMutex;
    std::string m_Status = "Idle";
    std::string m_InstalledVersion;
    std::optional<PeacockUpdater::Release> m_AvailableUpdate;
    bool m_ShowUpdatePopup = false;

    bool m_ShowSettings = false;
};

DEFINE_ZHM_PLUGIN(PeacockLauncher)
