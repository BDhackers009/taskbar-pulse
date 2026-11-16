// Taskbar Pulse - CPU, GPU, memory and network at a glance, living in the Windows 11 taskbar.
//
// Design notes
//  * One small layered top-level window, re-positioned against the taskbar's
//    notification area. It never activates and never takes focus.
//  * Rendering is Direct2D + DirectWrite into a 32-bpp DIB, pushed with
//    UpdateLayeredWindow (per-pixel alpha). ~1 paint per second.
//  * Metrics are sampled on a worker thread so a slow performance counter can
//    never stall the UI. GPU uses the WDDM "GPU Engine" PDH counters, the same
//    data Task Manager is built on.
//  * No installer, no runtime, no telemetry, no admin rights.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2ipdef.h>
#include <windows.h>
#include <windowsx.h>
#include <process.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <psapi.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <dxgi1_2.h>
#include <dwmapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <vector>

#include "resource.h"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "iphlpapi.lib")

using Microsoft::WRL::ComPtr;

// Small helpers

static constexpr wchar_t kAppName[] = L"Taskbar Pulse";
static constexpr wchar_t kVersion[] = L"1.1";
static constexpr wchar_t kWndClass[] = L"TaskbarPulse.Widget";

static const UINT WM_APP_SAMPLE = WM_APP + 10;
static const UINT WM_APP_TRAY = WM_APP + 11;
static const UINT WM_APP_GEOMETRY = WM_APP + 12;

enum : UINT {
  IDM_CPU = 1000, IDM_GPU, IDM_RAM, IDM_NET, IDM_BARS,
  IDM_SPEED = 1010,
  IDM_AUTOSTART = 1020, IDM_EXIT,
  IDM_SETTINGS = 1030, IDM_LOCK,
};

static bool g_debug = false;
static void Log(const wchar_t* fmt, ...) {
  if (!g_debug) return;
  wchar_t dir[MAX_PATH]{};
  DWORD n = GetTempPathW(MAX_PATH, dir);
  if (!n) return;
  wchar_t path[MAX_PATH];
  swprintf_s(path, L"%sTaskbarPulse.log", dir);
  HANDLE file = CreateFileW(path, FILE_APPEND_DATA,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return;
  wchar_t buffer[1024];
  va_list ap;
  va_start(ap, fmt);
  _vsnwprintf_s(buffer, _TRUNCATE, fmt, ap);
  va_end(ap);
  DWORD written = 0;
  WriteFile(file, buffer, (DWORD)(wcslen(buffer) * sizeof(wchar_t)), &written, nullptr);
  WriteFile(file, L"\r\n", 4, &written, nullptr);
  CloseHandle(file);
}

static float ClampF(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int Rounded(float v) { return (int)std::lround(v); }

// Bytes per second in the taskbar's four-character budget: 834B, 12.4K, 123M.
static void FormatRate(float bps, wchar_t* out, size_t cap) {
  if (bps < 0.0f) {
    wcscpy_s(out, cap, L"\x2014");
    return;
  }
  static const wchar_t kUnits[3] = {L'K', L'M', L'G'};
  double v = bps;
  int unit = -1;
  if (v >= 1024.0) {
    v /= 1024.0;
    unit = 0;
  }
  while (v >= 1024.0 && unit < 2) {
    v /= 1024.0;
    ++unit;
  }
  // Round into the next unit rather than printing 1024K.
  if (v >= 1023.5 && unit < 2) {
    v /= 1024.0;
    ++unit;
  }
  if (unit < 0)
    swprintf_s(out, cap, L"%.0fB", v);
  else if (v < 10.0)
    swprintf_s(out, cap, L"%.1f%lc", v, kUnits[unit]);
  else
    swprintf_s(out, cap, L"%.0f%lc", v, kUnits[unit]);
}

static void LogMem(const wchar_t* stage) {
  if (!g_debug) return;
  PROCESS_MEMORY_COUNTERS_EX pmc{};
  if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc)))
    Log(L"mem %-22ls ws=%llu KB private=%llu KB", stage, pmc.WorkingSetSize / 1024,
        pmc.PrivateUsage / 1024);
}

static D2D1_COLOR_F Rgba(float r, float g, float b, float a) {
  return D2D1::ColorF(r, g, b, a);
}

// Settings

struct Settings {
  bool showCpu = true;
  bool showGpu = true;
  bool showRam = true;
  bool showNet = true;
  bool showBars = true;
  bool showTemps = false;
  bool borderless = false;
  bool locked = false;
  int intervalMs = 1000;
  int offset = 0;  // extra logical pixels to the left of the tray
  int warnPct[3] = {70, 70, 70};
  int critPct[3] = {90, 90, 90};
  unsigned long long gpuLuid = 0;  // 0 = automatic (busiest adapter)
};

struct Theme {
  bool light = false;
  D2D1_COLOR_F text = Rgba(1, 1, 1, 0.95f);
  D2D1_COLOR_F label = Rgba(1, 1, 1, 0.56f);
  D2D1_COLOR_F track = Rgba(1, 1, 1, 0.13f);
  D2D1_COLOR_F bg = Rgba(1, 1, 1, 0.055f);
  D2D1_COLOR_F border = Rgba(1, 1, 1, 0.085f);
  D2D1_COLOR_F accent = Rgba(0.0f, 0.47f, 0.83f, 1.0f);
  D2D1_COLOR_F warn = Rgba(0.91f, 0.64f, 0.22f, 1.0f);
  D2D1_COLOR_F hot = Rgba(0.90f, 0.32f, 0.30f, 1.0f);
  D2D1_COLOR_F menuBg = Rgba(0.115f, 0.115f, 0.125f, 0.98f);
  D2D1_COLOR_F menuBorder = Rgba(1, 1, 1, 0.10f);
  D2D1_COLOR_F menuHover = Rgba(1, 1, 1, 0.075f);
  D2D1_COLOR_F shadow = Rgba(0, 0, 0, 0.18f);
};

static std::wstring ConfigDir() {
  PWSTR base = nullptr;
  std::wstring dir;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &base))) {
    dir = base;
    CoTaskMemFree(base);
  } else {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    dir = tmp;
  }
  dir += L"\\TaskbarPulse";
  CreateDirectoryW(dir.c_str(), nullptr);
  return dir;
}

static std::wstring ConfigPath() { return ConfigDir() + L"\\settings.ini"; }

static void ClampThresholds(Settings& s) {
  for (int i = 0; i < 3; ++i) {
    if (s.warnPct[i] < 10) s.warnPct[i] = 10;
    if (s.warnPct[i] > 95) s.warnPct[i] = 95;
    if (s.critPct[i] <= s.warnPct[i]) s.critPct[i] = s.warnPct[i] + 5;
    if (s.critPct[i] > 99) s.critPct[i] = 99;
  }
}

static int NearestInterval(int ms) {
  const int presets[4] = {500, 1000, 2000, 5000};
  int best = presets[0];
  for (int preset : presets)
    if (std::abs(preset - ms) < std::abs(best - ms)) best = preset;
  return best;
}

static void LoadSettings(Settings& s) {
  std::wstring path = ConfigPath();
  s.showCpu = GetPrivateProfileIntW(L"TaskbarPulse", L"ShowCpu", 1, path.c_str()) != 0;
  s.showGpu = GetPrivateProfileIntW(L"TaskbarPulse", L"ShowGpu", 1, path.c_str()) != 0;
  s.showRam = GetPrivateProfileIntW(L"TaskbarPulse", L"ShowRam", 1, path.c_str()) != 0;
  s.showNet = GetPrivateProfileIntW(L"TaskbarPulse", L"ShowNet", 1, path.c_str()) != 0;
  s.showBars = GetPrivateProfileIntW(L"TaskbarPulse", L"ShowBars", 1, path.c_str()) != 0;
  s.showTemps = GetPrivateProfileIntW(L"TaskbarPulse", L"ShowTemps", 0, path.c_str()) != 0;
  s.borderless = GetPrivateProfileIntW(L"TaskbarPulse", L"Borderless", 0, path.c_str()) != 0;
  s.locked = GetPrivateProfileIntW(L"TaskbarPulse", L"Locked", 0, path.c_str()) != 0;
  s.intervalMs = GetPrivateProfileIntW(L"TaskbarPulse", L"IntervalMs", 1000, path.c_str());
  s.offset = GetPrivateProfileIntW(L"TaskbarPulse", L"Offset", 0, path.c_str());

  const int legacyWarn = GetPrivateProfileIntW(L"TaskbarPulse", L"WarnPct", 70, path.c_str());
  const int legacyCrit = GetPrivateProfileIntW(L"TaskbarPulse", L"CriticalPct", 90, path.c_str());
  const wchar_t* warnKeys[3] = {L"WarnCpu", L"WarnGpu", L"WarnRam"};
  const wchar_t* critKeys[3] = {L"CritCpu", L"CritGpu", L"CritRam"};
  for (int i = 0; i < 3; ++i) {
    s.warnPct[i] = GetPrivateProfileIntW(L"TaskbarPulse", warnKeys[i], legacyWarn, path.c_str());
    s.critPct[i] = GetPrivateProfileIntW(L"TaskbarPulse", critKeys[i], legacyCrit, path.c_str());
  }

  wchar_t luid[32] = L"0";
  GetPrivateProfileStringW(L"TaskbarPulse", L"GpuLuid", L"0", luid, 32, path.c_str());
  s.gpuLuid = wcstoull(luid, nullptr, 16);
  if (s.intervalMs < 250) s.intervalMs = 250;
  if (s.intervalMs > 10000) s.intervalMs = 10000;
  s.intervalMs = NearestInterval(s.intervalMs);
  if (!s.showCpu && !s.showGpu && !s.showRam && !s.showNet) s.showCpu = true;
  ClampThresholds(s);
}

static void SaveSettings(const Settings& s) {
  std::wstring path = ConfigPath();
  auto put = [&](const wchar_t* key, int value) {
    WritePrivateProfileStringW(L"TaskbarPulse", key, std::to_wstring(value).c_str(), path.c_str());
  };
  put(L"ShowCpu", s.showCpu);
  put(L"ShowGpu", s.showGpu);
  put(L"ShowRam", s.showRam);
  put(L"ShowNet", s.showNet);
  put(L"ShowBars", s.showBars);
  put(L"ShowTemps", s.showTemps);
  put(L"Borderless", s.borderless);
  put(L"Locked", s.locked);
  put(L"IntervalMs", s.intervalMs);
  put(L"Offset", s.offset);
  const wchar_t* warnKeys[3] = {L"WarnCpu", L"WarnGpu", L"WarnRam"};
  const wchar_t* critKeys[3] = {L"CritCpu", L"CritGpu", L"CritRam"};
  for (int i = 0; i < 3; ++i) {
    put(warnKeys[i], s.warnPct[i]);
    put(critKeys[i], s.critPct[i]);
  }
  wchar_t luid[32];
  swprintf_s(luid, L"%llx", s.gpuLuid);
  WritePrivateProfileStringW(L"TaskbarPulse", L"GpuLuid", luid, path.c_str());
}

// Autostart

static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kRunValue = L"TaskbarPulse";

static std::wstring ExePath() {
  wchar_t path[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
  return std::wstring(path, n);
}

static bool AutostartEnabled() {
  HKEY key;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
  wchar_t buf[1024];
  DWORD size = sizeof(buf) - sizeof(wchar_t);
  bool found = RegQueryValueExW(key, kRunValue, nullptr, nullptr, (BYTE*)buf, &size) == ERROR_SUCCESS;
  RegCloseKey(key);
  return found;
}

static void SetAutostart(bool on) {
  HKEY key;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
    return;
  if (on) {
    std::wstring cmd = L"\"" + ExePath() + L"\"";
    RegSetValueExW(key, kRunValue, 0, REG_SZ, (const BYTE*)cmd.c_str(),
                   (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
  } else {
    RegDeleteValueW(key, kRunValue);
  }
  RegCloseKey(key);
}

// Theme

static DWORD ReadDword(HKEY root, const wchar_t* sub, const wchar_t* name, DWORD fallback) {
  HKEY key;
  DWORD value = fallback, size = sizeof(value);
  if (RegOpenKeyExW(root, sub, 0, KEY_READ, &key) == ERROR_SUCCESS) {
    RegQueryValueExW(key, name, nullptr, nullptr, (BYTE*)&value, &size);
    RegCloseKey(key);
  }
  return value;
}

static Theme LoadTheme() {
  Theme t;
  t.light = ReadDword(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      L"SystemUsesLightTheme", 0) != 0;

  DWORD accent = ReadDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", 0);
  if ((accent >> 24) != 0) {  // 0xAABBGGRR
    t.accent = Rgba(((accent >> 0) & 0xFF) / 255.0f, ((accent >> 8) & 0xFF) / 255.0f,
                    ((accent >> 16) & 0xFF) / 255.0f, 1.0f);
  }
  if (t.light) {
    t.text = Rgba(0.07f, 0.07f, 0.07f, 0.95f);
    t.label = Rgba(0.07f, 0.07f, 0.07f, 0.58f);
    t.track = Rgba(0.0f, 0.0f, 0.0f, 0.13f);
    t.bg = Rgba(0.0f, 0.0f, 0.0f, 0.045f);
    t.border = Rgba(0.0f, 0.0f, 0.0f, 0.075f);
    t.warn = Rgba(0.71f, 0.42f, 0.02f, 1.0f);
    t.hot = Rgba(0.77f, 0.16f, 0.10f, 1.0f);
    t.menuBg = Rgba(0.975f, 0.975f, 0.98f, 0.99f);
    t.menuBorder = Rgba(0.0f, 0.0f, 0.0f, 0.10f);
    t.menuHover = Rgba(0.0f, 0.0f, 0.0f, 0.055f);
    t.shadow = Rgba(0.0f, 0.0f, 0.0f, 0.20f);
  }
  return t;
}

// Menu model

struct MenuEntry {
  bool separator = false;
  bool header = false;
  UINT id = 0;
  std::wstring text;
  bool checked = false;
};

namespace menuui {
constexpr float kItemH = 28.0f;
constexpr float kSepH = 7.0f;
constexpr float kPadV = 6.0f;
constexpr float kTextX = 34.0f;
constexpr float kRightPad = 14.0f;
constexpr float kRadius = 10.0f;
constexpr float kShadow = 9.0f;
constexpr float kMinWidth = 168.0f;
}  // namespace menuui

// Metrics

struct Sample {
  float cpu = 0.0f;
  float gpu = -1.0f;  // negative == unavailable
  float ram = 0.0f;
  float cpuTemp = -1.0f;
  float downBps = -1.0f;  // bytes per second, negative == unavailable
  float upBps = -1.0f;
  struct GpuUtil {
    ULONGLONG luid = 0;
    float util = 0.0f;
  } gpus[8];
  int gpuCount = 0;
};

struct AdapterInfo {
  ULONGLONG luid = 0;
  std::wstring name;
  bool software = false;
};

static void LoadAdapters(std::vector<AdapterInfo>& out) {
  ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)factory.GetAddressOf()))) return;
  for (UINT i = 0;; ++i) {
    ComPtr<IDXGIAdapter1> adapter;
    if (factory->EnumAdapters1(i, adapter.GetAddressOf()) == DXGI_ERROR_NOT_FOUND) break;
    DXGI_ADAPTER_DESC1 desc{};
    if (SUCCEEDED(adapter->GetDesc1(&desc))) {
      AdapterInfo info;
      info.luid = ((ULONGLONG)(DWORD)desc.AdapterLuid.HighPart << 32) | desc.AdapterLuid.LowPart;
      info.name = desc.Description;
      info.software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
      Log(L"adapter: %ls luid=%llx flags=%u vram=%lluMB", info.name.c_str(), info.luid,
          desc.Flags, desc.DedicatedVideoMemory / (1024 * 1024));
      out.push_back(info);
    }
  }
}

static ULONGLONG InternetRouteLuid() {
  // Ask the routing table which interface carries internet traffic instead of
  // summing adapters: filter drivers and VPN tunnels report the same bytes as
  // the physical NIC, so a sum multiplies the real traffic.
  SOCKADDR_INET dest{};
  dest.si_family = AF_INET;
  dest.Ipv4.sin_addr.s_addr = 0x01010101;  // 1.1.1.1, byte-symmetric
  MIB_IPFORWARD_ROW2 route{};
  SOCKADDR_INET source{};
  if (GetBestRoute2(nullptr, 0, nullptr, &dest, 0, &route, &source) == NO_ERROR)
    return route.InterfaceLuid.Value;

  SOCKADDR_INET dest6{};
  dest6.si_family = AF_INET6;
  static const unsigned char kProbe[16] = {0x20, 0x01, 0x48, 0x60, 0x48, 0x60, 0, 0,
                                           0, 0, 0, 0, 0, 0, 0x88, 0x88};
  memcpy(dest6.Ipv6.sin6_addr.u.Byte, kProbe, sizeof(kProbe));
  if (GetBestRoute2(nullptr, 0, nullptr, &dest6, 0, &route, &source) == NO_ERROR)
    return route.InterfaceLuid.Value;
  return 0;
}

class Sampler {
 public:
  Sampler() { QueryPerformanceFrequency(&netFreq_); }
  ~Sampler() { CloseGpu(); CloseTemp(); }

  void Prime() {
    ULONGLONG idle, kernel, user;
    if (ReadCpu(idle, kernel, user)) {
      cpuPrimed_ = true;
      lastIdle_ = idle;
      lastKernel_ = kernel;
      lastUser_ = user;
    }
    StartGpu();  // baseline collect; a real value needs a second sample later
  }

  Sample Next(bool readTemp, bool readNet, int intervalMs) {
    Sample s;
    ReadCpuSample(s.cpu);

    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof(mem);
    if (GlobalMemoryStatusEx(&mem)) {
      s.ram = (float)mem.dwMemoryLoad;
    }

    s.gpu = SampleGpu(s.gpus, &s.gpuCount);
    if (readTemp) {
      s.cpuTemp = ReadThermal();
    } else {
      CloseTemp();
      s.cpuTemp = -1.0f;
    }
    if (readNet) ReadNet(s, intervalMs);
    return s;
  }

 private:
  void ReadNet(Sample& s, int intervalMs) {
    const ULONGLONG now = GetTickCount64();
    if (now - netResolved_ >= 5000) {
      netResolved_ = now;
      const ULONGLONG luid = InternetRouteLuid();
      if (luid != netLuid_) {
        // Do not claim 0 B/s on a new interface: nothing has been measured yet.
        netLuid_ = luid;
        netPrimed_ = false;
        netValid_ = false;
        netDown_ = netUp_ = -1.0f;
        Log(L"net: interface luid=%llx", luid);
      }
    }
    if (!netLuid_) {
      s.downBps = s.upBps = -1.0f;
      return;
    }

    MIB_IF_ROW2 row{};
    row.InterfaceLuid.Value = netLuid_;
    if (GetIfEntry2(&row) != NO_ERROR || row.OperStatus != IfOperStatusUp) {
      netPrimed_ = false;
      netValid_ = false;
      s.downBps = s.upBps = -1.0f;
      return;
    }

    LARGE_INTEGER qpc{};
    QueryPerformanceCounter(&qpc);
    const double freq = (double)netFreq_.QuadPart;
    const double seconds = freq > 0.0 ? (double)(qpc.QuadPart - netTime_.QuadPart) / freq : 0.0;
    // QPC is not guaranteed to advance across sleep, so the gap test also uses
    // the wall clock. A long gap means sampling was paused: rebaseline instead
    // of averaging across it.
    const double wallGap = (double)(now - netTicks_) / 1000.0;
    const double maxGap = (double)intervalMs / 1000.0 * 4.0 + 1.0;
    if (!netPrimed_ || row.InOctets < netIn_ || row.OutOctets < netOut_ ||
        !(seconds > 0.0 && seconds <= maxGap) || wallGap > maxGap) {
      Log(L"net: rebaseline dt=%.3f wall=%.3f", seconds, wallGap);
      netPrimed_ = true;
      netIn_ = row.InOctets;
      netOut_ = row.OutOctets;
      netTime_ = qpc;
      netTicks_ = now;
      // A short pause keeps the last reading instead of flashing a dash; after a
      // long one the old number means nothing.
      const bool recent = netValid_ && wallGap <= 30.0;
      s.downBps = recent ? netDown_ : -1.0f;
      s.upBps = recent ? netUp_ : -1.0f;
      return;
    }

    s.downBps = netDown_ = (float)((double)(row.InOctets - netIn_) / seconds);
    s.upBps = netUp_ = (float)((double)(row.OutOctets - netOut_) / seconds);
    netValid_ = true;
    netIn_ = row.InOctets;
    netOut_ = row.OutOctets;
    netTime_ = qpc;
    netTicks_ = now;
    Log(L"net: luid=%llx t=%llu dt=%.3f in=%llu out=%llu down=%.0f up=%.0f", netLuid_, netTicks_,
        seconds, row.InOctets, row.OutOctets, s.downBps, s.upBps);
  }

  bool ReadCpu(ULONGLONG& idle, ULONGLONG& kernel, ULONGLONG& user) {
    FILETIME i{}, k{}, u{};
    if (!GetSystemTimes(&i, &k, &u)) return false;
    idle = ((ULONGLONG)i.dwHighDateTime << 32) | i.dwLowDateTime;
    kernel = ((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime;
    user = ((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime;
    return true;
  }

  void ReadCpuSample(float& out) {
    ULONGLONG idle, kernel, user;
    if (!ReadCpu(idle, kernel, user)) {
      out = 0.0f;
      return;
    }
    if (!cpuPrimed_) {
      cpuPrimed_ = true;
      lastIdle_ = idle;
      lastKernel_ = kernel;
      lastUser_ = user;
      out = 0.0f;
      return;
    }
    ULONGLONG dIdle = idle - lastIdle_;
    ULONGLONG dKernel = kernel - lastKernel_;
    ULONGLONG dUser = user - lastUser_;
    lastIdle_ = idle;
    lastKernel_ = kernel;
    lastUser_ = user;
    ULONGLONG total = dKernel + dUser;  // kernel time includes idle time
    out = total ? ClampF((float)(100.0 * (double)(total - dIdle) / (double)total), 0.0f, 100.0f) : 0.0f;
  }

  bool StartGpu() {
    if (query_) return true;
    if (GetTickCount64() < nextGpuRetry_) return false;
    if (PdhOpenQueryW(nullptr, 0, &query_) != ERROR_SUCCESS) {
      query_ = nullptr;
      nextGpuRetry_ = GetTickCount64() + 30000;
      return false;
    }
    if (PdhAddEnglishCounterW(query_, L"\\GPU Engine(*)\\Utilization Percentage", 0, &counter_) != ERROR_SUCCESS) {
      CloseGpu();
      nextGpuRetry_ = GetTickCount64() + 30000;
      return false;
    }
    PdhCollectQueryData(query_);  // baseline sample; values are valid after the next collect
    return true;
  }

  void CloseGpu() {
    if (query_) {
      PdhCloseQuery(query_);
      query_ = nullptr;
      counter_ = nullptr;
    }
  }

  // The thermal zone has its own query. Sharing the GPU query would make the
  // temperature depend on WDDM counters that VMs and RDP sessions do not expose,
  // and would collect the zones while temperatures are switched off.
  bool StartTemp() {
    if (tempQuery_) return true;
    const ULONGLONG now = GetTickCount64();
    if (now < nextTempRetry_) return false;
    if (PdhOpenQueryW(nullptr, 0, &tempQuery_) != ERROR_SUCCESS) {
      tempQuery_ = nullptr;
      nextTempRetry_ = now + 30000;
      return false;
    }
    thermalTenths_ = true;
    if (PdhAddEnglishCounterW(tempQuery_, L"\\Thermal Zone Information(*)\\High Precision Temperature", 0,
                              &tempCounter_) != ERROR_SUCCESS) {
      tempCounter_ = nullptr;
      // High Precision is in tenths of Kelvin; the plain counter is whole Kelvin.
      if (PdhAddEnglishCounterW(tempQuery_, L"\\Thermal Zone Information(*)\\Temperature", 0,
                                &tempCounter_) != ERROR_SUCCESS) {
        CloseTemp();
        nextTempRetry_ = now + 30000;
        return false;
      }
      thermalTenths_ = false;
    }
    PdhCollectQueryData(tempQuery_);
    return true;
  }

  void CloseTemp() {
    if (tempQuery_) {
      PdhCloseQuery(tempQuery_);
      tempQuery_ = nullptr;
      tempCounter_ = nullptr;
    }
  }

  float ReadThermal() {
    if (!StartTemp()) return -1.0f;
    if (PdhCollectQueryData(tempQuery_) != ERROR_SUCCESS) {
      CloseTemp();
      nextTempRetry_ = GetTickCount64() + 30000;
      return -1.0f;
    }
    DWORD size = 0, count = 0;
    PDH_STATUS status = PdhGetFormattedCounterArrayW(tempCounter_, PDH_FMT_DOUBLE, &size, &count, nullptr);
    if (status == PDH_MORE_DATA) {
      if (thermalBuf_.size() < size) thermalBuf_.resize(size);
      status = PdhGetFormattedCounterArrayW(tempCounter_, PDH_FMT_DOUBLE, &size, &count,
                                            (PDH_FMT_COUNTERVALUE_ITEM_W*)thermalBuf_.data());
    }
    if (status != ERROR_SUCCESS) return -1.0f;

    auto* items = (PDH_FMT_COUNTERVALUE_ITEM_W*)thermalBuf_.data();
    double warmest = -1.0;
    for (DWORD i = 0; i < count; ++i) {
      if (items[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA &&
          items[i].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA)
        continue;
      if (items[i].FmtValue.doubleValue > warmest) warmest = items[i].FmtValue.doubleValue;
    }
    if (warmest <= 0.0) return -1.0f;
    double kelvin = thermalTenths_ ? warmest / 10.0 : warmest;
    double celsius = kelvin - 273.15;
    if (celsius < -20.0 || celsius > 130.0) return -1.0f;
    return (float)celsius;
  }

  // Task Manager semantics: sum each engine's utilisation across processes,
  // cap every engine at 100 %, then report the busiest engine.
  float SampleGpu(Sample::GpuUtil* out, int* outCount) {
    *outCount = 0;
    if (!StartGpu()) return -1.0f;
    if (PdhCollectQueryData(query_) != ERROR_SUCCESS) {
      CloseGpu();
      nextGpuRetry_ = GetTickCount64() + 30000;
      return -1.0f;
    }

    DWORD size = 0, count = 0;
    PDH_STATUS status = PdhGetFormattedCounterArrayW(counter_, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, nullptr);
    if (status == PDH_MORE_DATA) {
      if (buffer_.size() < size) buffer_.resize(size);
      status = PdhGetFormattedCounterArrayW(counter_, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count,
                                            (PDH_FMT_COUNTERVALUE_ITEM_W*)buffer_.data());
      if (status == PDH_MORE_DATA) {
        // The instance list grew between the probe and the fill.
        buffer_.resize(size);
        status = PdhGetFormattedCounterArrayW(counter_, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size,
                                              &count, (PDH_FMT_COUNTERVALUE_ITEM_W*)buffer_.data());
      }
    }
    if (status != ERROR_SUCCESS) {
      buffer_.clear();
      return -1.0f;
    }

    struct Engine {
      ULONGLONG luid;
      unsigned phys, eng;
      double sum;
    };
    Engine engines[96];
    int engineCount = 0;

    auto* items = (PDH_FMT_COUNTERVALUE_ITEM_W*)buffer_.data();
    for (DWORD i = 0; i < count; ++i) {
      if (items[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA &&
          items[i].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA)
        continue;
      const wchar_t* p = wcsstr(items[i].szName, L"luid_0x");
      if (!p) continue;
      wchar_t* end = nullptr;
      unsigned long long hi = wcstoull(p + 7, &end, 16);
      if (!end || *end != L'_') continue;
      const wchar_t* loStart = end + 1;
      if (loStart[0] == L'0' && loStart[1] == L'x') loStart += 2;
      unsigned long long lo = wcstoull(loStart, &end, 16);
      unsigned phys = 0, eng = 0;
      if (end && wcsncmp(end, L"_phys_", 6) == 0) {
        phys = (unsigned)wcstoul(end + 6, &end, 10);
        if (end && wcsncmp(end, L"_eng_", 5) == 0) eng = (unsigned)wcstoul(end + 5, &end, 10);
      }
      ULONGLONG luid = (hi << 32) | lo;
      int found = -1;
      for (int e = 0; e < engineCount; ++e) {
        if (engines[e].luid == luid && engines[e].phys == phys && engines[e].eng == eng) {
          found = e;
          break;
        }
      }
      if (found < 0) {
        if (engineCount >= 96) continue;
        found = engineCount++;
        engines[found] = {luid, phys, eng, 0.0};
      }
      engines[found].sum += items[i].FmtValue.doubleValue;
    }

    // Per adapter: the busiest engine. Overall: the busiest adapter.
    float best = 0.0f;
    for (int e = 0; e < engineCount; ++e) {
      double util = std::min(engines[e].sum, 100.0);
      int slot = -1;
      for (int g = 0; g < *outCount; ++g) {
        if (out[g].luid == engines[e].luid) {
          slot = g;
          break;
        }
      }
      if (slot < 0) {
        if (*outCount >= 8) continue;
        slot = (*outCount)++;
        out[slot].luid = engines[e].luid;
        out[slot].util = 0.0f;
      }
      if ((float)util > out[slot].util) out[slot].util = (float)util;
      if ((float)util > best) best = (float)util;
    }

    // No instances at all: the counter exists but nothing is using a GPU.
    if (engineCount == 0) return 0.0f;
    return ClampF(best, 0.0f, 100.0f);
  }

  PDH_HQUERY query_ = nullptr;
  PDH_HCOUNTER counter_ = nullptr;
  PDH_HQUERY tempQuery_ = nullptr;
  PDH_HCOUNTER tempCounter_ = nullptr;
  bool thermalTenths_ = true;
  std::vector<BYTE> buffer_;
  std::vector<BYTE> thermalBuf_;
  ULONGLONG nextGpuRetry_ = 0;
  ULONGLONG nextTempRetry_ = 0;
  bool cpuPrimed_ = false;
  ULONGLONG lastIdle_ = 0, lastKernel_ = 0, lastUser_ = 0;
  ULONGLONG netLuid_ = 0;
  ULONGLONG netIn_ = 0, netOut_ = 0;
  LARGE_INTEGER netTime_{};
  LARGE_INTEGER netFreq_{};
  ULONGLONG netResolved_ = 0;
  ULONGLONG netTicks_ = 0;
  bool netPrimed_ = false;
  bool netValid_ = false;
  float netDown_ = -1.0f, netUp_ = -1.0f;
};

// Layout (logical pixels, 96 dpi)

namespace ui {
constexpr float kPadX = 10.0f;
constexpr float kGapX = 9.0f;
constexpr float kLabelPx = 9.0f;
constexpr float kValuePx = 12.5f;
constexpr float kLabelBaseline = 10.6f;
constexpr float kValueBaseline = 25.4f;
constexpr float kBarTop = 28.0f;
constexpr float kBarH = 2.0f;
constexpr float kRadius = 8.0f;
constexpr float kGapFromTray = 4.0f;
constexpr float kHeightBars = 32.0f;
}  // namespace ui

struct Layout {
  int w = 0, h = 0;
  int contentH = 0;
  int offsetY = 0;
  bool bordered = true;
  bool showTemps = false;
  int count = 0;
  int colW = 0;  // width of a percentage column
  int xs[4] = {0, 0, 0, 0};
  int colWs[4] = {0, 0, 0, 0};
  int visible[4] = {0, 0, 0, 0};  // metric index per column
  int labelBaseline = 0;
  int valueBaseline = 0;
  int barTop = 0;
  int barH = 0;
  int radius = 0;
};

struct TextMetrics {
  float width = 0;
  float baseline = 0;
};

// Renderer

class Renderer {
 public:
  bool Init(HDC screen) {
    screen_ = screen;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), nullptr,
                                 (void**)factory_.GetAddressOf())))
      return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   (IUnknown**)write_.GetAddressOf())))
      return false;
    if (SUCCEEDED(write_->CreateTypography(typography_.GetAddressOf()))) {
      DWRITE_FONT_FEATURE feature{DWRITE_FONT_FEATURE_TAG_TABULAR_FIGURES, 1};
      typography_->AddFontFeature(feature);
    }
    return true;
  }

  void SetScale(float scale, float labelPx, float valuePx) {
    if (scale == scale_ && labelPx == labelPx_ && valuePx == valuePx_) return;
    scale_ = scale;
    labelPx_ = labelPx;
    valuePx_ = valuePx;
    label_.Reset();
    value_.Reset();
    const wchar_t* family = L"Segoe UI Variable Text";
    write_->CreateTextFormat(family, nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                             DWRITE_FONT_STRETCH_NORMAL, labelPx, L"", label_.GetAddressOf());
    write_->CreateTextFormat(family, nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                             DWRITE_FONT_STRETCH_NORMAL, valuePx, L"", value_.GetAddressOf());
    write_->CreateTextFormat(family, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                             DWRITE_FONT_STRETCH_NORMAL, 12.5f * scale, L"", menu_.GetAddressOf());
    write_->CreateTextFormat(family, nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                             DWRITE_FONT_STRETCH_NORMAL, 9.0f * scale, L"", section_.GetAddressOf());
    write_->CreateTextFormat(family, nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                             DWRITE_FONT_STRETCH_NORMAL, 14.0f * scale, L"", title_.GetAddressOf());
    for (IDWriteTextFormat* f : {label_.Get(), value_.Get(), menu_.Get(), section_.Get(), title_.Get()}) {
      if (!f) continue;
      f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
      f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
      f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    if (menu_) menu_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    if (section_) section_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    if (title_) title_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
  }

  TextMetrics Measure(IDWriteTextFormat* format, const wchar_t* text) {
    TextMetrics m;
    if (!format) return m;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(write_->CreateTextLayout(text, (UINT32)wcslen(text), format, 1000.0f, 100.0f,
                                        layout.GetAddressOf())))
      return m;
    DWRITE_TEXT_METRICS tm{};
    layout->GetMetrics(&tm);
    m.width = tm.widthIncludingTrailingWhitespace;
    DWRITE_LINE_METRICS lm{};
    UINT32 lines = 0;
    if (SUCCEEDED(layout->GetLineMetrics(&lm, 1, &lines)) && lines) m.baseline = lm.baseline;
    return m;
  }

  bool Paint(HWND hwnd, int x, int y, const Layout& L, const Theme& theme, bool hover,
             const Sample& sample, float gpuShown, const int alert[3]);
  bool PaintMenu(HWND hwnd, int x, int y, const Theme& theme, const std::vector<MenuEntry>& items,
                 int hoverIndex, int width, int height);
  bool PaintSettings(HWND hwnd, int x, int y, const Theme& theme, const struct SettingsLayout& layout,
                     const struct SettingsView& view, int hover);

  float scale() const { return scale_; }
  IDWriteTextFormat* labelFormat() const { return label_.Get(); }
  IDWriteTextFormat* valueFormat() const { return value_.Get(); }
  IDWriteTextFormat* menuFormat() const { return menu_.Get(); }
  IDWriteTextFormat* sectionFormat() const { return section_.Get(); }
  IDWriteTextFormat* titleFormat() const { return title_.Get(); }

 private:
  bool EnsureSurface(int w, int h) {
    if (w == width_ && h == height_ && memoryDc_) return true;
    ReleaseSurface();
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    bitmap_ = CreateDIBSection(screen_, &bmi, DIB_RGB_COLORS, &bits_, nullptr, 0);
    if (!bitmap_) return false;
    memoryDc_ = CreateCompatibleDC(screen_);
    if (!memoryDc_) return false;
    SelectObject(memoryDc_, bitmap_);
    width_ = w;
    height_ = h;

    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    if (FAILED(factory_->CreateDCRenderTarget(&props, target_.ReleaseAndGetAddressOf()))) return false;
    target_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    target_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    target_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), brush_.ReleaseAndGetAddressOf());
    return brush_ != nullptr;
  }

  void ReleaseSurface() {
    target_.Reset();
    brush_.Reset();
    if (memoryDc_) {
      DeleteDC(memoryDc_);
      memoryDc_ = nullptr;
    }
    if (bitmap_) {
      DeleteObject(bitmap_);
      bitmap_ = nullptr;
    }
    bits_ = nullptr;
    width_ = height_ = 0;
  }

  void DrawTextAt(IDWriteTextFormat* format, const wchar_t* text, float x, float baseline,
                  float maxWidth, const D2D1_COLOR_F& color) {
    if (!format) return;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(write_->CreateTextLayout(text, (UINT32)wcslen(text), format, maxWidth, 200.0f,
                                        layout.GetAddressOf())))
      return;
    if (typography_) {
      DWRITE_TEXT_RANGE range{0, (UINT32)wcslen(text)};
      layout->SetTypography(typography_.Get(), range);
    }
    DWRITE_LINE_METRICS lm{};
    UINT32 lines = 0;
    if (FAILED(layout->GetLineMetrics(&lm, 1, &lines)) || !lines) return;
    brush_->SetColor(color);
    target_->DrawTextLayout(D2D1::Point2F(std::round(x), std::round(baseline - lm.baseline)), layout.Get(),
                            brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
  }

  ComPtr<ID2D1Factory> factory_;
  ComPtr<IDWriteFactory> write_;
  ComPtr<IDWriteTypography> typography_;
  ComPtr<ID2D1DCRenderTarget> target_;
  ComPtr<ID2D1SolidColorBrush> brush_;
  ComPtr<IDWriteTextFormat> label_, value_, menu_, section_, title_;
  HDC screen_ = nullptr;
  HDC memoryDc_ = nullptr;
  HBITMAP bitmap_ = nullptr;
  void* bits_ = nullptr;
  int width_ = 0, height_ = 0;
  float scale_ = 1.0f, labelPx_ = 0, valuePx_ = 0;
};

// Settings window layout (physical pixels)

struct SettingsLayout {
  int w = 0, h = 0;
  int shadow = 0;
  RECT body{};
  RECT title{};
  RECT closeBtn{};
  RECT secMetrics{}, secRefresh{}, secAlerts{}, secBehavior{}, secGpu{};
  RECT cpu{}, gpu{}, ram{}, net{};
  RECT barsRow{}, barsSwitch{};
  RECT tempRow{}, tempSwitch{};
  RECT alertChip[3]{};
  RECT rate[4]{};
  RECT warnRow{}, warnMinus{}, warnPlus{}, warnValue{};
  RECT critRow{}, critMinus{}, critPlus{}, critValue{};
  RECT lockRow{}, lockSwitch{};
  RECT borderRow{}, borderSwitch{};
  RECT autoRow{}, autoSwitch{};
  RECT gpuRow{}, gpuPrev{}, gpuNext{}, gpuName{};
  RECT reset{}, exitBtn{}, footer{};
  bool gpuSelector = false;
};

struct SettingsView {
  bool cpu = true, gpu = true, ram = true, network = true, bars = true, temps = false,
       borderless = false;
  bool locked = false, autostart = false;
  int intervalMs = 1000;
  int warn[3] = {70, 70, 70};
  int critical[3] = {90, 90, 90};
  int alertMetric = 0;
  int focusId = 0;
  bool focusVisible = false;
  std::vector<std::wstring> gpuNames;  // index 0 is "Automatic"
  int gpuIndex = 0;
};

// Settings window primitives, in logical pixels.
namespace setui {
constexpr float kWidth = 340.0f;
constexpr float kShadow = 10.0f;
constexpr float kPadX = 18.0f;
constexpr float kTitleH = 36.0f;
constexpr float kSectionH = 18.0f;
constexpr float kRowH = 30.0f;
constexpr float kGap = 8.0f;
constexpr float kRadius = 10.0f;
constexpr float kSwitchW = 40.0f;
constexpr float kSwitchH = 22.0f;
constexpr float kButton = 28.0f;
constexpr float kFooterH = 30.0f;
}  // namespace setui

enum : int {
  CTL_NONE = 0,
  CTL_TITLE,
  CTL_CLOSE,
  CTL_CPU,
  CTL_GPU,
  CTL_RAM,
  CTL_NET,
  CTL_BARS,
  CTL_TEMPS,
  CTL_ALERT_CPU,
  CTL_ALERT_GPU,
  CTL_ALERT_RAM,
  CTL_RATE0,
  CTL_RATE1,
  CTL_RATE2,
  CTL_RATE3,
  CTL_WARN_MINUS,
  CTL_WARN_PLUS,
  CTL_CRIT_MINUS,
  CTL_CRIT_PLUS,
  CTL_LOCK,
  CTL_BORDERLESS,
  CTL_AUTOSTART,
  CTL_GPU_PREV,
  CTL_GPU_NEXT,
  CTL_RESET,
  CTL_EXIT,
};

static RECT SettingsFocusRect(const SettingsLayout& L, int id) {
  switch (id) {
    case CTL_CLOSE: return L.closeBtn;
    case CTL_CPU: return L.cpu;
    case CTL_GPU: return L.gpu;
    case CTL_RAM: return L.ram;
    case CTL_NET: return L.net;
    case CTL_BARS: return L.barsSwitch;
    case CTL_TEMPS: return L.tempSwitch;
    case CTL_ALERT_CPU: return L.alertChip[0];
    case CTL_ALERT_GPU: return L.alertChip[1];
    case CTL_ALERT_RAM: return L.alertChip[2];
    case CTL_RATE0: return L.rate[0];
    case CTL_RATE1: return L.rate[1];
    case CTL_RATE2: return L.rate[2];
    case CTL_RATE3: return L.rate[3];
    case CTL_WARN_MINUS: return L.warnMinus;
    case CTL_WARN_PLUS: return L.warnPlus;
    case CTL_CRIT_MINUS: return L.critMinus;
    case CTL_CRIT_PLUS: return L.critPlus;
    case CTL_LOCK: return L.lockSwitch;
    case CTL_BORDERLESS: return L.borderSwitch;
    case CTL_AUTOSTART: return L.autoSwitch;
    case CTL_GPU_PREV: return L.gpuPrev;
    case CTL_GPU_NEXT: return L.gpuNext;
    case CTL_RESET: return L.reset;
    case CTL_EXIT: return L.exitBtn;
    default: return RECT{0, 0, 0, 0};
  }
}

bool Renderer::PaintSettings(HWND hwnd, int x, int y, const Theme& theme, const SettingsLayout& L,
                             const SettingsView& view, int hover) {
  if (!EnsureSurface(L.w, L.h)) return false;
  RECT rc{0, 0, L.w, L.h};
  if (FAILED(target_->BindDC(memoryDc_, &rc))) return false;

  const float s = scale_;
  const float shadow = (float)L.shadow;
  const float radius = setui::kRadius * s;
  const D2D1_RECT_F body = D2D1::RectF(shadow, shadow, (float)L.w - shadow, (float)L.h - shadow);

  target_->BeginDraw();
  target_->Clear(D2D1::ColorF(0, 0, 0, 0));

  // Per-pixel hit testing skips fully transparent pixels. A near-invisible fill
  // keeps the top drag band clickable right up to the window edge.
  brush_->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f / 255.0f));
  target_->FillRectangle(D2D1::RectF(0.0f, 0.0f, (float)L.w, (float)L.h), brush_.Get());

  for (int i = 5; i >= 1; --i) {
    float spread = i * 1.4f * s;
    D2D1_ROUNDED_RECT glow{D2D1::RectF(body.left - spread, body.top - spread + 2.0f * s,
                                        body.right + spread, body.bottom + spread + 2.0f * s),
                            radius + spread, radius + spread};
    D2D1_COLOR_F c = theme.shadow;
    c.a *= 0.22f;
    brush_->SetColor(c);
    target_->FillRoundedRectangle(glow, brush_.Get());
  }

  D2D1_ROUNDED_RECT pill{body, radius, radius};
  brush_->SetColor(theme.menuBg);
  target_->FillRoundedRectangle(pill, brush_.Get());
  brush_->SetColor(theme.menuBorder);
  target_->DrawRoundedRectangle(pill, brush_.Get(), std::max(1.0f, s));

  auto rectf = [](const RECT& r) {
    return D2D1::RectF((float)r.left, (float)r.top, (float)r.right, (float)r.bottom);
  };
  auto fillRound = [&](const RECT& r, float rad, const D2D1_COLOR_F& c) {
    D2D1_ROUNDED_RECT rr{rectf(r), rad, rad};
    brush_->SetColor(c);
    target_->FillRoundedRectangle(rr, brush_.Get());
  };

  DrawTextAt(title_.Get(), L"Taskbar Pulse", body.left + 18.0f * s, (float)L.title.bottom - 11.0f * s,
             220.0f * s, theme.text);
  if (hover == CTL_CLOSE) fillRound(L.closeBtn, 6.0f * s, theme.menuHover);
  {
    float cx = (L.closeBtn.left + L.closeBtn.right) * 0.5f;
    float cy = (L.closeBtn.top + L.closeBtn.bottom) * 0.5f;
    float d = 4.5f * s;
    float w = 1.4f * s;
    brush_->SetColor(hover == CTL_CLOSE ? theme.text : theme.label);
    target_->DrawLine({cx - d, cy - d}, {cx + d, cy + d}, brush_.Get(), w);
    target_->DrawLine({cx + d, cy - d}, {cx - d, cy + d}, brush_.Get(), w);
  }

  auto section = [&](const RECT& r, const wchar_t* text) {
    DrawTextAt(section_.Get(), text, (float)r.left, (float)r.bottom - 3.0f * s,
               (float)(r.right - r.left), theme.label);
  };
  section(L.secMetrics, L"SHOWN METRICS");
  section(L.secRefresh, L"REFRESH RATE");
  section(L.secAlerts, L"ALERT COLOURS");
  section(L.secBehavior, L"BEHAVIOUR");
  if (L.gpuSelector) section(L.secGpu, L"GPU SOURCE");

  D2D1_COLOR_F chip = theme.light ? Rgba(0, 0, 0, 0.075f) : Rgba(1, 1, 1, 0.125f);
  auto metricPill = [&](const RECT& r, bool on, const wchar_t* text) {
    if (on) fillRound(r, 7.0f * s, chip);
    DrawTextAt(value_.Get(), text, (float)r.left, (float)r.bottom - 10.5f * s,
               (float)(r.right - r.left), on ? theme.text : theme.label);
  };
  metricPill(L.cpu, view.cpu, L"CPU");
  metricPill(L.gpu, view.gpu, L"GPU");
  metricPill(L.ram, view.ram, L"RAM");
  metricPill(L.net, view.network, L"NET");

  auto switchRow = [&](const RECT& row, const RECT& sw, const wchar_t* label, bool on) {
    DrawTextAt(menu_.Get(), label, (float)row.left, (float)row.bottom - 10.0f * s,
               (float)(row.right - row.left), theme.text);
    float h = (float)(sw.bottom - sw.top);
    fillRound(sw, h / 2.0f,
              on ? theme.accent : (theme.light ? Rgba(0, 0, 0, 0.18f) : Rgba(1, 1, 1, 0.16f)));
    float knob = h - 4.0f * s;
    float kx = on ? (float)sw.right - 2.0f * s - knob : (float)sw.left + 2.0f * s;
    D2D1_ELLIPSE dot{D2D1::Point2F(kx + knob / 2.0f, (sw.top + sw.bottom) * 0.5f), knob / 2.0f,
                     knob / 2.0f};
    brush_->SetColor(Rgba(1, 1, 1, 0.98f));
    target_->FillEllipse(dot, brush_.Get());
  };
  switchRow(L.barsRow, L.barsSwitch, L"Show load bars", view.bars);
  switchRow(L.tempRow, L.tempSwitch, L"Show CPU temperature", view.temps);
  switchRow(L.lockRow, L.lockSwitch, L"Lock position", view.locked);
  switchRow(L.borderRow, L.borderSwitch, L"Borderless", view.borderless);
  switchRow(L.autoRow, L.autoSwitch, L"Start with Windows", view.autostart);

  const wchar_t* rates[4] = {L"0.5 s", L"1 s", L"2 s", L"5 s"};
  int current = view.intervalMs == 500 ? 0 : view.intervalMs == 1000 ? 1
                : view.intervalMs == 2000 ? 2
                                          : 3;
  for (int i = 0; i < 4; ++i) {
    bool on = i == current;
    if (on) fillRound(L.rate[i], 7.0f * s, chip);
    DrawTextAt(value_.Get(), rates[i], (float)L.rate[i].left, (float)L.rate[i].bottom - 10.5f * s,
               (float)(L.rate[i].right - L.rate[i].left), on ? theme.text : theme.label);
  }

  auto stepper = [&](const RECT& minus, const RECT& valueBox, const RECT& plus, int percent,
                     int idMinus, int idPlus) {
    D2D1_COLOR_F faint = theme.menuHover;
    faint.a *= 0.55f;
    fillRound(minus, 6.0f * s, hover == idMinus ? theme.menuHover : faint);
    fillRound(plus, 6.0f * s, hover == idPlus ? theme.menuHover : faint);
    float w = 1.4f * s;
    brush_->SetColor(theme.text);
    float cy = (minus.top + minus.bottom) * 0.5f;
    float cx = (minus.left + minus.right) * 0.5f;
    target_->DrawLine({cx - 4.5f * s, cy}, {cx + 4.5f * s, cy}, brush_.Get(), w);
    cx = (plus.left + plus.right) * 0.5f;
    target_->DrawLine({cx - 4.5f * s, cy}, {cx + 4.5f * s, cy}, brush_.Get(), w);
    target_->DrawLine({cx, cy - 4.5f * s}, {cx, cy + 4.5f * s}, brush_.Get(), w);
    wchar_t buf[16];
    swprintf_s(buf, L"%d%%", percent);
    DrawTextAt(value_.Get(), buf, (float)valueBox.left, (float)valueBox.bottom - 10.0f * s,
               (float)(valueBox.right - valueBox.left), theme.text);
  };
  const wchar_t* metricNames[3] = {L"CPU", L"GPU", L"RAM"};
  for (int i = 0; i < 3; ++i) {
    bool on = view.alertMetric == i;
    if (on) fillRound(L.alertChip[i], 7.0f * s, chip);
    DrawTextAt(value_.Get(), metricNames[i], (float)L.alertChip[i].left,
               (float)L.alertChip[i].bottom - 10.5f * s,
               (float)(L.alertChip[i].right - L.alertChip[i].left), on ? theme.text : theme.label);
  }
  DrawTextAt(menu_.Get(), L"Warn at", (float)L.warnRow.left, (float)L.warnRow.bottom - 10.0f * s,
             (float)(L.warnRow.right - L.warnRow.left), theme.text);
  stepper(L.warnMinus, L.warnValue, L.warnPlus, view.warn[view.alertMetric], CTL_WARN_MINUS,
          CTL_WARN_PLUS);
  DrawTextAt(menu_.Get(), L"Critical at", (float)L.critRow.left, (float)L.critRow.bottom - 10.0f * s,
             (float)(L.critRow.right - L.critRow.left), theme.text);
  stepper(L.critMinus, L.critValue, L.critPlus, view.critical[view.alertMetric], CTL_CRIT_MINUS,
          CTL_CRIT_PLUS);

  if (L.gpuSelector) {
    D2D1_COLOR_F faint = theme.menuHover;
    faint.a *= 0.55f;
    fillRound(L.gpuPrev, 6.0f * s, hover == CTL_GPU_PREV ? theme.menuHover : faint);
    fillRound(L.gpuNext, 6.0f * s, hover == CTL_GPU_NEXT ? theme.menuHover : faint);
    float w = 1.4f * s;
    brush_->SetColor(theme.text);
    {
      float cx = (L.gpuPrev.left + L.gpuPrev.right) * 0.5f + 1.5f * s;
      float cy = (L.gpuPrev.top + L.gpuPrev.bottom) * 0.5f;
      float d = 4.0f * s;
      target_->DrawLine({cx + d * 0.4f, cy - d}, {cx - d * 0.6f, cy}, brush_.Get(), w);
      target_->DrawLine({cx - d * 0.6f, cy}, {cx + d * 0.4f, cy + d}, brush_.Get(), w);
    }
    {
      float cx = (L.gpuNext.left + L.gpuNext.right) * 0.5f - 1.5f * s;
      float cy = (L.gpuNext.top + L.gpuNext.bottom) * 0.5f;
      float d = 4.0f * s;
      target_->DrawLine({cx - d * 0.4f, cy - d}, {cx + d * 0.6f, cy}, brush_.Get(), w);
      target_->DrawLine({cx + d * 0.6f, cy}, {cx - d * 0.4f, cy + d}, brush_.Get(), w);
    }
    const std::wstring& name =
        view.gpuNames.empty()
            ? std::wstring(L"")
            : view.gpuNames[std::min<size_t>((size_t)view.gpuIndex, view.gpuNames.size() - 1)];
    DrawTextAt(value_.Get(), name.c_str(), (float)L.gpuName.left, (float)L.gpuName.bottom - 10.0f * s,
               (float)(L.gpuName.right - L.gpuName.left), theme.text);
  }

  if (hover == CTL_RESET) fillRound(L.reset, 6.0f * s, theme.menuHover);
  DrawTextAt(menu_.Get(), L"Reset position", (float)L.reset.left + 8.0f * s,
             (float)L.reset.bottom - 9.0f * s, (float)(L.reset.right - L.reset.left), theme.accent);
  if (hover == CTL_EXIT) fillRound(L.exitBtn, 6.0f * s, theme.menuHover);
  DrawTextAt(menu_.Get(), L"Exit", (float)L.exitBtn.left + 6.0f * s,
             (float)L.exitBtn.bottom - 9.0f * s, (float)(L.exitBtn.right - L.exitBtn.left),
             theme.label);
  {
    TextMetrics m = Measure(menu_.Get(), kVersion);
    DrawTextAt(menu_.Get(), kVersion, (float)L.footer.right - m.width - 2.0f * s,
               (float)L.footer.bottom - 9.0f * s, m.width + 4.0f * s, theme.label);
  }

  if (view.focusVisible && view.focusId) {
    RECT fr = SettingsFocusRect(L, view.focusId);
    if (fr.right > fr.left) {
      float pad = 2.0f * s;
      D2D1_ROUNDED_RECT ring{D2D1::RectF((float)fr.left - pad, (float)fr.top - pad,
                                         (float)fr.right + pad, (float)fr.bottom + pad),
                             7.0f * s, 7.0f * s};
      brush_->SetColor(theme.accent);
      target_->DrawRoundedRectangle(ring, brush_.Get(), 1.6f * s);
    }
  }

  HRESULT hr = target_->EndDraw();
  if (FAILED(hr)) {
    Log(L"settings EndDraw failed %08lX", (unsigned long)hr);
    ReleaseSurface();
    return false;
  }
  GdiFlush();

  POINT dst{x, y};
  POINT src{0, 0};
  SIZE size{L.w, L.h};
  BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
  BOOL ok = UpdateLayeredWindow(hwnd, screen_, &dst, &size, memoryDc_, &src, 0, &blend, ULW_ALPHA);
  if (!ok) Log(L"settings UpdateLayeredWindow failed %lu", GetLastError());
  return ok != FALSE;
}

// App state

struct App {
  HINSTANCE instance = nullptr;
  HWND hwnd = nullptr;
  HDC screen = nullptr;
  Settings settings;
  Theme theme;
  Renderer renderer;
  Layout layout;
  Sample sample;

  HWND taskbar = nullptr;
  HWND tray = nullptr;
  UINT taskbarDpi = 96;
  int x = 0, y = 0;
  int ow = 0, oh = 0;
  bool laidOut = false;
  bool visible = false;
  bool hasSample = false;
  bool hover = false;
  bool dragging = false;
  int dragStartCursorX = 0;
  int dragStartOffset = 0;
  int dragMoved = 0;

  HWND menuHwnd = nullptr;
  bool menuOpen = false;
  int menuHover = -1;
  bool menuButtonGuard = false;
  RECT menuRect{};
  std::vector<MenuEntry> menuItems;
  int menuW = 0, menuH = 0;

  HWND settingsHwnd = nullptr;
  bool settingsOpen = false;
  SettingsLayout settingsLayout;
  SettingsView settingsView;
  std::vector<int> focusOrder;
  int focusId = 0;
  bool focusVisible = false;
  int settingsHover = -1;
  int settingsW = 0, settingsH = 0;
  int settingsX = 0, settingsY = 0;
  bool settingsDragging = false;
  POINT settingsDragStart{};
  RECT settingsDragRect{};
  HWND settingsPrevious = nullptr;

  float gpuShown = -1.0f;
  std::atomic<bool> samplingActive{true};
  int lastPaintCpu = -999, lastPaintGpu = -999, lastPaintRam = -999;
  int lastPaintAlert[3] = {-1, -1, -1};
  int lastPaintCpuTemp = -999;
  wchar_t lastNetDown[16] = L"";
  wchar_t lastNetUp[16] = L"";
  bool tempsAvailable = false;

  struct MetricAlert {
    int level = 0;          // 0 = normal, 1 = warning, 2 = critical
    ULONGLONG warnSince = 0;
    ULONGLONG critSince = 0;
  } alerts[3];

  std::vector<AdapterInfo> adapters;
  std::wstring trayTip;
  HICON trayIcon = nullptr;

  ComPtr<IVirtualDesktopManager> vdm;

  // Worker thread
  HANDLE thread = nullptr;
  HANDLE stopEvent = nullptr;
  HANDLE wakeEvent = nullptr;
  CRITICAL_SECTION sampleLock;
  Sample pending;
  std::atomic<HWND> sampleTarget{nullptr};
  std::atomic<int> intervalMs{1000};
  std::atomic<bool> tempsEnabled{false};
  std::atomic<bool> netEnabled{false};
};

static App g_app;

static float DisplayGpu(const App& app) {
  if (app.sample.gpu < 0.0f) return -1.0f;
  if (app.settings.gpuLuid == 0) return app.sample.gpu;
  for (int i = 0; i < app.sample.gpuCount; ++i)
    if (app.sample.gpus[i].luid == app.settings.gpuLuid) return app.sample.gpus[i].util;
  // A pinned adapter that is present but idle has no counter instances. Report
  // zero rather than silently showing a different GPU's load.
  for (const AdapterInfo& adapter : app.adapters)
    if (adapter.luid == app.settings.gpuLuid) return 0.0f;
  return app.sample.gpu;  // the selected adapter is gone (disabled): fall back
}

// Layout + painting

static void RebuildLayout(App& app) {
  float scale = app.taskbarDpi / 96.0f;
  app.renderer.SetScale(scale, ui::kLabelPx * scale, ui::kValuePx * scale);
  auto* labelFmt = app.renderer.labelFormat();
  auto* valueFmt = app.renderer.valueFormat();

  float labelW = 0.0f;
  for (const wchar_t* text : {L"CPU", L"GPU", L"RAM"})
    labelW = std::max(labelW, app.renderer.Measure(labelFmt, text).width);
  float valueW = app.renderer.Measure(valueFmt, L"100%").width;

  float netW = 0.0f;
  if (app.settings.showNet) {
    // Down is the value line, up is the label line, so both set the column.
    float down = app.renderer.Measure(valueFmt, L"\x2193 1023K").width;
    float up = app.renderer.Measure(labelFmt, L"\x2191 1023K").width;
    netW = std::max(down, up) + 1.0f;
  }

  float tempW = 0.0f;
  if (app.settings.showCpu && app.settings.showTemps && app.tempsAvailable) {
    // Three digits so 100°C and above still fit.
    TextMetrics tm = app.renderer.Measure(labelFmt, L"110\x00B0");
    tempW = tm.width + 4.0f * scale;
  }

  Layout& L = app.layout;
  L.count = 0;
  if (app.settings.showCpu) L.visible[L.count++] = 0;
  if (app.settings.showGpu) L.visible[L.count++] = 1;
  if (app.settings.showRam) L.visible[L.count++] = 2;
  if (app.settings.showNet) L.visible[L.count++] = 3;

  int colW = (int)std::ceil(std::max(labelW, valueW)) + 1;
  int cpuW = tempW > 0.0f ? (int)std::ceil(std::max(labelW + tempW, valueW)) + 1 : colW;
  int padX = Rounded(ui::kPadX * scale);
  int gapX = Rounded(ui::kGapX * scale);
  L.colW = colW;
  int x = padX;
  for (int i = 0; i < L.count; ++i) {
    const int metric = L.visible[i];
    L.xs[i] = x;
    L.colWs[i] = metric == 0 ? cpuW : metric == 3 ? (int)std::ceil(netW) : colW;
    x += L.colWs[i];
    if (i + 1 < L.count) x += gapX;
  }
  L.w = x + padX;
  L.contentH = Rounded(ui::kHeightBars * scale);
  L.h = L.contentH;
  L.offsetY = 0;
  L.bordered = !app.settings.borderless;
  L.showTemps = app.settings.showTemps;
  L.labelBaseline = Rounded(ui::kLabelBaseline * scale);
  L.valueBaseline = Rounded(ui::kValueBaseline * scale);
  L.barTop = Rounded(ui::kBarTop * scale);
  L.barH = app.settings.showBars ? std::max(1, Rounded(ui::kBarH * scale)) : 0;
  L.radius = Rounded(ui::kRadius * scale);
  app.laidOut = true;
  Log(L"layout: w=%d h=%d content=%d cols=%d colW=%d dpi=%u temps=%d borderless=%d", L.w, L.h,
      L.contentH, L.count, L.colW, app.taskbarDpi, L.showTemps ? 1 : 0, L.bordered ? 0 : 1);
}

static void UpdateAlerts(App& app) {
  const float pct[3] = {app.sample.cpu, app.gpuShown, app.sample.ram};
  const ULONGLONG now = GetTickCount64();
  for (int i = 0; i < 3; ++i) {
    App::MetricAlert& a = app.alerts[i];
    const int warn = app.settings.warnPct[i];
    const int crit = app.settings.critPct[i];
    float p = pct[i];
    if (p < 0.0f) {
      a = App::MetricAlert{};
      continue;
    }
    if (p >= (float)crit) {
      if (!a.critSince) a.critSince = now;
    } else {
      a.critSince = 0;
    }
    if (p >= (float)warn) {
      if (!a.warnSince) a.warnSince = now;
    } else {
      a.warnSince = 0;
    }

    // Release with a little hysteresis so the colour does not flicker.
    if (a.level == 2 && p <= (float)(crit - 2)) a.level = 1;
    if (a.level >= 1 && p <= (float)(warn - 2)) a.level = 0;

    // Engage only after the load has held for 1.5 s.
    if (a.level < 2 && a.critSince && now - a.critSince >= 1500) a.level = 2;
    if (a.level < 1 && a.warnSince && now - a.warnSince >= 1500) a.level = 1;
  }
}

static void PaintWidget(App& app) {
  if (!app.hwnd || !app.laidOut) return;
  const int alert[3] = {app.alerts[0].level, app.alerts[1].level, app.alerts[2].level};
  app.renderer.Paint(app.hwnd, app.x, app.y, app.layout, app.theme, app.hover, app.sample,
                     app.gpuShown, alert);
  app.lastPaintCpu = (int)std::lround(app.sample.cpu);
  app.lastPaintGpu = app.gpuShown < 0 ? -1 : (int)std::lround(app.gpuShown);
  app.lastPaintRam = (int)std::lround(app.sample.ram);
  for (int i = 0; i < 3; ++i) app.lastPaintAlert[i] = alert[i];
  app.lastPaintCpuTemp =
      app.sample.cpuTemp < 0.0f ? -999 : (int)std::lround(app.sample.cpuTemp);
  FormatRate(app.sample.downBps, app.lastNetDown, 16);
  FormatRate(app.sample.upBps, app.lastNetUp, 16);
}

bool Renderer::Paint(HWND hwnd, int x, int y, const Layout& L, const Theme& theme, bool hover,
                     const Sample& sample, float gpuShown, const int alert[3]) {
  if (!EnsureSurface(L.w, L.h)) return false;
  RECT rc{0, 0, L.w, L.h};
  if (FAILED(target_->BindDC(memoryDc_, &rc))) return false;

  target_->BeginDraw();
  target_->Clear(D2D1::ColorF(0, 0, 0, 0));

  // Per-pixel hit testing ignores fully transparent pixels, so borderless mode
  // needs a near-invisible fill to make the whole window clickable.
  if (!L.bordered) {
    brush_->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f / 255.0f));
    target_->FillRectangle(D2D1::RectF(0.0f, 0.0f, (float)L.w, (float)L.h), brush_.Get());
  }

  const float s = scale_;
  float oy = (float)L.offsetY;
  float radius = (float)L.radius;
  D2D1_ROUNDED_RECT pill{
      D2D1::RectF(0.5f, oy + 0.5f, (float)L.w - 0.5f, oy + (float)L.contentH - 0.5f), radius, radius};
  D2D1_COLOR_F bg = theme.bg;
  D2D1_COLOR_F border = theme.border;
  if (hover) {
    bg.a = std::min(1.0f, bg.a + 0.035f);
    border.a = std::min(1.0f, border.a + 0.045f);
  }
  if (L.bordered) {
    brush_->SetColor(bg);
    target_->FillRoundedRectangle(pill, brush_.Get());
    brush_->SetColor(border);
    target_->DrawRoundedRectangle(pill, brush_.Get(), std::max(1.0f, 1.0f * s));
  }

  float percents[3] = {sample.cpu, gpuShown, sample.ram};
  const wchar_t* labels[3] = {L"CPU", L"GPU", L"RAM"};
  wchar_t values[3][16] = {};
  for (int i = 0; i < 3; ++i) {
    if (percents[i] < 0.0f)
      wcscpy_s(values[i], L"\x2014");
    else
      swprintf_s(values[i], L"%d%%", (int)std::lround(percents[i]));
  }

  for (int c = 0; c < L.count; ++c) {
    int metric = L.visible[c];
    float colX = (float)L.xs[c];
    float colW = (float)L.colWs[c];
    if (metric == 3) {
      wchar_t down[16], up[16];
      FormatRate(sample.downBps, down, 16);
      FormatRate(sample.upBps, up, 16);
      const bool available = sample.downBps >= 0.0f && sample.upBps >= 0.0f;
      wchar_t upLine[24], downLine[24];
      if (available) {
        swprintf_s(upLine, L"\x2191 %ls", up);
        swprintf_s(downLine, L"\x2193 %ls", down);
      } else {
        wcscpy_s(upLine, L"\x2191 \x2014");
        wcscpy_s(downLine, L"\x2193 \x2014");
      }
      DrawTextAt(labelFormat(), upLine, colX, (float)L.labelBaseline + oy, colW, theme.label);
      DrawTextAt(valueFormat(), downLine, colX, (float)L.valueBaseline + oy, colW,
                 available ? theme.text : theme.label);
      continue;  // traffic has no percentage, so no bar
    }
    float pct = percents[metric];
    D2D1_COLOR_F valueColor = theme.text;
    if (pct < 0.0f)
      valueColor = theme.label;
    else if (alert[metric] >= 2)
      valueColor = theme.hot;
    else if (alert[metric] == 1)
      valueColor = theme.warn;
    bool tempShown = false;
    if (L.showTemps && metric == 0 && sample.cpuTemp >= 0.0f) {
      tempShown = true;
      wchar_t tbuf[8];
      swprintf_s(tbuf, L"%d\x00B0", (int)std::lround(sample.cpuTemp));
      TextMetrics lm = Measure(labelFormat(), labels[metric]);
      TextMetrics tm = Measure(labelFormat(), tbuf);
      float total = lm.width + 4.0f * s + tm.width;
      float sx = colX + (colW - total) * 0.5f;
      DrawTextAt(labelFormat(), labels[metric], sx, (float)L.labelBaseline + oy, lm.width + 2.0f,
                 theme.label);
      D2D1_COLOR_F tc = theme.label;
      tc.a *= 0.88f;
      DrawTextAt(labelFormat(), tbuf, sx + lm.width + 4.0f * s, (float)L.labelBaseline + oy,
                 tm.width + 2.0f, tc);
    }
    if (!tempShown)
      DrawTextAt(labelFormat(), labels[metric], colX, (float)L.labelBaseline + oy, colW,
                 theme.label);
    DrawTextAt(valueFormat(), values[metric], colX, (float)L.valueBaseline + oy, colW, valueColor);

    if (L.barH > 0) {
      float trackTop = (float)L.barTop + oy;
      float barW = colW;
      D2D1_ROUNDED_RECT track{D2D1::RectF(colX, trackTop, colX + barW, trackTop + L.barH),
                              L.barH / 2.0f, L.barH / 2.0f};
      brush_->SetColor(theme.track);
      target_->FillRoundedRectangle(track, brush_.Get());
      if (pct >= 0.0f) {
        float fill = barW * ClampF(pct, 0.0f, 100.0f) / 100.0f;
        if (fill > 0.5f) {
          D2D1_COLOR_F color = theme.accent;
          if (alert[metric] >= 2)
            color = theme.hot;
          else if (alert[metric] == 1)
            color = theme.warn;
          D2D1_ROUNDED_RECT done{D2D1::RectF(colX, trackTop, colX + fill, trackTop + L.barH),
                                 L.barH / 2.0f, L.barH / 2.0f};
          brush_->SetColor(color);
          target_->FillRoundedRectangle(done, brush_.Get());
        }
      }
    }
  }

  HRESULT hr = target_->EndDraw();
  if (FAILED(hr)) {
    Log(L"EndDraw failed %08lX", (unsigned long)hr);
    ReleaseSurface();
    return false;
  }
  GdiFlush();

  POINT dst{x, y};
  POINT src{0, 0};
  SIZE size{L.w, L.h};
  BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
  BOOL ok = UpdateLayeredWindow(hwnd, screen_, &dst, &size, memoryDc_, &src, 0, &blend, ULW_ALPHA);
  if (!ok) Log(L"UpdateLayeredWindow failed %lu", GetLastError());
  return ok != FALSE;
}

bool Renderer::PaintMenu(HWND hwnd, int x, int y, const Theme& theme,
                         const std::vector<MenuEntry>& items, int hoverIndex, int width,
                         int height) {
  if (!EnsureSurface(width, height)) return false;
  RECT rc{0, 0, width, height};
  if (FAILED(target_->BindDC(memoryDc_, &rc))) return false;

  const float s = scale_;
  const float shadow = menuui::kShadow * s;
  const float radius = menuui::kRadius * s;
  const D2D1_RECT_F body =
      D2D1::RectF(shadow, shadow, (float)width - shadow, (float)height - shadow);

  target_->BeginDraw();
  target_->Clear(D2D1::ColorF(0, 0, 0, 0));

  // Soft, cheap shadow: a few concentric rounded rects.
  for (int i = 5; i >= 1; --i) {
    float spread = i * 1.4f * s;
    D2D1_ROUNDED_RECT glow{
        D2D1::RectF(body.left - spread, body.top - spread + 2.0f * s, body.right + spread,
                    body.bottom + spread + 2.0f * s),
        radius + spread, radius + spread};
    D2D1_COLOR_F c = theme.shadow;
    c.a *= 0.22f;
    brush_->SetColor(c);
    target_->FillRoundedRectangle(glow, brush_.Get());
  }

  D2D1_ROUNDED_RECT pill{body, radius, radius};
  brush_->SetColor(theme.menuBg);
  target_->FillRoundedRectangle(pill, brush_.Get());
  brush_->SetColor(theme.menuBorder);
  target_->DrawRoundedRectangle(pill, brush_.Get(), std::max(1.0f, s));

  float top = body.top + menuui::kPadV * s;
  for (int i = 0; i < (int)items.size(); ++i) {
    const MenuEntry& item = items[i];
    if (item.separator) {
      float lineY = top + menuui::kSepH * s * 0.5f;
      D2D1_COLOR_F c = theme.menuBorder;
      c.a *= 0.8f;
      brush_->SetColor(c);
      target_->DrawLine(D2D1::Point2F(body.left + 12.0f * s, lineY),
                        D2D1::Point2F(body.right - 12.0f * s, lineY), brush_.Get(), std::max(1.0f, s));
      top += menuui::kSepH * s;
      continue;
    }

    const float itemH = menuui::kItemH * s;
    if (i == hoverIndex) {
      D2D1_ROUNDED_RECT hot{D2D1::RectF(body.left + 4.0f * s, top + 1.0f * s,
                                        body.right - 4.0f * s, top + itemH - 1.0f * s),
                            5.0f * s, 5.0f * s};
      brush_->SetColor(theme.menuHover);
      target_->FillRoundedRectangle(hot, brush_.Get());
    }

    const float baseline = top + itemH * 0.665f;
    if (item.checked)
      DrawTextAt(menu_.Get(), L"\x2713", body.left + 12.0f * s, baseline, 20.0f * s, theme.accent);
    DrawTextAt(menu_.Get(), item.text.c_str(), body.left + menuui::kTextX * s, baseline,
               std::max(10.0f, body.right - body.left - menuui::kTextX * s - 10.0f * s),
               item.header ? theme.label : theme.text);
    top += itemH;
  }

  HRESULT hr = target_->EndDraw();
  if (FAILED(hr)) {
    Log(L"menu EndDraw failed %08lX", (unsigned long)hr);
    ReleaseSurface();
    return false;
  }
  GdiFlush();

  POINT dst{x, y};
  POINT src{0, 0};
  SIZE size{width, height};
  BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
  BOOL ok = UpdateLayeredWindow(hwnd, screen_, &dst, &size, memoryDc_, &src, 0, &blend, ULW_ALPHA);
  if (!ok) Log(L"menu UpdateLayeredWindow failed %lu", GetLastError());
  return ok != FALSE;
}

// Taskbar geometry

static BOOL CALLBACK FindSecondaryTray(HWND hwnd, LPARAM param) {
  wchar_t cls[64];
  if (GetClassNameW(hwnd, cls, 64) && wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0) {
    *(HWND*)param = hwnd;
    return FALSE;
  }
  return TRUE;
}

static HWINEVENTHOOK g_geomHook = nullptr;
static HWND g_geomTaskbar = nullptr;
static HWND g_geomTray = nullptr;
static ULONGLONG g_geomLastPost = 0;

static void CALLBACK GeometryEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG,
                                       DWORD, DWORD) {
  if (idObject != OBJID_WINDOW) return;
  if (hwnd != g_geomTaskbar && hwnd != g_geomTray) return;
  ULONGLONG now = GetTickCount64();
  if (now - g_geomLastPost < 40) return;
  g_geomLastPost = now;
  if (g_app.hwnd) PostMessageW(g_app.hwnd, WM_APP_GEOMETRY, 0, 0);
}

static void RefreshGeometryHook(App& app) {
  if (app.taskbar != g_geomTaskbar) {
    if (g_geomHook) {
      UnhookWinEvent(g_geomHook);
      g_geomHook = nullptr;
    }
    g_geomTaskbar = app.taskbar;
    g_geomTray = app.tray;
    if (app.taskbar) {
      // Any Explorer thread can own the tray, so filter by process and check
      // the window handle in the callback.
      DWORD pid = 0;
      GetWindowThreadProcessId(app.taskbar, &pid);
      g_geomHook = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE,
                                   nullptr, GeometryEventProc, pid, 0, WINEVENT_OUTOFCONTEXT);
      Log(L"geometry hook %p pid=%lu taskbar=%p tray=%p", g_geomHook, pid, app.taskbar, app.tray);
    }
  } else {
    g_geomTray = app.tray;
  }
}

static HWINEVENTHOOK g_fgHook = nullptr;

static void CALLBACK ForegroundEventProc(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) {
  if (g_app.hwnd) PostMessageW(g_app.hwnd, WM_APP_GEOMETRY, 0, 0);
}

static void FindTaskbar(App& app) {
  if (app.taskbar && IsWindow(app.taskbar)) {
    if (!app.tray || !IsWindow(app.tray)) {
      app.tray = FindWindowExW(app.taskbar, nullptr, L"TrayNotifyWnd", nullptr);
    }
    RefreshGeometryHook(app);
    return;
  }
  app.taskbar = nullptr;
  app.tray = nullptr;
  HWND primary = FindWindowW(L"Shell_TrayWnd", nullptr);
  if (!primary) {
    HWND secondary = nullptr;
    EnumWindows(FindSecondaryTray, (LPARAM)&secondary);
    primary = secondary;
  }
  if (!primary) return;
  app.taskbar = primary;
  app.tray = FindWindowExW(primary, nullptr, L"TrayNotifyWnd", nullptr);
  UINT dpi = GetDpiForWindow(primary);
  if (dpi) app.taskbarDpi = dpi;
  RefreshGeometryHook(app);
}

static bool TaskbarVisible(App& app, const RECT& rect) {
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  if (!GetMonitorInfoW(MonitorFromWindow(app.taskbar, MONITOR_DEFAULTTONEAREST), &mi)) return true;
  RECT overlap;
  if (!IntersectRect(&overlap, &rect, &mi.rcMonitor)) return false;
  int width = rect.right - rect.left;
  int height = rect.bottom - rect.top;
  if (width > height)
    return (overlap.bottom - overlap.top) * 2 >= height;
  return (overlap.right - overlap.left) * 2 >= width;
}

static bool FullscreenOnTaskbarMonitor(App& app) {
  HWND fg = GetForegroundWindow();
  if (!fg || fg == app.hwnd || fg == app.taskbar) return false;
  // A hidden window can still be "foreground" while an app launches. Treating
  // its stale rectangle as fullscreen would hide the widget until focus moves.
  if (!IsWindowVisible(fg)) return false;
  wchar_t cls[64] = {};
  GetClassNameW(fg, cls, 64);
  // Shell surfaces sit over the taskbar during Start, Search and flyouts; they
  // are not apps that should make the widget disappear.
  static const wchar_t* kShellClasses[] = {
      L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd",
      L"ForegroundStaging", L"XamlExplorerHostIslandWindow",
      L"TopLevelWindowForOverflowXamlIsland", L"Wnd_StartFeed",
      L"Windows.UI.Core.CoreWindow"};
  for (const wchar_t* shell : kShellClasses)
    if (!wcscmp(cls, shell)) return false;
  if (!wcsncmp(cls, L"DesktopFlyoutHostClass.", 23)) return false;
  DWORD cloaked = 0;
  if (SUCCEEDED(DwmGetWindowAttribute(fg, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return false;
  RECT wr{};
  if (!GetWindowRect(fg, &wr)) return false;
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  if (!GetMonitorInfoW(MonitorFromWindow(app.taskbar, MONITOR_DEFAULTTONEAREST), &mi)) return false;
  const RECT& m = mi.rcMonitor;
  return wr.left <= m.left && wr.top <= m.top && wr.right >= m.right && wr.bottom >= m.bottom;
}

static bool TaskbarCoversWidget(App& app) {
  if (!app.taskbar || !app.hwnd || !IsWindow(app.taskbar)) return false;
  // While Start or Search is open the shell lifts the taskbar above every
  // topmost window. Walk the z-order above us (a server-side query) instead of
  // WindowFromPoint, which sends a message to whatever is under the point and
  // can hang the UI thread while an app is busy launching.
  HWND h = GetWindow(app.hwnd, GW_HWNDPREV);
  for (int guard = 0; h && guard < 256; ++guard, h = GetWindow(h, GW_HWNDPREV))
    if (h == app.taskbar) return true;
  return false;
}

// A fullscreen window can cover the taskbar without ever taking focus. Compare
// the z-order instead of asking what is under a point: if a monitor-sized window
// appears above the taskbar, the strip is hidden. EnumWindows and GetWindowRect
// are server-side and cannot block on a busy application.
struct CoverSearch {
  HWND taskbar;
  HWND self;
  RECT monitor;
  bool covered;
};

static BOOL CALLBACK CoverProc(HWND hwnd, LPARAM param) {
  auto* s = (CoverSearch*)param;
  if (hwnd == s->taskbar) {
    return FALSE;  // the taskbar is above everything else we have seen
  }
  if (!IsWindowVisible(hwnd) || hwnd == s->self) return TRUE;
  wchar_t cls[64] = {};
  GetClassNameW(hwnd, cls, 64);
  static const wchar_t* kShellClasses[] = {
      L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd",
      L"ForegroundStaging", L"XamlExplorerHostIslandWindow",
      L"TopLevelWindowForOverflowXamlIsland", L"Wnd_StartFeed",
      L"Windows.UI.Core.CoreWindow"};
  for (const wchar_t* shell : kShellClasses)
    if (!wcscmp(cls, shell)) return TRUE;
  if (!wcsncmp(cls, L"DesktopFlyoutHostClass.", 23)) return TRUE;
  DWORD cloaked = 0;
  if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked)
    return TRUE;
  RECT wr{};
  if (!GetWindowRect(hwnd, &wr)) return TRUE;
  const RECT& m = s->monitor;
  if (wr.left <= m.left && wr.top <= m.top && wr.right >= m.right && wr.bottom >= m.bottom) {
    s->covered = true;
    return FALSE;
  }
  return TRUE;
}

static bool TaskbarCoveredByFullscreen(App& app) {
  if (!app.taskbar || !IsWindow(app.taskbar)) return false;
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  if (!GetMonitorInfoW(MonitorFromWindow(app.taskbar, MONITOR_DEFAULTTONEAREST), &mi)) return false;
  CoverSearch s{app.taskbar, app.hwnd, mi.rcMonitor, false};
  EnumWindows(CoverProc, (LPARAM)&s);
  return s.covered;
}

static void ShowWidget(App& app, bool show) {
  if (show == app.visible) return;
  if (show) {
    ShowWindow(app.hwnd, SW_SHOWNOACTIVATE);
    SetWindowPos(app.hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
  } else {
    ShowWindow(app.hwnd, SW_HIDE);
  }
  app.visible = show;
}

// Virtual desktops: keep the widget on whichever desktop is in front.

static void EnsureVirtualDesktop(App& app) {
  if (!app.vdm || !app.hwnd) return;
  static ULONGLONG lastCheck = 0;
  static ULONGLONG lastFix = 0;
  ULONGLONG now = GetTickCount64();
  if (now - lastCheck < 400) return;
  lastCheck = now;
  if (now - lastFix < 1500) return;

  BOOL current = TRUE;
  if (FAILED(app.vdm->IsWindowOnCurrentVirtualDesktop(app.hwnd, &current)) || current) return;
  lastFix = now;

  // The widget is a tool window, so Windows normally keeps it visible on every
  // desktop. If it is not, move it to the desktop the foreground window is on.
  HWND fg = GetForegroundWindow();
  GUID desktop{};
  if (fg && SUCCEEDED(app.vdm->GetWindowDesktopId(fg, &desktop)) && !IsEqualGUID(desktop, GUID{})) {
    if (SUCCEEDED(app.vdm->MoveWindowToDesktop(app.hwnd, desktop)))
      Log(L"virtual desktop: moved widget to the active desktop");
  }
}

// Worker thread

static unsigned __stdcall SamplerThread(void* param) {
  App& app = *(App*)param;
  Sampler sampler;
  sampler.Prime();

  HANDLE events[2] = {app.stopEvent, app.wakeEvent};
  // Let the counters accumulate a real interval before the first sample, so
  // the widget never flashes a bogus 0%.
  WaitForMultipleObjects(2, events, FALSE, 300);

  while (true) {
    int interval = app.intervalMs.load();
    if (interval < 250) interval = 250;
    DWORD w = WaitForMultipleObjects(2, events, FALSE, (DWORD)interval);
    if (w == WAIT_OBJECT_0) break;
    if (w == WAIT_OBJECT_0 + 1) continue;  // settings changed: re-read the interval
    if (!app.samplingActive.load()) continue;

    Sample s = sampler.Next(app.tempsEnabled.load(), app.netEnabled.load(), interval);
    EnterCriticalSection(&app.sampleLock);
    app.pending = s;
    LeaveCriticalSection(&app.sampleLock);
    if (HWND target = app.sampleTarget.load()) PostMessageW(target, WM_APP_SAMPLE, 0, 0);
  }
  return 0;
}

static void StartSampler(App& app) {
  app.intervalMs.store(app.settings.intervalMs);
  app.tempsEnabled.store(app.settings.showTemps);
  app.netEnabled.store(app.settings.showNet);
  if (app.thread) {
    SetEvent(app.wakeEvent);
    return;
  }
  app.thread = (HANDLE)_beginthreadex(nullptr, 0, SamplerThread, &app, 0, nullptr);
  Log(L"sampler thread %p", app.thread);
}

static void StopSampler(App& app) {
  if (!app.thread) return;
  SetEvent(app.stopEvent);
  if (WaitForSingleObject(app.thread, 10000) == WAIT_OBJECT_0) {
    CloseHandle(app.thread);
    app.thread = nullptr;
  } else {
    Log(L"sampler did not stop in time");
  }
}

// Settings window

static void UpdatePosition(App& app, bool force);
static void PaintWidget(App& app);
static void HideMenu(App& app);

static void RefreshAdapterList(App& app) {
  std::vector<AdapterInfo> all;
  LoadAdapters(all);
  app.adapters.clear();
  auto inSample = [&](ULONGLONG luid) {
    for (int i = 0; i < app.sample.gpuCount; ++i)
      if (app.sample.gpus[i].luid == luid) return true;
    return false;
  };
  for (const AdapterInfo& candidate : all) {
    if (candidate.software) continue;  // WARP / Basic Render Driver is not a GPU
    bool merged = false;
    for (AdapterInfo& existing : app.adapters) {
      if (existing.name == candidate.name) {
        // The same physical GPU can be enumerated twice with different LUIDs.
        // Keep whichever one the performance counters actually report.
        if (!inSample(existing.luid) && inSample(candidate.luid)) existing.luid = candidate.luid;
        merged = true;
        break;
      }
    }
    if (!merged) app.adapters.push_back(candidate);
  }
}

static void BuildSettingsView(App& app) {
  SettingsView& v = app.settingsView;
  v.cpu = app.settings.showCpu;
  v.gpu = app.settings.showGpu;
  v.ram = app.settings.showRam;
  v.network = app.settings.showNet;
  v.bars = app.settings.showBars;
  v.temps = app.settings.showTemps;
  v.borderless = app.settings.borderless;
  v.locked = app.settings.locked;
  v.autostart = AutostartEnabled();
  v.intervalMs = app.settings.intervalMs;
  for (int i = 0; i < 3; ++i) {
    v.warn[i] = app.settings.warnPct[i];
    v.critical[i] = app.settings.critPct[i];
  }

  v.gpuNames.clear();
  v.gpuNames.push_back(L"Automatic");
  v.gpuIndex = 0;
  for (size_t i = 0; i < app.adapters.size(); ++i) {
    v.gpuNames.push_back(app.adapters[i].name);
    if (app.adapters[i].luid == app.settings.gpuLuid) v.gpuIndex = (int)i + 1;
  }
}

static void BuildSettingsLayout(App& app) {
  float s = app.taskbarDpi / 96.0f;
  auto P = [s](float v) { return Rounded(v * s); };
  SettingsLayout& L = app.settingsLayout;

  L.shadow = P(setui::kShadow);
  int padX = P(setui::kPadX);
  int titleH = P(setui::kTitleH);
  int sectionH = P(setui::kSectionH);
  int rowH = P(setui::kRowH);
  int gap = P(setui::kGap);
  int btn = P(setui::kButton);
  int swW = P(setui::kSwitchW);
  int swH = P(setui::kSwitchH);
  int w = P(setui::kWidth);
  int x0 = L.shadow + padX;
  int right = w - L.shadow - padX;
  int contentW = right - x0;

  int y = L.shadow + titleH + P(2);
  L.title = {0, 0, w, L.shadow + titleH};
  L.closeBtn = {right - btn, L.shadow + (titleH - btn) / 2, right,
                L.shadow + (titleH - btn) / 2 + btn};

  auto section = [&](RECT& out) {
    out = {x0, y, right, y + sectionH};
    y += sectionH;
  };
  auto row = [&]() {
    RECT r{x0, y, right, y + rowH};
    y += rowH;
    return r;
  };
  auto addGap = [&]() { y += gap; };

  section(L.secMetrics);
  RECT triple = row();
  int gpill = P(6);
  int cellpill = (contentW - gpill * 3) / 4;
  L.cpu = {triple.left, triple.top, triple.left + cellpill, triple.bottom};
  L.gpu = {triple.left + cellpill + gpill, triple.top, triple.left + cellpill * 2 + gpill,
           triple.bottom};
  L.ram = {triple.left + (cellpill + gpill) * 2, triple.top, triple.left + cellpill * 3 + gpill * 2,
           triple.bottom};
  L.net = {triple.left + (cellpill + gpill) * 3, triple.top, triple.right, triple.bottom};
  L.barsRow = row();
  L.barsSwitch = {L.barsRow.right - swW, L.barsRow.top + (rowH - swH) / 2, L.barsRow.right,
                  L.barsRow.top + (rowH - swH) / 2 + swH};
  L.tempRow = row();
  L.tempSwitch = {L.tempRow.right - swW, L.tempRow.top + (rowH - swH) / 2, L.tempRow.right,
                  L.tempRow.top + (rowH - swH) / 2 + swH};
  addGap();

  section(L.secRefresh);
  RECT rateRow = row();
  int g4 = P(6);
  int cell4 = (contentW - g4 * 3) / 4;
  for (int i = 0; i < 4; ++i) {
    int left = rateRow.left + i * (cell4 + g4);
    int r2 = (i == 3) ? rateRow.right : left + cell4;
    L.rate[i] = {left, rateRow.top, r2, rateRow.bottom};
  }
  addGap();

  section(L.secAlerts);
  RECT chipRow = row();
  int g3 = P(6);
  int cell3 = (contentW - g3 * 2) / 3;
  for (int i = 0; i < 3; ++i) {
    int left = chipRow.left + i * (cell3 + g3);
    int r2 = (i == 2) ? chipRow.right : left + cell3;
    L.alertChip[i] = {left, chipRow.top, r2, chipRow.bottom};
  }
  int valueW = P(56);
  int groupW = btn * 2 + valueW;
  L.warnRow = row();
  int bx = L.warnRow.right - groupW;
  int by = L.warnRow.top + (rowH - btn) / 2;
  L.warnMinus = {bx, by, bx + btn, by + btn};
  L.warnValue = {bx + btn, L.warnRow.top, bx + btn + valueW, L.warnRow.bottom};
  L.warnPlus = {bx + btn + valueW, by, bx + btn * 2 + valueW, by + btn};
  L.critRow = row();
  bx = L.critRow.right - groupW;
  by = L.critRow.top + (rowH - btn) / 2;
  L.critMinus = {bx, by, bx + btn, by + btn};
  L.critValue = {bx + btn, L.critRow.top, bx + btn + valueW, L.critRow.bottom};
  L.critPlus = {bx + btn + valueW, by, bx + btn * 2 + valueW, by + btn};
  addGap();

  section(L.secBehavior);
  L.lockRow = row();
  L.lockSwitch = {L.lockRow.right - swW, L.lockRow.top + (rowH - swH) / 2, L.lockRow.right,
                  L.lockRow.top + (rowH - swH) / 2 + swH};
  L.borderRow = row();
  L.borderSwitch = {L.borderRow.right - swW, L.borderRow.top + (rowH - swH) / 2, L.borderRow.right,
                    L.borderRow.top + (rowH - swH) / 2 + swH};
  L.autoRow = row();
  L.autoSwitch = {L.autoRow.right - swW, L.autoRow.top + (rowH - swH) / 2, L.autoRow.right,
                  L.autoRow.top + (rowH - swH) / 2 + swH};
  addGap();

  L.gpuSelector = app.adapters.size() > 1;
  if (L.gpuSelector) {
    section(L.secGpu);
    L.gpuRow = row();
    int arrow = P(24);
    L.gpuPrev = {L.gpuRow.left, L.gpuRow.top + (rowH - btn) / 2, L.gpuRow.left + arrow,
                 L.gpuRow.top + (rowH - btn) / 2 + btn};
    L.gpuNext = {L.gpuRow.right - arrow, L.gpuRow.top + (rowH - btn) / 2, L.gpuRow.right,
                 L.gpuRow.top + (rowH - btn) / 2 + btn};
    L.gpuName = {L.gpuRow.left + arrow + P(4), L.gpuRow.top, L.gpuRow.right - arrow - P(4),
                 L.gpuRow.bottom};
    addGap();
  }

  int footerH = P(setui::kFooterH);
  L.footer = {x0, y, right, y + footerH};
  L.reset = {L.footer.left, L.footer.top + (footerH - btn) / 2, L.footer.left + P(120),
             L.footer.top + (footerH - btn) / 2 + btn};
  L.exitBtn = {right - P(64), L.footer.top + (footerH - btn) / 2, right - P(26),
               L.footer.top + (footerH - btn) / 2 + btn};
  y += footerH;

  L.h = y + L.shadow;
  L.w = w;
  L.body = {L.shadow, L.shadow, w - L.shadow, L.h - L.shadow};
  app.settingsW = L.w;
  app.settingsH = L.h;

  app.focusOrder.clear();
  for (int id : {CTL_CLOSE, CTL_CPU, CTL_GPU, CTL_RAM, CTL_NET, CTL_BARS, CTL_TEMPS})
    app.focusOrder.push_back(id);
  for (int id : {CTL_ALERT_CPU, CTL_ALERT_GPU, CTL_ALERT_RAM})
    app.focusOrder.push_back(id);
  for (int id : {CTL_RATE0, CTL_RATE1, CTL_RATE2, CTL_RATE3})
    app.focusOrder.push_back(id);
  for (int id : {CTL_WARN_MINUS, CTL_WARN_PLUS, CTL_CRIT_MINUS, CTL_CRIT_PLUS})
    app.focusOrder.push_back(id);
  for (int id : {CTL_LOCK, CTL_BORDERLESS, CTL_AUTOSTART})
    app.focusOrder.push_back(id);
  if (L.gpuSelector) {
    app.focusOrder.push_back(CTL_GPU_PREV);
    app.focusOrder.push_back(CTL_GPU_NEXT);
  }
  app.focusOrder.push_back(CTL_RESET);
  app.focusOrder.push_back(CTL_EXIT);
  if (std::find(app.focusOrder.begin(), app.focusOrder.end(), app.focusId) == app.focusOrder.end())
    app.focusId = app.focusOrder.empty() ? 0 : app.focusOrder.front();
}

static void PaintSettingsNow(App& app) {
  if (!app.settingsOpen || !app.settingsHwnd) return;
  app.settingsView.focusId = app.focusId;
  app.settingsView.focusVisible = app.focusVisible;
  app.renderer.PaintSettings(app.settingsHwnd, app.settingsX, app.settingsY, app.theme,
                             app.settingsLayout, app.settingsView, app.settingsHover);
}

static int HitTestSettings(App& app, int x, int y) {
  const SettingsLayout& L = app.settingsLayout;
  auto in = [&](const RECT& r) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; };
  if (in(L.closeBtn)) return CTL_CLOSE;
  if (in(L.cpu)) return CTL_CPU;
  if (in(L.gpu)) return CTL_GPU;
  if (in(L.ram)) return CTL_RAM;
  if (in(L.net)) return CTL_NET;
  if (in(L.barsRow)) return CTL_BARS;
  if (in(L.tempRow)) return CTL_TEMPS;
  for (int i = 0; i < 3; ++i)
    if (in(L.alertChip[i])) return CTL_ALERT_CPU + i;
  for (int i = 0; i < 4; ++i)
    if (in(L.rate[i])) return CTL_RATE0 + i;
  if (in(L.warnMinus)) return CTL_WARN_MINUS;
  if (in(L.warnPlus)) return CTL_WARN_PLUS;
  if (in(L.critMinus)) return CTL_CRIT_MINUS;
  if (in(L.critPlus)) return CTL_CRIT_PLUS;
  if (in(L.lockRow)) return CTL_LOCK;
  if (in(L.borderRow)) return CTL_BORDERLESS;
  if (in(L.autoRow)) return CTL_AUTOSTART;
  if (L.gpuSelector) {
    if (in(L.gpuPrev)) return CTL_GPU_PREV;
    if (in(L.gpuNext)) return CTL_GPU_NEXT;
  }
  if (in(L.reset)) return CTL_RESET;
  if (in(L.exitBtn)) return CTL_EXIT;
  if (in(L.title)) return CTL_TITLE;
  return CTL_NONE;
}

static void UpdateTrayTip(App& app);

static void CommitSettings(App& app, bool layoutChanged, bool restartSampler) {
  SaveSettings(app.settings);
  if (restartSampler) StartSampler(app);
  if (layoutChanged) {
    app.laidOut = false;
    RebuildLayout(app);
    UpdatePosition(app, true);
  }
  app.gpuShown = DisplayGpu(app);
  UpdateAlerts(app);
  BuildSettingsView(app);
  PaintWidget(app);
  PaintSettingsNow(app);
  UpdateTrayTip(app);
}

static void HideSettings(App& app) {
  if (!app.settingsOpen) return;
  app.settingsOpen = false;
  ShowWindow(app.settingsHwnd, SW_HIDE);
  if (app.settingsPrevious && IsWindow(app.settingsPrevious) && app.settingsPrevious != app.hwnd)
    SetForegroundWindow(app.settingsPrevious);
}

static void SettingsCommand(App& app, int id) {
  Settings& s = app.settings;
  const int m = app.settingsView.alertMetric;
  switch (id) {
    case CTL_CLOSE:
      HideSettings(app);
      return;
    case CTL_CPU:
    case CTL_GPU:
    case CTL_RAM:
    case CTL_NET: {
      bool* target = id == CTL_CPU ? &s.showCpu
                     : id == CTL_GPU ? &s.showGpu
                     : id == CTL_RAM ? &s.showRam
                                     : &s.showNet;
      int enabled = (s.showCpu ? 1 : 0) + (s.showGpu ? 1 : 0) + (s.showRam ? 1 : 0) +
                    (s.showNet ? 1 : 0);
      if (*target && enabled <= 1) {
        MessageBeep(MB_ICONASTERISK);
        return;
      }
      *target = !*target;
      CommitSettings(app, true, true);
      return;
    }
    case CTL_BARS:
      s.showBars = !s.showBars;
      CommitSettings(app, true, false);
      return;
    case CTL_TEMPS:
      s.showTemps = !s.showTemps;
      CommitSettings(app, true, true);
      return;
    case CTL_BORDERLESS:
      s.borderless = !s.borderless;
      CommitSettings(app, true, false);
      return;
    case CTL_ALERT_CPU:
      app.settingsView.alertMetric = 0;
      PaintSettingsNow(app);
      return;
    case CTL_ALERT_GPU:
      app.settingsView.alertMetric = 1;
      PaintSettingsNow(app);
      return;
    case CTL_ALERT_RAM:
      app.settingsView.alertMetric = 2;
      PaintSettingsNow(app);
      return;
    case CTL_RATE0:
      s.intervalMs = 500;
      CommitSettings(app, false, true);
      return;
    case CTL_RATE1:
      s.intervalMs = 1000;
      CommitSettings(app, false, true);
      return;
    case CTL_RATE2:
      s.intervalMs = 2000;
      CommitSettings(app, false, true);
      return;
    case CTL_RATE3:
      s.intervalMs = 5000;
      CommitSettings(app, false, true);
      return;
    case CTL_WARN_MINUS:
      s.warnPct[m] = std::max(10, s.warnPct[m] - 5);
      if (s.critPct[m] <= s.warnPct[m]) s.critPct[m] = std::min(99, s.warnPct[m] + 5);
      CommitSettings(app, false, false);
      return;
    case CTL_WARN_PLUS:
      s.warnPct[m] = std::min(95, s.warnPct[m] + 5);
      if (s.critPct[m] <= s.warnPct[m]) s.critPct[m] = std::min(99, s.warnPct[m] + 5);
      CommitSettings(app, false, false);
      return;
    case CTL_CRIT_MINUS:
      s.critPct[m] = std::max(s.warnPct[m] + 5, s.critPct[m] - 5);
      CommitSettings(app, false, false);
      return;
    case CTL_CRIT_PLUS:
      s.critPct[m] = std::min(99, s.critPct[m] + 5);
      CommitSettings(app, false, false);
      return;
    case CTL_LOCK:
      s.locked = !s.locked;
      CommitSettings(app, false, false);
      return;
    case CTL_AUTOSTART:
      SetAutostart(!AutostartEnabled());
      BuildSettingsView(app);
      PaintSettingsNow(app);
      return;
    case CTL_GPU_PREV:
    case CTL_GPU_NEXT: {
      int n = (int)app.adapters.size();
      if (n == 0) return;
      int idx = app.settingsView.gpuIndex + (id == CTL_GPU_NEXT ? 1 : -1);
      if (idx < 0) idx = n;
      if (idx > n) idx = 0;
      s.gpuLuid = idx == 0 ? 0 : app.adapters[idx - 1].luid;
      CommitSettings(app, false, false);
      return;
    }
    case CTL_RESET:
      s.offset = 0;
      SaveSettings(s);
      UpdatePosition(app, true);
      PaintWidget(app);
      PaintSettingsNow(app);
      return;
    case CTL_EXIT:
      PostMessageW(app.hwnd, WM_CLOSE, 0, 0);
      return;
    default:
      return;
  }
}

static void TakeForeground(HWND hwnd) {
  HWND fg = GetForegroundWindow();
  DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
  DWORD myThread = GetCurrentThreadId();
  // AttachThreadInput waits on the other thread. If the foreground app is hung,
  // that would freeze Settings before it even appears; fall back instead.
  if (fgThread && fgThread != myThread && !IsHungAppWindow(fg)) {
    AttachThreadInput(myThread, fgThread, TRUE);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
    AttachThreadInput(myThread, fgThread, FALSE);
  } else {
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
  }
}

static void ShowSettings(App& app) {
  if (app.settingsOpen) {
    SetWindowPos(app.settingsHwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    ShowWindow(app.settingsHwnd, SW_SHOW);
    SetForegroundWindow(app.settingsHwnd);
    return;
  }
  HideMenu(app);
  RefreshAdapterList(app);
  BuildSettingsView(app);
  BuildSettingsLayout(app);

  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  GetMonitorInfoW(MonitorFromWindow(app.hwnd, MONITOR_DEFAULTTONEAREST), &mi);
  float s = app.taskbarDpi / 96.0f;
  int w = app.settingsW;
  int h = app.settingsH;
  int x = app.x + (app.layout.w - w) / 2;
  int y = app.visible ? app.y - h - Rounded(6.0f * s) : mi.rcWork.bottom - h - Rounded(20.0f * s);
  if (y < mi.rcWork.top + 8) y = mi.rcWork.top + 8;
  if (y + h > mi.rcWork.bottom - 8) y = mi.rcWork.bottom - h - 8;
  if (x + w > mi.rcWork.right - 8) x = mi.rcWork.right - 8 - w;
  if (x < mi.rcWork.left + 8) x = mi.rcWork.left + 8;

  app.settingsPrevious = GetForegroundWindow();
  app.settingsOpen = true;
  app.settingsHover = CTL_NONE;
  app.settingsX = x;
  app.settingsY = y;
  PaintSettingsNow(app);  // size, position and content before it becomes visible
  ShowWindow(app.settingsHwnd, SW_SHOW);
  TakeForeground(app.settingsHwnd);
  Log(L"settings shown at %d,%d %dx%d", x, y, w, h);
}

static void MoveSettingsFocus(App& app, int delta) {
  if (app.focusOrder.empty()) return;
  int index = 0;
  for (int i = 0; i < (int)app.focusOrder.size(); ++i)
    if (app.focusOrder[i] == app.focusId) {
      index = i;
      break;
    }
  const int count = (int)app.focusOrder.size();
  index = (index + delta + count) % count;
  app.focusId = app.focusOrder[index];
  app.focusVisible = true;
  PaintSettingsNow(app);
}

static LRESULT CALLBACK SettingsProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  App* app = &g_app;
  switch (msg) {
    case WM_NCCREATE:
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lParam)->lpCreateParams);
      return DefWindowProcW(hwnd, msg, wParam, lParam);
    case WM_NCHITTEST: {
      RECT r{};
      GetWindowRect(hwnd, &r);
      int s = app->settingsLayout.shadow;
      int x = GET_X_LPARAM(lParam) - r.left;
      int y = GET_Y_LPARAM(lParam) - r.top;
      // A borderless window has no system title bar, so the whole top band —
      // including the shadow margin — is the drag handle and must stay
      // clickable instead of passing through to the window underneath.
      if (y >= 0 && y < app->settingsLayout.title.bottom) return HTCLIENT;
      if (x < s || y < s || x >= (r.right - r.left) - s || y >= (r.bottom - r.top) - s)
        return HTTRANSPARENT;
      return HTCLIENT;
    }
    case WM_MOUSEMOVE: {
      // A repaint can deliver a mouse move without the cursor having moved;
      // only real movement should take the keyboard focus away.
      static POINT lastPoint{-1, -1};
      POINT here{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
      const bool moved = here.x != lastPoint.x || here.y != lastPoint.y;
      lastPoint = here;
      if (moved && app->focusVisible && !app->settingsDragging) {
        app->focusVisible = false;
        PaintSettingsNow(*app);
      }
      if (app->settingsDragging) {
        POINT p{};
        GetCursorPos(&p);
        int nx = app->settingsDragRect.left + (p.x - app->settingsDragStart.x);
        int ny = app->settingsDragRect.top + (p.y - app->settingsDragStart.y);
        SetWindowPos(hwnd, HWND_TOPMOST, nx, ny, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
        app->settingsX = nx;
        app->settingsY = ny;
        return 0;
      }
      int id = HitTestSettings(*app, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
      if (id != app->settingsHover) {
        app->settingsHover = id;
        SetCursor(LoadCursorW(nullptr, id == CTL_TITLE ? IDC_SIZEALL : IDC_ARROW));
        PaintSettingsNow(*app);
      }
      TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
      TrackMouseEvent(&track);
      return 0;
    }
    case WM_MOUSELEAVE:
      if (app->settingsHover != CTL_NONE) {
        app->settingsHover = CTL_NONE;
        PaintSettingsNow(*app);
      }
      return 0;
    case WM_LBUTTONDOWN: {
      int id = HitTestSettings(*app, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
      if (id != CTL_NONE && id != CTL_TITLE) {
        app->focusId = id;
        app->focusVisible = false;
        PaintSettingsNow(*app);
      }
      if (id == CTL_TITLE) {
        app->settingsDragging = true;
        GetCursorPos(&app->settingsDragStart);
        GetWindowRect(hwnd, &app->settingsDragRect);
        SetCapture(hwnd);
        SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
      }
      return 0;
    }
    case WM_LBUTTONUP: {
      if (app->settingsDragging) {
        app->settingsDragging = false;
        ReleaseCapture();
        return 0;
      }
      int id = HitTestSettings(*app, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
      if (id != CTL_NONE) SettingsCommand(*app, id);
      return 0;
    }
    case WM_KEYDOWN: {
      if (wParam == VK_ESCAPE) {
        HideSettings(*app);
        return 0;
      }
      if (wParam == VK_TAB) {
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (!app->focusVisible) {
          app->focusId = app->focusOrder.empty() ? 0 : app->focusOrder.front();
          app->focusVisible = true;
          PaintSettingsNow(*app);
        } else {
          MoveSettingsFocus(*app, shift ? -1 : 1);
        }
        return 0;
      }
      if (!app->focusVisible) break;

      const int f = app->focusId;
      if (wParam == VK_SPACE || wParam == VK_RETURN) {
        SettingsCommand(*app, f);
        return 0;
      }
      if (wParam == VK_LEFT || wParam == VK_RIGHT) {
        const bool right = wParam == VK_RIGHT;
        if (f == CTL_WARN_MINUS || f == CTL_WARN_PLUS) {
          SettingsCommand(*app, right ? CTL_WARN_PLUS : CTL_WARN_MINUS);
        } else if (f == CTL_CRIT_MINUS || f == CTL_CRIT_PLUS) {
          SettingsCommand(*app, right ? CTL_CRIT_PLUS : CTL_CRIT_MINUS);
        } else if (f >= CTL_RATE0 && f <= CTL_RATE3) {
          int index = std::clamp(f - CTL_RATE0 + (right ? 1 : -1), 0, 3);
          app->focusId = CTL_RATE0 + index;
          SettingsCommand(*app, app->focusId);
        } else if (f >= CTL_ALERT_CPU && f <= CTL_ALERT_RAM) {
          int index = std::clamp(f - CTL_ALERT_CPU + (right ? 1 : -1), 0, 2);
          app->focusId = CTL_ALERT_CPU + index;
          SettingsCommand(*app, app->focusId);
        }
        return 0;
      }
      break;
    }
    case WM_ACTIVATE:
      // Stay above other windows while active, then behave like a normal
      // window so it does not hover over whatever the user switches to.
      SetWindowPos(hwnd, LOWORD(wParam) == WA_INACTIVE ? HWND_NOTOPMOST : HWND_TOPMOST, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
      return 0;
    case WM_CAPTURECHANGED:
      if (app->settingsDragging) app->settingsDragging = false;
      return 0;
    case WM_CLOSE:
      HideSettings(*app);
      return 0;
    case WM_ERASEBKGND:
      return 1;
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// Tray icon + menu

static void UpdateTrayTip(App& app) {
  auto tempSuffix = [](float t) {
    std::wstring s;
    if (t >= 0.0f) {
      wchar_t b[16];
      swprintf_s(b, L" %d\x00B0", (int)std::lround(t));
      s = b;
    }
    return s;
  };
  wchar_t gpuText[16];
  if (app.gpuShown < 0.0f)
    wcscpy_s(gpuText, L"\x2014");
  else
    swprintf_s(gpuText, L"%d%%", (int)std::lround(app.gpuShown));

  const bool temps = app.settings.showTemps;
  wchar_t netLine[64] = {};
  if (app.settings.showNet && app.sample.downBps >= 0.0f && app.sample.upBps >= 0.0f) {
    wchar_t down[16], up[16];
    FormatRate(app.sample.downBps, down, 16);
    FormatRate(app.sample.upBps, up, 16);
    swprintf_s(netLine, L"\nNET \x2193 %ls/s  \x2191 %ls/s", down, up);
  }
  wchar_t line[192];
  swprintf_s(line, L"Taskbar Pulse %ls\nCPU %d%%%ls  GPU %ls  RAM %d%%%ls", kVersion,
             (int)std::lround(app.sample.cpu), temps ? tempSuffix(app.sample.cpuTemp).c_str() : L"",
             gpuText, (int)std::lround(app.sample.ram), netLine);
  if (app.trayTip == line) return;
  app.trayTip = line;

  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof(nid);
  nid.hWnd = app.hwnd;
  nid.uID = 1;
  nid.uFlags = NIF_TIP;
  wcscpy_s(nid.szTip, line);
  Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void AddTrayIcon(App& app) {
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof(nid);
  nid.hWnd = app.hwnd;
  nid.uID = 1;
  nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
  nid.uCallbackMessage = WM_APP_TRAY;
  if (!app.trayIcon) {
    int size = MulDiv(16, (int)app.taskbarDpi, 96);
    if (size < 16) size = 16;
    app.trayIcon = (HICON)LoadImageW(app.instance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, size, size,
                                     LR_DEFAULTCOLOR);
  }
  nid.hIcon = app.trayIcon;
  wcscpy_s(nid.szTip, kAppName);
  app.trayTip = kAppName;
  Shell_NotifyIconW(NIM_ADD, &nid);
  nid.uVersion = NOTIFYICON_VERSION_4;
  Shell_NotifyIconW(NIM_SETVERSION, &nid);
}

static void RemoveTrayIcon(App& app) {
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof(nid);
  nid.hWnd = app.hwnd;
  nid.uID = 1;
  Shell_NotifyIconW(NIM_DELETE, &nid);
}

static void UpdatePosition(App& app, bool force);

static void ApplySettings(App& app, bool save) {
  if (save) SaveSettings(app.settings);
  app.laidOut = false;
  RebuildLayout(app);
  StartSampler(app);
  app.gpuShown = DisplayGpu(app);
  UpdatePosition(app, true);
  if (app.settingsOpen) PaintSettingsNow(app);
}

static void HandleCommand(App& app, UINT cmd) {
  switch (cmd) {
    case IDM_CPU:
    case IDM_GPU:
    case IDM_RAM:
    case IDM_NET: {
      int enabled = (app.settings.showCpu ? 1 : 0) + (app.settings.showGpu ? 1 : 0) +
                    (app.settings.showRam ? 1 : 0) + (app.settings.showNet ? 1 : 0);
      bool* target = cmd == IDM_CPU ? &app.settings.showCpu
                     : cmd == IDM_GPU ? &app.settings.showGpu
                     : cmd == IDM_RAM ? &app.settings.showRam
                                      : &app.settings.showNet;
      if (*target && enabled <= 1) {
        MessageBeep(MB_ICONASTERISK);
      } else {
        *target = !*target;
        ApplySettings(app, true);
      }
      break;
    }
    case IDM_BARS:
      app.settings.showBars = !app.settings.showBars;
      ApplySettings(app, true);
      break;
    case IDM_SPEED:
      app.settings.intervalMs = app.settings.intervalMs == 500    ? 1000
                                : app.settings.intervalMs == 1000 ? 2000
                                : app.settings.intervalMs == 2000 ? 5000
                                                                  : 500;
      ApplySettings(app, true);
      break;
    case IDM_AUTOSTART:
      SetAutostart(!AutostartEnabled());
      break;
    case IDM_SETTINGS:
      ShowSettings(app);
      break;
    case IDM_LOCK:
      app.settings.locked = !app.settings.locked;
      CommitSettings(app, false, false);
      break;
    case IDM_EXIT:
      PostMessageW(app.hwnd, WM_CLOSE, 0, 0);
      break;
    default:
      break;
  }
}

static void BuildMenuItems(App& app) {
  auto& v = app.menuItems;
  v.clear();
  v.push_back({false, false, IDM_SETTINGS, L"Settings\x2026", false});
  v.push_back({true});
  v.push_back({false, false, IDM_CPU, L"CPU", app.settings.showCpu});
  v.push_back({false, false, IDM_GPU, L"GPU", app.settings.showGpu});
  v.push_back({false, false, IDM_RAM, L"RAM", app.settings.showRam});
  v.push_back({false, false, IDM_NET, L"Network", app.settings.showNet});
  v.push_back({false, false, IDM_BARS, L"Show load bars", app.settings.showBars});
  v.push_back({true});
  wchar_t speed[64];
  swprintf_s(speed, L"Refresh rate: %ls", app.settings.intervalMs == 500    ? L"0.5 s"
                                          : app.settings.intervalMs == 1000 ? L"1 s"
                                          : app.settings.intervalMs == 2000 ? L"2 s"
                                                                            : L"5 s");
  v.push_back({false, false, IDM_SPEED, speed, false});
  v.push_back({false, false, IDM_LOCK, L"Lock position", app.settings.locked});
  v.push_back({false, false, IDM_AUTOSTART, L"Start with Windows", AutostartEnabled()});
  v.push_back({true});
  v.push_back({false, false, IDM_EXIT, L"Exit", false});
}

static void PaintMenuNow(App& app) {
  if (!app.menuOpen || !app.menuHwnd) return;
  app.renderer.PaintMenu(app.menuHwnd, app.menuRect.left, app.menuRect.top, app.theme, app.menuItems,
                         app.menuHover, app.menuW, app.menuH);
}

static void HideMenu(App& app) {
  if (!app.menuOpen) return;
  app.menuOpen = false;
  app.menuHover = -1;
  app.menuButtonGuard = false;
  if (app.menuHwnd) {
    KillTimer(app.menuHwnd, 2);
    ShowWindow(app.menuHwnd, SW_HIDE);
  }
}

static int HitTestMenu(App& app, int y) {
  float scale = app.taskbarDpi / 96.0f;
  int top = Rounded(menuui::kShadow * scale) + Rounded(menuui::kPadV * scale);
  for (int i = 0; i < (int)app.menuItems.size(); ++i) {
    const MenuEntry& item = app.menuItems[i];
    if (item.separator) {
      top += Rounded(menuui::kSepH * scale);
      continue;
    }
    int h = Rounded(menuui::kItemH * scale);
    if (y >= top && y < top + h) return i;
    top += h;
  }
  return -1;
}

static void ShowMenu(App& app, bool atCursor) {
  if (app.menuOpen) {
    HideMenu(app);
    return;
  }
  if (!app.menuHwnd) return;

  BuildMenuItems(app);
  float scale = app.taskbarDpi / 96.0f;
  float textW = 0.0f;
  for (const MenuEntry& item : app.menuItems) {
    if (item.separator) continue;
    textW = std::max(textW, app.renderer.Measure(app.renderer.menuFormat(), item.text.c_str()).width);
  }
  int shadow = Rounded(menuui::kShadow * scale);
  int bodyW = std::max(Rounded(menuui::kMinWidth * scale),
                       Rounded(textW + (menuui::kTextX + menuui::kRightPad) * scale));
  int bodyH = Rounded(menuui::kPadV * 2.0f * scale);
  for (const MenuEntry& item : app.menuItems)
    bodyH += Rounded((item.separator ? menuui::kSepH : menuui::kItemH) * scale);

  app.menuW = bodyW + shadow * 2;
  app.menuH = bodyH + shadow * 2;

  POINT pt{};
  GetCursorPos(&pt);
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
  int x = 0, y = 0;
  if (atCursor) {
    x = pt.x - bodyW - shadow;
    y = pt.y - bodyH - shadow;
  } else {
    x = app.x + app.layout.w - bodyW - shadow;
    y = app.y - bodyH - shadow - Rounded(6.0f * scale);
  }
  if (x + app.menuW > mi.rcWork.right - 4) x = mi.rcWork.right - app.menuW - 4;
  if (x < mi.rcWork.left + 4) x = mi.rcWork.left + 4;
  if (y + app.menuH > mi.rcWork.bottom - 4) y = mi.rcWork.bottom - app.menuH - 4;
  if (y < mi.rcWork.top + 4) y = mi.rcWork.top + 4;

  app.menuRect = {x + shadow, y + shadow, x + shadow + bodyW, y + shadow + bodyH};
  app.menuHover = -1;
  app.menuOpen = true;
  // The click that opened the menu may still be held down. Ignore button
  // state until it is released, otherwise the dismissal check closes it at once.
  app.menuButtonGuard = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 ||
                        (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
  PaintMenuNow(app);  // sets size, position and content while still hidden
  ShowWindow(app.menuHwnd, SW_SHOWNOACTIVATE);
  SetWindowPos(app.menuHwnd, HWND_TOPMOST, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
  SetTimer(app.menuHwnd, 2, 70, nullptr);
  Log(L"menu shown at %d,%d %dx%d", x, y, app.menuW, app.menuH);
}

static LRESULT CALLBACK MenuProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  App* app = &g_app;
  switch (msg) {
    case WM_NCCREATE:
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lParam)->lpCreateParams);
      return DefWindowProcW(hwnd, msg, wParam, lParam);
    case WM_NCHITTEST: {
      POINT p{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
      RECT r{};
      GetWindowRect(hwnd, &r);
      float scale = app->taskbarDpi / 96.0f;
      int s = Rounded(menuui::kShadow * scale);
      int x = p.x - r.left;
      int y = p.y - r.top;
      if (x < s || y < s || x >= (r.right - r.left) - s || y >= (r.bottom - r.top) - s)
        return HTTRANSPARENT;
      return HTCLIENT;
    }
    case WM_MOUSEMOVE: {
      int idx = HitTestMenu(*app, GET_Y_LPARAM(lParam));
      if (idx != app->menuHover) {
        app->menuHover = idx;
        PaintMenuNow(*app);
      }
      TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
      TrackMouseEvent(&track);
      return 0;
    }
    case WM_MOUSELEAVE:
      if (app->menuHover != -1) {
        app->menuHover = -1;
        PaintMenuNow(*app);
      }
      return 0;
    case WM_LBUTTONUP: {
      int idx = HitTestMenu(*app, GET_Y_LPARAM(lParam));
      if (idx >= 0) {
        UINT id = app->menuItems[idx].id;
        HideMenu(*app);
        HandleCommand(*app, id);
      }
      return 0;
    }
    case WM_RBUTTONUP:
      HideMenu(*app);
      return 0;
    case WM_TIMER:
      if (wParam == 2) {
        if (!app->visible) {
          HideMenu(*app);
          return 0;
        }
        bool left = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        bool right = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
        bool escape = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
        POINT p{};
        GetCursorPos(&p);
        if (escape) {
          HideMenu(*app);
          return 0;
        }
        if (app->menuButtonGuard) {
          if (!left && !right) app->menuButtonGuard = false;
          return 0;
        }
        if ((left || right) && !PtInRect(&app->menuRect, p)) HideMenu(*app);
      }
      return 0;
    case WM_ERASEBKGND:
      return 1;
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// Position + visibility

static void UpdatePosition(App& app, bool force) {
  FindTaskbar(app);
  if (!app.taskbar) {
    ShowWidget(app, false);
    return;
  }
  RECT taskbarRect{};
  if (!GetWindowRect(app.taskbar, &taskbarRect)) {
    Log(L"taskbar rect failed handle=%p valid=%d", app.taskbar, IsWindow(app.taskbar));
    return;
  }

  UINT dpi = GetDpiForWindow(app.taskbar);
  if (dpi == 0) dpi = 96;
  if (dpi != app.taskbarDpi) {
    app.taskbarDpi = dpi;
    app.laidOut = false;
  }
  if (!app.laidOut) RebuildLayout(app);

  Layout& L = app.layout;
  float scale = app.taskbarDpi / 96.0f;
  if (app.settings.borderless) {
    int full = (taskbarRect.bottom - taskbarRect.top) - Rounded(6.0f * scale);
    if (full < L.contentH) full = L.contentH;
    L.h = full;
    L.offsetY = (L.h - L.contentH) / 2;
  } else {
    L.h = L.contentH;
    L.offsetY = 0;
  }
  bool horizontal = (taskbarRect.right - taskbarRect.left) >= (taskbarRect.bottom - taskbarRect.top);

  int x = 0, y = 0;
  if (horizontal) {
    RECT trayRect{};
    int rightEdge = taskbarRect.right - Rounded(180.0f * scale);
    if (app.tray && GetWindowRect(app.tray, &trayRect)) rightEdge = trayRect.left;
    int gap = Rounded((ui::kGapFromTray + app.settings.offset) * scale);
    x = rightEdge - gap - L.w;
    y = taskbarRect.top + (taskbarRect.bottom - taskbarRect.top - L.h) / 2;
  } else {
    RECT trayRect{};
    int topEdge = taskbarRect.top + Rounded(180.0f * scale);
    if (app.tray && GetWindowRect(app.tray, &trayRect)) topEdge = trayRect.bottom;
    int gap = Rounded((ui::kGapFromTray + app.settings.offset) * scale);
    y = topEdge + gap;
    x = taskbarRect.left + (taskbarRect.right - taskbarRect.left - L.w) / 2;
  }

  int minX = taskbarRect.left + 4;
  int maxX = taskbarRect.right - L.w - 4;
  if (x < minX) x = minX;
  if (x > maxX) x = maxX;
  int minY = taskbarRect.top + 2;
  int maxY = taskbarRect.bottom - L.h - 2;
  if (y < minY) y = minY;
  if (y > maxY) y = maxY;

  bool changed = force || x != app.x || y != app.y || L.w != app.ow || L.h != app.oh || !app.visible;
  app.x = x;
  app.y = y;
  app.ow = L.w;
  app.oh = L.h;

  bool taskbarReady = TaskbarVisible(app, taskbarRect) && IsWindowVisible(app.taskbar) &&
                      !FullscreenOnTaskbarMonitor(app) && !TaskbarCoveredByFullscreen(app);
  bool show = taskbarReady && app.hasSample;
  app.samplingActive.store(taskbarReady);

  if (!show) {
    if (app.visible) ShowWidget(app, false);
    return;
  }

  if (changed) {
    PaintWidget(app);
    SetWindowPos(app.hwnd, HWND_TOPMOST, x, y, L.w, L.h, SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    if (!app.visible) {
      EnsureVirtualDesktop(app);
      ShowWidget(app, true);
    }
  } else if (TaskbarCoversWidget(app)) {
    // Fails harmlessly while Start or Search is open, succeeds as soon as the
    // shell drops the taskbar again. Throttled so the shell animation is not
    // fighting us four times a second.
    static ULONGLONG lastTry = 0;
    ULONGLONG now = GetTickCount64();
    if (now - lastTry >= 500) {
      lastTry = now;
      SetWindowPos(app.hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }
  }
}

// Window procedure

static void OnTrayMessage(App& app, LPARAM lParam) {
  UINT event = LOWORD(lParam);
  if (event == NIN_KEYSELECT) {
    ShowSettings(app);
    return;
  }
  // Version 4 sends WM_CONTEXTMENU; older shells send WM_RBUTTONUP. Handle
  // both, but ignore a duplicate within the same click.
  if (event == WM_CONTEXTMENU || event == WM_RBUTTONUP) {
    static ULONGLONG lastOpen = 0;
    ULONGLONG now = GetTickCount64();
    if (now - lastOpen < 250) return;
    lastOpen = now;
    ShowMenu(app, true);
  }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  App* app = &g_app;
  switch (msg) {
    case WM_NCCREATE: {
      auto* cs = (CREATESTRUCTW*)lParam;
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
      return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    case WM_NCHITTEST: {
      POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
      RECT rect{};
      GetWindowRect(hwnd, &rect);
      int lx = pt.x - rect.left;
      int ly = pt.y - rect.top;
      if (lx >= 0 && ly >= 0 && lx < app->layout.w && ly < app->layout.h) return HTCLIENT;
      return HTTRANSPARENT;
    }
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;
    case WM_MOUSEMOVE: {
      if (!app->hover) {
        app->hover = true;
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&track);
        PaintWidget(*app);
      }
      if (app->dragging) {
        POINT pt{};
        GetCursorPos(&pt);
        int delta = (app->dragStartCursorX - pt.x);
        int moved = delta < 0 ? -delta : delta;
        if (moved > app->dragMoved) app->dragMoved = moved;
        if (app->settings.locked) return 0;
        float scale = app->taskbarDpi / 96.0f;
        int offset = app->dragStartOffset + Rounded(delta / scale);
        if (offset < 0) offset = 0;
        if (offset > 4000) offset = 4000;
        if (offset != app->settings.offset) {
          app->settings.offset = offset;
          UpdatePosition(*app, true);
        }
      }
      return 0;
    }
    case WM_MOUSELEAVE:
      app->hover = false;
      if (!app->dragging) PaintWidget(*app);
      return 0;
    case WM_LBUTTONDOWN:
      HideMenu(*app);
      SetCapture(hwnd);
      app->dragging = true;
      app->dragMoved = 0;
      app->dragStartOffset = app->settings.offset;
      app->dragStartCursorX = GET_X_LPARAM(lParam) + app->x;
      if (!app->settings.locked) SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
      return 0;
    case WM_LBUTTONUP:
      if (app->dragging) {
        ReleaseCapture();
        app->dragging = false;
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        if (app->dragMoved >= Rounded(4.0f * (app->taskbarDpi / 96.0f)) && !app->settings.locked)
          SaveSettings(app->settings);
      }
      return 0;
    case WM_CAPTURECHANGED:
      if (app->dragging) {
        app->dragging = false;
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
      }
      return 0;
    case WM_RBUTTONUP:
      ShowMenu(*app, false);
      return 0;
    case WM_SETCURSOR:
      if (LOWORD(lParam) == HTCLIENT) {
        POINT pt{};
        GetCursorPos(&pt);
        RECT rect{};
        GetWindowRect(hwnd, &rect);
        SetCursor(LoadCursorW(nullptr, (!app->settings.locked && PtInRect(&rect, pt)) ? IDC_SIZEWE
                                                                                       : IDC_ARROW));
        return TRUE;
      }
      break;
    case WM_TIMER:
      if (wParam == 1) {
        static int themeTick = 0;
        if (++themeTick >= 8) {
          themeTick = 0;
          Theme fresh = LoadTheme();
          if (fresh.light != app->theme.light || fresh.accent.r != app->theme.accent.r ||
              fresh.accent.g != app->theme.accent.g || fresh.accent.b != app->theme.accent.b) {
            app->theme = fresh;
            PaintWidget(*app);
            PaintSettingsNow(*app);
            if (app->menuOpen) PaintMenuNow(*app);
          }
        }
        EnsureVirtualDesktop(*app);
        UpdatePosition(*app, false);
      }
      return 0;
    case WM_APP_SAMPLE: {
      Sample s;
      EnterCriticalSection(&app->sampleLock);
      s = app->pending;
      LeaveCriticalSection(&app->sampleLock);
      app->sample = s;
      app->gpuShown = DisplayGpu(*app);
      UpdateAlerts(*app);
      app->hasSample = true;

      const bool anyTemp = s.cpuTemp >= 0.0f;
      if (app->settings.showTemps && anyTemp != app->tempsAvailable) {
        app->tempsAvailable = anyTemp;
        app->laidOut = false;
        UpdatePosition(*app, true);
      }

      if (!app->visible) UpdatePosition(*app, false);
      if (app->visible) {
        int pc = (int)std::lround(app->sample.cpu);
        int pg = app->gpuShown < 0 ? -1 : (int)std::lround(app->gpuShown);
        int pr = (int)std::lround(app->sample.ram);
        bool alertChanged = false;
        for (int i = 0; i < 3; ++i)
          if (app->alerts[i].level != app->lastPaintAlert[i]) alertChanged = true;
        bool tempChanged = false;
        if (app->settings.showTemps) {
          int ti = s.cpuTemp < 0.0f ? -999 : (int)std::lround(s.cpuTemp);
          if (ti != app->lastPaintCpuTemp) tempChanged = true;
        }
        bool netChanged = false;
        if (app->settings.showNet) {
          wchar_t down[16], up[16];
          FormatRate(s.downBps, down, 16);
          FormatRate(s.upBps, up, 16);
          netChanged = wcscmp(down, app->lastNetDown) != 0 || wcscmp(up, app->lastNetUp) != 0;
        }
        if (pc != app->lastPaintCpu || pg != app->lastPaintGpu || pr != app->lastPaintRam ||
            alertChanged || tempChanged || netChanged)
          PaintWidget(*app);
      }
      UpdateTrayTip(*app);
      return 0;
    }
    case WM_APP_GEOMETRY:
      UpdatePosition(*app, false);
      return 0;
    case WM_APP_TRAY:
      OnTrayMessage(*app, lParam);
      return 0;
    case WM_DPICHANGED:
    case WM_DISPLAYCHANGE:
      app->laidOut = false;
      if (app->settingsOpen) {
        HideSettings(*app);
        ShowSettings(*app);
      }
      UpdatePosition(*app, true);
      return 0;
    case WM_SETTINGCHANGE:
      if (lParam && !wcscmp((const wchar_t*)lParam, L"ImmersiveColorSet")) {
        app->theme = LoadTheme();
        PaintWidget(*app);
        PaintSettingsNow(*app);
        if (app->menuOpen) PaintMenuNow(*app);
      }
      return 0;
    case WM_POWERBROADCAST:
      if (wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND) {
        StartSampler(*app);
        UpdatePosition(*app, true);
      }
      return TRUE;
    case WM_PAINT: {
      PAINTSTRUCT ps{};
      BeginPaint(hwnd, &ps);
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_CLOSE:
      HideSettings(*app);
      HideMenu(*app);
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }

  static UINT taskbarCreated = 0;
  if (!taskbarCreated) taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
  if (msg == taskbarCreated) {
    Log(L"TaskbarCreated");
    app->taskbar = nullptr;
    app->tray = nullptr;
    AddTrayIcon(*app);
    app->visible = false;
    UpdatePosition(*app, true);
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// Entry point

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
  if (wcsstr(GetCommandLineW(), L"--debug")) g_debug = true;
  Log(L"start build"); LogMem(L"start");
  SetProcessDpiAwarenessContext((DPI_AWARENESS_CONTEXT)-4);
  Log(L"dpi set");

  HANDLE mutex = CreateMutexW(nullptr, TRUE, L"TaskbarPulse.SingleInstance");
  if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) return 0;

  HRESULT hrCom = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  Log(L"com %08lX", (unsigned long)hrCom);
  Log(L"dark menus");

  App& app = g_app;
  app.instance = instance;
  SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
  InitializeCriticalSection(&app.sampleLock);
  app.stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  app.wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

  LoadSettings(app.settings);
  Log(L"settings loaded");
  // First run: start with Windows so the widget is there after a reboot.
  bool firstRun = GetFileAttributesW(ConfigPath().c_str()) == INVALID_FILE_ATTRIBUTES;
  if (firstRun) SetAutostart(true);
  app.theme = LoadTheme();
  LoadAdapters(app.adapters);
  Log(L"adapters %zu", app.adapters.size());
  if (AutostartEnabled()) SetAutostart(true);  // refresh the path if the exe moved

  CoCreateInstance(CLSID_VirtualDesktopManager, nullptr, CLSCTX_ALL, IID_PPV_ARGS(app.vdm.GetAddressOf()));
  Log(L"vdm %p", app.vdm.Get());

  app.screen = GetDC(nullptr);
  if (!app.renderer.Init(app.screen)) return 1;
  Log(L"renderer"); LogMem(L"after renderer");

  WNDCLASSEXW wc{sizeof(wc)};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = instance;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.lpszClassName = kWndClass;
  if (!RegisterClassExW(&wc)) return 1;
  Log(L"class");

  WNDCLASSEXW mc{sizeof(mc)};
  mc.lpfnWndProc = MenuProc;
  mc.hInstance = instance;
  mc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  mc.lpszClassName = L"TaskbarPulse.Menu";
  if (!RegisterClassExW(&mc)) return 1;

  WNDCLASSEXW sc{sizeof(sc)};
  sc.lpfnWndProc = SettingsProc;
  sc.hInstance = instance;
  sc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  sc.lpszClassName = L"TaskbarPulse.Settings";
  if (!RegisterClassExW(&sc)) return 1;

  app.hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
                             kWndClass, kAppName, WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, instance,
                             &app);
  if (!app.hwnd) return 1;
  app.sampleTarget.store(app.hwnd);
  Log(L"window %p", app.hwnd); LogMem(L"after window");

  app.menuHwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
                                 L"TaskbarPulse.Menu", L"", WS_POPUP, 0, 0, 10, 10, nullptr,
                                 nullptr, instance, &app);
  if (!app.menuHwnd) return 1;

  app.settingsHwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                                     L"TaskbarPulse.Settings", L"Taskbar Pulse", WS_POPUP, 0, 0, 10,
                                     10, nullptr, nullptr, instance, &app);
  if (!app.settingsHwnd) return 1;
  g_fgHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
                             ForegroundEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
  app.taskbarDpi = 96;
  FindTaskbar(app);
  Log(L"taskbar %p tray %p dpi %u", app.taskbar, app.tray, app.taskbarDpi);
  RebuildLayout(app);
  Log(L"layout done"); LogMem(L"after layout");
  AddTrayIcon(app);
  Log(L"tray");
  StartSampler(app);
  Log(L"sampler");

  // Wait for the first sample before showing, so the first frame is accurate.
  UpdatePosition(app, true);
  Log(L"positioned %d,%d %dx%d", app.x, app.y, app.layout.w, app.layout.h);
  LogMem(L"after first paint");

  SetTimer(app.hwnd, 1, 250, nullptr);
  Log(L"entering message loop");
  LogMem(L"loop start");

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  StopSampler(app);
  RemoveTrayIcon(app);
  KillTimer(app.hwnd, 1);
  if (g_geomHook) {
    UnhookWinEvent(g_geomHook);
    g_geomHook = nullptr;
  }
  if (g_fgHook) {
    UnhookWinEvent(g_fgHook);
    g_fgHook = nullptr;
  }
  if (app.menuHwnd) DestroyWindow(app.menuHwnd);
  if (app.settingsHwnd) DestroyWindow(app.settingsHwnd);
  if (app.hwnd) DestroyWindow(app.hwnd);
  if (app.trayIcon) DestroyIcon(app.trayIcon);
  if (!app.thread) DeleteCriticalSection(&app.sampleLock);
  if (mutex) CloseHandle(mutex);
  CoUninitialize();
  return 0;
}












