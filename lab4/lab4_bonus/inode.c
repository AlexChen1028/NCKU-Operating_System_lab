#include <linux/fs.h>
#include <linux/uaccess.h>
#include "osfs.h"

// --- 基礎函式 (與 Requirement 版本相同) ---

/**
 * 函式: osfs_get_osfs_inode
 * 描述: 取得底層 OSFS Inode 指標。
 */
struct osfs_inode *osfs_get_osfs_inode(struct super_block *sb, uint32_t ino)
{
    struct osfs_sb_info *sb_info = sb->s_fs_info;

    if (ino == 0 || ino >= sb_info->inode_count) 
        return NULL;
    return &((struct osfs_inode *)(sb_info->inode_table))[ino];
}

/**
 * 函式: osfs_get_free_inode
 * 描述: 掃描 Bitmap 取得一個空閒的 Inode 編號。
 */
int osfs_get_free_inode(struct osfs_sb_info *sb_info)
{
    uint32_t ino;

    for (ino = 1; ino < sb_info->inode_count; ino++) {
        if (!test_bit(ino, sb_info->inode_bitmap)) {
            set_bit(ino, sb_info->inode_bitmap);
            sb_info->nr_free_inodes--;
            return ino;
        }
    }
    pr_err("osfs_get_free_inode: No free inode available\n");
    return -ENOSPC;
}

/**
 * 函式: osfs_iget
 * 描述: 建立或讀取 VFS Inode，並掛載對應的操作函式。
 */
struct inode *osfs_iget(struct super_block *sb, unsigned long ino)
{
    struct osfs_inode *osfs_inode;
    struct inode *inode;

    osfs_inode = osfs_get_osfs_inode(sb, ino);
    if (!osfs_inode)
        return ERR_PTR(-EFAULT);

    inode = new_inode(sb);
    if (!inode)
        return ERR_PTR(-ENOMEM);

    inode->i_ino = ino;
    inode->i_sb = sb;
    inode->i_mode = osfs_inode->i_mode;
    i_uid_write(inode, osfs_inode->i_uid);
    i_gid_write(inode, osfs_inode->i_gid);
    
    inode_set_atime_to_ts(inode, osfs_inode->__i_atime);
    inode_set_mtime_to_ts(inode, osfs_inode->__i_mtime);
    inode_set_ctime_to_ts(inode, osfs_inode->__i_ctime);
    inode->i_size = osfs_inode->i_size;
    inode->i_blocks = osfs_inode->i_blocks;
    inode->i_private = osfs_inode;

    if (S_ISDIR(inode->i_mode)) {
        inode->i_op = &osfs_dir_inode_operations;
        inode->i_fop = &osfs_dir_operations;
    } else if (S_ISREG(inode->i_mode)) {
        inode->i_op = &osfs_file_inode_operations;
        inode->i_fop = &osfs_file_operations;
    }

    insert_inode_hash(inode);

    return inode;
}

/**
 * 函式: osfs_alloc_data_block
 * 描述: 掃描 Bitmap 分配一個空閒的資料區塊。
 */
int osfs_alloc_data_block(struct osfs_sb_info *sb_info, uint32_t *block_no)
{
    uint32_t i;

    for (i = 0; i < sb_info->block_count; i++) {
        if (!test_bit(i, sb_info->block_bitmap)) {
            set_bit(i, sb_info->block_bitmap);
            sb_info->nr_free_blocks--;
            *block_no = i;
            return 0;
        }
    }
    pr_err("osfs_alloc_data_block: No free data block available\n");
    return -ENOSPC;
}

/**
 * 函式: osfs_free_data_block
 * 描述: 釋放指定的資料區塊 (將 Bitmap 歸零)。
 */
void osfs_free_data_block(struct osfs_sb_info *sb_info, uint32_t block_no)
{
    clear_bit(block_no, sb_info->block_bitmap);
    sb_info->nr_free_blocks++;
}

// --- Bonus 核心函式 ---

/**
 * 函式: osfs_get_block
 * 描述: 將檔案的「邏輯區塊號碼」(第幾塊) 映射到「實體區塊號碼」(記憶體位址)。
 * 支援 Direct, Indirect, Double Indirect 三種模式。
 * 輸入: 
 * - inode: 檔案的 Inode
 * - block: 邏輯區塊號碼 (例如檔案的第 100 塊)
 * - phys_block: (輸出) 查到的實體區塊號碼
 * - create: 1 代表寫入模式 (不存在就建立)，0 代表讀取模式 (不存在回傳錯誤)
 */
int osfs_get_block(struct inode *inode, sector_t block, uint32_t *phys_block, int create)
{
    struct osfs_sb_info *sb_info = inode->i_sb->s_fs_info;
    struct osfs_inode *osfs_inode = inode->i_private;
    uint32_t *indirect_block;   // 指向一級間接表的指標
    uint32_t *dindirect_block;  // 指向二級間接表的指標
    uint32_t indirect_idx, dindirect_idx1, dindirect_idx2;
    int ret;

    *phys_block = 0;

    // --- 1. 直接區塊範圍 (Direct blocks [0-11]) ---
    if (block < OSFS_N_DIRECT) {
        // 如果該位置是空的，且需要建立 (create=1)
        if (osfs_inode->i_block[block] == 0 && create) {
            ret = osfs_alloc_data_block(sb_info, &osfs_inode->i_block[block]);
            if (ret) {
                pr_err("osfs_get_block: Failed to allocate direct block\n");
                return ret;
            }
            osfs_inode->i_blocks++;
            mark_inode_dirty(inode);
        }
        // 回傳實體區塊號碼
        *phys_block = osfs_inode->i_block[block];
        return 0;
    }

    // 扣除直接區塊的數量，計算相對偏移
    block -= OSFS_N_DIRECT;

    // --- 2. 一級間接區塊 (Indirect blocks [12]) ---
    if (block < OSFS_ADDR_PER_BLOCK) {
        // 2.1 檢查「索引表」是否存在，若無則分配
        if (osfs_inode->i_block[OSFS_N_DIRECT] == 0) {
            if (!create) return -ENOENT; // 讀取模式下若無索引表則回傳錯誤
            
            // 分配索引表 Block
            ret = osfs_alloc_data_block(sb_info, &osfs_inode->i_block[OSFS_N_DIRECT]);
            if (ret) {
                pr_err("osfs_get_block: Failed to allocate indirect block\n");
                return ret;
            }
            // 清空新分配的索引表 (防止裡面有垃圾值)
            indirect_block = (uint32_t *)(sb_info->data_blocks + 
                                         osfs_inode->i_block[OSFS_N_DIRECT] * BLOCK_SIZE);
            memset(indirect_block, 0, BLOCK_SIZE);
            osfs_inode->i_blocks++;
            mark_inode_dirty(inode);
        }

        // 取得索引表的記憶體位址
        indirect_block = (uint32_t *)(sb_info->data_blocks + 
                                     osfs_inode->i_block[OSFS_N_DIRECT] * BLOCK_SIZE);
        indirect_idx = block; // 在索引表中的 Index

        // 2.2 檢查索引表指向的「資料塊」是否存在，若無則分配
        if (indirect_block[indirect_idx] == 0 && create) {
            ret = osfs_alloc_data_block(sb_info, &indirect_block[indirect_idx]);
            if (ret) {
                pr_err("osfs_get_block: Failed to allocate data block in indirect\n");
                return ret;
            }
            osfs_inode->i_blocks++;
            mark_inode_dirty(inode);
        }
        *phys_block = indirect_block[indirect_idx];
        return 0;
    }

    // 扣除一級間接區塊能涵蓋的數量
    block -= OSFS_ADDR_PER_BLOCK;

    // --- 3. 二級間接區塊 (Double indirect blocks [13]) ---
    if (block < OSFS_ADDR_PER_BLOCK * OSFS_ADDR_PER_BLOCK) {
        // 3.1 檢查「第一層索引表」是否存在
        if (osfs_inode->i_block[OSFS_N_DIRECT + 1] == 0) {
            if (!create) return -ENOENT;
            ret = osfs_alloc_data_block(sb_info, &osfs_inode->i_block[OSFS_N_DIRECT + 1]);
            if (ret) {
                pr_err("osfs_get_block: Failed to allocate double indirect block\n");
                return ret;
            }
            // 清空第一層索引表
            dindirect_block = (uint32_t *)(sb_info->data_blocks + 
                                          osfs_inode->i_block[OSFS_N_DIRECT + 1] * BLOCK_SIZE);
            memset(dindirect_block, 0, BLOCK_SIZE);
            osfs_inode->i_blocks++;
            mark_inode_dirty(inode);
        }

        // 取得第一層索引表位址
        dindirect_block = (uint32_t *)(sb_info->data_blocks + 
                                      osfs_inode->i_block[OSFS_N_DIRECT + 1] * BLOCK_SIZE);
        
        // 計算兩層索引的 Index
        dindirect_idx1 = block / OSFS_ADDR_PER_BLOCK; // 第一層 Index
        dindirect_idx2 = block % OSFS_ADDR_PER_BLOCK; // 第二層 Index

        // 3.2 檢查「第二層索引表」是否存在
        if (dindirect_block[dindirect_idx1] == 0) {
            if (!create) return -ENOENT;
            ret = osfs_alloc_data_block(sb_info, &dindirect_block[dindirect_idx1]);
            if (ret) {
                pr_err("osfs_get_block: Failed to allocate indirect in double indirect\n");
                return ret;
            }
            // 清空第二層索引表
            indirect_block = (uint32_t *)(sb_info->data_blocks + 
                                         dindirect_block[dindirect_idx1] * BLOCK_SIZE);
            memset(indirect_block, 0, BLOCK_SIZE);
            osfs_inode->i_blocks++;
            mark_inode_dirty(inode);
        }

        // 取得第二層索引表位址
        indirect_block = (uint32_t *)(sb_info->data_blocks + 
                                     dindirect_block[dindirect_idx1] * BLOCK_SIZE);

        // 3.3 檢查「資料塊」是否存在
        if (indirect_block[dindirect_idx2] == 0 && create) {
            ret = osfs_alloc_data_block(sb_info, &indirect_block[dindirect_idx2]);
            if (ret) {
                pr_err("osfs_get_block: Failed to allocate data block in double indirect\n");
                return ret;
            }
            osfs_inode->i_blocks++;
            mark_inode_dirty(inode);
        }
        *phys_block = indirect_block[dindirect_idx2];
        return 0;
    }

    pr_err("osfs_get_block: Block number too large\n");
    return -EFBIG; // 檔案太大了 (File Too Big)
}

/**
 * 函式: osfs_free_inode_blocks
 * 描述: 釋放一個 Inode 佔用的所有區塊 (包含直接、間接、雙重間接)。
 * 通常在刪除檔案時呼叫。
 */
void osfs_free_inode_blocks(struct inode *inode)
{
    struct osfs_sb_info *sb_info = inode->i_sb->s_fs_info;
    struct osfs_inode *osfs_inode = inode->i_private;
    uint32_t *indirect_block, *dindirect_block;
    int i, j;

    if (!osfs_inode)
        return;

    // 1. 釋放直接區塊 (Direct Blocks)
    for (i = 0; i < OSFS_N_DIRECT; i++) {
        if (osfs_inode->i_block[i] != 0) {
            osfs_free_data_block(sb_info, osfs_inode->i_block[i]);
            osfs_inode->i_block[i] = 0;
        }
    }

    // 2. 釋放一級間接區塊 (Indirect Blocks)
    if (osfs_inode->i_block[OSFS_N_DIRECT] != 0) {
        // 讀取索引表
        indirect_block = (uint32_t *)(sb_info->data_blocks + 
                                     osfs_inode->i_block[OSFS_N_DIRECT] * BLOCK_SIZE);
        // 遍歷索引表，釋放裡面指到的資料塊
        for (i = 0; i < OSFS_ADDR_PER_BLOCK; i++) {
            if (indirect_block[i] != 0) {
                osfs_free_data_block(sb_info, indirect_block[i]);
            }
        }
        // 最後釋放索引表本身
        osfs_free_data_block(sb_info, osfs_inode->i_block[OSFS_N_DIRECT]);
        osfs_inode->i_block[OSFS_N_DIRECT] = 0;
    }

    // 3. 釋放二級間接區塊 (Double Indirect Blocks)
    if (osfs_inode->i_block[OSFS_N_DIRECT + 1] != 0) {
        // 讀取第一層索引表
        dindirect_block = (uint32_t *)(sb_info->data_blocks + 
                                      osfs_inode->i_block[OSFS_N_DIRECT + 1] * BLOCK_SIZE);
        
        for (i = 0; i < OSFS_ADDR_PER_BLOCK; i++) {
            if (dindirect_block[i] != 0) {
                // 讀取第二層索引表
                indirect_block = (uint32_t *)(sb_info->data_blocks + 
                                             dindirect_block[i] * BLOCK_SIZE);
                // 釋放資料塊
                for (j = 0; j < OSFS_ADDR_PER_BLOCK; j++) {
                    if (indirect_block[j] != 0) {
                        osfs_free_data_block(sb_info, indirect_block[j]);
                    }
                }
                // 釋放第二層索引表本身
                osfs_free_data_block(sb_info, dindirect_block[i]);
            }
        }
        // 最後釋放第一層索引表本身
        osfs_free_data_block(sb_info, osfs_inode->i_block[OSFS_N_DIRECT + 1]);
        osfs_inode->i_block[OSFS_N_DIRECT + 1] = 0;
    }

    osfs_inode->i_blocks = 0;
}