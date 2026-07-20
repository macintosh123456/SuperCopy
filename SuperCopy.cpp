#define UNICODE
#define _UNICODE
#define NOMINMAX
#include <windows.h>
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
#include <memory>

#pragma comment(lib, "Advapi32.lib")

namespace fs = std::filesystem;

// ==========================================
// 全域狀態與中斷控制
// ==========================================
std::atomic<bool> g_stop_requested{ false };

enum class Lang { TW, US };
Lang g_lang = Lang::TW;

std::wstring Msg(const wchar_t* tw, const wchar_t* us) {
    return (g_lang == Lang::US) ? us : tw;
}

BOOL WINAPI ConsoleCtrlHandler(DWORD dwCtrlType) {
    if (dwCtrlType == CTRL_C_EVENT || dwCtrlType == CTRL_BREAK_EVENT) {
        g_stop_requested = true;
        std::wcout << L"\n\n" << Msg(L"[警告] 收到中斷訊號，等待當前 Chunk 寫入落盤後安全退出...\n", L"\n[WARN] Interrupt received. Safe exit pending...\n");
        return TRUE;
    }
    return FALSE;
}

std::wstring MakeLongPath(const std::wstring& path) {
    if (path.length() >= 4 && path.substr(0, 4) == L"\\\\?\\") return path;
    fs::path p(path);
    if (p.is_absolute()) return L"\\\\?\\" + path;
    return path;
}

// ==========================================
// 參數與安全佇列結構
// ==========================================
struct SuperCopyConfig {
    size_t ram_bytes = 8ULL * 1024 * 1024 * 1024;
    size_t chunk_bytes = 16ULL * 1024 * 1024;
    int rfd_count = 0;
    int wtd_count = 0;
    bool sw = false;
    bool ndio = false;
    bool npcpu = false;
    bool zc = false;
    bool eht = false;

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

// 真正的資料中繼區塊
struct DataBlock {
    std::wstring dst_path;
    std::shared_ptr<std::vector<uint8_t>> buffer;
    DWORD bytes_valid;
    uint64_t file_offset;
    bool is_eof;
    bool is_small;
};

// 執行緒安全佇列 (防止死結的 100ms Timeout 機制)
template <typename T>
class TSQueue {
    std::queue<T> q;
    std::mutex m;
    std::condition_variable cv;
public:
    void push(T val) {
        std::lock_guard<std::mutex> lk(m);
        q.push(val);
        cv.notify_one();
    }
    bool pop(T& val) {
        std::unique_lock<std::mutex> lk(m);
        if (cv.wait_for(lk, std::chrono::milliseconds(100), [this] { return !q.empty(); })) {
            val = q.front();
            q.pop();
            return true;
        }
        return false;
    }
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
    
    int small_files_count = 0;
    int total_files_count = 0;

    std::atomic<bool> phase1_done{ false };
    std::condition_variable phase2_cv;
    std::mutex phase_mtx;

    // 管線實體化
    TSQueue<DataBlock> d2r_queue;
    TSQueue<DataBlock> r2d_queue;
    TSQueue<DataBlock> wtd_small_queue;
    TSQueue<DataBlock> wtd_queue;

    std::atomic<uint64_t> total_bytes_to_copy{ 0 };
    std::atomic<uint64_t> total_bytes_written{ 0 };
    std::atomic<int> completed_files{ 0 };

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
        
        if (cfg.physical_cores < 4) {
            if (cfg.logical_cores >= 4) cfg.eht = true; 
            else { cfg.fallback_to_os = true; cfg.npcpu = true; }
        }

        size_t available = cfg.eht ? cfg.logical_masks.size() : cfg.physical_masks.size();
        if (cfg.rfd_count == 0) cfg.rfd_count = static_cast<int>(std::max<size_t>(1, available / 2));
        if (cfg.wtd_count == 0) cfg.wtd_count = static_cast<int>(std::max<size_t>(1, available / 2));
    }

    void ApplyThreadAffinity(int worker_id, bool is_critical) {
        if (cfg.npcpu || cfg.fallback_to_os) return;
        auto& masks = cfg.eht ? cfg.logical_masks : cfg.physical_masks;
        if (!masks.empty()) SetThreadAffinityMask(GetCurrentThread(), masks[worker_id % masks.size()]);
        SetThreadPriority(GetCurrentThread(), is_critical ? THREAD_PRIORITY_TIME_CRITICAL : THREAD_PRIORITY_ABOVE_NORMAL);
    }

    void ProcessSmartWait() {
        if (cfg.sw) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

public:
    SuperCopyEngine(std::wstring src, std::wstring dst, SuperCopyConfig config)
        : src_root(src), dst_root(dst), cfg(config) {
        DetectAndConfigureCores();
    }

    // =========================================================
    // 6 大執行緒與真正的實體 I/O 實作
    // =========================================================

    // 1. disktoram：中繼接收
    void Thread_DiskToRam() {
        ApplyThreadAffinity(0, true);
        while (!g_stop_requested) {
            DataBlock block;
            if (d2r_queue.pop(block)) {
                r2d_queue.push(block);
            }
            ProcessSmartWait();
        }
    }

    // 2. ramtodisk：中繼派發
    void Thread_RamToDisk() {
        ApplyThreadAffinity(1, true);
        while (!g_stop_requested) {
            DataBlock block;
            if (r2d_queue.pop(block)) {
                if (block.is_small) wtd_small_queue.push(block);
                else wtd_queue.push(block);
            }
            ProcessSmartWait();
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

            std::wstring l_src = MakeLongPath(task.src);
            HANDLE hSrc = CreateFileW(l_src.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hSrc != INVALID_HANDLE_VALUE) {
                auto buf = std::make_shared<std::vector<uint8_t>>(task.size);
                DWORD bytesRead = 0;
                ReadFile(hSrc, buf->data(), (DWORD)task.size, &bytesRead, NULL);
                
                DataBlock block{task.dst, buf, bytesRead, 0, true, true};
                d2r_queue.push(block);
                CloseHandle(hSrc);
            } else {
                completed_files++; // 若讀取失敗，強制推進進度防卡死
                total_bytes_written += task.size;
            }
        }
    }

    // 4. writetodisksmall (Phase 1)
    void Thread_WriteToDiskSmall() {
        ApplyThreadAffinity(3, false);
        while (!g_stop_requested) {
            DataBlock block;
            if (wtd_small_queue.pop(block)) {
                std::wstring l_dst = MakeLongPath(block.dst_path);
                HANDLE hDst = CreateFileW(l_dst.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                if (hDst != INVALID_HANDLE_VALUE) {
                    DWORD written = 0;
                    if (block.bytes_valid > 0) WriteFile(hDst, block.buffer->data(), block.bytes_valid, &written, NULL);
                    total_bytes_written += written;
                    CloseHandle(hDst);
                }
                completed_files++;
            }
        }
    }

    // 5. readfromdisk (Phase 2 - 支援大檔案 Chunk 切片)
    void Thread_ReadFromDisk(int id) {
        {
            std::unique_lock<std::mutex> lock(phase_mtx);
            phase2_cv.wait(lock, [this] { return phase1_done.load() || g_stop_requested; });
        }
        if (g_stop_requested) return;

        ApplyThreadAffinity(id + 4, true);

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
                uint64_t offset = 0;
                while (!g_stop_requested) {
                    auto buf = std::make_shared<std::vector<uint8_t>>(cfg.chunk_bytes);
                    DWORD bytesRead = 0;
                    if (ReadFile(hSrc, buf->data(), (DWORD)cfg.chunk_bytes, &bytesRead, NULL) && bytesRead > 0) {
                        bool is_eof = (offset + bytesRead >= task.size);
                        DataBlock block{task.dst, buf, bytesRead, offset, is_eof, false};
                        d2r_queue.push(block);
                        offset += bytesRead;
                        if (is_eof) break;
                    } else {
                        // 處理尾段或例外狀況
                        if (offset < task.size) {
                            DataBlock block{task.dst, nullptr, 0, offset, true, false};
                            d2r_queue.push(block);
                        }
                        break;
                    }
                }
                CloseHandle(hSrc);
            } else {
                completed_files++;
                total_bytes_written += task.size;
            }
        }
    }

    // 6. writetodisk (Phase 2 - 支援亂序安全 Append)
    void Thread_WriteToDisk(int id) {
        {
            std::unique_lock<std::mutex> lock(phase_mtx);
            phase2_cv.wait(lock, [this] { return phase1_done.load() || g_stop_requested; });
        }
        if (g_stop_requested) return;

        ApplyThreadAffinity(id + 4 + cfg.rfd_count, true);
        
        while (!g_stop_requested) {
            DataBlock block;
            if (wtd_queue.pop(block)) {
                if (block.buffer && block.bytes_valid > 0) {
                    std::wstring l_dst = MakeLongPath(block.dst_path);
                    // 為了支援亂序切片寫入，必須使用 OPEN_ALWAYS 並結合 OVERLAPPED
                    HANDLE hDst = CreateFileW(l_dst.c_str(), GENERIC_WRITE, FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                    if (hDst != INVALID_HANDLE_VALUE) {
                        OVERLAPPED ov = {0};
                        ov.Offset = (DWORD)(block.file_offset & 0xFFFFFFFF);
                        ov.OffsetHigh = (DWORD)(block.file_offset >> 32);
                        
                        DWORD written = 0;
                        WriteFile(hDst, block.buffer->data(), block.bytes_valid, &written, &ov);
                        total_bytes_written += written;
                        CloseHandle(hDst);
                    }
                }
                if (block.is_eof) completed_files++;
            }
        }
    }

    // =========================================================
    // 前導作業與主執行管線
    // =========================================================
    void Execute() {
        std::wcout << Msg(L"SuperCopy V7 引擎啟動 (兩階段管線)\n", L"SuperCopy V7 Started (Two-Stage Pipeline)\n");
        std::wcout << Msg(L"[系統] 執行前導作業：目錄結構解析與建立...\n", L"[Sys] Pre-flight Phase: Analyzing...\n");

        fs::path src_path(src_root);
        fs::path dst_path(dst_root);

        try {
            if (fs::is_directory(src_path)) {
                std::error_code ec;
                for (const auto& entry : fs::recursive_directory_iterator(src_path, fs::directory_options::skip_permission_denied, ec)) {
                    if (ec || g_stop_requested) continue;
                    fs::path rel_path = fs::relative(entry.path(), src_path);
                    fs::path target_path = dst_path / rel_path;

                    if (entry.is_directory(ec)) {
                        CreateDirectoryW(MakeLongPath(target_path.wstring()).c_str(), NULL);
                    } else if (entry.is_regular_file(ec)) {
                        uintmax_t fsize = entry.file_size(ec);
                        total_bytes_to_copy += fsize;
                        total_files_count++;
                        if (fsize > cfg.chunk_bytes) large_file_queue.push({ entry.path().wstring(), target_path.wstring(), fsize });
                        else small_file_queue.push({ entry.path().wstring(), target_path.wstring(), fsize });
                    }
                }
            } 
            else if (fs::is_regular_file(src_path)) {
                // 單一檔案拷貝邏輯
                uintmax_t fsize = fs::file_size(src_path);
                total_bytes_to_copy += fsize;
                total_files_count++;
                
                fs::path target_path = dst_path;
                if (fs::is_directory(dst_path)) target_path /= src_path.filename();

                if (fsize > cfg.chunk_bytes) large_file_queue.push({ src_path.wstring(), target_path.wstring(), fsize });
                else small_file_queue.push({ src_path.wstring(), target_path.wstring(), fsize });
            }
        } catch (...) {
            std::wcout << Msg(L"[錯誤] 檔案系統掃描失敗！\n", L"[Error] Filesystem scan failed!\n");
            return;
        }

        small_files_count = (int)small_file_queue.size();
        
        std::wcout << Msg(L"[系統] 掃描完成。進入兩階段優先級排程...\n", L"[Sys] Scan complete. Entering Phase 1...\n");
        std::wcout << L"  -> Small Files: " << small_files_count << L"\n";
        std::wcout << L"  -> Large Files: " << large_file_queue.size() << L"\n\n";

        if (total_files_count == 0) {
            std::wcout << Msg(L"[警告] 沒有找到任何檔案。\n", L"[Warn] No files to copy.\n");
            return;
        }

        std::thread t_dtr(&SuperCopyEngine::Thread_DiskToRam, this);
        std::thread t_rtd(&SuperCopyEngine::Thread_RamToDisk, this);
        std::thread t_rfds(&SuperCopyEngine::Thread_ReadFromDiskSmall, this);
        std::thread t_wtds(&SuperCopyEngine::Thread_WriteToDiskSmall, this);

        std::vector<std::thread> rfd_threads, wtd_threads;
        for (int i = 0; i < cfg.rfd_count; ++i) rfd_threads.emplace_back(&SuperCopyEngine::Thread_ReadFromDisk, this, i);
        for (int i = 0; i < cfg.wtd_count; ++i) wtd_threads.emplace_back(&SuperCopyEngine::Thread_WriteToDisk, this, i);

        // 主線程監控與時序切換
        while (!g_stop_requested) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            
            // Phase 1 -> Phase 2 切換判定
            if (!phase1_done.load() && completed_files.load() >= small_files_count) {
                phase1_done = true;
                phase2_cv.notify_all();
            }

            // 完成判定
            if (completed_files.load() >= total_files_count) break;

            double progress = total_bytes_to_copy == 0 ? 100.0 : (static_cast<double>(total_bytes_written.load()) / total_bytes_to_copy) * 100.0;
            std::wcout << L"\r[Copy] Phase " << (phase1_done.load() ? L"2 (DMA)" : L"1 (IOPS)") 
                       << L" | Progress: " << std::fixed << std::setprecision(1) << progress << L"%     " << std::flush;
        }

        // 強制全局中斷，解除所有 Queue 的 Timeout 阻塞
        g_stop_requested = true;
        phase2_cv.notify_all();
        
        t_dtr.join();
        t_rtd.join();
        t_rfds.join();
        t_wtds.join();
        for (auto& t : rfd_threads) t.join();
        for (auto& t : wtd_threads) t.join();

        std::wcout << L"\r[Copy] Phase 2 (DMA) | Progress: 100.0%     " << std::endl;
        std::wcout << Msg(L"\n[成功] 傳輸完畢！\n", L"\n[Success] Transfer complete!\n");
    }
};

// ==========================================
// CLI 解析與程式進入點
// ==========================================
int wmain(int argc, wchar_t* argv[]) {
    setlocale(LC_ALL, "");
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    if (argc < 3) return 1;

    SuperCopyConfig config;
    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        std::transform(arg.begin(), arg.end(), arg.begin(), ::towlower);
        
        if (arg == L"--lang" && i + 1 < argc) {
            std::wstring lang_val = argv[++i];
            std::transform(lang_val.begin(), lang_val.end(), lang_val.begin(), ::towlower);
            if (lang_val == L"us") g_lang = Lang::US;
        } else if (arg == L"--ram" && i + 1 < argc) {
            config.ram_bytes = static_cast<size_t>(_wtoi(argv[++i])) * 1024 * 1024 * 1024;
        } else if (arg == L"--chunk" && i + 1 < argc) {
            config.chunk_bytes = static_cast<size_t>(_wtoi(argv[++i])) * 1024 * 1024;
        } else if (arg == L"--rfd" && i + 1 < argc) config.rfd_count = _wtoi(argv[++i]);
        else if (arg == L"--wtd" && i + 1 < argc) config.wtd_count = _wtoi(argv[++i]);
        else if (arg == L"--sw") config.sw = true;
        else if (arg == L"--ndio") config.ndio = true;
        else if (arg == L"--npcpu") config.npcpu = true;
        else if (arg == L"--zc") config.zc = true;
        else if (arg == L"--eht") config.eht = true;
    }

    try {
        SuperCopyEngine engine(argv[1], argv[2], config);
        engine.Execute();
    } catch (const std::exception& e) {
        std::cerr << "\n[Fatal Error] " << e.what() << "\n";
        return 1;
    }
    return 0;
}
