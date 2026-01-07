#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include "osfs.h"

/**
 * 結構: osfs_super_ops
 * 描述: 定義超級區塊的操作函式表。
 * 當 Kernel 需要對檔案系統本身進行操作時 (如卸載、查詢容量)，會呼叫這裡定義的函式。
 */
const struct super_operations osfs_super_ops = {
    .statfs = simple_statfs,            // 提供檔案系統統計資訊 (給 df 指令用)
    .drop_inode = generic_delete_inode, // 通用的 Inode 刪除邏輯
    .destroy_inode = osfs_destroy_inode,// 銷毀 Inode 時的自訂清理函式
};

/**
 * 函式: osfs_destroy_inode
 * 描述: 當一個 Inode 被釋放時呼叫。
 * 我們需要斷開 VFS Inode 與我們自定義 OSFS Inode 的連結。
 */
void osfs_destroy_inode(struct inode *inode)
{
    if (inode->i_private) {
        inode->i_private = NULL; // 清空私有資料指標
    }
}

/**
 * 函式: osfs_fill_super
 * 描述: 初始化超級區塊。這是掛載 (Mount) 過程中最關鍵的一步。
 * 輸入:
 * - sb: VFS 傳進來的超級區塊結構，我們要負責填滿它。
 * - data: 掛載選項 (mount -o ...)。
 * - silent: 是否要安靜模式 (不印錯誤訊息)。
 * 回傳:
 * - 0 表示成功。
 * - 負值表示失敗 (如 -ENOMEM)。
 */
int osfs_fill_super(struct super_block *sb, void *data, int silent)
{
    pr_info("osfs: Filling super start\n");
    struct inode *root_inode;
    struct osfs_sb_info *sb_info;
    void *memory_region;
    size_t total_memory_size;

    // 1. 計算整個檔案系統所需的記憶體大小
    // 包含: Superblock Info 標頭 + Inode 位圖 + Block 位圖 + Inode 表格 + 所有資料區塊
    total_memory_size = sizeof(struct osfs_sb_info) +
                        INODE_BITMAP_SIZE * sizeof(unsigned long) +
                        BLOCK_BITMAP_SIZE * sizeof(unsigned long) +
                        INODE_COUNT * sizeof(struct osfs_inode) +
                        DATA_BLOCK_COUNT * BLOCK_SIZE;

    // 2. 向 Kernel 申請一大塊虛擬記憶體 (vmalloc 用於分配大塊連續虛擬記憶體)
    memory_region = vmalloc(total_memory_size);
    if (!memory_region)
        return -ENOMEM; // 記憶體不足

    memset(memory_region, 0, total_memory_size); // 將整塊記憶體清零

    // 3. 初始化 Superblock Info 結構 (放在記憶體最開頭)
    sb_info = (struct osfs_sb_info *)memory_region;
    sb_info->magic = OSFS_MAGIC;           // 設定魔術數字
    sb_info->block_size = BLOCK_SIZE;      // 設定 Block 大小
    sb_info->inode_count = INODE_COUNT;    // 設定 Inode 總數
    sb_info->block_count = DATA_BLOCK_COUNT; // 設定 Block 總數
    sb_info->nr_free_inodes = INODE_COUNT - 1; // 扣掉 Root Inode
    sb_info->nr_free_blocks = DATA_BLOCK_COUNT;

    // 4. 切割記憶體：設定各個區域的指標
    // 記憶體配置圖： [ SB_Info | Inode_Bitmap | Block_Bitmap | Inode_Table | Data_Blocks ... ]
    
    // Inode 位圖緊接在 sb_info 之後
    sb_info->inode_bitmap = (unsigned long *)(sb_info + 1);
    
    // Block 位圖緊接在 Inode 位圖之後
    sb_info->block_bitmap = sb_info->inode_bitmap + INODE_BITMAP_SIZE;
    
    // Inode 表格緊接在 Block 位圖之後
    sb_info->inode_table = (void *)(sb_info->block_bitmap + BLOCK_BITMAP_SIZE);
    
    // 資料區塊區域緊接在 Inode 表格之後
    sb_info->data_blocks = (void *)((char *)sb_info->inode_table +
                                    INODE_COUNT * sizeof(struct osfs_inode));

    // 初始化位圖 (前面 memset 已經清零過了，這裡再次確認)
    memset(sb_info->inode_bitmap, 0, INODE_BITMAP_SIZE * sizeof(unsigned long));
    memset(sb_info->block_bitmap, 0, BLOCK_BITMAP_SIZE * sizeof(unsigned long));

    // 5. 設定 VFS 的 Superblock 欄位
    sb->s_magic = sb_info->magic;
    sb->s_fs_info = sb_info; // 將我們的私有資訊掛上去
    sb->s_op = &osfs_super_ops; // 設定操作函式

    // 6. 建立根目錄 (Root Directory) 的 Inode
    root_inode = new_inode(sb);
    if (!root_inode) {
        vfree(memory_region); // 失敗要記得釋放記憶體
        return -ENOMEM;
    }

    // 設定根目錄 VFS Inode 屬性
    root_inode->i_ino = ROOT_INODE; // 通常是 1
    root_inode->i_sb = sb;
    root_inode->i_op = &osfs_dir_inode_operations; // 設定目錄專用的 Inode 操作 (如 lookup, create)
    root_inode->i_fop = &osfs_dir_operations;      // 設定目錄專用的檔案操作 (如 iterate/ls)
    root_inode->i_mode = S_IFDIR | 0755;           // 設定為目錄 (Directory) 且權限 755
    set_nlink(root_inode, 2);                      // 設定連結數 (本身 + 父目錄)
    simple_inode_init_ts(root_inode);              // 初始化時間
    
    // 7. 初始化根目錄的 OSFS Inode (位於我們切出來的 Inode Table 中)
    struct osfs_inode *root_osfs_inode = osfs_get_osfs_inode(sb, ROOT_INODE);
    if (!root_osfs_inode) {
        iput(root_inode);
        vfree(memory_region);
        return -EIO;
    }
    memset(root_osfs_inode, 0, sizeof(*root_osfs_inode));

    // 填入根目錄的 OSFS 屬性
    root_osfs_inode->i_ino = ROOT_INODE;
    root_osfs_inode->i_mode = root_inode->i_mode;
    root_osfs_inode->i_links_count = 2;
    // 設定時間
    root_osfs_inode->__i_atime = root_osfs_inode->__i_mtime = root_osfs_inode->__i_ctime = current_time(root_inode);
    
    // 關鍵：將 VFS inode 與 OSFS inode 連結起來
    root_inode->i_private = root_osfs_inode;

    // 8. 在位圖中標記 Root Inode 已被使用
    set_bit(ROOT_INODE, sb_info->inode_bitmap);

    // 更新根目錄大小 (初始為 0)
    root_inode->i_size = 0;
    inode_init_owner(&nop_mnt_idmap, root_inode, NULL, root_inode->i_mode);
    
    // 9. 建立根目錄的 dentry (Directory Entry)，並設為 Superblock 的根
    sb->s_root = d_make_root(root_inode);
    if (!sb->s_root) {
        iput(root_inode);
        vfree(memory_region);
        return -ENOMEM;
    }
    
    pr_info("osfs: Superblock filled successfully \n");
    return 0;
}