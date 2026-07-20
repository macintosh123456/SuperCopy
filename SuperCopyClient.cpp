#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <filesystem>
#include <string>
#include <vector>
#include <algorithm>

// 啟用 Windows 現代視覺樣式
#pragma comment(linker,"\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// ==========================================
// 補上缺少的 Windows 函式庫連結
// ==========================================
#pragma comment(lib, "User32.lib")    // 負責 GetMessage, CreateWindowEx 等 UI 核心
#pragma comment(lib, "Advapi32.lib")  // 負責 UAC 權限與 Token 檢查
#pragma comment(lib, "Shell32.lib")   // 負責 ShellExecuteExW 執行外部程式
#pragma comment(lib, "Gdi32.lib")     // 負責 CreateFontW 字型繪製
#pragma comment(lib, "Ole32.lib")     // 負責基礎 COM 元件

namespace fs = std::filesystem;

// ==========================================
// 全域變數與參數設定狀態
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
} g_config;

// UI 介面控制代碼
HWND hMainWnd, hExePathEdit;
HWND hLeftList, hRightList, hLeftPath, hRightPath;
HWND hBtnCopyL2R;

std::wstring currentLeftPath = L"C:\\";
std::wstring currentRightPath = L"D:\\";

// 選單 ID 定義
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
#define ID_BTN_COPY 200
#define ID_LEFT_LST 201
#define ID_RGHT_LST 202

// ==========================================
// 管理者權限檢查與提權
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
            MessageBoxW(NULL, L"必須以系統管理員身分執行，以獲得硬體鎖定與 Direct I/O 權限！", L"權限錯誤", MB_ICONERROR);
            ExitProcess(1);
        }
        ExitProcess(0);
    }
}

// ==========================================
// 檔案總管窗格邏輯
// ==========================================
void LoadDirectory(HWND hList, HWND hLabel, std::wstring& currentPath) {
    SendMessage(hList, LB_RESETCONTENT, 0, 0);
    SetWindowTextW(hLabel, currentPath.c_str());

    std::vector<std::wstring> folders;
    std::vector<std::wstring> files;

    // 處理根目錄的特例，加入返回上一層
    if (currentPath.length() > 3) {
        SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)L"[ .. ] (返回上一層)");
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
    } catch (...) { /* 忽略權限不足無法讀取的資料夾 */ }

    std::sort(folders.begin(), folders.end());
    std::sort(files.begin(), files.end());

    for (const auto& f : folders) SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)f.c_str());
    for (const auto& f : files)   SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)f.c_str());
}

void HandleListDoubleClick(HWND hList, HWND hLabel, std::wstring& currentPath) {
    int idx = (int)SendMessage(hList, LB_GETCURSEL, 0, 0);
    if (idx == LB_ERR) return;

    wchar_t buf[MAX_PATH];
    SendMessageW(hList, LB_GETTEXT, idx, (LPARAM)buf);
    std::wstring selection(buf);

    if (selection == L"[ .. ] (返回上一層)") {
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
}

// ==========================================
// 核心執行與選單 UI 狀態
// ==========================================
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
}

void ExecuteEngine() {
    // 獲取 exe 路徑
    wchar_t exeBuf[MAX_PATH];
    GetWindowTextW(hExePathEdit, exeBuf, MAX_PATH);
    std::wstring exePath(exeBuf);

    // 獲取來源與目的 (從 ListBox 選擇，若無選擇則傳整個目錄)
    int selIdx = (int)SendMessage(hLeftList, LB_GETCURSEL, 0, 0);
    std::wstring srcPath = currentLeftPath;
    if (selIdx != LB_ERR) {
        wchar_t itemBuf[MAX_PATH];
        SendMessageW(hLeftList, LB_GETTEXT, selIdx, (LPARAM)itemBuf);
        std::wstring item(itemBuf);
        if (item.substr(0, 6) == L"[DIR] ") item = item.substr(6);
        if (item != L"[ .. ] (返回上一層)") srcPath = (fs::path(currentLeftPath) / item).wstring();
    }
    std::wstring dstPath = currentRightPath;

    // 組合 CLI 參數
    std::wstring cmd = L"\"" + exePath + L"\" \"" + srcPath + L"\" \"" + dstPath + L"\"";
    cmd += L" --ram " + std::to_wstring(g_config.ram_gb);
    cmd += L" --chunk " + std::to_wstring(g_config.chunk_mb);
    if (g_config.sw) cmd += L" --sw";
    if (g_config.ndio) cmd += L" --ndio";
    if (g_config.npcpu) cmd += L" --npcpu";
    if (g_config.zc) cmd += L" --zc";
    if (g_config.eht) cmd += L" --eht";

    // 呼叫 CLI 視窗執行
    std::wstring fullCmd = L"/k " + cmd; // /k 執行完後保留視窗
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.lpVerb = L"open";
    sei.lpFile = L"cmd.exe";
    sei.lpParameters = fullCmd.c_str();
    sei.nShow = SW_SHOW;
    
    if (!ShellExecuteExW(&sei)) {
        MessageBoxW(hMainWnd, L"無法啟動 SuperCopy 引擎，請確認路徑是否正確。", L"錯誤", MB_ICONERROR);
    }
}

// ==========================================
// Windows 訊息處理與 UI 建立
// ==========================================
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            // 建立選單
            HMENU hMenu = CreateMenu();
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
            AppendMenuW(hMenuOpt, MF_STRING, IDM_SW, L"Smart Wait (--SW)");
            AppendMenuW(hMenuOpt, MF_STRING, IDM_NDIO, L"No Direct I/O (--NDIO)");
            AppendMenuW(hMenuOpt, MF_STRING, IDM_NPCPU, L"No Pin CPU (--NPCPU)");
            AppendMenuW(hMenuOpt, MF_STRING, IDM_ZC, L"Zero Copy (--ZC)");
            AppendMenuW(hMenuOpt, MF_STRING, IDM_EHT, L"Enable Hyper-Threading (--EHT)");

            HMENU hMenuMain = CreateMenu();
            AppendMenuW(hMenuMain, MF_POPUP, (UINT_PTR)hMenuRam, L"記憶體總量 (RAM)");
            AppendMenuW(hMenuMain, MF_POPUP, (UINT_PTR)hMenuChunk, L"分流界線 (Chunk)");
            AppendMenuW(hMenuMain, MF_POPUP, (UINT_PTR)hMenuOpt, L"引擎進階選項");
            SetMenu(hwnd, hMenuMain);
            UpdateMenuState(hMenuMain);

            // 建立 UI 控制項
            CreateWindowW(L"STATIC", L"SuperCopy.exe 引擎路徑:", WS_CHILD | WS_VISIBLE, 10, 10, 180, 20, hwnd, NULL, NULL, NULL);
            hExePathEdit = CreateWindowW(L"EDIT", g_config.exe_path.c_str(), WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 190, 8, 580, 24, hwnd, NULL, NULL, NULL);
            
            // 左側窗格 (來源)
            hLeftPath = CreateWindowW(L"STATIC", L"C:\\", WS_CHILD | WS_VISIBLE | SS_PATHELLIPSIS, 10, 45, 370, 20, hwnd, NULL, NULL, NULL);
            hLeftList = CreateWindowW(L"LISTBOX", NULL, WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | LBS_NOTIFY, 10, 65, 370, 430, hwnd, (HMENU)ID_LEFT_LST, NULL, NULL);
            
            // 右側窗格 (目的)
            hRightPath = CreateWindowW(L"STATIC", L"D:\\", WS_CHILD | WS_VISIBLE | SS_PATHELLIPSIS, 400, 45, 370, 20, hwnd, NULL, NULL, NULL);
            hRightList = CreateWindowW(L"LISTBOX", NULL, WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | LBS_NOTIFY, 400, 65, 370, 430, hwnd, (HMENU)ID_RGHT_LST, NULL, NULL);

            // 執行按鈕
            hBtnCopyL2R = CreateWindowW(L"BUTTON", L">>> 啟動 SuperCopy V7 (左側複製到右側) >>>", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 10, 500, 760, 40, hwnd, (HMENU)ID_BTN_COPY, NULL, NULL);
            
            // 載入初始目錄
            LoadDirectory(hLeftList, hLeftPath, currentLeftPath);
            LoadDirectory(hRightList, hRightPath, currentRightPath);
            break;
        }
        case WM_COMMAND: {
            int wmId = LOWORD(wParam);
            int wmEvent = HIWORD(wParam);

            // 處理雙擊 ListBox
            if (wmEvent == LBN_DBLCLK) {
                if (wmId == ID_LEFT_LST) HandleListDoubleClick(hLeftList, hLeftPath, currentLeftPath);
                if (wmId == ID_RGHT_LST) HandleListDoubleClick(hRightList, hRightPath, currentRightPath);
            }
            
            // 處理選單與按鈕
            switch (wmId) {
                case IDM_RAM_4:  g_config.ram_gb = 4; break;
                case IDM_RAM_8:  g_config.ram_gb = 8; break;
                case IDM_RAM_16: g_config.ram_gb = 16; break;
                case IDM_RAM_32: g_config.ram_gb = 32; break;
                
                case IDM_CHK_8:   g_config.chunk_mb = 8; break;
                case IDM_CHK_16:  g_config.chunk_mb = 16; break;
                case IDM_CHK_64:  g_config.chunk_mb = 64; break;
                case IDM_CHK_256: g_config.chunk_mb = 256; break;

                case IDM_SW:    g_config.sw = !g_config.sw; break;
                case IDM_NDIO:  g_config.ndio = !g_config.ndio; break;
                case IDM_NPCPU: g_config.npcpu = !g_config.npcpu; break;
                case IDM_ZC:    g_config.zc = !g_config.zc; break;
                case IDM_EHT:   g_config.eht = !g_config.eht; break;
                
                case ID_BTN_COPY: ExecuteEngine(); break;
            }
            UpdateMenuState(GetMenu(hwnd));
            break;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            break;
        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow) {
    // 檢查是否擁有管理者權限，若無則重啟提權
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
        0, CLASS_NAME, L"SuperCopy V7 控制中心 (管理者模式)",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 800, 620,
        NULL, NULL, hInstance, NULL
    );

    if (hMainWnd == NULL) return 0;
    
    // 設定介面字型
    HFONT hFont = CreateFontW(18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Microsoft JhengHei");
    EnumChildWindows(hMainWnd, [](HWND hwnd, LPARAM lParam) -> BOOL {
        SendMessage(hwnd, WM_SETFONT, lParam, TRUE); return TRUE;
    }, (LPARAM)hFont);

    ShowWindow(hMainWnd, nCmdShow);

    MSG msg = { };
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}
