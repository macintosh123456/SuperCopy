#THIS PROJECT IS A BORING GUY WHO WASTES HIS TIME FOR ONE DAY TO SEE HOW GOOD THE GEMINI WILL DO.
#DO NOT TAKE THIS SERIOUSLY AND USE IT ON YOUR OWN FILES!!!!!!!!!!





# SuperCopy
**硬體感知極速複製引擎**<br><br>

SuperCopy 是一個專為 Windows 環境打造的極限效能命令列 (CLI) 工具。<br>
本程式完全捨棄了傳統的作業系統 API 與快取機制，利用底層硬體特性（巨型分頁、實體核心隔離、Direct I/O）將 NVMe SSD 與 PCIe 通道的物理頻寬榨乾到極致。<br><br>

## 核心亮點

### 一、智慧硬體感知 (Smart Hardware Fallback)
程式將「4 核心」視為預設的實體核心基準線。若系統實體核心數 **小於等於 2**，程式會自動偵測硬體環境：<br>
*   **有超線程 (HT)**：強制啟用超線程 (等同注入 `--EHT`)，避免資源餓死。<br>
*   **無超線程 (無 HT)**：為了避免系統與複製引擎搶奪極少的實體核心導致死鎖，程式將自動解除綁定，關閉綁核功能 (等同注入 `--NPCPU`)。<br><br>

### 二、實體核心隔離 (Hardware-Aware CPU Pinning)
程式會動態偵測 CPU 架構。預設情況下，程式會徹底屏除超線程 (Hyper-Threading) 所帶來的資源競爭，將執行緒嚴格綁定於獨立的「實體核心」上運行，保障 L1/L2 快取的絕對獨佔。<br><br>

### 三、動態切片上限 (Dynamic Chunk Limit)
打破傳統寫死的限制，現在的 `--chunk` 上限為 **N/2 (記憶體緩衝區總量的一半)**，讓你在極致大 RAM 的環境下，能使用巨無霸級別的 DMA 傳輸區塊。<br><br>

### 四、大小寫脫敏 CLI (Case-Insensitive)
所有的命令列參數皆可隨意混用大小寫 (如 `--rfd` 或 `--RFD` 皆可)。<br><br>

### 五、中英文雙語介面
透過 `--lang TW` 或 `--lang US` 即可無縫切換終端機輸出的語言介面。<br><br>


## 使用方法 (需以系統管理員身分執行)

```DOS
supercopy.exe <來源路徑> <目的路徑> [選項]
```
範例一：查詢所有指令<br>
supercopy.exe --help<br><br>

範例二：突破實體核心，啟用超線程與自訂參數<br>
supercopy.exe D:\data.raw E:\data.raw --lang tw --ram 16 --RFD 12 --WTD 12 --EHT<br><br>


參數選項 (大小寫不拘)
| 參數 | 說明 | 預設值 |
| ------------------ | -------------------------------- | ---- |
| --help | 顯示指令與參數說明畫面 | 無
| --lang <TW/US> | 切換輸出語言 (繁體中文/美式英語) | TW
| --ram <GB> | 設定緩衝池總容量。必須為 2 的倍數 | 8GB
| --chunk <MB> | 設定底層每次 I/O 請求的切片大小，最大為 RAM 總量的一半 (N/2) | 16MB
| --RFD <N> | 設定 readfromdisk 的數量。注意：RFD+WTD 總數不得超過可用核心數 | 可用核心/2
| --WTD <N> | 設定 writetodisk 的數量 | 可用核心/2
| --SW | 啟用 Smart Wait 智慧等待模式，讓出 CPU 資源避免過載 | 關閉 (自旋)
| --NDIO | No Direct I/O，關閉底層 DMA 傳輸，資料將經過系統快取 | 關閉
| --NPCPU | No Pin CPU，關閉 CPU 實體綁核功能 | 關閉
| --ZC | Zero Copy，放棄所有引擎架構，觸發系統原生複製 | 關閉
| --EHT | Enable Hyper-Threading。手動啟用超線程核心以突破實體核心上限 | 關閉


<br><br>
# SuperCopy
**Hardware-Aware High-Speed Copy Engine**<br><br>

SuperCopy is an extreme-performance Command Line Interface (CLI) tool designed for Windows. <br>
By completely bypassing traditional OS APIs and caching mechanisms, it leverages low-level hardware characteristics (Huge Pages, Physical Core Pinning, Direct I/O) to push the physical bandwidth of NVMe SSDs to their absolute limits.<br><br>


## Core Highlights

### 1. Smart Hardware Fallback
The engine assumes a 4-physical-core baseline. If your system has **2 or fewer physical cores**, it triggers dynamic self-preservation:<br>
*   **If HT is available**: Force-enables Hyper-Threading (behaves like `--EHT`) to prevent worker starvation.<br>
*   **If NO HT is available**: Force-disables CPU pinning (behaves like `--NPCPU`) to prevent OS-level deadlocks and UI freezing.<br><br>

### 2. Physical Core Isolation (CPU Pinning)
The program dynamically detects your CPU architecture. By default, it excludes Hyper-Threading (SMT) to avoid resource contention, strictly pinning threads to independent "Physical Cores" to guarantee absolute exclusivity of L1/L2 caches.<br><br>

### 3. Dynamic Chunk Limit
The maximum `--chunk` limit is now dynamically calculated as **N/2 (Half of your total RAM buffer)**, allowing massive DMA payload sizes on systems with vast amounts of memory.<br><br>

### 4. Case-Insensitive CLI
All command-line arguments can be written in any case (e.g., `--rfd` or `--RFD`).<br><br>

### 5. Bilingual Interface
Seamlessly switch the console output language using `--lang TW` or `--lang US`.<br><br>

---

## Usage (Administrator Privileges Required)

```DOS
supercopy.exe <Source> <Destination> [Options]
```
Example 1: Displaying the Help Menu<br>
supercopy.exe --help<br><br>


Example 2: Utilizing Hyper-Threading with Custom Parameters<br>
supercopy.exe D:\data.raw E:\data.raw --lang us --ram 16 --RFD 12 --WTD 12 --EHT<br><br>


Parameter Options (Case-Insensitive)
| Parameter | Description | Default |
| ------------------ | -------------------------------- | ---- |
| --help | Show the help menu.,None
| --lang <TW/US> | Switch output language (Traditional Chinese / US English) | TW
| --ram <GB> | Set total buffer pool size. Must be a multiple of 2 | 8GB
| --chunk <MB> | Set I/O request chunk size. Maximum is N/2 (Half of total RAM) | 16MB
| --RFD <N> | Number of readfromdisk threads. Note: RFD+WTD must not exceed available cores | Cores/2
| --WTD <N> | Number of writetodisk threads | Cores/2
| --SW | Enable Smart Wait mode to yield CPU resources | Off (Spin)
| --NDIO | No Direct I/O. Disables DMA transfer; data will pass through OS cache | Off
| --NPCPU | No Pin CPU. Disables physical core isolation pinning | Off
| --ZC | Zero Copy. Abandons the engine and triggers Windows native copy | Off
| --EHT | Enable Hyper-Threading. Manually enable logical processors | Off
