SuperCopy <br>
硬體感知極速複製引擎<br><br>

SuperCopy 是一個專為 Windows 環境打造的極限效能命令列 (CLI) 工具。<br>
本程式完全捨棄了傳統的作業系統 API 與快取機制，利用底層硬體特性（巨型分頁、實體核心隔離、Direct I/O）將 NVMe SSD 與 PCIe 通道的物理頻寬榨乾到極致。<br><br>

核心亮點<br>
一、實體核心隔離 (Hardware-Aware CPU Pinning):程式會動態偵測 CPU 架構。預設情況下，程式會徹底屏除超線程 (Hyper-Threading) 所帶來的資源競爭，將四重工作者嚴格綁定於獨立的「實體核心」上運行，保障 L1/L2 快取的絕對獨佔。<br>
二、大小寫脫敏 CLI (Case-Insensitive):所有的命令列參數皆可隨意混用大小寫 (如 --rfd 或 --RFD 皆可)。<br>
三、中英文雙語介面:透過 --lang TW 或 --lang US 即可無縫切換終端機輸出的語言介面。<br>

使用方法 (需以系統管理員身分執行)<br>
```DOS<br>
supercopy.exe <來源路徑> <目的路徑> [選項]
```

範例：突破實體核心，啟用超線程與自訂參數<br>
```DOS<br>
supercopy.exe D:\data.raw E:\data.raw --lang tw --ram 16 --RFD 12 --WTD 12 --EHT
```

參數選項 (大小寫不拘)
| 參數 | 說明 | 預設值 |
| ------------------ | -------------------------------- | ---- |
| --lang <TW/US>     | 切換輸出語言 (繁體中文/美式英語)    | TW |
| --ram <GB>         | 必須為 2 的倍數                   | 8 |
| --chunk <MB>       | 設定底層每次 I/O 請求的切片大小，最大為 1024 (1GB) | 16 |
| --RFD <N>          | 設定 readfromdisk 的數量。注意：RFD+WTD 總數不得超過可用核心數 | 可用核心/2 |
| --WTD <N>          | 設定 writetodisk 的數量 | 可用核心/2 |
| --SW               | 啟用 Smart Wait 智慧等待模式，讓出 CPU 資源避免過載 | 關閉 (自旋) |
| --NDIO             | No Direct I/O，關閉底層 DMA 傳輸，資料將經過系統快取 | 關閉  |
| --NPCPU            | No Pin CPU，關閉 CPU 實體綁核功能 | 關閉 |
| --ZC               | Zero Copy 放棄所有引擎架構，觸發系統原生複製 | 關閉 |
| --EHT              | 啟用 Hyper-Threading，啟用此參數後，系統將允許把任務指派到虛擬超線程核心上，突破實體核心的數量限制 | 關閉 |


SuperCopy<br>
Hardware-Aware High-Speed Copy Engine<br><br>

SuperCopy is an extreme-performance Command Line Interface (CLI) tool designed for Windows. <br>
By completely bypassing traditional OS APIs and caching mechanisms, it leverages low-level hardware characteristics (Huge Pages, Physical Core Pinning, Direct I/O) to push the physical bandwidth of NVMe SSDs to their absolute limits.<br><br>

Core Highlights<br>
1.Physical Core Isolation (CPU Pinning):The program dynamically detects your CPU architecture. By default, it excludes Hyper-Threading (SMT) to avoid resource contention, strictly pinning workers to independent "Physical Cores" to guarantee absolute exclusivity of L1/L2 caches.<br>
2.Case-Insensitive CLI:All command-line arguments can be written in any case (e.g., --rfd or --RFD).<br>
3.Bilingual Interface:Seamlessly switch the console output language using --lang TW or --lang US.<br>

Usage (Administrator Privileges Required)<br>
```DOS<br>
supercopy.exe <Source> <Destination> [Options]
```

Example: Utilizing Hyper-Threading with Custom Parameters<br>
```DOS<br>
supercopy.exe D:\data.raw E:\data.raw --lang us --ram 16 --RFD 12 --WTD 12 --EHT
```

Parameter Options (Case-Insensitive)
| Parameter | Description | Default |
| ------------------ | -------------------------------- | ---- |
| --lang <TW/US>     | Switch output language (Traditional Chinese / US English)    | TW |
| --ram <GB>         | Set total buffer pool size. Must be a multiple of 2                   | 8 |
| --chunk <MB>       | --chunk <MB>Set I/O request chunk size. Maximum is 1024 (1GB) | 16 |
| --RFD <N>          | --RFD <N>Number of readfromdisk threads. Note: RFD+WTD must not exceed available cores | Cores/2 |
| --WTD <N>          | Number of writetodisk threads | Cores/2 |
| --SW               | Enable Smart Wait mode to yield CPU resources | Off (Spin) |
| --NDIO             | --NDIONo Direct I/O. Disables DMA transfer; data will pass through OS cache | Off  |
| --NPCPU            | No Pin CPU. Disables physical core isolation pinning | Off |
| --ZC               | Zero Copy. Abandons the engine and triggers Windows native copy | Off |
| --EHT              | Enable Hyper-Threading. Unlocks logical processors, allowing RFD and WTD configurations to exceed physical core limits | Off |
