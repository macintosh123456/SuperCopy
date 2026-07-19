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
// 1. 全域中斷訊號與組態
// ==========================================
std::atomic<bool> g_stop_requested{false};

BOOL WINAPI ConsoleCtrlHandler(DWORD dwCtrlType) {
    if (dwCtrlType == CTRL_C_EVENT || dwCtrlType == CTRL_BREAK_EVENT) {
        g_stop_requested = true;
        std::wcout << L"\n\n[警告] 收到中斷訊號 (Ctrl+C)，正在等待當前 I/O 任務完成並安全退出...\n";
        return TRUE; // 攔截訊號，不讓 Windows 強制關閉程式
    }
    return FALSE;
}

enum class SyncMode { EXTREME_SPIN, SMART_WAIT };

struct SuperCopyConfig {
    size_t total_ram_bytes;       
    size_t chunk_size_bytes;      
    int read_workers;             
    int write_workers;            
    SyncMode sync_mode;           
    bool opt_direct_io;           
    bool opt_pin_cpu;             
    bool opt_zero_copy;           
};

// ==========================================
// 2. 同步器與任務佇列 (與先前相同)
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
                WaitOnAddress((volatile PVOID)&state, &undesired, sizeof(LONG), 100); // 每 100ms 醒來檢查是否收到 Ctrl+C
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
// 3. 核心引擎 (加入進度監控)
// ==========================================
class SuperCopyEngine {
private:
    SuperCopyConfig cfg;
    std::wstring src_path, dst_path;
    
    void* memory_pool = nullptr;
    uint8_t* buffer_N1 = nullptr;
    uint8_t* buffer_N2 = nullptr;
    
    PingPongSync sync_N1, sync_N2;
    PingPongSync sync_disk_N1, sync_disk_N2; 

    TaskQueue read_queue, write_queue;
    std::vector<std::thread> read_workers, write_workers;

    std::atomic<bool> global_eof{false};
    uint64_t total_file_size = 0;
    std::atomic<uint64_t> total_bytes_written{0}; // 用於進度條統計

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

    // --- 獨立的進度條監控執行緒 ---
    void ProgressMonitor() {
        auto start_time = std::chrono::steady_clock::now();
        uint64_t last_written = 0;

        while (!global_eof && !g_stop_requested) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (total_file_size == 0) continue;

            uint64_t current_written = total_bytes_written.load();
            auto current_time = std::chrono::steady_clock::now();
            std::chrono::duration<double> elapsed = current_time - start_time;

            // 計算速度 (MB/s)
            double speed_mb_s = ((current_written - last_written) / 1024.0 / 1024.0) / 0.5;
            last_written = current_written;

            // 計算進度百分比
            double progress = (static_cast<double>(current_written) / total_file_size) * 100.0;

            // 覆寫同一行輸出 (\r)
            std::wcout << L"\r[複製中] 進度: " << std::fixed << std::setprecision(1) << progress << L"% | "
                       << L"已完成: " << (current_written / 1024 / 1024) << L" MB / " << (total_file_size / 1024 / 1024) << L" MB | "
                       << L"速度: " << std::setprecision(2) << speed_mb_s << L" MB/s     " << std::flush;
        }
    }

public:
    SuperCopyEngine(std::wstring src, std::wstring dst, SuperCopyConfig config) 
        : src_path(src), dst_path(dst), cfg(config), 
          sync_N1(config.sync_mode), sync_N2(config.sync_mode),
          sync_disk_N1(config.sync_mode), sync_disk_N2(config.sync_mode) {
        
        const size_t TWO_GB = 2ULL * 1024 * 1024 * 1024;
        if (cfg.total_ram_bytes % TWO_GB != 0) throw std::invalid_argument("RAM 必須是 2GB 的倍數");
        if ((cfg.chunk_size_bytes & (cfg.chunk_size_bytes - 1)) != 0) throw std::invalid_argument("Chunk 必須是 2 的次方");
    }

    ~SuperCopyEngine() {
        if (memory_pool) VirtualFree(memory_pool, 0, MEM_RELEASE);
    }

    void Execute() {
        if (cfg.opt_zero_copy) {
            std::wcout << L"[模式] 啟動 Zero-Copy 模式...\n";
            CopyFileExW(src_path.c_str(), dst_path.c_str(), nullptr, nullptr, FALSE, COPY_FILE_NO_BUFFERING);
            return;
        }

        EnableHugePagesPrivilege();
        memory_pool = VirtualAlloc(NULL, cfg.total_ram_bytes, MEM_COMMIT | MEM_RESERVE | MEM_LARGE_PAGES, PAGE_READWRITE);
        if (!memory_pool) throw std::runtime_error("巨型分頁配置失敗，請確認是否以「系統管理員」身分執行程式！");

        size_t half_N = cfg.total_ram_bytes / 2;
        buffer_N1 = static_cast<uint8_t*>(memory_pool);
        buffer_N2 = buffer_N1 + half_N;

        sync_disk_N1.NotifyReady();
        sync_disk_N2.NotifyReady();

        for (int i = 0; i < cfg.read_workers; ++i) read_workers.emplace_back(&SuperCopyEngine::readfromdisk, this, i);
        for (int i = 0; i < cfg.write_workers; ++i) write_workers.emplace_back(&SuperCopyEngine::writetodisk, this, i);

        std::thread manager_read(&SuperCopyEngine::disktoram, this);
        std::thread manager_write(&SuperCopyEngine::ramtodisk, this);
        std::thread monitor(&SuperCopyEngine::ProgressMonitor, this); // 啟動進度條監控

        manager_read.join();
        manager_write.join();
        monitor.join();

        // 清理工作者執行緒
        for (int i = 0; i < cfg.read_workers; ++i) read_queue.Push({NULL, 0, nullptr, 0, nullptr, true});
        for (int i = 0; i < cfg.write_workers; ++i) write_queue.Push({NULL, 0, nullptr, 0, nullptr, true});

        for (auto& t : read_workers) t.join();
        for (auto& t : write_workers) t.join();

        if (g_stop_requested) {
            std::wcout << L"\n[中斷] 複製任務已被使用者中止。\n";
        } else {
            std::wcout << L"\n[成功] 超級複製完成！\n";
        }
    }

private:
    void disktoram() {
        if (cfg.opt_pin_cpu) SetThreadAffinityMask(GetCurrentThread(), 1); 

        DWORD flags = FILE_ATTRIBUTE_NORMAL;
        if (cfg.opt_direct_io) flags |= FILE_FLAG_NO_BUFFERING; 

        HANDLE hSrc = CreateFileW(src_path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, flags, NULL);
        if (hSrc == INVALID_HANDLE_VALUE) {
            std::wcerr << L"\n[錯誤] 無法開啟來源檔案。\n";
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
        global_eof = true; // 讀取完畢或被中斷
    }

    void ramtodisk() {
        if (cfg.opt_pin_cpu) SetThreadAffinityMask(GetCurrentThread(), 2); 

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

            total_bytes_written.fetch_add(bytes_to_drain, std::memory_order_relaxed); // 更新總進度
            source_disk_sync.NotifyReady();
            current_offset += bytes_to_drain;
            drain_N1 = !drain_N1;
        }
        CloseHandle(hDst);
    }

    void readfromdisk(int worker_id) {
        if (cfg.opt_pin_cpu) SetThreadAffinityMask(GetCurrentThread(), (1ULL << (worker_id + 2))); 
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
        if (cfg.opt_pin_cpu) SetThreadAffinityMask(GetCurrentThread(), (1ULL << (worker_id + cfg.read_workers + 2))); 
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
    std::wcout << L"SuperCopy 極速複製引擎 (CLI 完整版)\n"
               << L"用法: supercopy.exe <來源檔案> <目的檔案> [選項]\n\n"
               << L"參數選項:\n"
               << L"  --ram <GB>           設定緩衝區大小(必須是2的倍數)，預設: 40\n"
               << L"  --chunk <MB>         設定切片大小(必須是2的次方)，預設: 8\n"
               << L"  --read-workers <N>   設定讀取工人數量，預設: 8\n"
               << L"  --write-workers <N>  設定寫入工人數量，預設: 8\n"
               << L"  --smart-wait         啟用智慧等待模式 (預設為極限自旋)\n"
               << L"  --no-direct-io       關閉繞過系統快取 (預設為開啟)\n"
               << L"  --no-pin-cpu         關閉綁定 CPU 核心 (預設為開啟)\n"
               << L"  --zero-copy          使用 Windows 底層拷貝 (無視 RAM 與工人設定)\n";
}

int wmain(int argc, wchar_t* argv[]) {
    setlocale(LC_ALL, "");

    // 註冊 Ctrl+C 攔截器
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    if (argc < 3) {
        PrintHelp();
        return 1; // 錯誤返回碼
    }

    std::wstring src = argv[1];
    std::wstring dst = argv[2];

    SuperCopyConfig config;
    config.total_ram_bytes = 40ULL * 1024 * 1024 * 1024; 
    config.chunk_size_bytes = 8 * 1024 * 1024;           
    config.read_workers = 8;
    config.write_workers = 8;
    config.sync_mode = SyncMode::EXTREME_SPIN;
    config.opt_direct_io = true;
    config.opt_pin_cpu = true;
    config.opt_zero_copy = false;

    for (int i = 3; i < argc; ++i) {
        std::wstring arg = argv[i];
        if (arg == L"--ram" && i + 1 < argc) {
            config.total_ram_bytes = static_cast<size_t>(_wtoi(argv[++i])) * 1024 * 1024 * 1024;
        } else if (arg == L"--chunk" && i + 1 < argc) {
            config.chunk_size_bytes = static_cast<size_t>(_wtoi(argv[++i])) * 1024 * 1024;
        } else if (arg == L"--read-workers" && i + 1 < argc) {
            config.read_workers = _wtoi(argv[++i]);
        } else if (arg == L"--write-workers" && i + 1 < argc) {
            config.write_workers = _wtoi(argv[++i]);
        } else if (arg == L"--smart-wait") {
            config.sync_mode = SyncMode::SMART_WAIT;
        } else if (arg == L"--no-direct-io") {
            config.opt_direct_io = false;
        } else if (arg == L"--no-pin-cpu") {
            config.opt_pin_cpu = false;
        } else if (arg == L"--zero-copy") {
            config.opt_zero_copy = true;
        } else {
            std::wcerr << L"未知的參數: " << arg << L"\n";
            PrintHelp();
            return 1;
        }
    }

    try {
        std::wcout << L"=========================================\n"
                   << L"  SuperCopy 極速複製啟動\n"
                   << L"=========================================\n"
                   << L"來源: " << src << L"\n"
                   << L"目的: " << dst << L"\n"
                   << L"RAM : " << (config.total_ram_bytes / 1024 / 1024 / 1024) << L" GB\n"
                   << L"=========================================\n\n";

        SuperCopyEngine engine(src, dst, config);
        engine.Execute();
        
        if (g_stop_requested) return 130; // 130 是標準的 SIGINT (Ctrl+C) 退出碼

    } catch (const std::exception& e) {
        std::cerr << "\n[致命錯誤] " << e.what() << "\n";
        return 1; 
    }
    return 0; // 成功返回碼
}
