#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <mutex>
#include <atomic>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(linker, "/SUBSYSTEM:WINDOWS")

// ==================== UTF-8 -> Wide 转换 ====================
std::wstring W(const char* utf8) {
    if (!utf8 || !*utf8) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (len <= 0) return L"";
    std::wstring result(len - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &result[0], len);
    return result;
}
std::wstring W(int n) { return std::to_wstring(n); }
std::wstring W(unsigned long long n) { return std::to_wstring(n); }

// ==================== 常量 ====================
static const wchar_t* WINDOW_CLASS = L"AutoEnterSchedulerClass";
static const wchar_t* APP_TITLE    = L"AutoEnterScheduler v2.0";

#define ID_BTN_REFRESH    1001
#define ID_BTN_FILTER     1002
#define ID_BTN_SELECT_ALL 1003
#define ID_BTN_CLEAR      1004
#define ID_BTN_START      1005
#define ID_BTN_STOP       1006
#define ID_BTN_TEST       1007
#define ID_BTN_NOW        1008
#define ID_LIST_WINDOWS   1009
#define ID_EDIT_TIME      1010
#define ID_EDIT_COUNT     1011
#define ID_EDIT_INTERVAL  1012
#define ID_EDIT_GAP       1013
#define ID_EDIT_FILTER    1014
#define ID_LABEL_STATUS   1015
#define ID_LABEL_NEXT     1016
#define ID_LABEL_SELECTED 1017
#define ID_LOG_TEXT       1018

// ==================== 数据结构 ====================
struct WindowInfo {
    HWND hwnd;
    std::wstring title;
    std::wstring appName;
    HICON hIcon;
};

// ==================== 全局变量 ====================
HINSTANCE g_hInst = NULL;
HWND g_hMainWnd = NULL;

HWND g_hListView = NULL;
HWND g_hEditTime = NULL;
HWND g_hEditCount = NULL;
HWND g_hEditInterval = NULL;
HWND g_hEditGap = NULL;
HWND g_hEditFilter = NULL;
HWND g_hBtnRefresh = NULL;
HWND g_hBtnSelectAll = NULL;
HWND g_hBtnClear = NULL;
HWND g_hBtnStart = NULL;
HWND g_hBtnStop = NULL;
HWND g_hBtnTest = NULL;
HWND g_hBtnNow = NULL;
HWND g_hLabelStatus = NULL;
HWND g_hLabelNext = NULL;
HWND g_hLabelSelected = NULL;
HWND g_hLogText = NULL;

HIMAGELIST g_hImageList = NULL;
std::vector<WindowInfo> g_windows;
std::vector<int> g_selectedIndices;

std::atomic<bool> g_running(false);
std::thread g_schedulerThread;
std::mutex g_logMutex;

// ==================== 工具函数 ====================
std::wstring FormatTime(int h, int m) {
    wchar_t buf[16];
    swprintf_s(buf, L"%02d:%02d", h, m);
    return buf;
}

std::wstring GetCurrentTimeStr() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto tm = *std::localtime(&time);
    wchar_t buf[16];
    swprintf_s(buf, L"%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

void LogMessage(const std::wstring& msg) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    std::wstring line = L"[" + GetCurrentTimeStr() + L"] " + msg + L"\r\n";
    if (g_hLogText) {
        int len = GetWindowTextLength(g_hLogText);
        SendMessage(g_hLogText, EM_SETSEL, len, len);
        SendMessage(g_hLogText, EM_REPLACESEL, FALSE, (LPARAM)line.c_str());
    }
}

// 获取窗口图标
HICON GetWindowIcon(HWND hwnd) {
    // 先尝试大图标
    HICON hIcon = NULL;
    SendMessageTimeoutW(hwnd, WM_GETICON, ICON_BIG, 0, SMTO_ABORTIFHUNG, 100, (PDWORD_PTR)&hIcon);
    if (hIcon) return hIcon;

    // 再尝试小图标
    SendMessageTimeoutW(hwnd, WM_GETICON, ICON_SMALL, 0, SMTO_ABORTIFHUNG, 100, (PDWORD_PTR)&hIcon);
    if (hIcon) return hIcon;

    // 从类获取
    hIcon = (HICON)GetClassLongPtrW(hwnd, GCLP_HICON);
    if (hIcon) return hIcon;

    hIcon = (HICON)GetClassLongPtrW(hwnd, GCLP_HICONSM);
    if (hIcon) return hIcon;

    // 最后用 exe 文件图标
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (hProc) {
        WCHAR exePath[MAX_PATH] = {0};
        DWORD size = MAX_PATH;
        if (QueryFullProcessImageNameW(hProc, 0, exePath, &size)) {
            SHFILEINFOW sfi = {0};
            SHGetFileInfoW(exePath, 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_SMALLICON);
            hIcon = sfi.hIcon;
        }
        CloseHandle(hProc);
    }
    return hIcon;
}

// ==================== 窗口枚举 ====================
BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    if (!IsWindowVisible(hwnd)) return TRUE;
    WCHAR title[256] = {0};
    GetWindowTextW(hwnd, title, 256);
    if (wcslen(title) == 0) return TRUE;

    // 获取进程名
    std::wstring appName = L"?";
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (hProc) {
        WCHAR exePath[MAX_PATH] = {0};
        DWORD size = MAX_PATH;
        if (QueryFullProcessImageNameW(hProc, 0, exePath, &size)) {
            WCHAR* slash = wcsrchr(exePath, L'\\');
            appName = slash ? (slash + 1) : exePath;
        }
        CloseHandle(hProc);
    }

    HICON hIcon = GetWindowIcon(hwnd);

    WindowInfo info;
    info.hwnd = hwnd;
    info.title = title;
    info.appName = appName;
    info.hIcon = hIcon;
    g_windows.push_back(info);
    return TRUE;
}

void RefreshListView(const std::wstring& filter = L"") {
    g_windows.clear();
    EnumWindows(EnumWindowsProc, 0);

    // 过滤
    if (!filter.empty()) {
        std::vector<WindowInfo> filtered;
        std::wstring fl = filter;
        for (auto& c : fl) c = towlower(c);
        for (auto& w : g_windows) {
            std::wstring tl = w.title + L" " + w.appName;
            for (auto& c : tl) c = towlower(c);
            if (tl.find(fl) != std::wstring::npos) filtered.push_back(w);
        }
        g_windows = filtered;
    }

    // 重建 ImageList
    if (g_hImageList) { ImageList_Destroy(g_hImageList); g_hImageList = NULL; }
    g_hImageList = ImageList_Create(16, 16, ILC_COLOR32 | ILC_MASK, (int)g_windows.size(), 100);

    // 清空 ListView
    ListView_DeleteAllItems(g_hListView);

    for (int i = 0; i < (int)g_windows.size(); i++) {
        // 添加图标到 ImageList
        HICON hIcon = g_windows[i].hIcon;
        int iconIdx = ImageList_AddIcon(g_hImageList, hIcon ? hIcon : LoadIcon(NULL, IDI_APPLICATION));

        // 插入项
        LVITEMW lvi = {0};
        lvi.mask = LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM;
        lvi.iItem = i;
        lvi.iImage = iconIdx;
        lvi.lParam = (LPARAM)i;

        std::wstring display = L"[" + g_windows[i].appName + L"] " + g_windows[i].title;
        lvi.pszText = (LPWSTR)display.c_str();
        ListView_InsertItem(g_hListView, &lvi);

        // 释放从 SHGetFileInfo 获取的图标
        if (g_windows[i].hIcon) {
            // 不要 DestroyIcon，因为 SHGetFileInfo 获取的图标由系统管理
        }
    }

    // 关联 ImageList
    ListView_SetImageList(g_hListView, g_hImageList, LVSIL_SMALL);

    // 更新状态
    if (g_hLabelStatus) {
        SetWindowTextW(g_hLabelStatus, (W("已列出 ") + W(g_windows.size()) + W(" 个窗口")).c_str());
    }
}

// ==================== 发送回车 ====================
bool SendEnter(HWND hwnd) {
    DWORD_PTR result = 0;
    LRESULT lr = SendMessageTimeoutW(hwnd, WM_KEYDOWN, VK_RETURN, 0, SMTO_ABORTIFHUNG, 1000, &result);
    if (lr) {
        SendMessageTimeoutW(hwnd, WM_KEYUP, VK_RETURN, 0xC0000000, SMTO_ABORTIFHUNG, 1000, &result);
        return true;
    }

    DWORD targetTid = GetWindowThreadProcessId(hwnd, NULL);
    DWORD curTid = GetCurrentThreadId();
    if (targetTid != curTid) AttachThreadInput(curTid, targetTid, TRUE);

    SetForegroundWindow(hwnd);
    Sleep(50);
    keybd_event(VK_RETURN, 0x1C, 0, 0);
    Sleep(50);
    keybd_event(VK_RETURN, 0x1C, KEYEVENTF_KEYUP, 0);

    if (targetTid != curTid) AttachThreadInput(curTid, targetTid, FALSE);
    return true;
}

// ==================== 调度器 ====================
void SchedulerThread(int hour, int minute, int count, double interval, double winGap) {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto tm = *std::localtime(&time);

    int waitSec = 0;
    if (tm.tm_hour > hour || (tm.tm_hour == hour && tm.tm_min >= minute))
        waitSec = ((24 - tm.tm_hour + hour) * 3600) + ((minute - tm.tm_min) * 60) - tm.tm_sec;
    else
        waitSec = ((hour - tm.tm_hour) * 3600) + ((minute - tm.tm_min) * 60) - tm.tm_sec;

    LogMessage(W("已启动。目标窗口 ") + W(g_selectedIndices.size()) + W(" 个"));
    for (size_t i = 0; i < g_selectedIndices.size(); i++) {
        int idx = g_selectedIndices[i];
        if (idx < (int)g_windows.size()) {
            LogMessage(W("  目标") + W(i + 1) + W(": [") + g_windows[idx].appName + W("] ") + g_windows[idx].title);
        }
    }
    LogMessage(W("下次触发: ") + FormatTime(hour, minute) + W(" (约 ") + W(waitSec / 3600) + W(" 小时后)"));

    if (g_hLabelNext) {
        SetWindowTextW(g_hLabelNext, (FormatTime(hour, minute) + W(" (还剩 ") +
            W(waitSec / 3600) + W("时") + W((waitSec % 3600) / 60) + W("分") + W(waitSec % 60) + W("秒)")).c_str());
    }

    while (g_running && waitSec > 0) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        waitSec--;
        if (g_hLabelNext) {
            SetWindowTextW(g_hLabelNext, (FormatTime(hour, minute) + W(" (还剩 ") +
                W(waitSec / 3600) + W("时") + W((waitSec % 3600) / 60) + W("分") + W(waitSec % 60) + W("秒)")).c_str());
        }
    }

    if (!g_running) { LogMessage(W("已停止(等待期间)。")); return; }

    LogMessage(W("=== 到点触发，向 ") + W(g_selectedIndices.size()) + W(" 个窗口各发 ") + W(count) + W(" 次回车 ==="));

    for (size_t i = 0; i < g_selectedIndices.size(); i++) {
        if (!g_running) break;
        int idx = g_selectedIndices[i];
        if (idx >= (int)g_windows.size()) continue;

        HWND hwnd = g_windows[idx].hwnd;
        std::wstring title = g_windows[idx].title;
        if (title.length() > 30) title = title.substr(0, 28) + L"...";
        LogMessage(W("窗口") + W(i + 1) + W("/") + W(g_selectedIndices.size()) + W(": ") + title);

        for (int j = 0; j < count; j++) {
            if (!g_running) break;
            if (IsWindow(hwnd)) {
                SendEnter(hwnd);
                LogMessage(W("    第 ") + W(j + 1) + W("/") + W(count) + W(" 次 OK"));
            } else {
                LogMessage(W("    第 ") + W(j + 1) + W("/") + W(count) + W(" 次 失败:窗口已关闭"));
            }
            if (j < count - 1 && g_running)
                std::this_thread::sleep_for(std::chrono::milliseconds((int)(interval * 1000)));
        }
        if (i < g_selectedIndices.size() - 1 && g_running)
            std::this_thread::sleep_for(std::chrono::milliseconds((int)(winGap * 1000)));
    }

    LogMessage(W("=== 全部发送完毕 ==="));
    if (g_running) LogMessage(W("已自动安排明天同一时刻再次触发。"));
}

// ==================== GUI 操作 ====================
void UpdateSelection() {
    g_selectedIndices.clear();
    if (!g_hListView) return;
    int cnt = ListView_GetItemCount(g_hListView);
    for (int i = 0; i < cnt; i++) {
        if (ListView_GetItemState(g_hListView, i, LVIS_SELECTED) & LVIS_SELECTED) {
            // 从 lParam 获取原始索引
            LVITEMW lvi = {0};
            lvi.mask = LVIF_PARAM;
            lvi.iItem = i;
            ListView_GetItem(g_hListView, &lvi);
            g_selectedIndices.push_back((int)lvi.lParam);
        }
    }

    // 更新已选标签
    if (g_hLabelSelected) {
        std::wstring text = W(g_selectedIndices.size()) + W(" 个");
        if (!g_selectedIndices.empty() && g_selectedIndices.size() <= 3) {
            text += L": ";
            for (size_t i = 0; i < g_selectedIndices.size(); i++) {
                int idx = g_selectedIndices[i];
                if (idx < (int)g_windows.size()) {
                    std::wstring t = g_windows[idx].appName;
                    if (t.length() > 15) t = t.substr(0, 13) + L"...";
                    if (i > 0) text += L", ";
                    text += t;
                }
            }
        }
        SetWindowTextW(g_hLabelSelected, text.c_str());
    }
}

void OnSelectAll() {
    if (g_hListView) {
        int cnt = ListView_GetItemCount(g_hListView);
        for (int i = 0; i < cnt; i++)
            ListView_SetItemState(g_hListView, i, LVIS_SELECTED, LVIS_SELECTED);
        UpdateSelection();
    }
}

void OnClearSelection() {
    if (g_hListView) {
        int cnt = ListView_GetItemCount(g_hListView);
        for (int i = 0; i < cnt; i++)
            ListView_SetItemState(g_hListView, i, 0, LVIS_SELECTED);
        UpdateSelection();
    }
}

void OnRefresh() {
    WCHAR filter[256] = {0};
    if (g_hEditFilter) GetWindowTextW(g_hEditFilter, filter, 256);
    RefreshListView(filter);
    UpdateSelection();
}

void OnStart() {
    if (g_selectedIndices.empty()) {
        MessageBoxW(g_hMainWnd, W("请先选择至少一个目标窗口").c_str(), W("提示").c_str(), MB_OK | MB_ICONWARNING);
        return;
    }

    WCHAR timeStr[16] = {0}, countStr[16] = {0}, intervalStr[16] = {0}, gapStr[16] = {0};
    GetWindowTextW(g_hEditTime, timeStr, 16);
    GetWindowTextW(g_hEditCount, countStr, 16);
    GetWindowTextW(g_hEditInterval, intervalStr, 16);
    GetWindowTextW(g_hEditGap, gapStr, 16);

    int hour = 0, minute = 0;
    if (swscanf(timeStr, L"%d:%d", &hour, &minute) != 2 || hour < 0 || hour > 23 || minute < 0 || minute > 59) {
        MessageBoxW(g_hMainWnd, W("时间格式错误，应为 HH:MM").c_str(), W("错误").c_str(), MB_OK | MB_ICONERROR);
        return;
    }
    int cnt = _wtoi(countStr);
    double itv = _wtof(intervalStr);
    double gap = _wtof(gapStr);
    if (cnt < 1 || itv <= 0 || gap < 0) {
        MessageBoxW(g_hMainWnd, W("次数、间隔和窗口间隔需为正数").c_str(), W("错误").c_str(), MB_OK | MB_ICONERROR);
        return;
    }

    g_running = true;
    g_schedulerThread = std::thread(SchedulerThread, hour, minute, cnt, itv, gap);
    g_schedulerThread.detach();

    EnableWindow(g_hBtnStart, FALSE);
    EnableWindow(g_hBtnStop, TRUE);
    if (g_hLabelStatus) {
        SetWindowTextW(g_hLabelStatus, (W("定时中... ") + W(g_selectedIndices.size()) +
            W(" 个窗口 -> ") + FormatTime(hour, minute)).c_str());
    }
}

void OnStop() {
    g_running = false;
    EnableWindow(g_hBtnStart, TRUE);
    EnableWindow(g_hBtnStop, FALSE);
    if (g_hLabelStatus) SetWindowTextW(g_hLabelStatus, W("已停止").c_str());
    if (g_hLabelNext) SetWindowTextW(g_hLabelNext, L"-");
    LogMessage(W("用户点击停止。"));
}

void OnTest() {
    if (g_selectedIndices.empty()) {
        MessageBoxW(g_hMainWnd, W("请先选择至少一个目标窗口").c_str(), W("提示").c_str(), MB_OK | MB_ICONWARNING);
        return;
    }
    LogMessage(W("测试: 向 ") + W(g_selectedIndices.size()) + W(" 个窗口各发 1 次回车..."));
    int ok = 0;
    for (int idx : g_selectedIndices) {
        if (idx < (int)g_windows.size()) {
            HWND hwnd = g_windows[idx].hwnd;
            if (IsWindow(hwnd)) {
                SendEnter(hwnd);
                LogMessage(W("  OK [") + g_windows[idx].appName + W("] ") + g_windows[idx].title);
                ok++;
            } else {
                LogMessage(W("  失败 窗口已关闭: ") + g_windows[idx].title);
            }
            Sleep(300);
        }
    }
    LogMessage(W("测试完成: ") + W(ok) + W("/") + W(g_selectedIndices.size()) + W(" 成功。"));
}

// ==================== 窗口过程 ====================
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        int y = 10, lw = 110, ew = 130, bw = 80;
        int WND_W = 520;

        CreateWindowW(L"STATIC", W("[应用名.exe] 窗口标题  |  Ctrl/Shift多选  Ctrl+A全选").c_str(),
            WS_CHILD | WS_VISIBLE, 10, y, WND_W - 20, 20, hwnd, NULL, g_hInst, NULL);
        y += 22;

        g_hBtnRefresh = CreateWindowW(L"BUTTON", W("刷新").c_str(), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            10, y, bw, 25, hwnd, (HMENU)ID_BTN_REFRESH, g_hInst, NULL);
        g_hBtnSelectAll = CreateWindowW(L"BUTTON", W("全选").c_str(), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            100, y, bw, 25, hwnd, (HMENU)ID_BTN_SELECT_ALL, g_hInst, NULL);
        g_hBtnClear = CreateWindowW(L"BUTTON", W("清空").c_str(), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            190, y, bw, 25, hwnd, (HMENU)ID_BTN_CLEAR, g_hInst, NULL);
        y += 30;

        g_hEditFilter = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            10, y, 200, 25, hwnd, (HMENU)ID_EDIT_FILTER, g_hInst, NULL);
        CreateWindowW(L"BUTTON", W("过滤").c_str(), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            215, y, 60, 25, hwnd, (HMENU)ID_BTN_FILTER, g_hInst, NULL);
        y += 30;

        // ListView 替代 Listbox，支持图标
        g_hListView = CreateWindowW(WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_BORDER |
            WS_VSCROLL | LVS_REPORT | LVS_SHOWSELALWAYS,
            10, y, WND_W - 20, 200, hwnd, (HMENU)ID_LIST_WINDOWS, g_hInst, NULL);

        // 设置 ListView 扩展风格：整行选中 + 双缓冲
        ListView_SetExtendedListViewStyle(g_hListView,
            LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_INFOTIP);

        // 添加一列（占满整行）
        LVCOLUMNW lvc = {0};
        lvc.mask = LVCF_WIDTH | LVCF_TEXT;
        lvc.cx = WND_W - 40;
        lvc.pszText = (LPWSTR)L"";
        ListView_InsertColumn(g_hListView, 0, &lvc);

        y += 210;

        g_hLabelSelected = CreateWindowW(L"STATIC", W("0 个").c_str(), WS_CHILD | WS_VISIBLE,
            10, y, 300, 20, hwnd, (HMENU)ID_LABEL_SELECTED, g_hInst, NULL);
        y += 28;

        // 第一行：触发时间 + 发送次数
        CreateWindowW(L"STATIC", W("触发时间 (HH:MM):").c_str(), WS_CHILD | WS_VISIBLE,
            10, y, lw, 20, hwnd, NULL, g_hInst, NULL);
        g_hEditTime = CreateWindowW(L"EDIT", L"05:00", WS_CHILD | WS_VISIBLE | WS_BORDER,
            10 + lw, y, ew, 25, hwnd, (HMENU)ID_EDIT_TIME, g_hInst, NULL);
        CreateWindowW(L"STATIC", W("发送次数:").c_str(), WS_CHILD | WS_VISIBLE,
            10 + lw + ew + 20, y, 80, 20, hwnd, NULL, g_hInst, NULL);
        g_hEditCount = CreateWindowW(L"EDIT", L"1", WS_CHILD | WS_VISIBLE | WS_BORDER,
            10 + lw + ew + 100, y, 60, 25, hwnd, (HMENU)ID_EDIT_COUNT, g_hInst, NULL);
        y += 32;

        // 第二行：间隔 + 窗口间隔
        CreateWindowW(L"STATIC", W("发送间隔 (秒):").c_str(), WS_CHILD | WS_VISIBLE,
            10, y, lw, 20, hwnd, NULL, g_hInst, NULL);
        g_hEditInterval = CreateWindowW(L"EDIT", L"0.5", WS_CHILD | WS_VISIBLE | WS_BORDER,
            10 + lw, y, ew, 25, hwnd, (HMENU)ID_EDIT_INTERVAL, g_hInst, NULL);
        CreateWindowW(L"STATIC", W("窗口间隔 (秒):").c_str(), WS_CHILD | WS_VISIBLE,
            10 + lw + ew + 20, y, 100, 20, hwnd, NULL, g_hInst, NULL);
        g_hEditGap = CreateWindowW(L"EDIT", L"1.5", WS_CHILD | WS_VISIBLE | WS_BORDER,
            10 + lw + ew + 120, y, 60, 25, hwnd, (HMENU)ID_EDIT_GAP, g_hInst, NULL);
        y += 38;

        g_hBtnStart = CreateWindowW(L"BUTTON", W("启动定时").c_str(), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            10, y, 100, 30, hwnd, (HMENU)ID_BTN_START, g_hInst, NULL);
        g_hBtnStop = CreateWindowW(L"BUTTON", W("停止").c_str(), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED,
            120, y, 80, 30, hwnd, (HMENU)ID_BTN_STOP, g_hInst, NULL);
        g_hBtnTest = CreateWindowW(L"BUTTON", W("测试一次").c_str(), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            210, y, 90, 30, hwnd, (HMENU)ID_BTN_TEST, g_hInst, NULL);
        g_hBtnNow = CreateWindowW(L"BUTTON", W("立即执行").c_str(), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            310, y, 90, 30, hwnd, (HMENU)ID_BTN_NOW, g_hInst, NULL);
        y += 40;

        g_hLabelStatus = CreateWindowW(L"STATIC", W("就绪").c_str(), WS_CHILD | WS_VISIBLE,
            10, y, 200, 20, hwnd, (HMENU)ID_LABEL_STATUS, g_hInst, NULL);
        g_hLabelNext = CreateWindowW(L"STATIC", L"-", WS_CHILD | WS_VISIBLE,
            220, y, 270, 20, hwnd, (HMENU)ID_LABEL_NEXT, g_hInst, NULL);
        y += 28;

        CreateWindowW(L"STATIC", W("日志:").c_str(), WS_CHILD | WS_VISIBLE,
            10, y, 50, 20, hwnd, NULL, g_hInst, NULL);
        y += 22;

        g_hLogText = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER |
            WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            10, y, WND_W - 20, 150, hwnd, (HMENU)ID_LOG_TEXT, g_hInst, NULL);

        // 字体 - 全部使用微软雅黑，列表框稍大
        HFONT hFontDefault = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
        HFONT hFontList = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");

        HWND hChild = GetWindow(hwnd, GW_CHILD);
        while (hChild) {
            HFONT hUse = (hChild == g_hListView) ? hFontList : hFontDefault;
            SendMessage(hChild, WM_SETFONT, (WPARAM)hUse, TRUE);
            hChild = GetWindow(hChild, GW_HWNDNEXT);
        }

        RefreshListView();
        break;
    }

    case WM_NOTIFY: {
        NMHDR* nmhdr = (NMHDR*)lParam;
        if (nmhdr->hwndFrom == g_hListView && nmhdr->code == LVN_ITEMCHANGED) {
            NMLISTVIEW* nmlv = (NMLISTVIEW*)lParam;
            if (nmlv->uChanged & LVIF_STATE) UpdateSelection();
        }
        break;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case ID_BTN_REFRESH:    OnRefresh(); break;
        case ID_BTN_FILTER:     OnRefresh(); break;
        case ID_BTN_SELECT_ALL: OnSelectAll(); break;
        case ID_BTN_CLEAR:      OnClearSelection(); break;
        case ID_BTN_START:      OnStart(); break;
        case ID_BTN_STOP:       OnStop(); break;
        case ID_BTN_TEST:       OnTest(); break;
        case ID_BTN_NOW:        OnTest(); break;
        }
        break;

    case WM_DESTROY:
        g_running = false;
        if (g_hImageList) ImageList_Destroy(g_hImageList);
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return 0;
}

// ==================== 主函数 ====================
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow) {
    g_hInst = hInstance;

    INITCOMMONCONTROLSEX icex = {sizeof(icex), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icex);

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = WINDOW_CLASS;
    wc.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(1));
    wc.hIconSm = LoadIcon(hInstance, MAKEINTRESOURCE(1));
    RegisterClassExW(&wc);

    g_hMainWnd = CreateWindowExW(0, WINDOW_CLASS, APP_TITLE,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 535, 740, NULL, NULL, hInstance, NULL);

    if (!g_hMainWnd) {
        MessageBoxW(NULL, L"CreateWindow failed", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    ShowWindow(g_hMainWnd, nCmdShow);
    UpdateWindow(g_hMainWnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return (int)msg.wParam;
}
