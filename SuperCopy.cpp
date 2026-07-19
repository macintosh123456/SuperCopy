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

#pragma comment(lib, "Synchronization.lib")

// ==========================================
// 1. 組態與同步機制 (Sync & Config)
// ==========================================
enum class SyncMode { EXTREME_SPIN, SMART_WAIT };

struct SuperCopyConfig {
    size_t total_ram_bytes;       // N (需為 2GB 倍數)
    size_t chunk_size_bytes;      // F (需為 2 的次方)
    int read_workers;             // readfromdisk 人數
    int write_workers;            // writetodisk 人數
    SyncMode sync_mode;           
    bool opt_direct_io;           
    bool opt_pin_cpu;             
    bool opt_zero_copy;           
};

// 雙模式無鎖同步器 (Ping-Pong 狀態切換專用)
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
            while (InterlockedCompareExchange(&state, 0, 0) == 0) YieldProcessor();
        } else {
            LONG undesired = 0;
            while (InterlockedCompareExchange(&state, 0, 0) == 0) {
                WaitOnAddress((volatile PVOID)&state, &undesired, sizeof(LONG), INFINITE);
            }
        }
    }
    void Reset() { InterlockedExchange(&state, 0); }
};

// ==========================================
// 2. 工作者任務佇列與任務單 (Task Definitions)
// ==========================================
// 這是一張「任務單」，告訴工人去哪裡搬、搬到哪
struct IOTask {
    HANDLE hFile;
    uint64_t file_offset;
    uint8_t* mem_addr;
    DWORD size;
    std::atomic<int>* tasks_remaining_counter; // 工人做完後遞減此計數器，回報指揮官
    bool is_poison_pill; // 結束執行緒的毒藥訊號
};

// 簡單的執行緒安全任務佇列 (實務上在極限環境可替換為 Lock-free Queue 或 IOCP)
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
        cv.wait(lock, [this]() { return !tasks.empty(); });
        IOTask task = tasks.front();
        tasks.pop();
        return task;
    }
};

// ==========================================
// 3. 核心引擎 (SuperCopyEngine)
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

    TaskQueue read_queue;
    TaskQueue write_queue;
    std::vector<std::thread> read_workers;
    std::vector<std::thread> write_workers;

    std::atomic<bool> global_eof{false};
    uint64_t total_file_size = 0;

    // 啟動系統管理員巨型分頁權限
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

public:
    SuperCopyEngine(std::wstring src, std::wstring dst, SuperCopyConfig config) 
        : src_path(src), dst_path(dst), cfg(config), 
          sync_N1(config.sync_mode), sync_N2(config.sync_mode),
          sync_disk_N1(config.sync_mode), sync_disk_N2(config.sync_mode) {
        
        // 檢查約束條件
        const size_t TWO_GB = 2ULL * 1024 * 1024 * 1024;
        if (cfg.total_ram_bytes % TWO_GB != 0) throw std::invalid_argument("RAM 必須是 2GB 的倍數");
        if ((cfg.chunk_size_bytes & (cfg.chunk_size_bytes - 1)) != 0) throw std::invalid_argument("Chunk 必須是 2 的次方");
    }

    ~SuperCopyEngine() {
        if (memory_pool) VirtualFree(memory_pool, 0, MEM_RELEASE);
    }

    void Execute() {
        if (cfg.opt_zero_copy) {
            std::wcout << L"啟動 Zero-Copy 模式...\n";
            CopyFileExW(src_path.c_str(), dst_path.c_str(), nullptr, nullptr, FALSE, COPY_FILE_NO_BUFFERING);
            return;
        }

        EnableHugePagesPrivilege();
        memory_pool = VirtualAlloc(NULL, cfg.total_ram_bytes, MEM_COMMIT | MEM_RESERVE | MEM_LARGE_PAGES, PAGE_READWRITE);
        if (!memory_pool) throw std::runtime_error("巨型分頁配置失敗");

        size_t half_N = cfg.total_ram_bytes / 2;
        buffer_N1 = static_cast<uint8_t*>(memory_pool);
        buffer_N2 = buffer_N1 + half_N;

        sync_disk_N1.NotifyReady();
        sync_disk_N2.NotifyReady();

        // 啟動多位工人 (Workers)
        for (int i = 0; i < cfg.read_workers; ++i) read_workers.emplace_back(&SuperCopyEngine::readfromdisk, this, i);
        for (int i = 0; i < cfg.write_workers; ++i) write_workers.emplace_back(&SuperCopyEngine::writetodisk, this, i);

        // 啟動兩位指揮官 (Managers)
        std::thread manager_read(&SuperCopyEngine::disktoram, this);
        std::thread manager_write(&SuperCopyEngine::ramtodisk, this);

        manager_read.join();
        manager_write.join();

        // 派發結束任務單給工人
        for (int i = 0; i < cfg.read_workers; ++i) read_queue.Push({NULL, 0, nullptr, 0, nullptr, true});
        for (int i = 0; i < cfg.write_workers; ++i) write_queue.Push({NULL, 0, nullptr, 0, nullptr, true});

        for (auto& t : read_workers) t.join();
        for (auto& t : write_workers) t.join();

        std::wcout << L"超級複製完成！\n";
    }

private:
    // ==========================================
    // 4. 指揮官：負責大腦思考與派發任務
    // ==========================================
    void disktoram() {
        if (cfg.opt_pin_cpu) SetThreadAffinityMask(GetCurrentThread(), 1); // 指揮官綁定 CPU 0

        DWORD flags = FILE_ATTRIBUTE_NORMAL;
        if (cfg.opt_direct_io) flags |= FILE_FLAG_NO_BUFFERING; 

        HANDLE hSrc = CreateFileW(src_path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, flags, NULL);
        
        LARGE_INTEGER fs;
        GetFileSizeEx(hSrc, &fs);
        total_file_size = fs.QuadPart;

        size_t half_N = cfg.total_ram_bytes / 2;
        bool fill_N1 = true;
        uint64_t current_offset = 0;

        while (current_offset < total_file_size) {
            uint8_t* current_buf = fill_N1 ? buffer_N1 : buffer_N2;
            PingPongSync& target_disk_sync = fill_N1 ? sync_disk_N1 : sync_disk_N2;
            PingPongSync& target_ram_sync = fill_N1 ? sync_N1 : sync_N2;

            // 1. 等待這塊卡車空間 (RAM) 被清空
            target_disk_sync.WaitUntilReady();
            target_disk_sync.Reset();

            // 2. 計算這輪要塞多少資料、切幾份，發放任務單
            size_t bytes_to_fill = std::min((uint64_t)half_N, total_file_size - current_offset);
            int num_tasks = (bytes_to_fill + cfg.chunk_size_bytes - 1) / cfg.chunk_size_bytes;
            std::atomic<int> pending_read_tasks{num_tasks};

            size_t filled = 0;
            while (filled < bytes_to_fill) {
                DWORD chunk = static_cast<DWORD>(std::min((size_t)cfg.chunk_size_bytes, bytes_to_fill - filled));
                read_queue.Push({hSrc, current_offset + filled, current_buf + filled, chunk, &pending_read_tasks, false});
                filled += chunk;
            }

            // 3. 指揮官自旋等待，直到所有讀取工人回報任務完成
            while (pending_read_tasks.load() > 0) {
                YieldProcessor(); 
            }

            // 4. 卡車滿了，通知下車指揮官 (ramtodisk)
            target_ram_sync.NotifyReady();
            
            current_offset += bytes_to_fill;
            fill_N1 = !fill_N1;
        }
        CloseHandle(hSrc);
        global_eof = true;
    }

    void ramtodisk() {
        if (cfg.opt_pin_cpu) SetThreadAffinityMask(GetCurrentThread(), 2); // 指揮官綁定 CPU 1

        DWORD flags = FILE_ATTRIBUTE_NORMAL;
        if (cfg.opt_direct_io) flags |= FILE_FLAG_NO_BUFFERING;

        HANDLE hDst = CreateFileW(dst_path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, flags, NULL);

        // 消除 Metadata 更新 (預佔空間)
        LARGE_INTEGER size_li;
        size_li.QuadPart = total_file_size; // 此變數由 disktoram 設定
        // 確保檔案大小已知，短暫自旋等待 disktoram 取得檔案大小
        while(total_file_size == 0 && !global_eof) YieldProcessor(); 
        
        SetFilePointerEx(hDst, size_li, NULL, FILE_BEGIN);
        SetEndOfFile(hDst); 

        size_t half_N = cfg.total_ram_bytes / 2;
        bool drain_N1 = true;
        uint64_t current_offset = 0;

        while (current_offset < total_file_size) {
            uint8_t* current_buf = drain_N1 ? buffer_N1 : buffer_N2;
            PingPongSync& source_ram_sync = drain_N1 ? sync_N1 : sync_N2;
            PingPongSync& source_disk_sync = drain_N1 ? sync_disk_N1 : sync_disk_N2;

            // 1. 等待上車指揮官把卡車填滿
            source_ram_sync.WaitUntilReady();
            source_ram_sync.Reset();

            size_t bytes_to_drain = std::min((uint64_t)half_N, total_file_size - current_offset);
            int num_tasks = (bytes_to_drain + cfg.chunk_size_bytes - 1) / cfg.chunk_size_bytes;
            std::atomic<int> pending_write_tasks{num_tasks};

            // 2. 派發寫入任務單給工人
            size_t drained = 0;
            while (drained < bytes_to_drain) {
                DWORD chunk = static_cast<DWORD>(std::min((size_t)cfg.chunk_size_bytes, bytes_to_drain - drained));
                write_queue.Push({hDst, current_offset + drained, current_buf + drained, chunk, &pending_write_tasks, false});
                drained += chunk;
            }

            // 3. 指揮官自旋等待，直到所有寫入工人完成
            while (pending_write_tasks.load() > 0) {
                YieldProcessor();
            }

            // 4. 卡車空了，通知上車指揮官可以繼續裝貨
            source_disk_sync.NotifyReady();

            current_offset += bytes_to_drain;
            drain_N1 = !drain_N1;
        }
        CloseHandle(hDst);
    }

    // ==========================================
    // 5. 工人小隊：只負責幹體力活
    // ==========================================
    void readfromdisk(int worker_id) {
        // 可選：將工人綁定到特定 CPU 群組 (略過實作細節，僅示範邏輯)
        if (cfg.opt_pin_cpu) SetThreadAffinityMask(GetCurrentThread(), (1ULL << (worker_id + 2))); 

        while (true) {
            IOTask task = read_queue.Pop();
            if (task.is_poison_pill) break; // 收到毒藥，下班

            // 工人執行真正的 I/O 讀取
            OVERLAPPED ov = {0};
            ov.Offset = task.file_offset & 0xFFFFFFFF;
            ov.OffsetHigh = (task.file_offset >> 32);
            DWORD bytesRead;
            
            // 由於使用多線程讀取同一個 File Handle，必須傳入 OVERLAPPED 指定精確 Offset
            ReadFile(task.hFile, task.mem_addr, task.size, &bytesRead, &ov);

            // 回報指揮官任務完成 (無鎖遞減)
            task.tasks_remaining_counter->fetch_sub(1, std::memory_order_release);
        }
    }

    void writetodisk(int worker_id) {
        if (cfg.opt_pin_cpu) SetThreadAffinityMask(GetCurrentThread(), (1ULL << (worker_id + cfg.read_workers + 2))); 

        while (true) {
            IOTask task = write_queue.Pop();
            if (task.is_poison_pill) break; // 收到毒藥，下班

            // 工人執行真正的 I/O 寫入
            OVERLAPPED ov = {0};
            ov.Offset = task.file_offset & 0xFFFFFFFF;
            ov.OffsetHigh = (task.file_offset >> 32);
            DWORD bytesWritten;
            
            WriteFile(task.hFile, task.mem_addr, task.size, &bytesWritten, &ov);

            // 回報指揮官任務完成
            task.tasks_remaining_counter->fetch_sub(1, std::memory_order_release);
        }
    }
};

int main() {
    try {
        SuperCopyConfig config;
        config.total_ram_bytes = 40ULL * 1024 * 1024 * 1024; // 40GB
        config.chunk_size_bytes = 8 * 1024 * 1024;           // 8MB Chunks
        config.read_workers = 8;                             // 8 位讀取工人
        config.write_workers = 8;                            // 8 位寫入工人
        config.sync_mode = SyncMode::EXTREME_SPIN;           // 極限自旋模式
        config.opt_direct_io = true;                         
        config.opt_pin_cpu = true;                           
        config.opt_zero_copy = false;                        

        std::wcout << L"初始化 SuperCopy 引擎...\n";
        SuperCopyEngine engine(L"D:\\huge_source.raw", L"E:\\huge_target.raw", config);
        engine.Execute();
        
    } catch (const std::exception& e) {
        std::cerr << "錯誤: " << e.what() << "\n";
    }
    return 0;
}
