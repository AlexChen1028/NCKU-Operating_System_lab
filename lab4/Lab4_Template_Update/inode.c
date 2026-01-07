#include <linux/fs.h>
#include <linux/uaccess.h>
#include "osfs.h"

/**
 * 函式: osfs_get_osfs_inode
 * 描述: 根據 Inode 編號取得底層的 osfs_inode 結構。
 * 輸入:
 * - sb: 超級區塊 (Superblock) 指標。
 * - ino: 要尋找的 Inode 編號。
 * 回傳:
 * - 成功則回傳 osfs_inode 指標。
 * - 失敗 (編號無效) 則回傳 NULL。
 */
struct osfs_inode *osfs_get_osfs_inode(struct super_block *sb, uint32_t ino)
{
    struct osfs_sb_info *sb_info = sb->s_fs_info; // 取得自定義的 Superblock 資訊

    // 檢查 Inode 編號是否合法 (不能為 0，也不能超過總數)
    if (ino == 0 || ino >= sb_info->inode_count) 
        return NULL;
    
    // 從 Inode Table 的起始位址，加上偏移量 (ino * 結構大小)，取得該 Inode 的指標
    return &((struct osfs_inode *)(sb_info->inode_table))[ino];
}

/**
 * 函式: osfs_get_free_inode
 * 描述: 從 Inode Bitmap 中分配一個空閒的 Inode 編號。
 * 輸入:
 * - sb_info: 檔案系統的 Superblock 資訊。
 * 回傳:
 * - 成功則回傳分配到的 Inode 編號。
 * - 失敗 (沒有空位) 則回傳 -ENOSPC (No space left on device)。
 */
int osfs_get_free_inode(struct osfs_sb_info *sb_info)
{
    uint32_t ino;

    // 從 1 開始掃描 (通常 0 保留或不使用)
    for (ino = 1; ino < sb_info->inode_count; ino++) {
        // test_bit: 檢查該位元是否為 1 (1 代表已使用)
        if (!test_bit(ino, sb_info->inode_bitmap)) {
            set_bit(ino, sb_info->inode_bitmap); // 設為 1，標記為已使用
            sb_info->nr_free_inodes--;           // 更新剩餘 Inode 計數
            return ino;
        }
    }
    pr_err("osfs_get_free_inode: No free inode available\n");
    return -ENOSPC;
}

/**
 * 函式: osfs_iget
 * 描述: 建立或讀取一個 VFS inode (Linux Kernel 使用的結構)。
 * 這通常在 lookup (尋找檔案) 時被呼叫。
 * 輸入:
 * - sb: 超級區塊。
 * - ino: Inode 編號。
 * 回傳:
 * - 成功回傳 VFS inode 指標。
 * - 失敗回傳錯誤指標 (ERR_PTR)。
 */
struct inode *osfs_iget(struct super_block *sb, unsigned long ino)
{
    struct osfs_inode *osfs_inode;
    struct inode *inode;

    // 1. 先取得底層儲存的 osfs_inode 資料
    osfs_inode = osfs_get_osfs_inode(sb, ino);
    if (!osfs_inode)
        return ERR_PTR(-EFAULT);

    // 2. 分配一個新的 VFS inode 結構
    inode = new_inode(sb);
    if (!inode)
        return ERR_PTR(-ENOMEM);

    // 3. 將 osfs_inode 的資料複製到 VFS inode
    inode->i_ino = ino;
    inode->i_sb = sb;
    inode->i_mode = osfs_inode->i_mode;   // 檔案類型與權限
    i_uid_write(inode, osfs_inode->i_uid); // 設定 User ID
    i_gid_write(inode, osfs_inode->i_gid); // 設定 Group ID

    // 設定時間屬性 (atime: 存取, mtime: 修改, ctime: 狀態改變)
    // 舊版核心可能直接使用 inode->__i_atime，新版使用 inode_set_atime_to_ts
    inode_set_atime_to_ts(inode, osfs_inode->__i_atime);
    inode_set_mtime_to_ts(inode, osfs_inode->__i_mtime);
    inode_set_ctime_to_ts(inode, osfs_inode->__i_ctime);
    
    inode->i_size = osfs_inode->i_size;     // 檔案大小
    inode->i_blocks = osfs_inode->i_blocks; // 佔用的區塊數
    
    // 重要：將底層的 osfs_inode 存到 i_private，方便後續 read/write 直接存取
    inode->i_private = osfs_inode;

    // 4. 根據檔案類型設定對應的操作函式 (Operation Pointers)
    // 這樣 Kernel 才知道對這個 Inode 做 read/write 時要呼叫誰
    if (S_ISDIR(inode->i_mode)) {
        inode->i_op = &osfs_dir_inode_operations; // 目錄的操作 (如 create, lookup)
        inode->i_fop = &osfs_dir_operations;      // 目錄檔案的操作 (如 ls/iterate)
    } else if (S_ISREG(inode->i_mode)) {
        inode->i_op = &osfs_file_inode_operations; // 一般檔案的操作
        inode->i_fop = &osfs_file_operations;      // 一般檔案的讀寫 (如 read, write)
    }

    // 5. 將這個 inode 加入 Kernel 的 Hash Table，加速未來查找
    insert_inode_hash(inode);

    return inode;
}

/**
 * 函式: osfs_alloc_data_block
 * 描述: 從 Block Bitmap 中分配一個空閒的資料區塊。
 * 輸入:
 * - sb_info: Superblock 資訊。
 * - block_no: 用來回傳分配到的區塊編號指標。
 * 回傳:
 * - 0 表示成功。
 * - -ENOSPC 表示沒有空間了。
 */
int osfs_alloc_data_block(struct osfs_sb_info *sb_info, uint32_t *block_no)
{
    uint32_t i;

    // 掃描所有的 Block
    for (i = 0; i < sb_info->block_count; i++) {
        // 如果該 Bit 為 0 (未被使用)
        if (!test_bit(i, sb_info->block_bitmap)) {
            set_bit(i, sb_info->block_bitmap); // 標記為使用
            sb_info->nr_free_blocks--;         // 更新剩餘空間計數
            *block_no = i;                     // 回傳區塊編號
            return 0;
        }
    }
    pr_err("osfs_alloc_data_block: No free data block available\n");
    return -ENOSPC;
}

/**
 * 函式: osfs_free_data_block
 * 描述: 釋放一個資料區塊 (將 Bitmap 對應位置設為 0)。
 * 輸入:
 * - sb_info: Superblock 資訊。
 * - block_no: 要釋放的區塊編號。
 */
void osfs_free_data_block(struct osfs_sb_info *sb_info, uint32_t block_no)
{
    // 清除 Bit (設為 0)
    clear_bit(block_no, sb_info->block_bitmap);
    sb_info->nr_free_blocks++; // 增加剩餘空間計數
}