#define NOMINMAX          
#include <windows.h>
#include <synchapi.h>
#include <iostream>
#include <thread>
#include <atomic>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <stdexcept>
#include <algorithm>
#include <string>
#include <iomanip>
#include <chrono>

#pragma comment(lib, "Synchronization.lib")
#pragma comment(lib, "Advapi32.lib")

// ==========================================
// 1. 全域中斷、多國語言
// ==========================================
std::atomic<bool> g_stop_requested{false};

enum class Lang { TW, US };
Lang g_lang = Lang::TW; 

std::wstring Msg(const wchar_t* tw, const wchar_t* us) {
    return (g_lang == Lang::US) ? us : tw;
}

BOOL WINAPI ConsoleCtrlHandler(DWORD dwCtrlType) {
    if (dwCtrlType == CTRL_C_EVENT || dwCtrlType == CTRL_BREAK_EVENT) {
        g_stop_requested = true;
        std::wcout << L"\n\n" << Msg(
            L"[警告] 收到中斷訊號 (Ctrl+C)，正在等待當前任務完成並安全退出...\n",
            L"[WARN] Interrupt signal (Ctrl+C) received. Waiting for tasks to finish...\n"
        );
        return TRUE; 
    }
    return FALSE;
}

enum class SyncMode { EXTREME_SPIN, SMART_WAIT };

struct SuperCopyConfig {
    size_t total_ram_bytes;       
    size_t chunk_size_bytes;      
    int num_rfd;             
    int num_wtd;            
    SyncMode sync_mode;           
    bool opt_direct_io;           
    bool opt_pin_cpu;             
    bool opt_zero_copy;
    bool opt_eht; 
    std::vector<ULONG_PTR> cpu_masks; 
};

// ==========================================
// 2. 同步器與任務佇列
// ==========================================
class PingPongSync {
private:
    volatile LONG state; 
    SyncMode mode;
public:
    PingPongSync(SyncMode m) : state(0), mode(m) {}
    void NotifyReady() {
        InterlockedExchange(&state, 1);
        if (mode == SyncMode::SMART_WAIT) WakeByAddressSingle((PVOID)&state);
    }
    void WaitUntilReady() {
        if (mode == SyncMode::EXTREME_SPIN) {
            while (InterlockedCompareExchange(&state, 0, 0) == 0 && !g_stop_requested) YieldProcessor();
        } else {
            LONG undesired = 0;
            while (InterlockedCompareExchange(&state, 0, 0) == 0 && !g_stop_requested) {
                WaitOnAddress((volatile PVOID)&state, &undesired, sizeof(LONG), 100); 
            }
        }
    }
    void Reset() { InterlockedExchange(&state, 0); }
};

struct IOTask {
    HANDLE hFile;
    uint64_t file_offset;
    uint8_t* mem_addr;
    DWORD size;
    std::atomic<int>* tasks_remaining_counter; 
    bool is_poison_pill; 
};

class TaskQueue {
private:
    std::queue<IOTask> tasks;
    std::mutex mtx;
    std::condition_variable cv;
public:
    void Push(const IOTask& task) {
        {
            std::lock_guard<std::mutex> lock(mtx);
            tasks.push(task);
        }
        cv.notify_one();
    }
    IOTask Pop() {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [this]() { return !tasks.empty() || g_stop_requested; });
        if (g_stop_requested && tasks.empty()) return {NULL, 0, nullptr, 0, nullptr, true};
        IOTask task = tasks.front();
        tasks.pop();
        return task;
    }
};

// ==========================================
// 3. 核心引擎
// ==========================================
class SuperCopyEngine {
private:
    SuperCopyConfig cfg;
    std::wstring src_path, dst_path;
    
    void* memory_pool = nullptr;
    uint8_t* buffer_N1 = nullptr;
    uint8_t* buffer_N2 = nullptr;
    
    PingPongSync sync_N1, sync_N2, sync_disk_N1, sync_disk_N2; 
    TaskQueue read_queue, write_queue;
    std::vector<std::thread> thread_readfromdisk, thread_writetodisk;

    std::atomic<bool> global_eof{false};
    uint64_t total_file_size = 0;
    std::atomic<uint64_t> total_bytes_written{0}; 

    bool EnableHugePagesPrivilege() {
        HANDLE hToken;
        TOKEN_PRIVILEGES tp;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) return false;
        LookupPrivilegeValue(NULL, SE_LOCK_MEMORY_NAME, &tp.Privileges[0].Luid);
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(hToken, FALSE, &tp, 0, (PTOKEN_PRIVILEGES)NULL, 0);
        CloseHandle(hToken);
        return true;
    }

    void ProgressMonitor() {
        auto start_time = std::chrono::steady_clock::now();
        uint64_t last_written = 0;

        while (!global_eof && !g_stop_requested) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (total_file_size == 0) continue;

            uint64_t current_written = total_bytes_written.load();
            double speed_mb_s = ((current_written - last_written) / 1024.0 / 1024.0) / 0.5;
            last_written = current_written;
            double progress = (static_cast<double>(current_written) / total_file_size) * 100.0;

            if (g_lang == Lang::TW) {
                std::wcout << L"\r[進度] " << std::fixed << std::setprecision(1) << progress << L"% | "
                           << L"完成: " << (current_written / 1024 / 1024) << L" MB / " << (total_file_size / 1024 / 1024) << L" MB | "
                           << L"速度: " << std::setprecision(2) << speed_mb_s << L" MB/s     " << std::flush;
            } else {
                std::wcout << L"\r[Copy] " << std::fixed << std::setprecision(1) << progress << L"% | "
                           << L"Done: " << (current_written / 1024 / 1024) << L" MB / " << (total_file_size / 1024 / 1024) << L" MB | "
                           << L"Speed: " << std::setprecision(2) << speed_mb_s << L" MB/s     " << std::flush;
            }
        }
    }

public:
    SuperCopyEngine(std::wstring src, std::wstring dst, SuperCopyConfig config) 
        : src_path(src), dst_path(dst), cfg(config), 
          sync_N1(config.sync_mode), sync_N2(config.sync_mode),
          sync_disk_N1(config.sync_mode), sync_disk_N2(config.sync_mode) {
        
        const size_t TWO_GB = 2ULL * 1024 * 1024 * 1024;
        size_t half_N = cfg.total_ram_bytes / 2; // 每個 Buffer 的大小 (N/2)

        if (cfg.total_ram_bytes % TWO_GB != 0) 
            throw std::invalid_argument(g_lang == Lang::TW ? "RAM 必須是 2GB 的倍數" : "RAM must be a multiple of 2GB");
        if ((cfg.chunk_size_bytes & (cfg.chunk_size_bytes - 1)) != 0) 
            throw std::invalid_argument(g_lang == Lang::TW ? "Chunk 必須是 2 的次方" : "Chunk size must be a power of 2");
        if (cfg.chunk_size_bytes > half_N) 
            throw std::invalid_argument(g_lang == Lang::TW ? "Chunk 最大限制為 N/2 (RAM 總量的一半)" : "Max chunk size is N/2 (Half of total RAM)");
    }

    ~SuperCopyEngine() {
        if (memory_pool) VirtualFree(memory_pool, 0, MEM_RELEASE);
    }

    void Execute() {
        if (cfg.opt_zero_copy) {
            std::wcout << Msg(L"[模式] 啟動 Zero-Copy 模式...\n", L"[Mode] Starting Zero-Copy Mode...\n");
            CopyFileExW(src_path.c_str(), dst_path.c_str(), nullptr, nullptr, FALSE, COPY_FILE_NO_BUFFERING);
            return;
        }

        EnableHugePagesPrivilege();
        memory_pool = VirtualAlloc(NULL, cfg.total_ram_bytes, MEM_COMMIT | MEM_RESERVE | MEM_LARGE_PAGES, PAGE_READWRITE);
        if (!memory_pool) {
            throw std::runtime_error(g_lang == Lang::TW ? 
                "巨型分頁配置失敗，請確認以「系統管理員」身分執行！" : 
                "Huge Pages allocation failed. Run as Administrator!");
        }

        size_t half_N = cfg.total_ram_bytes / 2;
        buffer_N1 = static_cast<uint8_t*>(memory_pool);
        buffer_N2 = buffer_N1 + half_N;

        sync_disk_N1.NotifyReady();
        sync_disk_N2.NotifyReady();

        for (int i = 0; i < cfg.num_rfd; ++i) thread_readfromdisk.emplace_back(&SuperCopyEngine::readfromdisk, this, i);
        for (int i = 0; i < cfg.num_wtd; ++i) thread_writetodisk.emplace_back(&SuperCopyEngine::writetodisk, this, i);

        std::thread manager_read(&SuperCopyEngine::disktoram, this);
        std::thread manager_write(&SuperCopyEngine::ramtodisk, this);
        std::thread monitor(&SuperCopyEngine::ProgressMonitor, this); 

        manager_read.join();
        manager_write.join();
        monitor.join();

        for (int i = 0; i < cfg.num_rfd; ++i) read_queue.Push({NULL, 0, nullptr, 0, nullptr, true});
        for (int i = 0; i < cfg.num_wtd; ++i) write_queue.Push({NULL, 0, nullptr, 0, nullptr, true});

        for (auto& t : thread_readfromdisk) t.join();
        for (auto& t : thread_writetodisk) t.join();

        if (g_stop_requested) {
            std::wcout << Msg(L"\n[中斷] 任務已中止。\n", L"\n[Aborted] Task aborted.\n");
        } else {
            std::wcout << Msg(L"\n[成功] 複製完成！\n", L"\n[Success] Completed!\n");
        }
    }

private:
    void disktoram() {
        if (cfg.opt_pin_cpu && !cfg.cpu_masks.empty()) 
            SetThreadAffinityMask(GetCurrentThread(), cfg.cpu_masks[0 % cfg.cpu_masks.size()]);

        DWORD flags = FILE_ATTRIBUTE_NORMAL;
        if (cfg.opt_direct_io) flags |= FILE_FLAG_NO_BUFFERING; 

        HANDLE hSrc = CreateFileW(src_path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, flags, NULL);
        if (hSrc == INVALID_HANDLE_VALUE) {
            std::wcerr << Msg(L"\n[錯誤] 無法開啟來源。\n", L"\n[Error] Unable to open source.\n");
            global_eof = true;
            return;
        }
        
        LARGE_INTEGER fs;
        GetFileSizeEx(hSrc, &fs);
        total_file_size = fs.QuadPart;

        size_t half_N = cfg.total_ram_bytes / 2;
        bool fill_N1 = true;
        uint64_t current_offset = 0;

        while (current_offset < total_file_size && !g_stop_requested) {
            uint8_t* current_buf = fill_N1 ? buffer_N1 : buffer_N2;
            PingPongSync& target_disk_sync = fill_N1 ? sync_disk_N1 : sync_disk_N2;
            PingPongSync& target_ram_sync = fill_N1 ? sync_N1 : sync_N2;

            target_disk_sync.WaitUntilReady();
            if (g_stop_requested) break;
            target_disk_sync.Reset();

            size_t bytes_to_fill = std::min((uint64_t)half_N, total_file_size - current_offset);
            int num_tasks = (bytes_to_fill + cfg.chunk_size_bytes - 1) / cfg.chunk_size_bytes;
            std::atomic<int> pending_read_tasks{num_tasks};

            size_t filled = 0;
            while (filled < bytes_to_fill) {
                DWORD chunk = static_cast<DWORD>(std::min((size_t)cfg.chunk_size_bytes, bytes_to_fill - filled));
                read_queue.Push({hSrc, current_offset + filled, current_buf + filled, chunk, &pending_read_tasks, false});
                filled += chunk;
            }

            while (pending_read_tasks.load() > 0 && !g_stop_requested) YieldProcessor(); 

            target_ram_sync.NotifyReady();
            current_offset += bytes_to_fill;
            fill_N1 = !fill_N1;
        }
        CloseHandle(hSrc);
        global_eof = true; 
    }

    void ramtodisk() {
        if (cfg.opt_pin_cpu && !cfg.cpu_masks.empty()) 
            SetThreadAffinityMask(GetCurrentThread(), cfg.cpu_masks[1 % cfg.cpu_masks.size()]);

        DWORD flags = FILE_ATTRIBUTE_NORMAL;
        if (cfg.opt_direct_io) flags |= FILE_FLAG_NO_BUFFERING;

        HANDLE hDst = CreateFileW(dst_path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, flags, NULL);
        if (hDst == INVALID_HANDLE_VALUE) return;

        while(total_file_size == 0 && !global_eof && !g_stop_requested) YieldProcessor(); 
        if (total_file_size == 0 || g_stop_requested) { CloseHandle(hDst); return; } 
        
        LARGE_INTEGER size_li;
        size_li.QuadPart = total_file_size; 
        SetFilePointerEx(hDst, size_li, NULL, FILE_BEGIN);
        SetEndOfFile(hDst); 

        size_t half_N = cfg.total_ram_bytes / 2;
        bool drain_N1 = true;
        uint64_t current_offset = 0;

        while (current_offset < total_file_size && !g_stop_requested) {
            uint8_t* current_buf = drain_N1 ? buffer_N1 : buffer_N2;
            PingPongSync& source_ram_sync = drain_N1 ? sync_N1 : sync_N2;
            PingPongSync& source_disk_sync = drain_N1 ? sync_disk_N1 : sync_disk_N2;

            source_ram_sync.WaitUntilReady();
            if (g_stop_requested) break;
            source_ram_sync.Reset();

            size_t bytes_to_drain = std::min((uint64_t)half_N, total_file_size - current_offset);
            int num_tasks = (bytes_to_drain + cfg.chunk_size_bytes - 1) / cfg.chunk_size_bytes;
            std::atomic<int> pending_write_tasks{num_tasks};

            size_t drained = 0;
            while (drained < bytes_to_drain) {
                DWORD chunk = static_cast<DWORD>(std::min((size_t)cfg.chunk_size_bytes, bytes_to_drain - drained));
                write_queue.Push({hDst, current_offset + drained, current_buf + drained, chunk, &pending_write_tasks, false});
                drained += chunk;
            }

            while (pending_write_tasks.load() > 0) YieldProcessor();

            total_bytes_written.fetch_add(bytes_to_drain, std::memory_order_relaxed); 
            source_disk_sync.NotifyReady();
            current_offset += bytes_to_drain;
            drain_N1 = !drain_N1;
        }
        CloseHandle(hDst);
    }

    void readfromdisk(int worker_id) {
        if (cfg.opt_pin_cpu && !cfg.cpu_masks.empty()) 
            SetThreadAffinityMask(GetCurrentThread(), cfg.cpu_masks[(2 + worker_id) % cfg.cpu_masks.size()]);
            
        while (true) {
            IOTask task = read_queue.Pop();
            if (task.is_poison_pill || g_stop_requested) break; 
            OVERLAPPED ov = {0};
            ov.Offset = task.file_offset & 0xFFFFFFFF;
            ov.OffsetHigh = (task.file_offset >> 32);
            DWORD bytesRead;
            ReadFile(task.hFile, task.mem_addr, task.size, &bytesRead, &ov);
            task.tasks_remaining_counter->fetch_sub(1, std::memory_order_release);
        }
    }

    void writetodisk(int worker_id) {
        if (cfg.opt_pin_cpu && !cfg.cpu_masks.empty()) 
            SetThreadAffinityMask(GetCurrentThread(), cfg.cpu_masks[(2 + cfg.num_rfd + worker_id) % cfg.cpu_masks.size()]);
            
        while (true) {
            IOTask task = write_queue.Pop();
            if (task.is_poison_pill || g_stop_requested) break; 
            OVERLAPPED ov = {0};
            ov.Offset = task.file_offset & 0xFFFFFFFF;
            ov.OffsetHigh = (task.file_offset >> 32);
            DWORD bytesWritten;
            WriteFile(task.hFile, task.mem_addr, task.size, &bytesWritten, &ov);
            task.tasks_remaining_counter->fetch_sub(1, std::memory_order_release);
        }
    }
};

// ==========================================
// 4. CLI 參數解析與程式進入點
// ==========================================
void PrintHelp() {
    if (g_lang == Lang::TW) {
        std::wcout << L"SuperCopy 極速複製引擎\n"
                   << L"用法: supercopy.exe <來源> <目的> [選項 (大小寫不拘)]\n\n"
                   << L"參數選項:\n"
                   << L"  --helpH         顯示此說明畫面\n"
                   << L"  --lang <TW|US>  切換語言，預設: TW\n"
                   << L"  --ram <GB>      緩衝區大小(2的倍數)，預設: 8\n"
                   << L"  --chunk <MB>    切片大小(最大為RAM的一半 N/2)，預設: 16\n"
                   << L"  --RFD <N>       readfromdisk 數量，預設: 可用核心數/2\n"
                   << L"  --WTD <N>       writetodisk 數量，預設: 可用核心數/2\n"
                   << L"  --SW            啟用 Smart Wait (預設為極限自旋)\n"
                   << L"  --NDIO          關閉 Direct I/O (不繞過快取)\n"
                   << L"  --NPCPU         關閉 CPU 綁核\n"
                   << L"  --ZC            啟用 Zero-Copy (系統底層拷貝)\n"
                   << L"  --EHT           強制啟用超線程分配\n";
    } else {
        std::wcout << L"SuperCopy Ultimate Engine\n"
                   << L"Usage: supercopy.exe <Src> <Dst> [Options (Case-Insensitive)]\n\n"
                   << L"Options:\n"
                   << L"  --helpH         Show this help message\n"
                   << L"  --lang <TW|US>  Switch language, Default: TW\n"
                   << L"  --ram <GB>      Buffer size (Multiple of 2), Default: 8\n"
                   << L"  --chunk <MB>    Chunk size (Max N/2 of RAM), Default: 16\n"
                   << L"  --RFD <N>       readfromdisk threads, Default: Cores/2\n"
                   << L"  --WTD <N>       writetodisk threads, Default: Cores/2\n"
                   << L"  --SW            Enable Smart Wait\n"
                   << L"  --NDIO          Disable Direct I/O (Use OS Cache)\n"
                   << L"  --NPCPU         Disable CPU Pinning\n"
                   << L"  --ZC            Enable Zero-Copy (OS Native)\n"
                   << L"  --EHT           Enable Hyper-Threading usage\n";
    }
}

int wmain(int argc, wchar_t* argv[]) {
    setlocale(LC_ALL, "");
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    // 1. 優先掃描語言 (確保錯誤訊息語言正確) 與 helpH (獨立跳出)
    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        std::transform(arg.begin(), arg.end(), arg.begin(), ::towlower);
        if (arg == L"--lang" && i + 1 < argc) {
            std::wstring lang_val = argv[i+1];
            std::transform(lang_val.begin(), lang_val.end(), lang_val.begin(), ::towlower);
            if (lang_val == L"us") g_lang = Lang::US;
        } else if (arg == L"--helph") {
            PrintHelp();
            return 0;
        }
    }

    if (argc < 3) {
        PrintHelp();
        return 1; 
    }

    std::wstring src = argv[1];
    std::wstring dst = argv[2];

    SuperCopyConfig config;
    config.total_ram_bytes = 8ULL * 1024 * 1024 * 1024;  // 預設 8GB
    config.chunk_size_bytes = 16 * 1024 * 1024;          // 預設 16MB
    config.num_rfd = 0; 
    config.num_wtd = 0; 
    config.sync_mode = SyncMode::EXTREME_SPIN;
    config.opt_direct_io = true;
    config.opt_pin_cpu = true;
    config.opt_zero_copy = false;
    config.opt_eht = false;

    // 2. 優先掃描使用者是否輸入 EHT 與 NPCPU
    for (int i = 3; i < argc; ++i) {
        std::wstring arg = argv[i];
        std::transform(arg.begin(), arg.end(), arg.begin(), ::towlower);
        if (arg == L"--eht") config.opt_eht = true;
        if (arg == L"--npcpu") config.opt_pin_cpu = false;
    }

    // 3. 硬體感知：計算實體與邏輯核心
    DWORD len = 0;
    GetLogicalProcessorInformation(nullptr, &len);
    std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> buffer(len / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
    GetLogicalProcessorInformation(buffer.data(), &len);

    int phys_cores = 0;
    int log_cores = 0;
    for (const auto& info : buffer) {
        if (info.Relationship == RelationProcessorCore) {
            phys_cores++;
            ULONG_PTR m = info.ProcessorMask;
            for (int bit = 0; bit < sizeof(ULONG_PTR) * 8; ++bit) {
                if (m & (1ULL << bit)) log_cores++;
            }
        }
    }
    bool has_ht = (log_cores > phys_cores);

    // 4. 極端環境保護機制 (實體核心數預設基準線判斷)
    bool eht_auto_enabled = false;
    bool pin_auto_disabled = false;

    if (phys_cores <= 2) {
        if (has_ht) {
            config.opt_eht = true; // 強制啟用 HT
            eht_auto_enabled = true;
        } else {
            config.opt_pin_cpu = false; // 強制關閉綁核
            pin_auto_disabled = true;
        }
    }

    // 5. 建立核心遮罩
    config.cpu_masks.clear();
    for (const auto& info : buffer) {
        if (info.Relationship == RelationProcessorCore) {
            if (!config.opt_eht) {
                ULONG_PTR m = info.ProcessorMask;
                config.cpu_masks.push_back(m & (0 - m));
            } else {
                ULONG_PTR m = info.ProcessorMask;
                for (int bit = 0; bit < sizeof(ULONG_PTR) * 8; ++bit) {
                    if (m & (1ULL << bit)) config.cpu_masks.push_back(1ULL << bit);
                }
            }
        }
    }
    size_t available_cores = config.cpu_masks.size();

    // 6. 掃描剩餘參數
    for (int i = 3; i < argc; ++i) {
        std::wstring arg = argv[i];
        std::transform(arg.begin(), arg.end(), arg.begin(), ::towlower);
        
        if (arg == L"--lang" || arg == L"--eht" || arg == L"--npcpu" || arg == L"--helph") {
            if (arg == L"--lang") i++; 
            continue;
        } else if (arg == L"--ram" && i + 1 < argc) {
            config.total_ram_bytes = static_cast<size_t>(_wtoi(argv[++i])) * 1024 * 1024 * 1024;
        } else if (arg == L"--chunk" && i + 1 < argc) {
            config.chunk_size_bytes = static_cast<size_t>(_wtoi(argv[++i])) * 1024 * 1024;
        } else if (arg == L"--rfd" && i + 1 < argc) {
            config.num_rfd = _wtoi(argv[++i]);
        } else if (arg == L"--wtd" && i + 1 < argc) {
            config.num_wtd = _wtoi(argv[++i]);
        } else if (arg == L"--sw") {
            config.sync_mode = SyncMode::SMART_WAIT;
        } else if (arg == L"--ndio") {
            config.opt_direct_io = false;
        } else if (arg == L"--zc") {
            config.opt_zero_copy = true;
        } else {
            std::wcerr << Msg(L"未知參數: ", L"Unknown option: ") << argv[i] << L"\n";
            PrintHelp();
            return 1;
        }
    }

    if (config.num_rfd == 0) config.num_rfd = std::max<int>(1, available_cores / 2);
    if (config.num_wtd == 0) config.num_wtd = std::max<int>(1, available_cores / 2);

    if (config.num_rfd + config.num_wtd > available_cores) {
        std::wcerr << Msg(
            L"[錯誤] 設定的 RFD 與 WTD 總數超過可用核心上限。\n(當前可用核心: ", 
            L"[Error] RFD and WTD total exceeds available cores.\n(Available: "
        ) << available_cores << Msg(
            L" | 提示：若需突破實體核心限制，請加上 --EHT 參數)\n", 
            L" | Hint: Add --EHT to use Hyper-Threading logical cores)\n"
        );
        return 1;
    }

    try {
        std::wcout << L"=========================================\n"
                   << Msg(L"  SuperCopy 極速複製啟動\n", L"  SuperCopy Engine Started\n")
                   << L"=========================================\n";
                   
        // 顯示自動回退機制的系統提示
        if (eht_auto_enabled) {
            std::wcout << Msg(L"  [系統] 實體核心數 <= 2，自動強制啟用超線程 (--EHT)\n",
                              L"  [Sys] Physical cores <= 2, auto-enabled Hyper-Threading (--EHT)\n");
        } else if (pin_auto_disabled) {
            std::wcout << Msg(L"  [系統] 實體核心數 <= 2 且無超線程，自動關閉綁核 (--NPCPU)\n",
                              L"  [Sys] Physical cores <= 2 without HT, auto-disabled CPU pinning (--NPCPU)\n");
        }

        std::wcout << Msg(L"  來源: ", L"  Src : ") << src << L"\n"
                   << Msg(L"  目的: ", L"  Dst : ") << dst << L"\n"
                   << Msg(L"  記憶體: ", L"  RAM : ") << (config.total_ram_bytes / 1024 / 1024 / 1024) << L" GB\n"
                   << Msg(L"  總核心: ", L"  Core: ") << available_cores << Msg(L" (RFD:", L" (RFD:") << config.num_rfd << Msg(L", WTD:", L", WTD:") << config.num_wtd << L")\n"
                   << L"=========================================\n\n";

        SuperCopyEngine engine(src, dst, config);
        engine.Execute();
        
        if (g_stop_requested) return 130; 

    } catch (const std::exception& e) {
        std::cerr << "\n" << (g_lang == Lang::TW ? "[致命錯誤] " : "[Fatal Error] ") << e.what() << "\n";
        return 1; 
    }
    return 0; 
}
