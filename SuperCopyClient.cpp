#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <string>
#include <vector>

#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "User32.lib")
#pragma comment(lib, "Gdi32.lib")
#pragma comment(lib, "Comdlg32.lib")

// 隱藏 Console 視窗，指定為 Windows GUI 應用程式
#pragma comment(linker, "/SUBSYSTEM:windows /ENTRY:wmainCRTStartup")
// 啟用 Windows 現代視覺風格 (Visual Styles)
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// 控制項 ID
#define ID_BTN_LANG 101
#define ID_BTN_RUN 105
#define ID_BTN_CLI 106
#define ID_BTN_SRC 107
#define ID_BTN_DST 108
#define ID_EDIT_CLI 201
#define ID_EDIT_SRC 202
#define ID_EDIT_DST 203
#define ID_EDIT_ARGS 204

// 全域控制項控制代碼 (Handles)
HWND hLangBtn, hRunBtn, hCliBtn, hSrcBtn, hDstBtn;
HWND hCliLbl, hSrcLbl, hDstLbl, hArgsLbl;
HWND hCliEdit, hSrcEdit, hDstEdit, hArgsEdit;

std::wstring config_file = L".\\SuperCopyClient.ini";
std::wstring current_lang = L"US"; // 預設使用英文，避免亂碼

// ==========================================
// 1. UAC 與設定檔處理
// ==========================================
bool IsAdmin() {
    BOOL fIsRunAsAdmin = FALSE;
    PSID pAdministratorsGroup = NULL;
    SID_IDENTIFIER_AUTHORITY NtAuthority = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&NtAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &pAdministratorsGroup)) {
        CheckTokenMembership(NULL, pAdministratorsGroup, &fIsRunAsAdmin);
        FreeSid(pAdministratorsGroup);
    }
    return fIsRunAsAdmin;
}

void RelaunchAsAdmin() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.lpVerb = L"runas";
    sei.lpFile = path;
    sei.nShow = SW_NORMAL;
    ShellExecuteExW(&sei);
    exit(0);
}

void SaveConfig(HWND hwnd) {
    WritePrivateProfileStringW(L"Settings", L"Language", current_lang.c_str(), config_file.c_str());
    int len = GetWindowTextLengthW(hCliEdit);
    if (len > 0) {
        std::vector<wchar_t> buf(len + 1);
        GetWindowTextW(hCliEdit, buf.data(), len + 1);
        WritePrivateProfileStringW(L"Settings", L"CLIPath", buf.data(), config_file.c_str());
    }
}

void LoadConfig() {
    wchar_t buffer[MAX_PATH];
    GetPrivateProfileStringW(L"Settings", L"Language", L"US", buffer, MAX_PATH, config_file.c_str());
    current_lang = buffer;
    GetPrivateProfileStringW(L"Settings", L"CLIPath", L".\\supercopy.exe", buffer, MAX_PATH, config_file.c_str());
    SetWindowTextW(hCliEdit, buffer);
}

// ==========================================
// 2. GUI 輔助函數
// ==========================================
void UpdateUIText(HWND hwnd) {
    bool isTW = (current_lang == L"TW");
    SetWindowTextW(hwnd, isTW ? L"SuperCopy 啟動器 (管理員模式)" : L"SuperCopy Launcher (Admin)");
    SetWindowTextW(hLangBtn, isTW ? L"Switch to English" : L"切換至繁體中文");
    SetWindowTextW(hCliLbl, isTW ? L"SuperCopy.exe 存放路徑:" : L"SuperCopy.exe Path:");
    SetWindowTextW(hSrcLbl, isTW ? L"來源檔案 (Source):" : L"Source File:");
    SetWindowTextW(hDstLbl, isTW ? L"目的檔案 (Destination):" : L"Destination File:");
    SetWindowTextW(hArgsLbl, isTW ? L"額外參數 (例如: --ram 16 --EHT):" : L"Extra Args (e.g., --ram 16 --EHT):");
    SetWindowTextW(hCliBtn, isTW ? L"瀏覽..." : L"Browse...");
    SetWindowTextW(hSrcBtn, isTW ? L"瀏覽..." : L"Browse...");
    SetWindowTextW(hDstBtn, isTW ? L"瀏覽..." : L"Browse...");
    SetWindowTextW(hRunBtn, isTW ? L"開始複製 (START COPY)" : L"START COPY");
}

std::wstring GetTextFrom(HWND hEdit) {
    int len = GetWindowTextLengthW(hEdit);
    if (len == 0) return L"";
    std::vector<wchar_t> buf(len + 1);
    GetWindowTextW(hEdit, buf.data(), len + 1);
    return std::wstring(buf.data());
}

std::wstring OpenFileDialog(HWND hwnd, bool mustExist, bool isExe) {
    OPENFILENAMEW ofn;
    wchar_t szFile[MAX_PATH] = {0};
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile);
    ofn.lpstrFilter = isExe ? L"Executable (*.exe)\0*.exe\0All Files (*.*)\0*.*\0" : L"All Files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | (mustExist ? OFN_FILEMUSTEXIST : 0);
    
    if (mustExist) {
        if (GetOpenFileNameW(&ofn)) return ofn.lpstrFile;
    } else {
        if (GetSaveFileNameW(&ofn)) return ofn.lpstrFile;
    }
    return L"";
}

// ==========================================
// 3. 視窗訊息處理 (WndProc)
// ==========================================
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            HINSTANCE hInst = ((LPCREATESTRUCT)lParam)->hInstance;
            
            // 語言切換按鈕
            hLangBtn = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE, 400, 15, 140, 25, hwnd, (HMENU)ID_BTN_LANG, hInst, NULL);
            
            // CLI 路徑
            hCliLbl = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 20, 50, 300, 20, hwnd, NULL, hInst, NULL);
            hCliEdit = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 20, 70, 430, 25, hwnd, (HMENU)ID_EDIT_CLI, hInst, NULL);
            hCliBtn = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE, 460, 70, 80, 25, hwnd, (HMENU)ID_BTN_CLI, hInst, NULL);

            // 來源檔案
            hSrcLbl = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 20, 110, 300, 20, hwnd, NULL, hInst, NULL);
            hSrcEdit = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 20, 130, 430, 25, hwnd, (HMENU)ID_EDIT_SRC, hInst, NULL);
            hSrcBtn = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE, 460, 130, 80, 25, hwnd, (HMENU)ID_BTN_SRC, hInst, NULL);

            // 目的檔案
            hDstLbl = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 20, 170, 300, 20, hwnd, NULL, hInst, NULL);
            hDstEdit = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 20, 190, 430, 25, hwnd, (HMENU)ID_EDIT_DST, hInst, NULL);
            hDstBtn = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE, 460, 190, 80, 25, hwnd, (HMENU)ID_BTN_DST, hInst, NULL);

            // 額外參數
            hArgsLbl = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 20, 230, 300, 20, hwnd, NULL, hInst, NULL);
            hArgsEdit = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 20, 250, 430, 25, hwnd, (HMENU)ID_EDIT_ARGS, hInst, NULL);

            // 執行按鈕
            hRunBtn = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 200, 300, 160, 40, hwnd, (HMENU)ID_BTN_RUN, hInst, NULL);

            // 設定全域字體 (現代微軟正黑體/Segoe UI)
            HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
            EnumChildWindows(hwnd, [](HWND child, LPARAM lParam) -> BOOL {
                SendMessage(child, WM_SETFONT, lParam, TRUE);
                return TRUE;
            }, (LPARAM)hFont);

            LoadConfig();
            UpdateUIText(hwnd);
            break;
        }
        case WM_COMMAND: {
            int wmId = LOWORD(wParam);
            switch (wmId) {
                case ID_BTN_LANG:
                    current_lang = (current_lang == L"TW") ? L"US" : L"TW";
                    UpdateUIText(hwnd);
                    SaveConfig(hwnd);
                    break;
                case ID_BTN_CLI: {
                    std::wstring path = OpenFileDialog(hwnd, true, true);
                    if (!path.empty()) SetWindowTextW(hCliEdit, path.c_str());
                    break;
                }
                case ID_BTN_SRC: {
                    std::wstring path = OpenFileDialog(hwnd, true, false);
                    if (!path.empty()) SetWindowTextW(hSrcEdit, path.c_str());
                    break;
                }
                case ID_BTN_DST: {
                    std::wstring path = OpenFileDialog(hwnd, false, false);
                    if (!path.empty()) SetWindowTextW(hDstEdit, path.c_str());
                    break;
                }
                case ID_BTN_RUN: {
                    SaveConfig(hwnd);
                    std::wstring cli = GetTextFrom(hCliEdit);
                    std::wstring src = GetTextFrom(hSrcEdit);
                    std::wstring dst = GetTextFrom(hDstEdit);
                    std::wstring args = GetTextFrom(hArgsEdit);

                    if (cli.empty() || src.empty() || dst.empty()) {
                        MessageBoxW(hwnd, (current_lang == L"TW") ? L"路徑不可為空！" : L"Paths cannot be empty!", L"Error", MB_ICONERROR);
                        break;
                    }

                    // 利用 cmd.exe 執行，並加上 & pause 以便任務完成後使用者能看到結果與速度
                    std::wstring cmdArgs = L"/c \"\"" + cli + L"\" \"" + src + L"\" \"" + dst + L"\" --lang " + current_lang;
                    if (!args.empty()) cmdArgs += L" " + args;
                    cmdArgs += L" & pause\"";

                    ShellExecuteW(hwnd, L"open", L"cmd.exe", cmdArgs.c_str(), NULL, SW_SHOW);
                    break;
                }
            }
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

// ==========================================
// 4. 程式進入點
// ==========================================
int wmain(int argc, wchar_t* argv[]) {
    if (!IsAdmin()) {
        RelaunchAsAdmin();
    }

    HINSTANCE hInstance = GetModuleHandle(NULL);
    const wchar_t CLASS_NAME[] = L"SuperCopyClientClass";

    WNDCLASSW wc = {0};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW);
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);

    RegisterClassW(&wc);

    // 建立 580x400 大小的固定視窗
    HWND hwnd = CreateWindowExW(
        0, CLASS_NAME, L"SuperCopy Launcher", 
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, // 不允許改變大小
        CW_USEDEFAULT, CW_USEDEFAULT, 580, 400,
        NULL, NULL, hInstance, NULL
    );

    if (hwnd == NULL) return 0;

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg = {0};
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return 0;
}
