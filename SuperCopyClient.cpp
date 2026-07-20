#define UNICODE
#define _UNICODE
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <commdlg.h>
#include <filesystem>
#include <string>
#include <vector>
#include <algorithm>

#pragma comment(linker,"\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#pragma comment(lib, "User32.lib")
#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Gdi32.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Comdlg32.lib")

namespace fs = std::filesystem;

// ==========================================
// 全域狀態與 INI 配置
// ==========================================
struct AppConfig {
    std::wstring exe_path = L"supercopy.exe";
    int ram_gb = 8;
    int chunk_mb = 16;
    bool sw = false;
    bool ndio = false;
    bool npcpu = false;
    bool zc = false;
    bool eht = false;
    bool lang_us = false; // false = TW, true = US
} g_config;

HWND hMainWnd, hExePathEdit, hBtnBrowseExe, hLblExePath;
HWND hLeftList, hRightList, hLeftPath, hRightPath;
HWND hLeftDrv, hRightDrv; 
HWND hBtnCopyL2R;

std::wstring currentLeftPath = L"C:\\";
std::wstring currentRightPath = L"D:\\";

#define IDM_RAM_4   101
#define IDM_RAM_8   102
#define IDM_RAM_16  103
#define IDM_RAM_32  104
#define IDM_CHK_8   111
#define IDM_CHK_16  112
#define IDM_CHK_64  113
#define IDM_CHK_256 114
#define IDM_SW      121
#define IDM_NDIO    122
#define IDM_NPCPU   123
#define IDM_ZC      124
#define IDM_EHT     125
#define IDM_LANG_TW 131
#define IDM_LANG_US 132

#define ID_BTN_COPY   200
#define ID_LEFT_LST   201
#define ID_RGHT_LST   202
#define ID_LEFT_DRV   203
#define ID_RGHT_DRV   204
#define ID_BTN_BROWSE 205

// 多語系輔助函數
std::wstring Msg(const wchar_t* tw, const wchar_t* us) {
    return g_config.lang_us ? us : tw;
}

// ==========================================
// INI 檔案讀寫邏輯
// ==========================================
std::wstring GetIniPath() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    fs::path p(path);
    return (p.parent_path() / L"SCsetup.ini").wstring();
}

void SaveConfig() {
    std::wstring ini = GetIniPath();
    
    // 儲存文字路徑
    wchar_t exeBuf[MAX_PATH];
    GetWindowTextW(hExePathEdit, exeBuf, MAX_PATH);
    WritePrivateProfileStringW(L"Paths", L"ExePath", exeBuf, ini.c_str());
    WritePrivateProfileStringW(L"Paths", L"LeftPath", currentLeftPath.c_str(), ini.c_str());
    WritePrivateProfileStringW(L"Paths", L"RightPath", currentRightPath.c_str(), ini.c_str());

    // 儲存參數
    WritePrivateProfileStringW(L"Config", L"LangUS", g_config.lang_us ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"Config", L"RAM", std::to_wstring(g_config.ram_gb).c_str(), ini.c_str());
    WritePrivateProfileStringW(L"Config", L"Chunk", std::to_wstring(g_config.chunk_mb).c_str(), ini.c_str());
    WritePrivateProfileStringW(L"Config", L"SW", g_config.sw ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"Config", L"NDIO", g_config.ndio ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"Config", L"NPCPU", g_config.npcpu ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"Config", L"ZC", g_config.zc ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"Config", L"EHT", g_config.eht ? L"1" : L"0", ini.c_str());
}

void LoadConfig() {
    std::wstring ini = GetIniPath();
    wchar_t buf[MAX_PATH];
    
    if (GetPrivateProfileStringW(L"Paths", L"ExePath", L"supercopy.exe", buf, MAX_PATH, ini.c_str()))
        g_config.exe_path = buf;
    if (GetPrivateProfileStringW(L"Paths", L"LeftPath", L"C:\\", buf, MAX_PATH, ini.c_str()))
        currentLeftPath = fs::exists(buf) ? buf : L"C:\\";
    if (GetPrivateProfileStringW(L"Paths", L"RightPath", L"D:\\", buf, MAX_PATH, ini.c_str()))
        currentRightPath = fs::exists(buf) ? buf : L"D:\\";

    g_config.lang_us = GetPrivateProfileIntW(L"Config", L"LangUS", 0, ini.c_str()) != 0;
    g_config.ram_gb = GetPrivateProfileIntW(L"Config", L"RAM", 8, ini.c_str());
    g_config.chunk_mb = GetPrivateProfileIntW(L"Config", L"Chunk", 16, ini.c_str());
    g_config.sw = GetPrivateProfileIntW(L"Config", L"SW", 0, ini.c_str()) != 0;
    g_config.ndio = GetPrivateProfileIntW(L"Config", L"NDIO", 0, ini.c_str()) != 0;
    g_config.npcpu = GetPrivateProfileIntW(L"Config", L"NPCPU", 0, ini.c_str()) != 0;
    g_config.zc = GetPrivateProfileIntW(L"Config", L"ZC", 0, ini.c_str()) != 0;
    g_config.eht = GetPrivateProfileIntW(L"Config", L"EHT", 0, ini.c_str()) != 0;
}

// ==========================================
// UI 動態語系切換與選單建立
// ==========================================
HMENU BuildMenu() {
    HMENU hMenu = CreateMenu();
    HMENU hMenuLang = CreatePopupMenu();
    AppendMenuW(hMenuLang, MF_STRING, IDM_LANG_TW, Msg(L"繁體中文 (TW)", L"Traditional Chinese (TW)").c_str());
    AppendMenuW(hMenuLang, MF_STRING, IDM_LANG_US, Msg(L"English (US)", L"English (US)").c_str());

    HMENU hMenuRam = CreatePopupMenu();
    AppendMenuW(hMenuRam, MF_STRING, IDM_RAM_4, L"4 GB");
    AppendMenuW(hMenuRam, MF_STRING, IDM_RAM_8, L"8 GB");
    AppendMenuW(hMenuRam, MF_STRING, IDM_RAM_16, L"16 GB");
    AppendMenuW(hMenuRam, MF_STRING, IDM_RAM_32, L"32 GB");

    HMENU hMenuChunk = CreatePopupMenu();
    AppendMenuW(hMenuChunk, MF_STRING, IDM_CHK_8, L"8 MB");
    AppendMenuW(hMenuChunk, MF_STRING, IDM_CHK_16, L"16 MB");
    AppendMenuW(hMenuChunk, MF_STRING, IDM_CHK_64, L"64 MB");
    AppendMenuW(hMenuChunk, MF_STRING, IDM_CHK_256, L"256 MB");

    HMENU hMenuOpt = CreatePopupMenu();
    AppendMenuW(hMenuOpt, MF_STRING, IDM_SW, Msg(L"Smart Wait (--SW)", L"Smart Wait (--SW)").c_str());
    AppendMenuW(hMenuOpt, MF_STRING, IDM_NDIO, Msg(L"No Direct I/O (--NDIO)", L"No Direct I/O (--NDIO)").c_str());
    AppendMenuW(hMenuOpt, MF_STRING, IDM_NPCPU, Msg(L"No Pin CPU (--NPCPU)", L"No Pin CPU (--NPCPU)").c_str());
    AppendMenuW(hMenuOpt, MF_STRING, IDM_ZC, Msg(L"Zero Copy (--ZC)", L"Zero Copy (--ZC)").c_str());
    AppendMenuW(hMenuOpt, MF_STRING, IDM_EHT, Msg(L"Enable Hyper-Threading (--EHT)", L"Enable Hyper-Threading (--EHT)").c_str());

    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hMenuLang, Msg(L"引擎顯示語言", L"Language").c_str());
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hMenuRam, Msg(L"記憶體總量 (RAM)", L"Total RAM").c_str());
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hMenuChunk, Msg(L"分流界線 (Chunk)", L"File Threshold (Chunk)").c_str());
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hMenuOpt, Msg(L"引擎進階選項", L"Engine Options").c_str());
    
    return hMenu;
}

void UpdateMenuState(HMENU hMenu) {
    CheckMenuItem(hMenu, IDM_RAM_4,  (g_config.ram_gb == 4)  ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(hMenu, IDM_RAM_8,  (g_config.ram_gb == 8)  ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(hMenu, IDM_RAM_16, (g_config.ram_gb == 16) ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(hMenu, IDM_RAM_32, (g_config.ram_gb == 32) ? MF_CHECKED : MF_UNCHECKED);
    
    CheckMenuItem(hMenu, IDM_CHK_8,   (g_config.chunk_mb == 8)   ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(hMenu, IDM_CHK_16,  (g_config.chunk_mb == 16)  ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(hMenu, IDM_CHK_64,  (g_config.chunk_mb == 64)  ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(hMenu, IDM_CHK_256, (g_config.chunk_mb == 256) ? MF_CHECKED : MF_UNCHECKED);

    CheckMenuItem(hMenu, IDM_SW,    g_config.sw ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(hMenu, IDM_NDIO,  g_config.ndio ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(hMenu, IDM_NPCPU, g_config.npcpu ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(hMenu, IDM_ZC,    g_config.zc ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(hMenu, IDM_EHT,   g_config.eht ? MF_CHECKED : MF_UNCHECKED);

    CheckMenuItem(hMenu, IDM_LANG_TW, !g_config.lang_us ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(hMenu, IDM_LANG_US, g_config.lang_us ? MF_CHECKED : MF_UNCHECKED);
}

void UpdateUILanguage() {
    SetWindowTextW(hMainWnd, Msg(L"SuperCopy V7 控制中心", L"SuperCopy V7 Control Center").c_str());
    SetWindowTextW(hLblExePath, Msg(L"SuperCopy.exe 引擎路徑:", L"SuperCopy.exe Engine Path:").c_str());
    SetWindowTextW(hBtnBrowseExe, Msg(L"瀏覽...", L"Browse...").c_str());
    SetWindowTextW(hBtnCopyL2R, Msg(L">>> 啟動 SuperCopy V7 (左側複製到右側) >>>", L">>> Launch SuperCopy V7 (Left to Right) >>>").c_str());

    HMENU hOldMenu = GetMenu(hMainWnd);
    HMENU hNewMenu = BuildMenu();
    SetMenu(hMainWnd, hNewMenu);
    UpdateMenuState(hNewMenu);
    if (hOldMenu) DestroyMenu(hOldMenu);
    DrawMenuBar(hMainWnd);
}

// ==========================================
// 權限與檔案/磁碟機邏輯
// ==========================================
bool IsRunAsAdmin() {
    BOOL isAdmin = FALSE;
    PSID adminGroup;
    SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&ntAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &adminGroup)) {
        CheckTokenMembership(NULL, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }
    return isAdmin;
}

void ElevatePrivileges() {
    wchar_t szPath[MAX_PATH];
    if (GetModuleFileNameW(NULL, szPath, ARRAYSIZE(szPath))) {
        SHELLEXECUTEINFOW sei = { sizeof(sei) };
        sei.lpVerb = L"runas";
        sei.lpFile = szPath;
        sei.nShow = SW_NORMAL;
        if (!ShellExecuteExW(&sei)) {
            MessageBoxW(NULL, L"Must run as Admin!", L"Permission Error", MB_ICONERROR);
            ExitProcess(1);
        }
        ExitProcess(0);
    }
}

void PopulateDrives(HWND hCombo) {
    SendMessageW(hCombo, CB_RESETCONTENT, 0, 0);
    wchar_t drives[256];
    GetLogicalDriveStringsW(256, drives);
    wchar_t* d = drives;
    while (*d) {
        SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)d);
        d += wcslen(d) + 1;
    }
}

void BrowseForEngine() {
    wchar_t szFile[MAX_PATH] = { 0 };
    OPENFILENAMEW ofn = { 0 };
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hMainWnd;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile) / sizeof(wchar_t);
    ofn.lpstrFilter = L"Executable (*.exe)\0*.exe\0All Files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;

    if (GetOpenFileNameW(&ofn)) {
        SetWindowTextW(hExePathEdit, szFile);
        SaveConfig();
    }
}

// ==========================================
// 檔案總管窗格邏輯
// ==========================================
void LoadDirectory(HWND hList, HWND hLabel, std::wstring& currentPath) {
    SendMessageW(hList, LB_RESETCONTENT, 0, 0);
    SetWindowTextW(hLabel, currentPath.c_str());

    std::vector<std::wstring> folders;
    std::vector<std::wstring> files;

    if (currentPath.length() > 3) {
        SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)Msg(L"[ .. ] (返回上一層)", L"[ .. ] (Go Up)").c_str());
    }

    try {
        for (const auto& entry : fs::directory_iterator(currentPath)) {
            std::wstring name = entry.path().filename().wstring();
            if (fs::is_directory(entry.status())) {
                folders.push_back(L"[DIR] " + name);
            } else {
                files.push_back(name);
            }
        }
    } catch (...) { }

    std::sort(folders.begin(), folders.end());
    std::sort(files.begin(), files.end());

    for (const auto& f : folders) SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)f.c_str());
    for (const auto& f : files)   SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)f.c_str());
}

void HandleListDoubleClick(HWND hList, HWND hLabel, std::wstring& currentPath) {
    int idx = (int)SendMessageW(hList, LB_GETCURSEL, 0, 0);
    if (idx == LB_ERR) return;

    wchar_t buf[MAX_PATH];
    SendMessageW(hList, LB_GETTEXT, idx, (LPARAM)buf);
    std::wstring selection(buf);

    if (selection == Msg(L"[ .. ] (返回上一層)", L"[ .. ] (Go Up)")) {
        fs::path p(currentPath);
        currentPath = p.parent_path().wstring();
        if (currentPath.back() != L'\\') currentPath += L"\\";
        LoadDirectory(hList, hLabel, currentPath);
    } 
    else if (selection.substr(0, 6) == L"[DIR] ") {
        std::wstring folderName = selection.substr(6);
        fs::path p(currentPath);
        p /= folderName;
        currentPath = p.wstring();
        if (currentPath.back() != L'\\') currentPath += L"\\";
        LoadDirectory(hList, hLabel, currentPath);
    }
    SaveConfig();
}

void HandleDriveChange(HWND hCombo, HWND hList, HWND hLabel, std::wstring& currentPath) {
    int idx = (int)SendMessageW(hCombo, CB_GETCURSEL, 0, 0);
    if (idx == CB_ERR) return;
    
    wchar_t buf[10];
    SendMessageW(hCombo, CB_GETLBTEXT, idx, (LPARAM)buf);
    currentPath = buf; 
    LoadDirectory(hList, hLabel, currentPath);
    SaveConfig(); 
}

void ExecuteEngine() {
    wchar_t exeBuf[MAX_PATH];
    GetWindowTextW(hExePathEdit, exeBuf, MAX_PATH);
    std::wstring exePath(exeBuf);

    int selIdx = (int)SendMessageW(hLeftList, LB_GETCURSEL, 0, 0);
    std::wstring srcPath = currentLeftPath;
    if (selIdx != LB_ERR) {
        wchar_t itemBuf[MAX_PATH];
        SendMessageW(hLeftList, LB_GETTEXT, selIdx, (LPARAM)itemBuf);
        std::wstring item(itemBuf);
        if (item.substr(0, 6) == L"[DIR] ") item = item.substr(6);
        if (item != Msg(L"[ .. ] (返回上一層)", L"[ .. ] (Go Up)")) srcPath = (fs::path(currentLeftPath) / item).wstring();
    }
    std::wstring dstPath = currentRightPath;

    std::wstring cmd = L"\"" + exePath + L"\" \"" + srcPath + L"\" \"" + dstPath + L"\"";
    cmd += L" --ram " + std::to_wstring(g_config.ram_gb);
    cmd += L" --chunk " + std::to_wstring(g_config.chunk_mb);
    cmd += g_config.lang_us ? L" --lang US" : L" --lang TW"; 
    
    if (g_config.sw) cmd += L" --sw";
    if (g_config.ndio) cmd += L" --ndio";
    if (g_config.npcpu) cmd += L" --npcpu";
    if (g_config.zc) cmd += L" --zc";
    if (g_config.eht) cmd += L" --eht";

    std::wstring fullCmd = L"/k " + cmd; 
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.lpVerb = L"open";
    sei.lpFile = L"cmd.exe";
    sei.lpParameters = fullCmd.c_str();
    sei.nShow = SW_SHOW;
    
    SaveConfig(); 

    if (!ShellExecuteExW(&sei)) {
        MessageBoxW(hMainWnd, Msg(L"無法啟動引擎，請確認路徑。", L"Failed to start engine.").c_str(), L"Error", MB_ICONERROR);
    }
}

// ==========================================
// Windows 訊息處理與 UI 建立
// ==========================================
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            LoadConfig();

            hLblExePath = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 10, 10, 200, 20, hwnd, NULL, NULL, NULL);
            hExePathEdit = CreateWindowW(L"EDIT", g_config.exe_path.c_str(), WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 215, 8, 470, 24, hwnd, NULL, NULL, NULL);
            hBtnBrowseExe = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 695, 7, 75, 26, hwnd, (HMENU)ID_BTN_BROWSE, NULL, NULL);

            hLeftDrv = CreateWindowW(L"COMBOBOX", NULL, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 10, 40, 370, 200, hwnd, (HMENU)ID_LEFT_DRV, NULL, NULL);
            hLeftPath = CreateWindowW(L"STATIC", currentLeftPath.c_str(), WS_CHILD | WS_VISIBLE | SS_PATHELLIPSIS, 10, 70, 370, 20, hwnd, NULL, NULL, NULL);
            hLeftList = CreateWindowW(L"LISTBOX", NULL, WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | LBS_NOTIFY, 10, 90, 370, 400, hwnd, (HMENU)ID_LEFT_LST, NULL, NULL);
            
            hRightDrv = CreateWindowW(L"COMBOBOX", NULL, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 400, 40, 370, 200, hwnd, (HMENU)ID_RGHT_DRV, NULL, NULL);
            hRightPath = CreateWindowW(L"STATIC", currentRightPath.c_str(), WS_CHILD | WS_VISIBLE | SS_PATHELLIPSIS, 400, 70, 370, 20, hwnd, NULL, NULL, NULL);
            hRightList = CreateWindowW(L"LISTBOX", NULL, WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | LBS_NOTIFY, 400, 90, 370, 400, hwnd, (HMENU)ID_RGHT_LST, NULL, NULL);

            hBtnCopyL2R = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 10, 500, 760, 40, hwnd, (HMENU)ID_BTN_COPY, NULL, NULL);
            
            PopulateDrives(hLeftDrv);
            PopulateDrives(hRightDrv);
            
            std::wstring leftRoot = currentLeftPath.substr(0, 3);
            std::wstring rightRoot = currentRightPath.substr(0, 3);
            
            int c_idx = (int)SendMessageW(hLeftDrv, CB_FINDSTRINGEXACT, -1, (LPARAM)leftRoot.c_str());
            SendMessageW(hLeftDrv, CB_SETCURSEL, c_idx != CB_ERR ? c_idx : 0, 0);
            
            int d_idx = (int)SendMessageW(hRightDrv, CB_FINDSTRINGEXACT, -1, (LPARAM)rightRoot.c_str());
            SendMessageW(hRightDrv, CB_SETCURSEL, d_idx != CB_ERR ? d_idx : 0, 0);

            UpdateUILanguage();

            LoadDirectory(hLeftList, hLeftPath, currentLeftPath);
            LoadDirectory(hRightList, hRightPath, currentRightPath);
            break;
        }
        case WM_COMMAND: {
            int wmId = LOWORD(wParam);
            int wmEvent = HIWORD(wParam);

            if (wmEvent == LBN_DBLCLK) {
                if (wmId == ID_LEFT_LST) HandleListDoubleClick(hLeftList, hLeftPath, currentLeftPath);
                if (wmId == ID_RGHT_LST) HandleListDoubleClick(hRightList, hRightPath, currentRightPath);
            }

            if (wmEvent == CBN_SELCHANGE) {
                if (wmId == ID_LEFT_DRV) HandleDriveChange(hLeftDrv, hLeftList, hLeftPath, currentLeftPath);
                if (wmId == ID_RGHT_DRV) HandleDriveChange(hRightDrv, hRightList, hRightPath, currentRightPath);
            }
            
            bool configChanged = false;
            switch (wmId) {
                case ID_BTN_BROWSE: BrowseForEngine(); break;
                case ID_BTN_COPY: ExecuteEngine(); break;

                case IDM_LANG_TW: g_config.lang_us = false; UpdateUILanguage(); configChanged = true; break;
                case IDM_LANG_US: g_config.lang_us = true; UpdateUILanguage(); configChanged = true; break;

                case IDM_RAM_4:  g_config.ram_gb = 4; configChanged = true; break;
                case IDM_RAM_8:  g_config.ram_gb = 8; configChanged = true; break;
                case IDM_RAM_16: g_config.ram_gb = 16; configChanged = true; break;
                case IDM_RAM_32: g_config.ram_gb = 32; configChanged = true; break;
                
                case IDM_CHK_8:   g_config.chunk_mb = 8; configChanged = true; break;
                case IDM_CHK_16:  g_config.chunk_mb = 16; configChanged = true; break;
                case IDM_CHK_64:  g_config.chunk_mb = 64; configChanged = true; break;
                case IDM_CHK_256: g_config.chunk_mb = 256; configChanged = true; break;

                case IDM_SW:    g_config.sw = !g_config.sw; configChanged = true; break;
                case IDM_NDIO:  g_config.ndio = !g_config.ndio; configChanged = true; break;
                case IDM_NPCPU: g_config.npcpu = !g_config.npcpu; configChanged = true; break;
                case IDM_ZC:    g_config.zc = !g_config.zc; configChanged = true; break;
                case IDM_EHT:   g_config.eht = !g_config.eht; configChanged = true; break;
            }
            
            if (configChanged) {
                UpdateMenuState(GetMenu(hwnd));
                SaveConfig(); 
            }
            break;
        }
        case WM_DESTROY:
            SaveConfig(); 
            PostQuitMessage(0);
            break;
        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow) {
    if (!IsRunAsAdmin()) ElevatePrivileges();

    const wchar_t CLASS_NAME[] = L"SuperCopyClientClass";
    WNDCLASSW wc = { };
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);

    hMainWnd = CreateWindowExW(
        0, CLASS_NAME, L"SuperCopy V7 控制中心",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 800, 620,
        NULL, NULL, hInstance, NULL
    );

    if (hMainWnd == NULL) return 0;
    
    HFONT hFont = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Microsoft JhengHei");
    EnumChildWindows(hMainWnd, [](HWND hwnd, LPARAM lParam) -> BOOL {
        SendMessageW(hwnd, WM_SETFONT, lParam, TRUE); return TRUE;
    }, (LPARAM)hFont);

    ShowWindow(hMainWnd, nCmdShow);

    MSG msg = { };
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
