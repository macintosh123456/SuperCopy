#define NOMINMAX
#include <windows.h>
#include <iostream>
#include <string>
#include <fstream>
#include <shlobj.h>
#include <io.h>
#include <fcntl.h>

#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Shell32.lib")

// ==========================================
// 1. 管理員權限檢查與自動提權
// ==========================================
bool IsAdmin() {
    BOOL fIsRunAsAdmin = FALSE;
    DWORD dwError = ERROR_SUCCESS;
    PSID pAdministratorsGroup = NULL;
    SID_IDENTIFIER_AUTHORITY NtAuthority = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&NtAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &pAdministratorsGroup)) {
        if (!CheckTokenMembership(NULL, pAdministratorsGroup, &fIsRunAsAdmin)) {
            fIsRunAsAdmin = FALSE;
        }
        FreeSid(pAdministratorsGroup);
    }
    return fIsRunAsAdmin;
}

void RelaunchAsAdmin(wchar_t* exePath) {
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.lpVerb = L"runas";     // 觸發 UAC 提權
    sei.lpFile = exePath;
    sei.hwnd = NULL;
    sei.nShow = SW_NORMAL;
    if (!ShellExecuteExW(&sei)) {
        std::wcerr << L"[錯誤] 必須以系統管理員身分執行才能操作硬體層 I/O！\n";
        exit(1);
    }
    exit(0); // 成功啟動管理員實例後，關閉目前的普通權限實例
}

// ==========================================
// 2. 組態設定檔 (INI) 管理
// ==========================================
std::wstring config_file = L".\\SuperCopyClient.ini";
std::wstring current_lang = L"US";
std::wstring cli_path = L".\\supercopy.exe";

void LoadConfig() {
    wchar_t buffer[MAX_PATH];
    GetPrivateProfileStringW(L"Settings", L"Language", L"US", buffer, MAX_PATH, config_file.c_str());
    current_lang = buffer;
    
    GetPrivateProfileStringW(L"Settings", L"CLIPath", L".\\supercopy.exe", buffer, MAX_PATH, config_file.c_str());
    cli_path = buffer;
}

void SaveConfig() {
    WritePrivateProfileStringW(L"Settings", L"Language", current_lang.c_str(), config_file.c_str());
    WritePrivateProfileStringW(L"Settings", L"CLIPath", cli_path.c_str(), config_file.c_str());
}

std::wstring Msg(const wchar_t* tw, const wchar_t* us) {
    return (current_lang == L"US") ? us : tw;
}

// ==========================================
// 3. 互動式主程式
// ==========================================
void SetLanguage() {
    std::wcout << Msg(L"\n請選擇語言 (1) 繁體中文 (2) English: ", L"\nSelect Language (1) 繁體中文 (2) English: ");
    int choice;
    std::wcin >> choice;
    if (choice == 1) current_lang = L"TW";
    else if (choice == 2) current_lang = L"US";
    SaveConfig();
    std::wcout << Msg(L"語言已更新。\n", L"Language updated.\n");
}

void SetCLIPath() {
    std::wcout << Msg(L"\n請輸入 SuperCopy.exe 的完整路徑 (或相對路徑): \n> ", 
                      L"\nEnter the full or relative path to SuperCopy.exe: \n> ");
    std::wcin.ignore();
    std::getline(std::wcin, cli_path);
    SaveConfig();
    std::wcout << Msg(L"CLI 路徑已更新。\n", L"CLI path updated.\n");
}

void RunSuperCopy() {
    std::wstring src, dst, extra_args;
    
    std::wcin.ignore();
    std::wcout << Msg(L"\n[1] 請輸入「來源檔案」路徑: \n> ", L"\n[1] Enter 'Source' file path: \n> ");
    std::getline(std::wcin, src);
    
    std::wcout << Msg(L"\n[2] 請輸入「目的檔案」路徑: \n> ", L"\n[2] Enter 'Destination' file path: \n> ");
    std::getline(std::wcin, dst);

    std::wcout << Msg(L"\n[3] 需要加入額外參數嗎？(例如: --ram 16 --EHT) \n直接按 Enter 跳過，或輸入參數: \n> ", 
                      L"\n[3] Any extra arguments? (e.g., --ram 16 --EHT) \nPress Enter to skip, or type them: \n> ");
    std::getline(std::wcin, extra_args);

    // 組合命令列指令 (自動包上雙引號避免路徑有空白)
    std::wstring cmd = L"\"" + cli_path + L"\" \"" + src + L"\" \"" + dst + L"\" --lang " + current_lang;
    if (!extra_args.empty()) {
        cmd += L" " + extra_args;
    }

    std::wcout << L"\n======================================================\n";
    std::wcout << Msg(L"即將執行指令:\n", L"Executing command:\n") << cmd << L"\n";
    std::wcout << L"======================================================\n\n";

    // 使用系統呼叫執行 CLI 程式
    _wsystem(cmd.c_str());
    
    std::wcout << Msg(L"\n[任務結束] 請按任意鍵返回主選單...\n", L"\n[Task Finished] Press any key to return to menu...\n");
    system("pause > nul");
}

int wmain(int argc, wchar_t* argv[]) {
    // 讓 Console 支援 Unicode 中文輸入/輸出
    _setmode(_fileno(stdout), _O_U16TEXT);
    _setmode(_fileno(stdin), _O_U16TEXT);
    _setmode(_fileno(stderr), _O_U16TEXT);

    // 檢查是否為管理員，若不是則自動提權並重新啟動自己
    if (!IsAdmin()) {
        RelaunchAsAdmin(argv[0]);
    }

    LoadConfig();

    while (true) {
        system("cls"); // 清空畫面
        std::wcout << L"=========================================\n"
                   << Msg(L"    SuperCopy 互動式啟動器 (管理員模式)\n", L"    SuperCopy Interactive Launcher (Admin)\n")
                   << L"=========================================\n"
                   << Msg(L" 目前 CLI 路徑 : ", L" Current CLI Path : ") << cli_path << L"\n"
                   << Msg(L" 目前顯示語言  : ", L" Current Language : ") << current_lang << L"\n"
                   << L"=========================================\n"
                   << Msg(L" [1] 啟動極速複製任務\n", L" [1] Start Copy Task\n")
                   << Msg(L" [2] 設定 CLI 存放路徑\n", L" [2] Set CLI Path\n")
                   << Msg(L" [3] 切換顯示語言 (Language)\n", L" [3] Switch Language\n")
                   << Msg(L" [0] 退出程式\n", L" [0] Exit\n")
                   << L"=========================================\n"
                   << Msg(L" 請輸入選項 (0-3): ", L" Select an option (0-3): ");

        int choice;
        if (!(std::wcin >> choice)) {
            std::wcin.clear();
            std::wcin.ignore(10000, L'\n');
            continue;
        }

        switch (choice) {
            case 1: RunSuperCopy(); break;
            case 2: SetCLIPath(); break;
            case 3: SetLanguage(); break;
            case 0: return 0;
            default: break;
        }
    }
    return 0;
}
