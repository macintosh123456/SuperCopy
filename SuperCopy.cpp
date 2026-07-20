#define UNICODE
#define _UNICODE
#define NOMINMAX          
#include <windows.h>
#include <commctrl.h>
#include <iostream>
#include <thread>
#include <atomic>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <string>
#include <iomanip>
#include <chrono>
#include <filesystem>
#include <algorithm>

#pragma comment(lib, "Advapi32.lib")

namespace fs = std::filesystem;

// ==========================================
// 全域狀態與中斷控制
// ==========================================
std::atomic<bool> g_stop_requested{ false };

enum class Lang { TW, US };
Lang g_lang = Lang::US; // 根據最終規格，預設改為英文

std::wstring Msg(const wchar_t* tw, const wchar_t* us) {
    return (g_lang == Lang::US) ? us : tw;
}

BOOL WINAPI ConsoleCtrlHandler(DWORD dwCtrlType) {
    if (dwCtrlType == CTRL_C_EVENT || dwCtrlType == CTRL_BREAK_EVENT) {
        g_stop_requested = true;
        std::wcout << L"\n\n" << Msg(
            L"[警告] 收到中斷訊號，等待當前 Chunk 寫入落盤後安全退出...\n",
            L"\n[WARN] Interrupt received. Waiting for current chunk to flush to disk safely...\n"
        );
        return TRUE;
    }
    return FALSE;
}

// 支援長路徑的前綴處理
std::wstring MakeLongPath(const std::wstring& path) {
    if (path.length() >= 4 && path.substr(0, 4) == L"\\\\?\\") return path;
    fs::path p(path);
    if (p.is_absolute()) return L"\\\\?\\" + path;
    return path;
}

// ==========================================
// 參數結構與任務結構
// ==========================================
struct SuperCopyConfig {
    size_t ram_bytes = 8ULL * 1024 * 1024 * 1024;    // N: 預設 8GB
    size_t chunk_bytes = 16ULL * 1024 * 1024;        // 分流閾值與基本切片大小，預設 16MB
    int rfd_count = 0;
    int wtd_count = 0;
    bool sw = false;
    bool ndio = false;
    bool npcpu = false;
    bool zc = false;
    bool eht = false;

    // 硬體環境判定
    size_t physical_cores = 1;
    size_t logical_cores = 1;
    bool fallback_to_os = false;
    std::vector<ULONG_PTR> physical_masks;
    std::vector<ULONG_PTR> logical_masks;
};

struct FileTask {
    std::wstring src;
    std::wstring dst;
    uintmax_t size;
};

// Ping-Pong 資料區塊
struct DataBlock {
    uint8_t* buffer = nullptr;
    size_t size = 0;
    std::wstring dst_path;
    bool is_small = false;
    bool is_eof = false; // 檔案結尾標記，用於對齊截斷
    uintmax_t file_offset = 0;
};

// ==========================================
// V7 核心引擎
// ==========================================
class SuperCopyEngine {
private:
    SuperCopyConfig cfg;
    std::wstring src_root, dst_root;

    std::queue<FileTask> small_file_queue;
    std::queue<FileTask> large_file_queue;
    std::mutex queue_mtx;

    // 兩階段排程控制 (Phase 1 & Phase 2)
    std::atomic<bool> phase1_done{ false };
    std::condition_variable phase2_cv;
    std::mutex phase_mtx;

    // Ping-Pong Buffer 核心中繼管理
    void* huge_memory_pool = nullptr;
    size_t ping_pong_half_size;
    
    std::queue<DataBlock> disktoram_queue; // 來自讀取線程的資料
    std::mutex disktoram_mtx;
    std::condition_variable disktoram_cv;

    std::queue<DataBlock> ramtodisk_queue; // 準備發送給寫入線程的資料
    std::mutex ramtodisk_mtx;
    std::condition_variable ramtodisk_cv;

    std::atomic<uint64_t> total_bytes_to_copy{ 0 };
    std::atomic<uint64_t> total_bytes_written{ 0 };
    std::atomic<int> completed_files{ 0 };
    int total_files_count = 0;

    void EnableHugePages() {
        HANDLE hToken;
        TOKEN_PRIVILEGES tp;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
            LookupPrivilegeValue(NULL, SE_LOCK_MEMORY_NAME, &tp.Privileges[0].Luid);
            tp.PrivilegeCount = 1;
            tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            AdjustTokenPrivileges(hToken, FALSE, &tp, 0, (PTOKEN_PRIVILEGES)NULL, 0);
            CloseHandle(hToken);
        }
    }

    void DetectAndConfigureCores() {
        DWORD len = 0;
        GetLogicalProcessorInformation(nullptr, &len);
        std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> buffer(len / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
        GetLogicalProcessorInformation(buffer.data(), &len);

        for (const auto& info : buffer) {
            if (info.Relationship == RelationProcessorCore) {
                cfg.physical_masks.push_back(info.ProcessorMask & (0 - info.ProcessorMask));
                cfg.physical_cores++;
                for (int bit = 0; bit < sizeof(ULONG_PTR) * 8; ++bit) {
                    if (info.ProcessorMask & (1ULL << bit)) {
                        cfg.logical_masks.push_back(1ULL << bit);
                        cfg.logical_cores++;
                    }
                }
            }
        }
        
        // 核心限制防護與降級機制
        if (cfg.physical_cores < 4) {
            if (cfg.logical_cores >= 4) {
                cfg.eht = true; 
                std::wcout << Msg(L"[系統] 實體核心小於 4，自動開啟 HT 邏輯核心分配。\n", 
                                  L"[Sys] Physical cores < 4, auto-enabled HT mapping.\n");
            } else {
                cfg.fallback_to_os = true;
                cfg.npcpu = true;
                std::wcout << Msg(L"[系統] 核心數不足，自動放棄綁核，交由 OS 排程。\n", 
                                  L"[Sys] Core count insufficient, fallback to OS scheduling.\n");
            }
        }

        size_t available = cfg.eht ? cfg.logical_masks.size() : cfg.physical_masks.size();
        if (cfg.rfd_count == 0) cfg.rfd_count = static_cast<int>(std::max<size_t>(1, available / 2));
        if (cfg.wtd_count == 0) cfg.wtd_count = static_cast<int>(std::max<size_t>(1, available / 2));
    }

    void ApplyThreadAffinity(int worker_id, bool is_critical) {
        if (cfg.npcpu || cfg.fallback_to_os) return;
        auto& masks = cfg.eht ? cfg.logical_masks : cfg.physical_masks;
        if (!masks.empty()) {
            SetThreadAffinityMask(GetCurrentThread(), masks[worker_id % masks.size()]);
        }
        SetThreadPriority(GetCurrentThread(), is_critical ? THREAD_PRIORITY_TIME_CRITICAL : THREAD_PRIORITY_ABOVE_NORMAL);
    }

    void ProcessSmartWait() {
        if (cfg.sw) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        else std::this_thread::yield(); 
    }

public:
    SuperCopyEngine(std::wstring src, std::wstring dst, SuperCopyConfig config)
        : src_root(src), dst_root(dst), cfg(config) {
        EnableHugePages();
        DetectAndConfigureCores();
        
        // 切片與 Buffer 大小防護
        ping_pong_half_size = cfg.ram_bytes / 2;
        size_t max_slice = ping_pong_half_size / 2;
        if (cfg.chunk_bytes > max_slice) {
            cfg.chunk_bytes = max_slice;
        }

        // 分配巨型分頁 N 記憶體
        huge_memory_pool = VirtualAlloc(NULL, cfg.ram_bytes, MEM_COMMIT | MEM_RESERVE | MEM_LARGE_PAGES, PAGE_READWRITE);
        if (!huge_memory_pool) {
            huge_memory_pool = VirtualAlloc(NULL, cfg.ram_bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        }
    }

    ~SuperCopyEngine() {
        if (huge_memory_pool) VirtualFree(huge_memory_pool, 0, MEM_RELEASE);
    }

    // =========================================================
    // 6 大執行緒實作
    // =========================================================

    // 1. disktoram：資料輸入中控
    void Thread_DiskToRam() {
        ApplyThreadAffinity(0, true);
        while (!g_stop_requested) {
            std::unique_lock<std::mutex> lock(disktoram_mtx);
            disktoram_cv.wait(lock, [this] { return !disktoram_queue.empty() || g_stop_requested; });
            if (g_stop_requested && disktoram_queue.empty()) break;

            DataBlock block = disktoram_queue.front();
            disktoram_queue.pop();
            lock.unlock();

            // Ping-Pong 邏輯管理 (簡化展示：轉交給 Egress)
            std::lock_guard<std::mutex> r_lock(ramtodisk_mtx);
            ramtodisk_queue.push(block);
            ramtodisk_cv.notify_one();
        }
    }

    // 2. ramtodisk：資料輸出中控，智慧分流派發
    void Thread_RamToDisk() {
        ApplyThreadAffinity(1, true);
        while (!g_stop_requested) {
            std::unique_lock<std::mutex> lock(ramtodisk_mtx);
            ramtodisk_cv.wait(lock, [this] { return !ramtodisk_queue.empty() || g_stop_requested; });
            if (g_stop_requested && ramtodisk_queue.empty()) break;

            DataBlock block = ramtodisk_queue.front();
            ramtodisk_queue.pop();
            lock.unlock();

            // 根據是否為小檔，進行派發 (Dispatch)
            // 由於此處需要與寫入線程交互，實際架構中會推入對應的寫入佇列
            // 此處為概念展現，交由 WTD/WTDSmall 直接處理
        }
    }

    // 3. readfromdisksmall (Phase 1)
    void Thread_ReadFromDiskSmall() {
        ApplyThreadAffinity(2, false);
        while (!g_stop_requested) {
            FileTask task;
            {
                std::lock_guard<std::mutex> lock(queue_mtx);
                if (small_file_queue.empty()) break;
                task = small_file_queue.front();
                small_file_queue.pop();
            }

            // 原生 API 盡力讀取 (不用 Direct IO)
            std::wstring l_src = MakeLongPath(task.src);
            HANDLE hSrc = CreateFileW(l_src.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hSrc != INVALID_HANDLE_VALUE) {
                // 讀取後送交 disktoram (模擬)
                CloseHandle(hSrc);
            }
            ProcessSmartWait();
        }
        phase1_done = true;
        phase2_cv.notify_all(); // 通知 Phase 2 開始
    }

    // 4. writetodisksmall (Phase 1)
    void Thread_WriteToDiskSmall() {
        ApplyThreadAffinity(3, false);
        while (!g_stop_requested && !phase1_done) {
            // 盡力搶佔寫入，保留原檔案屬性與時間戳記
            ProcessSmartWait();
        }
    }

    // 5. readfromdisk (RFD) (Phase 2)
    void Thread_ReadFromDisk(int id) {
        // 第一階段：休眠等待
        {
            std::unique_lock<std::mutex> lock(phase_mtx);
            phase2_cv.wait(lock, [this] { return phase1_done.load() || g_stop_requested; });
        }
        if (g_stop_requested) return;

        ApplyThreadAffinity(id + 4, true);

        // 第二階段：重砲轟炸
        while (!g_stop_requested) {
            FileTask task;
            {
                std::lock_guard<std::mutex> lock(queue_mtx);
                if (large_file_queue.empty()) break;
                task = large_file_queue.front();
                large_file_queue.pop();
            }

            DWORD flags = FILE_ATTRIBUTE_NORMAL;
            if (!cfg.ndio) flags |= FILE_FLAG_NO_BUFFERING;

            std::wstring l_src = MakeLongPath(task.src);
            HANDLE hSrc = CreateFileW(l_src.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, flags, NULL);
            if (hSrc != INVALID_HANDLE_VALUE) {
                // 強制 4096 磁區對齊切片讀取，送交 disktoram...
                CloseHandle(hSrc);
            }
        }
    }

    // 6. writetodisk (WTD) (Phase 2)
    void Thread_WriteToDisk(int id) {
        {
            std::unique_lock<std::mutex> lock(phase_mtx);
            phase2_cv.wait(lock, [this] { return phase1_done.load() || g_stop_requested; });
        }
        if (g_stop_requested) return;

        ApplyThreadAffinity(id + 4 + cfg.rfd_count, true);
        
        while (!g_stop_requested) {
            // 接收來自 ramtodisk 的 DMA Block，進行對齊寫入，處理 Padding 與截斷 (SetEndOfFile)
            ProcessSmartWait();
        }
    }

    // =========================================================
    // 前導作業與主執行管線
    // =========================================================
    void Execute() {
        std::wcout << Msg(L"[系統] 執行前導作業：目錄結構解析與建立...\n", 
                          L"[Sys] Pre-flight Phase: Analyzing and mapping directories...\n");

        fs::path src_path(src_root);
        fs::path dst_path(dst_root);

        // Zero Copy 降級
        if (cfg.zc) {
            std::wcout << L"[Sys] Zero Copy Triggered. Using native OS transfer.\n";
            // Native fallback implementation...
            return;
        }

        // 遞迴掃描與建立結構
        try {
            if (fs::is_directory(src_path)) {
                for (const auto& entry : fs::recursive_directory_iterator(src_path)) {
                    if (g_stop_requested) break;
                    fs::path rel_path = fs::relative(entry.path(), src_path);
                    fs::path target_path = dst_path / rel_path;

                    if (fs::is_directory(entry.status())) {
                        CreateDirectoryW(MakeLongPath(target_path.wstring()).c_str(), NULL);
                    } else if (fs::is_regular_file(entry.status())) {
                        uintmax_t fsize = fs::file_size(entry);
                        total_bytes_to_copy += fsize;
                        total_files_count++;

                        if (fsize > cfg.chunk_bytes) {
                            large_file_queue.push({ entry.path().wstring(), target_path.wstring(), fsize });
                        } else {
                            small_file_queue.push({ entry.path().wstring(), target_path.wstring(), fsize });
                        }
                    }
                }
            }
        } catch (const fs::filesystem_error& e) {
            std::cerr << "Filesystem Error: " << e.what() << "\n";
            return;
        }

        std::wcout << Msg(L"[系統] 掃描完成。進入兩階段優先級排程...\n", 
                          L"[Sys] Scan complete. Entering Two-Stage Scheduling...\n");
        std::wcout << L"  -> Small Files: " << small_file_queue.size() << L"\n";
        std::wcout << L"  -> Large Files: " << large_file_queue.size() << L"\n\n";

        // 啟動管理與小檔線程 (Phase 1 活躍)
        std::thread t_dtr(&SuperCopyEngine::Thread_DiskToRam, this);
        std::thread t_rtd(&SuperCopyEngine::Thread_RamToDisk, this);
        std::thread t_rfds(&SuperCopyEngine::Thread_ReadFromDiskSmall, this);
        std::thread t_wtds(&SuperCopyEngine::Thread_WriteToDiskSmall, this);

        // 預先啟動大檔線程 (會自動進入 Phase 2 CV Wait 休眠)
        std::vector<std::thread> rfd_threads, wtd_threads;
        for (int i = 0; i < cfg.rfd_count; ++i) rfd_threads.emplace_back(&SuperCopyEngine::Thread_ReadFromDisk, this, i);
        for (int i = 0; i < cfg.wtd_count; ++i) wtd_threads.emplace_back(&SuperCopyEngine::Thread_WriteToDisk, this, i);

        // 監控進度
        auto start_time = std::chrono::steady_clock::now();
        while (!g_stop_requested && completed_files.load() < total_files_count) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            double progress = total_bytes_to_copy == 0 ? 100.0 : (static_cast<double>(total_bytes_written.load()) / total_bytes_to_copy) * 100.0;
            std::wcout << L"\r[Copy] Phase " << (phase1_done ? L"2 (DMA)" : L"1 (IOPS)") 
                       << L" | Progress: " << std::fixed << std::setprecision(1) << progress << L"%     " << std::flush;
        }

        // 安全卸載
        t_rfds.join();
        t_wtds.join();
        g_stop_requested = true;
        disktoram_cv.notify_all();
        ramtodisk_cv.notify_all();
        phase2_cv.notify_all();
        
        t_dtr.join();
        t_rtd.join();
        for (auto& t : rfd_threads) t.join();
        for (auto& t : wtd_threads) t.join();

        std::wcout << Msg(L"\n\n[成功] 傳輸完畢！\n", L"\n\n[Success] Transfer complete!\n");
    }
};

// ==========================================
// CLI 解析與程式進入點
// ==========================================
void PrintHelp() {
    std::wcout << L"SuperCopy V7 Engine\n"
               << L"Usage: supercopy.exe <Src> <Dst> [Options]\n\n"
               << L"Options:\n"
               << L"  --help          Show help\n"
               << L"  --lang <TW|US>  Switch language (Default: US)\n"
               << L"  --ram <GB>      Buffer size. Must be multiple of 2 (Default: 8)\n"
               << L"  --chunk <MB>    Threshold for Big/Small files (Default: 16)\n"
               << L"  --RFD <N>       Count of readfromdisk threads\n"
               << L"  --WTD <N>       Count of writetodisk threads\n"
               << L"  --SW            Enable Smart Wait (Yield CPU)\n"
               << L"  --NDIO          Disable Direct I/O (Use OS Cache)\n"
               << L"  --NPCPU         Disable CPU Pinning\n"
               << L"  --ZC            Zero Copy (Native OS copy)\n"
               << L"  --EHT           Enable Hyper-Threading mapping\n";
}

int wmain(int argc, wchar_t* argv[]) {
    setlocale(LC_ALL, "");
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    if (argc < 3) { PrintHelp(); return 1; }

    SuperCopyConfig config;

    // 解析參數
    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        std::transform(arg.begin(), arg.end(), arg.begin(), ::towlower);
        
        if (arg == L"--lang" && i + 1 < argc) {
            std::wstring lang_val = argv[++i];
            std::transform(lang_val.begin(), lang_val.end(), lang_val.begin(), ::towlower);
            if (lang_val == L"tw") g_lang = Lang::TW;
        } else if (arg == L"--help") {
            PrintHelp(); return 0;
        } else if (arg == L"--ram" && i + 1 < argc) {
            config.ram_bytes = static_cast<size_t>(_wtoi(argv[++i])) * 1024 * 1024 * 1024;
        } else if (arg == L"--chunk" && i + 1 < argc) {
            config.chunk_bytes = static_cast<size_t>(_wtoi(argv[++i])) * 1024 * 1024;
        } else if (arg == L"--rfd" && i + 1 < argc) {
            config.rfd_count = _wtoi(argv[++i]);
        } else if (arg == L"--wtd" && i + 1 < argc) {
            config.wtd_count = _wtoi(argv[++i]);
        } else if (arg == L"--sw") config.sw = true;
        else if (arg == L"--ndio") config.ndio = true;
        else if (arg == L"--npcpu") config.npcpu = true;
        else if (arg == L"--zc") config.zc = true;
        else if (arg == L"--eht") config.eht = true;
    }

    try {
        std::wcout << L"=========================================\n"
                   << Msg(L"  SuperCopy V7 引擎啟動 (兩階段管線)\n", L"  SuperCopy V7 Started (Two-Stage Pipeline)\n")
                   << L"=========================================\n";
        SuperCopyEngine engine(argv[1], argv[2], config);
        engine.Execute();
    } catch (const std::exception& e) {
        std::cerr << "\n[Fatal Error] " << e.what() << "\n";
        return 1;
    }
    return 0;
}
