// SPDX-License-Identifier: GPL-3.0-or-later
// AppToMic - Copyright (C) 2026 Dalier Dek
// This program is free software: you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software Foundation,
// either version 3 of the License, or (at your option) any later version.
// It is distributed WITHOUT ANY WARRANTY; see the LICENSE file for details.
//
// AppToMic - send the audio of one specific app into a virtual microphone.
//
// How it works:
//   * Windows' per-process loopback capture (Windows 11 / build 20348+) grabs only
//     the audio produced by the chosen app (and its child processes).
//   * That audio is played into a playback device of your choice - normally
//     "CABLE Input (VB-Audio Virtual Cable)". Everything played there comes out of
//     the "CABLE Output" microphone, which you select in Discord / Zoom / OBS etc.
//   * The app keeps playing on your speakers as usual, so you still hear it.
//
// Build (MinGW):  see build.sh

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <tlhelp32.h>
#include <objbase.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <avrt.h>
#include <propidl.h>
#include <atomic>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <cmath>
#include <cwchar>

// ---------------------------------------------------------------------------
// Declarations MinGW doesn't ship (from audioclientactivationparams.h / mmdeviceapi.h)
// ---------------------------------------------------------------------------
#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif

enum ATM_ACTIVATION_TYPE { ATM_ACTIVATION_TYPE_DEFAULT = 0, ATM_ACTIVATION_TYPE_PROCESS_LOOPBACK = 1 };
enum ATM_LOOPBACK_MODE { ATM_LOOPBACK_INCLUDE_TREE = 0, ATM_LOOPBACK_EXCLUDE_TREE = 1 };
struct ATM_PROCESS_LOOPBACK_PARAMS { DWORD TargetProcessId; ATM_LOOPBACK_MODE ProcessLoopbackMode; };
struct ATM_ACTIVATION_PARAMS { ATM_ACTIVATION_TYPE ActivationType; ATM_PROCESS_LOOPBACK_PARAMS ProcessLoopbackParams; };

struct IActivateOp : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetActivateResult(HRESULT* activateResult, IUnknown** activatedInterface) = 0;
};
struct IActivateCompletion : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateOp* op) = 0;
};
typedef HRESULT(WINAPI* PFN_ActivateAudioInterfaceAsync)(LPCWSTR, REFIID, PROPVARIANT*, IActivateCompletion*, IActivateOp**);

static const GUID kCLSID_MMDeviceEnumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const GUID kIID_IMMDeviceEnumerator  = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const GUID kIID_IAudioClient         = {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
static const GUID kIID_IAudioRenderClient   = {0xF294ACFC, 0x3146, 0x4483, {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};
static const GUID kIID_IAudioCaptureClient  = {0xC8ADBD64, 0xE71E, 0x48A0, {0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17}};
static const GUID kIID_IAudioSessionManager2= {0x77AA99A0, 0x1BD6, 0x484F, {0x8B, 0xC7, 0x2C, 0x65, 0x4C, 0x9A, 0x9B, 0x6F}};
static const GUID kIID_IAudioSessionControl2= {0xBFB7FF88, 0x7239, 0x4FC9, {0x8F, 0xA2, 0x07, 0xC9, 0x50, 0xBE, 0x9C, 0x6D}};
static const GUID kIID_IActivateCompletion  = {0x41D949AB, 0x9862, 0x444A, {0x80, 0xF6, 0xC2, 0x61, 0x33, 0x4D, 0xA5, 0xEB}};
static const GUID kIID_IAgileObject         = {0x94EA2B94, 0xE9CC, 0x49E0, {0xC0, 0xFF, 0xEE, 0x64, 0xCA, 0x8F, 0x5B, 0x90}};
static const PROPERTYKEY kPKEY_FriendlyName = {{0xA45C254E, 0xDF1C, 0x4EFD, {0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0}}, 14};
static const GUID kKSDATAFORMAT_SUBTYPE_IEEE_FLOAT = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
static const GUID kKSDATAFORMAT_SUBTYPE_PCM        = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

template <class T> static void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

// ---------------------------------------------------------------------------
// Completion handler for ActivateAudioInterfaceAsync
// ---------------------------------------------------------------------------
class ActivateHandler final : public IActivateCompletion {
    LONG ref_ = 1;
public:
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HRESULT result = E_FAIL;
    IAudioClient* client = nullptr;
    ~ActivateHandler() { CloseHandle(done); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, kIID_IActivateCompletion) || IsEqualIID(riid, kIID_IAgileObject)) {
            *ppv = static_cast<IActivateCompletion*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref_); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&ref_);
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateOp* op) override {
        HRESULT activateHr = E_FAIL;
        IUnknown* unk = nullptr;
        HRESULT hr = op->GetActivateResult(&activateHr, &unk);
        if (SUCCEEDED(hr) && SUCCEEDED(activateHr) && unk) {
            hr = unk->QueryInterface(kIID_IAudioClient, (void**)&client);
            result = hr;
        } else {
            result = FAILED(hr) ? hr : activateHr;
        }
        if (unk) unk->Release();
        SetEvent(done);
        return S_OK;
    }
};

// ---------------------------------------------------------------------------
// App state
// ---------------------------------------------------------------------------
enum {
    IDC_LIST = 101, IDC_REFRESH, IDC_SHOWALL, IDC_DEVICE, IDC_VOLUME, IDC_VOLTEXT,
    IDC_START, IDC_LEVEL, IDC_STATUS, IDC_HINT, IDC_LBL1, IDC_LBL2, IDC_LBLVOL
};
#define WM_APP_STATUS (WM_APP + 1)   // lParam = new std::wstring*, wParam = 1 if error
#define WM_APP_STOPPED (WM_APP + 2)
#define TIMER_LEVEL 1

struct AppEntry { DWORD pid; std::wstring exe; std::wstring title; bool playing; bool hasSession; };
struct DeviceEntry { std::wstring id; std::wstring name; };

static HWND g_hwnd, g_list, g_refresh, g_showAll, g_device, g_volume, g_volText, g_start, g_level, g_status;
static HFONT g_font, g_boldFont;
static std::vector<AppEntry> g_apps;
static std::vector<DeviceEntry> g_devices;
static HANDLE g_thread = nullptr;
static HANDLE g_stopEvent = nullptr;
static std::atomic<float> g_gain{1.0f};
static std::atomic<float> g_peak{0.0f};
static std::atomic<bool> g_running{false};
static float g_meter = 0.0f;
static int g_dpi = 96;
static std::wstring g_lastExe, g_lastDevice;

struct WorkerArgs { DWORD pid; std::wstring deviceId; std::wstring appName; };

static int S(int v) { return MulDiv(v, g_dpi, 96); }

static void PostStatus(const std::wstring& text, bool error = false) {
    PostMessageW(g_hwnd, WM_APP_STATUS, error ? 1 : 0, (LPARAM) new std::wstring(text));
}

static std::wstring HrText(HRESULT hr) {
    wchar_t buf[32];
    swprintf(buf, 32, L"0x%08lX", (unsigned long)hr);
    return buf;
}

static DWORD WindowsBuild() {
    typedef LONG(WINAPI * RtlGetVersionFn)(OSVERSIONINFOW*);
    OSVERSIONINFOW vi = {sizeof(vi)};
    auto fn = (RtlGetVersionFn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    if (fn && fn(&vi) == 0) return vi.dwBuildNumber;
    return 0;
}

// ---------------------------------------------------------------------------
// Settings (HKCU\Software\AppToMic)
// ---------------------------------------------------------------------------
static std::wstring RegGet(const wchar_t* name) {
    wchar_t buf[1024];
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\AppToMic", name, RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS)
        return buf;
    return L"";
}
static void RegSet(const wchar_t* name, const std::wstring& v) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\AppToMic", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(key, name, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
    }
}

// ---------------------------------------------------------------------------
// Enumeration: playback devices, processes, audio sessions
// ---------------------------------------------------------------------------
static void LoadDevices() {
    g_devices.clear();
    IMMDeviceEnumerator* en = nullptr;
    IMMDeviceCollection* col = nullptr;
    if (FAILED(CoCreateInstance(kCLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, kIID_IMMDeviceEnumerator, (void**)&en))) return;
    if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col))) {
        UINT count = 0;
        col->GetCount(&count);
        for (UINT i = 0; i < count; i++) {
            IMMDevice* dev = nullptr;
            if (FAILED(col->Item(i, &dev))) continue;
            LPWSTR id = nullptr;
            IPropertyStore* props = nullptr;
            DeviceEntry e;
            if (SUCCEEDED(dev->GetId(&id))) { e.id = id; CoTaskMemFree(id); }
            if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
                PROPVARIANT pv;
                PropVariantInit(&pv);
                if (SUCCEEDED(props->GetValue(kPKEY_FriendlyName, &pv)) && pv.vt == VT_LPWSTR) e.name = pv.pwszVal;
                PropVariantClear(&pv);
                props->Release();
            }
            if (!e.id.empty()) g_devices.push_back(e);
            dev->Release();
        }
        col->Release();
    }
    en->Release();
}

static void FillDeviceCombo() {
    LoadDevices();
    SendMessageW(g_device, CB_RESETCONTENT, 0, 0);
    int pick = -1, cable = -1;
    for (size_t i = 0; i < g_devices.size(); i++) {
        SendMessageW(g_device, CB_ADDSTRING, 0, (LPARAM)g_devices[i].name.c_str());
        if (!g_lastDevice.empty() && g_devices[i].name == g_lastDevice) pick = (int)i;
        std::wstring low = g_devices[i].name;
        std::transform(low.begin(), low.end(), low.begin(), ::towlower);
        if (cable < 0 && low.find(L"cable input") != std::wstring::npos) cable = (int)i;
    }
    if (pick < 0) pick = cable;
    if (pick < 0 && !g_devices.empty()) pick = 0;
    SendMessageW(g_device, CB_SETCURSEL, pick, 0);
}

struct ProcInfo { DWORD parent; std::wstring exe; };

static std::map<DWORD, ProcInfo> SnapshotProcesses() {
    std::map<DWORD, ProcInfo> m;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return m;
    PROCESSENTRY32W pe = {sizeof(pe)};
    if (Process32FirstW(snap, &pe)) {
        do { m[pe.th32ProcessID] = {pe.th32ParentProcessID, pe.szExeFile}; } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return m;
}

// Walk up to the top-most ancestor with the same exe name (e.g. the main chrome.exe),
// so capturing its process tree catches audio from helper/child processes.
static DWORD RootOf(DWORD pid, const std::map<DWORD, ProcInfo>& procs) {
    std::set<DWORD> seen;
    DWORD cur = pid;
    while (true) {
        auto it = procs.find(cur);
        if (it == procs.end()) break;
        auto par = procs.find(it->second.parent);
        if (par == procs.end() || seen.count(par->first) || _wcsicmp(par->second.exe.c_str(), it->second.exe.c_str()) != 0) break;
        seen.insert(cur);
        cur = par->first;
    }
    return cur;
}

static std::map<DWORD, std::wstring> g_titles;
static BOOL CALLBACK EnumWinProc(HWND w, LPARAM) {
    if (!IsWindowVisible(w) || GetWindow(w, GW_OWNER)) return TRUE;
    wchar_t title[256];
    if (GetWindowTextW(w, title, 256) <= 0) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (!g_titles.count(pid)) g_titles[pid] = title;
    return TRUE;
}

static void RefreshApps() {
    DWORD selectedPid = 0;
    int sel = (int)SendMessageW(g_list, LB_GETCURSEL, 0, 0);
    if (sel >= 0 && sel < (int)g_apps.size()) selectedPid = g_apps[sel].pid;

    auto procs = SnapshotProcesses();
    g_titles.clear();
    EnumWindows(EnumWinProc, 0);

    std::map<DWORD, AppEntry> found;  // root pid -> entry
    DWORD self = GetCurrentProcessId();

    // 1) Apps that have an audio session on any playback device
    IMMDeviceEnumerator* en = nullptr;
    IMMDeviceCollection* col = nullptr;
    if (SUCCEEDED(CoCreateInstance(kCLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, kIID_IMMDeviceEnumerator, (void**)&en)) &&
        SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col))) {
        UINT count = 0;
        col->GetCount(&count);
        for (UINT i = 0; i < count; i++) {
            IMMDevice* dev = nullptr;
            IAudioSessionManager2* mgr = nullptr;
            IAudioSessionEnumerator* se = nullptr;
            if (FAILED(col->Item(i, &dev))) continue;
            if (SUCCEEDED(dev->Activate(kIID_IAudioSessionManager2, CLSCTX_ALL, nullptr, (void**)&mgr)) &&
                SUCCEEDED(mgr->GetSessionEnumerator(&se))) {
                int n = 0;
                se->GetCount(&n);
                for (int k = 0; k < n; k++) {
                    IAudioSessionControl* ctl = nullptr;
                    IAudioSessionControl2* ctl2 = nullptr;
                    if (FAILED(se->GetSession(k, &ctl))) continue;
                    if (SUCCEEDED(ctl->QueryInterface(kIID_IAudioSessionControl2, (void**)&ctl2))) {
                        DWORD pid = 0;
                        AudioSessionState st = AudioSessionStateInactive;
                        ctl2->GetProcessId(&pid);
                        ctl2->GetState(&st);
                        if (pid != 0 && pid != self && st != AudioSessionStateExpired && procs.count(pid)) {
                            DWORD root = RootOf(pid, procs);
                            AppEntry& e = found[root];
                            e.pid = root;
                            e.exe = procs[root].exe;
                            e.hasSession = true;
                            if (st == AudioSessionStateActive) e.playing = true;
                        }
                        ctl2->Release();
                    }
                    ctl->Release();
                }
            }
            SafeRelease(se);
            SafeRelease(mgr);
            dev->Release();
        }
    }
    SafeRelease(col);
    SafeRelease(en);

    // 2) Optionally every app that has a visible window
    if (SendMessageW(g_showAll, BM_GETCHECK, 0, 0) == BST_CHECKED) {
        for (auto& t : g_titles) {
            if (t.first == self || !procs.count(t.first)) continue;
            DWORD root = RootOf(t.first, procs);
            if (root == self) continue;
            if (_wcsicmp(procs[root].exe.c_str(), L"explorer.exe") == 0 && !found.count(root)) continue;
            AppEntry& e = found[root];
            e.pid = root;
            e.exe = procs[root].exe;
        }
    }

    // Window titles: prefer the root's window, else any window in the same exe family
    for (auto& f : found) {
        if (g_titles.count(f.first)) { f.second.title = g_titles[f.first]; continue; }
        for (auto& t : g_titles) {
            if (procs.count(t.first) && RootOf(t.first, procs) == f.first) { f.second.title = t.second; break; }
        }
    }

    g_apps.clear();
    for (auto& f : found) g_apps.push_back(f.second);
    std::sort(g_apps.begin(), g_apps.end(), [](const AppEntry& a, const AppEntry& b) {
        if (a.playing != b.playing) return a.playing;
        if (a.hasSession != b.hasSession) return a.hasSession;
        return _wcsicmp(a.exe.c_str(), b.exe.c_str()) < 0;
    });

    SendMessageW(g_list, WM_SETREDRAW, FALSE, 0);
    SendMessageW(g_list, LB_RESETCONTENT, 0, 0);
    int newSel = -1;
    for (size_t i = 0; i < g_apps.size(); i++) {
        const AppEntry& a = g_apps[i];
        std::wstring line = a.playing ? L"♪  " : L"     ";
        line += a.exe;
        if (!a.title.empty()) {
            std::wstring t = a.title.size() > 60 ? a.title.substr(0, 57) + L"..." : a.title;
            line += L"  —  " + t;
        }
        line += L"   (PID " + std::to_wstring(a.pid) + L")";
        SendMessageW(g_list, LB_ADDSTRING, 0, (LPARAM)line.c_str());
        if (a.pid == selectedPid) newSel = (int)i;
        if (newSel < 0 && selectedPid == 0 && !g_lastExe.empty() && _wcsicmp(a.exe.c_str(), g_lastExe.c_str()) == 0) newSel = (int)i;
    }
    SendMessageW(g_list, LB_SETCURSEL, newSel, 0);
    SendMessageW(g_list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_list, nullptr, TRUE);

    if (!g_running && g_apps.empty())
        SetWindowTextW(g_status, L"No app is using audio right now. Start playing something in the app, then press Refresh "
                                 L"(or tick \"Show all apps with a window\").");
}

// ---------------------------------------------------------------------------
// Audio worker: process-loopback capture  ->  playback device (CABLE Input)
// ---------------------------------------------------------------------------
static WAVEFORMATEXTENSIBLE MakeFormat(bool useFloat) {
    WAVEFORMATEXTENSIBLE f = {};
    f.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    f.Format.nChannels = 2;
    f.Format.nSamplesPerSec = 48000;
    f.Format.wBitsPerSample = useFloat ? 32 : 16;
    f.Format.nBlockAlign = f.Format.nChannels * f.Format.wBitsPerSample / 8;
    f.Format.nAvgBytesPerSec = f.Format.nSamplesPerSec * f.Format.nBlockAlign;
    f.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    f.Samples.wValidBitsPerSample = f.Format.wBitsPerSample;
    f.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    f.SubFormat = useFloat ? kKSDATAFORMAT_SUBTYPE_IEEE_FLOAT : kKSDATAFORMAT_SUBTYPE_PCM;
    return f;
}

static HRESULT ActivateProcessLoopback(DWORD pid, IAudioClient** out) {
    HMODULE mod = LoadLibraryW(L"Mmdevapi.dll");
    auto activate = mod ? (PFN_ActivateAudioInterfaceAsync)GetProcAddress(mod, "ActivateAudioInterfaceAsync") : nullptr;
    if (!activate) return E_NOTIMPL;

    ATM_ACTIVATION_PARAMS params = {};
    params.ActivationType = ATM_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = pid;
    params.ProcessLoopbackParams.ProcessLoopbackMode = ATM_LOOPBACK_INCLUDE_TREE;

    PROPVARIANT pv;
    PropVariantInit(&pv);
    pv.vt = VT_BLOB;
    pv.blob.cbSize = sizeof(params);
    pv.blob.pBlobData = (BYTE*)&params;

    ActivateHandler* handler = new ActivateHandler();
    IActivateOp* op = nullptr;
    HRESULT hr = activate(L"VAD\\Process_Loopback", kIID_IAudioClient, &pv, handler, &op);
    if (SUCCEEDED(hr)) {
        if (WaitForSingleObject(handler->done, 5000) != WAIT_OBJECT_0) hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        else if (FAILED(handler->result)) hr = handler->result;
        else { *out = handler->client; handler->client = nullptr; }
    }
    SafeRelease(op);
    handler->Release();
    return hr;
}

static DWORD WINAPI AudioThread(LPVOID param) {
    WorkerArgs* args = (WorkerArgs*)param;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    IMMDeviceEnumerator* en = nullptr;
    IMMDevice* outDev = nullptr;
    IAudioClient* capClient = nullptr;
    IAudioClient* outClient = nullptr;
    IAudioCaptureClient* cap = nullptr;
    IAudioRenderClient* rend = nullptr;
    HANDLE capEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    UINT32 outFrames = 0;
    bool capFloat = true;
    std::wstring err;
    HRESULT hr;

    // --- capture side (only the chosen app) ---
    hr = ActivateProcessLoopback(args->pid, &capClient);
    if (FAILED(hr)) {
        err = L"Couldn't hook into that app's audio (" + HrText(hr) + L"). Per-app capture needs Windows 11 or newer.";
        goto done;
    }
    {
        WAVEFORMATEXTENSIBLE f = MakeFormat(true);
        DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM;
        hr = capClient->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 200000, 0, (WAVEFORMATEX*)&f, nullptr);
        if (FAILED(hr)) {
            // Fall back to 16-bit PCM (what Microsoft's own sample uses). Needs a fresh client.
            SafeRelease(capClient);
            hr = ActivateProcessLoopback(args->pid, &capClient);
            if (SUCCEEDED(hr)) {
                capFloat = false;
                f = MakeFormat(false);
                hr = capClient->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 200000, 0, (WAVEFORMATEX*)&f, nullptr);
            }
        }
        if (FAILED(hr)) { err = L"Couldn't start capturing the app (" + HrText(hr) + L")."; goto done; }
    }
    if (FAILED(hr = capClient->SetEventHandle(capEvent)) ||
        FAILED(hr = capClient->GetService(kIID_IAudioCaptureClient, (void**)&cap))) {
        err = L"Capture setup failed (" + HrText(hr) + L").";
        goto done;
    }

    // --- output side (CABLE Input) ---
    if (FAILED(hr = CoCreateInstance(kCLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, kIID_IMMDeviceEnumerator, (void**)&en)) ||
        FAILED(hr = en->GetDevice(args->deviceId.c_str(), &outDev)) ||
        FAILED(hr = outDev->Activate(kIID_IAudioClient, CLSCTX_ALL, nullptr, (void**)&outClient))) {
        err = L"Couldn't open the output device (" + HrText(hr) + L"). Is it still connected?";
        goto done;
    }
    {
        WAVEFORMATEXTENSIBLE f = MakeFormat(true);
        DWORD flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        if (FAILED(hr = outClient->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 1000000, 0, (WAVEFORMATEX*)&f, nullptr)) ||
            FAILED(hr = outClient->GetBufferSize(&outFrames)) ||
            FAILED(hr = outClient->GetService(kIID_IAudioRenderClient, (void**)&rend))) {
            err = L"Couldn't start the output device (" + HrText(hr) + L").";
            goto done;
        }
    }

    if (FAILED(hr = outClient->Start()) || FAILED(hr = capClient->Start())) {
        err = L"Couldn't start streaming (" + HrText(hr) + L").";
        goto done;
    }

    PostStatus(L"Running: " + args->appName + L" → microphone. Select \"CABLE Output\" as the mic in your other app.");

    {
        const UINT32 maxQueued = 48000 * 80 / 1000;  // keep latency under ~80 ms
        std::vector<float> buf;
        HANDLE waits[2] = {capEvent, g_stopEvent};
        bool stop = false;
        while (!stop) {
            DWORD w = WaitForMultipleObjects(2, waits, FALSE, 200);
            if (w == WAIT_OBJECT_0 + 1) break;

            UINT32 packet = 0;
            while (SUCCEEDED(hr = cap->GetNextPacketSize(&packet)) && packet > 0) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                if (FAILED(hr = cap->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;

                float gain = g_gain.load();
                buf.resize((size_t)frames * 2);
                float peak = 0.0f;
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    std::fill(buf.begin(), buf.end(), 0.0f);
                } else if (capFloat) {
                    const float* src = (const float*)data;
                    for (size_t i = 0; i < buf.size(); i++) {
                        float v = src[i] * gain;
                        buf[i] = v;
                        float a = std::fabs(v);
                        if (a > peak) peak = a;
                    }
                } else {
                    const int16_t* src = (const int16_t*)data;
                    for (size_t i = 0; i < buf.size(); i++) {
                        float v = (src[i] / 32768.0f) * gain;
                        buf[i] = v;
                        float a = std::fabs(v);
                        if (a > peak) peak = a;
                    }
                }
                cap->ReleaseBuffer(frames);
                if (peak > g_peak.load()) g_peak.store(peak);

                UINT32 padding = 0;
                if (FAILED(hr = outClient->GetCurrentPadding(&padding))) { stop = true; break; }
                if (padding > maxQueued) continue;  // drift guard: drop this packet
                UINT32 n = std::min(frames, outFrames - padding);
                if (n == 0) continue;
                BYTE* out = nullptr;
                if (FAILED(hr = rend->GetBuffer(n, &out))) { stop = true; break; }
                memcpy(out, buf.data(), (size_t)n * 2 * sizeof(float));
                rend->ReleaseBuffer(n, 0);
            }
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
                err = L"The output device was disconnected or changed. Press Start again.";
                break;
            }
        }
    }
    capClient->Stop();
    outClient->Stop();

done:
    SafeRelease(rend);
    SafeRelease(cap);
    SafeRelease(outClient);
    SafeRelease(capClient);
    SafeRelease(outDev);
    SafeRelease(en);
    CloseHandle(capEvent);
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    CoUninitialize();
    if (!err.empty()) PostStatus(err, true);
    PostMessageW(g_hwnd, WM_APP_STOPPED, err.empty() ? 0 : 1, 0);
    delete args;
    return 0;
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------
static void SetRunningUi(bool running) {
    g_running = running;
    EnableWindow(g_list, !running);
    EnableWindow(g_refresh, !running);
    EnableWindow(g_showAll, !running);
    EnableWindow(g_device, !running);
    SetWindowTextW(g_start, running ? L"Stop" : L"Start");
}

static void StartCapture() {
    int sel = (int)SendMessageW(g_list, LB_GETCURSEL, 0, 0);
    int dsel = (int)SendMessageW(g_device, CB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= (int)g_apps.size()) { SetWindowTextW(g_status, L"Pick the app you want to capture first."); return; }
    if (dsel < 0 || dsel >= (int)g_devices.size()) { SetWindowTextW(g_status, L"Pick where to send the audio (CABLE Input)."); return; }

    const AppEntry& app = g_apps[sel];
    const DeviceEntry& dev = g_devices[dsel];
    std::wstring low = dev.name;
    std::transform(low.begin(), low.end(), low.begin(), ::towlower);
    if (low.find(L"cable") == std::wstring::npos &&
        MessageBoxW(g_hwnd, L"The selected output isn't the VB-Audio virtual cable, so the audio won't reach a microphone "
                            L"(you'd just hear it twice).\n\nStart anyway?", L"AppToMic", MB_YESNO | MB_ICONWARNING) != IDYES)
        return;

    g_lastExe = app.exe;
    g_lastDevice = dev.name;
    RegSet(L"LastApp", g_lastExe);
    RegSet(L"LastDevice", g_lastDevice);

    ResetEvent(g_stopEvent);
    WorkerArgs* args = new WorkerArgs{app.pid, dev.id, app.exe};
    SetWindowTextW(g_status, L"Starting...");
    SetRunningUi(true);
    g_thread = CreateThread(nullptr, 0, AudioThread, args, 0, nullptr);
    if (!g_thread) { delete args; SetRunningUi(false); SetWindowTextW(g_status, L"Couldn't start the audio thread."); }
}

static void StopCapture() {
    if (g_thread) {
        SetEvent(g_stopEvent);
        WaitForSingleObject(g_thread, 3000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
}

static void UpdateVolumeText() {
    int v = (int)SendMessageW(g_volume, TBM_GETPOS, 0, 0);
    g_gain.store(v / 100.0f);
    std::wstring t = std::to_wstring(v) + L"%";
    SetWindowTextW(g_volText, t.c_str());
}

static HWND MakeCtl(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, DWORD ex = 0) {
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), g_hwnd,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    return c;
}

static void CreateUi() {
    NONCLIENTMETRICSW ncm = {sizeof(ncm)};
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    ncm.lfMessageFont.lfHeight = -MulDiv(9, g_dpi, 72);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    ncm.lfMessageFont.lfWeight = FW_SEMIBOLD;
    g_boldFont = CreateFontIndirectW(&ncm.lfMessageFont);

    HWND l1 = MakeCtl(L"STATIC", L"1.  App to capture", 0, 12, 14, 300, 18, IDC_LBL1);
    SendMessageW(l1, WM_SETFONT, (WPARAM)g_boldFont, TRUE);
    g_refresh = MakeCtl(L"BUTTON", L"Refresh", BS_PUSHBUTTON | WS_TABSTOP, 368, 9, 80, 26, IDC_REFRESH);
    g_list = MakeCtl(L"LISTBOX", L"", LBS_NOTIFY | WS_VSCROLL | WS_TABSTOP | LBS_NOINTEGRALHEIGHT, 12, 40, 436, 170, IDC_LIST, WS_EX_CLIENTEDGE);
    g_showAll = MakeCtl(L"BUTTON", L"Show all apps with a window (not only ones that have played sound)", BS_AUTOCHECKBOX | WS_TABSTOP, 12, 214, 436, 22, IDC_SHOWALL);

    HWND l2 = MakeCtl(L"STATIC", L"2.  Send its audio to", 0, 12, 248, 300, 18, IDC_LBL2);
    SendMessageW(l2, WM_SETFONT, (WPARAM)g_boldFont, TRUE);
    g_device = MakeCtl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 12, 270, 436, 300, IDC_DEVICE);
    MakeCtl(L"STATIC", L"Pick \"CABLE Input\". In Discord, Zoom, OBS etc. choose \"CABLE Output\" as the microphone.", 0, 12, 300, 436, 32, IDC_HINT);

    MakeCtl(L"STATIC", L"Volume", 0, 12, 342, 56, 20, IDC_LBLVOL);
    g_volume = MakeCtl(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, 66, 338, 320, 28, IDC_VOLUME);
    SendMessageW(g_volume, TBM_SETRANGE, TRUE, MAKELPARAM(0, 200));
    SendMessageW(g_volume, TBM_SETPAGESIZE, 0, 10);
    g_volText = MakeCtl(L"STATIC", L"100%", SS_RIGHT, 392, 342, 56, 20, IDC_VOLTEXT);

    g_start = MakeCtl(L"BUTTON", L"Start", BS_DEFPUSHBUTTON | WS_TABSTOP, 12, 378, 130, 34, IDC_START);
    SendMessageW(g_start, WM_SETFONT, (WPARAM)g_boldFont, TRUE);
    g_level = MakeCtl(PROGRESS_CLASSW, L"", PBS_SMOOTH, 156, 388, 292, 14, IDC_LEVEL);
    SendMessageW(g_level, PBM_SETRANGE32, 0, 1000);

    g_status = MakeCtl(L"STATIC", L"Choose an app and press Start.", SS_LEFT, 12, 424, 436, 40, IDC_STATUS);

    int vol = 100;
    std::wstring savedVol = RegGet(L"Volume");
    if (!savedVol.empty()) vol = std::max(0, std::min(200, _wtoi(savedVol.c_str())));
    SendMessageW(g_volume, TBM_SETPOS, TRUE, vol);
    UpdateVolumeText();
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g_hwnd = hwnd;
        CreateUi();
        FillDeviceCombo();
        RefreshApps();
        SetTimer(hwnd, TIMER_LEVEL, 50, nullptr);
        if (WindowsBuild() && WindowsBuild() < 20348)
            SetWindowTextW(g_status, L"Warning: this Windows version is too old for per-app capture. You need Windows 11.");
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_REFRESH: RefreshApps(); FillDeviceCombo(); return 0;
        case IDC_SHOWALL: RefreshApps(); return 0;
        case IDC_START:
            if (g_running) { StopCapture(); } else { StartCapture(); }
            return 0;
        case IDC_LIST:
            if (HIWORD(wp) == LBN_DBLCLK && !g_running) StartCapture();
            return 0;
        }
        break;
    case WM_HSCROLL:
        if ((HWND)lp == g_volume) {
            UpdateVolumeText();
            RegSet(L"Volume", std::to_wstring((int)SendMessageW(g_volume, TBM_GETPOS, 0, 0)));
        }
        return 0;
    case WM_TIMER:
        if (wp == TIMER_LEVEL) {
            float p = g_peak.exchange(0.0f);
            g_meter = std::max(p, g_meter * 0.85f);
            if (!g_running) g_meter = 0.0f;
            int pos = (int)(std::min(1.0f, g_meter) * 1000);
            SendMessageW(g_level, PBM_SETPOS, pos, 0);
        }
        return 0;
    case WM_APP_STATUS: {
        std::wstring* s = (std::wstring*)lp;
        SetWindowTextW(g_status, s->c_str());
        delete s;
        return 0;
    }
    case WM_APP_STOPPED:
        if (g_thread) { WaitForSingleObject(g_thread, 1000); CloseHandle(g_thread); g_thread = nullptr; }
        SetRunningUi(false);
        if (wp == 0) SetWindowTextW(g_status, L"Stopped.");
        return 0;
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        if ((HWND)lp == g_status && g_status) {
            wchar_t t[16];
            GetWindowTextW(g_status, t, 16);
            if (wcsncmp(t, L"Running", 7) == 0) SetTextColor(dc, RGB(0, 120, 60));
        }
        SetBkMode(dc, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
    case WM_DESTROY:
        StopCapture();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    SetProcessDPIAware();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES | ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&icc);

    HDC screen = GetDC(nullptr);
    g_dpi = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(nullptr, screen);

    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_lastExe = RegGet(L"LastApp");
    g_lastDevice = RegGet(L"LastDevice");

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.lpszClassName = L"AppToMicWnd";
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r = {0, 0, S(460), S(472)};
    AdjustWindowRect(&r, style, FALSE);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"AppToMic — send an app's audio to a microphone", style,
                                CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        if (!IsDialogMessageW(hwnd, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    CoUninitialize();
    return 0;
}
