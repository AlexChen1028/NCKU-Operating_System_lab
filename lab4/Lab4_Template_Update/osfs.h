#ifndef _OSFS_H
#define _OSFS_H

#include <linux/types.h>      // 引入基本型別定義 (如 uint32_t)
#include <linux/fs.h>         // Linux 檔案系統核心定義
#include <linux/bitmap.h>     // 用於位圖 (Bitmap) 操作
#include <linux/time.h>       // 時間相關結構
#include <linux/slab.h>       // 記憶體分配 (kmalloc 等)
#include <linux/vmalloc.h>    // 虛擬記憶體分配 (vmalloc)
#include <linux/string.h>     // 字串處理
#include <linux/module.h>     // 核心模組定義

// 檔案系統的魔術數字 (Magic Number)，用來識別這是 "OSFS" 檔案系統
#define OSFS_MAGIC 0x051AB520

//#define BLOCK_SIZE 4096       // 每個資料區塊的大小為 4KB (通常依賴 PAGE_SIZE)
#define INODE_COUNT 20          // 檔案系統限制：最多 20 個 Inode (檔案/目錄)
#define DATA_BLOCK_COUNT 20     // 檔案系統限制：假設只有 20 個資料區塊
#define MAX_FILENAME_LEN 255    // 檔名最大長度
// 計算一個 Block 可以存多少個目錄項目 (Directory Entries)
#define MAX_DIR_ENTRIES (BLOCK_SIZE / sizeof(struct osfs_dir_entry))

// 輔助巨集：計算位圖 (Bitmap) 需要多少個 unsigned long 來儲存
// BITS_PER_LONG 在 64位元系統是 64，32位元系統是 32
#define BITMAP_SIZE(bits) (((bits) + BITS_PER_LONG - 1) / BITS_PER_LONG)

// 計算 Inode Bitmap 和 Block Bitmap 的大小
#define INODE_BITMAP_SIZE BITMAP_SIZE(INODE_COUNT)
#define BLOCK_BITMAP_SIZE BITMAP_SIZE(DATA_BLOCK_COUNT)

#define ROOT_INODE 1            // 定義根目錄 (Root Directory) 的 Inode 編號為 1

/**
 * 結構: osfs_sb_info
 * 描述: OSFS 的超級區塊資訊 (Superblock Info)，存在記憶體中，管理整個檔案系統。
 */
struct osfs_sb_info {
    uint32_t magic;              // 魔術數字 (驗證檔案系統身分)
    uint32_t block_size;         // 資料區塊大小
    uint32_t inode_count;        // Inode 總數
    uint32_t block_count;        // 資料區塊總數
    uint32_t nr_free_inodes;     // 目前剩餘可用的 Inode 數量
    uint32_t nr_free_blocks;     // 目前剩餘可用的資料區塊數量
    unsigned long *inode_bitmap; // 指向 Inode 位圖的指標 (用來追蹤哪個 Inode 被使用了)
    unsigned long *block_bitmap; // 指向 Block 位圖的指標 (用來追蹤哪個 Block 被使用了)
    void *inode_table;           // 指向 Inode 表格的指標 (存放所有 osfs_inode 結構的陣列)
    void *data_blocks;           // 指向實際資料儲存區的指標 (所有資料塊都在這)
};

/**
 * 結構: osfs_dir_entry
 * 描述: 目錄項目結構，代表目錄下的一個檔案。
 */
struct osfs_dir_entry {
    char filename[MAX_FILENAME_LEN]; // 檔名
    uint32_t inode_no;               // 對應的 Inode 編號
};

/**
 * 結構: osfs_inode
 * 描述: OSFS 特有的 Inode 結構，儲存檔案的 Metadata (屬性)。
 */
struct osfs_inode {
    uint32_t i_ino;                     // Inode 編號
    uint32_t i_size;                    // 檔案大小 (Bytes)
    uint32_t i_blocks;                  // 檔案佔用的區塊數
    uint16_t i_mode;                    // 檔案模式 (包含權限 rwx 與類型如目錄/檔案)
    uint16_t i_links_count;             // 硬連結數量 (Hard Links)
    uint32_t i_uid;                     // 擁有者的 User ID
    uint32_t i_gid;                     // 擁有者的 Group ID
    struct timespec64 __i_atime;        // 最後存取時間 (Access Time)
    struct timespec64 __i_mtime;        // 最後修改時間 (Modification Time)
    struct timespec64 __i_ctime;        // 狀態改變時間 (Change Time)
    
    // 這是基礎版 (Template) 的特徵：只有一個 block 指標。
    // 這代表一個檔案最多只能存 4KB (一個 Block) 的資料。
    uint32_t i_block;                   
};

// --- 函式宣告 (Prototypes) ---

// 根據編號建立或取得 VFS inode
struct inode *osfs_iget(struct super_block *sb, unsigned long ino);

// 根據編號取得底層 osfs_inode
struct osfs_inode *osfs_get_osfs_inode(struct super_block *sb, uint32_t ino);

// 尋找並分配一個空閒的 Inode 編號
int osfs_get_free_inode(struct osfs_sb_info *sb_info);

// 尋找並分配一個空閒的資料區塊
int osfs_alloc_data_block(struct osfs_sb_info *sb_info, uint32_t *block_no);

// 初始化 Superblock (掛載時呼叫)
int osfs_fill_super(struct super_block *sb, void *data, int silent);

// 建立新 Inode 的輔助函式
struct inode *osfs_new_inode(const struct inode *dir, umode_t mode);

// 釋放資料區塊
void osfs_free_data_block(struct osfs_sb_info *sb_info, uint32_t block_no);

// 銷毀 Inode (釋放資源)
void osfs_destroy_inode(struct inode *inode);

// --- 外部變數宣告 (定義在各個 .c 檔中) ---

// 定義在 file.c，一般檔案的 Inode 操作與檔案操作
extern const struct inode_operations osfs_file_inode_operations;
extern const struct file_operations osfs_file_operations;

// 定義在 dir.c，目錄的 Inode 操作與檔案操作
extern const struct inode_operations osfs_dir_inode_operations;
extern const struct file_operations osfs_dir_operations;

// 定義在 super.c，超級區塊操作
extern const struct super_operations osfs_super_ops;

#endif /* _OSFS_H */