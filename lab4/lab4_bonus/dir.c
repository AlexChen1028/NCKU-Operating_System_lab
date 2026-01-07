#include <linux/fs.h>
#include <linux/string.h>
#include <linux/slab.h>
#include "osfs.h"

/**
 * 函式: osfs_lookup
 * 描述: 在目錄中尋找特定檔名的檔案。
 * 輸入:
 * - dir: 父目錄的 Inode。
 * - dentry: 包含要尋找檔名的 dentry。
 * - flags: 查找旗標。
 * 回傳:
 * - 找到時回傳 dentry 指標。
 * - 沒找到回傳 NULL。
 */
static struct dentry *osfs_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags)
{
    struct osfs_sb_info *sb_info = dir->i_sb->s_fs_info;
    struct osfs_inode *parent_inode = dir->i_private;
    void *dir_data_block;
    struct osfs_dir_entry *dir_entries;
    int dir_entry_count;
    int i;
    struct inode *inode = NULL;

    pr_info("osfs_lookup: Looking up '%.*s' in inode %lu\n",
            (int)dentry->d_name.len, dentry->d_name.name, dir->i_ino);

    // 讀取父目錄的資料區塊
    // 【Bonus 差異點】因為 i_block 變成了陣列 (支援多層索引)，
    // 目錄通常只用第一個直接區塊，所以這裡用 parent_inode->i_block[0]
    dir_data_block = sb_info->data_blocks + parent_inode->i_block[0] * BLOCK_SIZE;

    // 計算目錄項目數量
    dir_entry_count = parent_inode->i_size / sizeof(struct osfs_dir_entry);
    dir_entries = (struct osfs_dir_entry *)dir_data_block;

    // 遍歷目錄項目尋找檔名
    for (i = 0; i < dir_entry_count; i++) {
        if (strlen(dir_entries[i].filename) == dentry->d_name.len &&
            strncmp(dir_entries[i].filename, dentry->d_name.name, dentry->d_name.len) == 0) {
            // 找到了，取得 Inode
            inode = osfs_iget(dir->i_sb, dir_entries[i].inode_no);
            if (IS_ERR(inode)) {
                pr_err("osfs_lookup: Error getting inode %u\n", dir_entries[i].inode_no);
                return ERR_CAST(inode);
            }
            return d_splice_alias(inode, dentry);
        }
    }

    return NULL;
}

/**
 * 函式: osfs_iterate
 * 描述: 遍歷目錄項目 (供 ls 使用)。
 */
static int osfs_iterate(struct file *filp, struct dir_context *ctx)
{
    struct inode *inode = file_inode(filp);
    struct osfs_sb_info *sb_info = inode->i_sb->s_fs_info;
    struct osfs_inode *osfs_inode = inode->i_private;
    void *dir_data_block;
    struct osfs_dir_entry *dir_entries;
    int dir_entry_count;
    int i;

    // 處理 . 和 ..
    if (ctx->pos == 0) {
        if (!dir_emit_dots(filp, ctx))
            return 0;
    }

    // 【Bonus 差異點】使用 i_block[0] 存取目錄的第一個data block
    dir_data_block = sb_info->data_blocks + osfs_inode->i_block[0] * BLOCK_SIZE;
    dir_entry_count = osfs_inode->i_size / sizeof(struct osfs_dir_entry);
    dir_entries = (struct osfs_dir_entry *)dir_data_block;

    /* 調整索引 (跳過 . 和 ..) */
    i = ctx->pos - 2;

    for (; i < dir_entry_count; i++) {
        struct osfs_dir_entry *entry = &dir_entries[i];
        unsigned int type = DT_UNKNOWN;

        if (!dir_emit(ctx, entry->filename, strlen(entry->filename), entry->inode_no, type)) {
            pr_err("osfs_iterate: dir_emit failed for entry '%s'\n", entry->filename);
            return -EINVAL;
        }

        ctx->pos++;
    }

    return 0;
}

/**
 * 函式: osfs_new_inode
 * 描述: 建立新的 Inode 陣列。
 */
struct inode *osfs_new_inode(const struct inode *dir, umode_t mode)
{
    struct super_block *sb = dir->i_sb;
    struct osfs_sb_info *sb_info = sb->s_fs_info;
    struct inode *inode;
    struct osfs_inode *osfs_inode;
    int ino;

    /* 檢查檔案類型是否支援 */
    if (!S_ISDIR(mode) && !S_ISREG(mode) && !S_ISLNK(mode)) {
        pr_err("File type not supported (only directory, regular file and symlink supported)\n");
        return ERR_PTR(-EINVAL);
    }

    /* 檢查是否有空閒 Inode */
    if (sb_info->nr_free_inodes == 0)
        return ERR_PTR(-ENOSPC);

    /* 分配 Inode 編號 */
    ino = osfs_get_free_inode(sb_info);
    if (ino < 0 || ino >= sb_info->inode_count)
        return ERR_PTR(-ENOSPC);

    /* 分配 VFS Inode */
    inode = new_inode(sb);
    if (!inode)
        return ERR_PTR(-ENOMEM);

    /* 初始化 VFS Inode */
    inode_init_owner(&nop_mnt_idmap, inode, dir, mode);
    inode->i_ino = ino;
    inode->i_sb = sb;
    inode->i_blocks = 0;
    simple_inode_init_ts(inode);

    /* 設定操作函式 */
    if (S_ISDIR(mode)) {
        inode->i_op = &osfs_dir_inode_operations;
        inode->i_fop = &osfs_dir_operations;
        set_nlink(inode, 2); /* . and .. */
        inode->i_size = 0;
    } else if (S_ISREG(mode)) {
        inode->i_op = &osfs_file_inode_operations;
        inode->i_fop = &osfs_file_operations;
        set_nlink(inode, 1);
        inode->i_size = 0;
    } else if (S_ISLNK(mode)) {
        // inode->i_op = &osfs_symlink_inode_operations;
        set_nlink(inode, 1);
        inode->i_size = 0;
    }

    /* 取得底層 OSFS Inode */
    osfs_inode = osfs_get_osfs_inode(sb, ino);
    if (!osfs_inode) {
        pr_err("osfs_new_inode: Failed to get osfs_inode for inode %d\n", ino);
        iput(inode);
        return ERR_PTR(-EIO);
    }
    memset(osfs_inode, 0, sizeof(*osfs_inode));

    /* 初始化 OSFS Inode */
    osfs_inode->i_ino = ino;
    osfs_inode->i_mode = inode->i_mode;
    osfs_inode->i_uid = i_uid_read(inode);
    osfs_inode->i_gid = i_gid_read(inode);
    osfs_inode->i_size = inode->i_size;
    osfs_inode->i_blocks = 0;
    osfs_inode->__i_atime = osfs_inode->__i_mtime = osfs_inode->__i_ctime = current_time(inode);
    
    // 【Bonus 差異點】清空整個 Block 索引陣列
    // 因為 Bonus 版支援多層索引，i_block 是陣列，必須全部清零
    memset(osfs_inode->i_block, 0, sizeof(osfs_inode->i_block));
    
    inode->i_private = osfs_inode;

    /* 更新 Superblock 資訊 */
    sb_info->nr_free_inodes--;

    /* 標記 Inode 為 Dirty */
    mark_inode_dirty(inode);

    return inode;
}

static int osfs_add_dir_entry(struct inode *dir, uint32_t inode_no, const char *name, size_t name_len)
{
    struct osfs_sb_info *sb_info = dir->i_sb->s_fs_info;
    struct osfs_inode *parent_inode = dir->i_private;
    void *dir_data_block;
    struct osfs_dir_entry *dir_entries;
    int dir_entry_count;
    int i;
    int ret;

    // 如果父目錄還沒有分配資料區塊，先分配一個 (針對剛建立的目錄)
    // 【Bonus 差異點】使用 i_block[0]
    if (parent_inode->i_blocks == 0) {
        ret = osfs_alloc_data_block(sb_info, &parent_inode->i_block[0]);
        if (ret) {
            pr_err("osfs_add_dir_entry: Failed to allocate data block for directory\n");
            return ret;
        }
        parent_inode->i_blocks = 1;
        // 清空新分配的區塊
        dir_data_block = sb_info->data_blocks + parent_inode->i_block[0] * BLOCK_SIZE;
        memset(dir_data_block, 0, BLOCK_SIZE);
    }

    // 讀取父目錄的資料區塊
    // 【Bonus 差異點】使用 i_block[0]
    dir_data_block = sb_info->data_blocks + parent_inode->i_block[0] * BLOCK_SIZE;

    // 計算目錄項目數量
    dir_entry_count = parent_inode->i_size / sizeof(struct osfs_dir_entry);
    if (dir_entry_count >= MAX_DIR_ENTRIES) {
        pr_err("osfs_add_dir_entry: Parent directory is full\n");
        return -ENOSPC;
    }

    dir_entries = (struct osfs_dir_entry *)dir_data_block;

    // 檢查是否有重複檔名
    for (i = 0; i < dir_entry_count; i++) {
        if (strlen(dir_entries[i].filename) == name_len &&
            strncmp(dir_entries[i].filename, name, name_len) == 0) {
            pr_warn("osfs_add_dir_entry: File '%.*s' already exists\n", (int)name_len, name);
            return -EEXIST;
        }
    }

    // 新增目錄項目
    strncpy(dir_entries[dir_entry_count].filename, name, name_len);
    dir_entries[dir_entry_count].filename[name_len] = '\0';
    dir_entries[dir_entry_count].inode_no = inode_no;

    // 更新父目錄大小
    parent_inode->i_size += sizeof(struct osfs_dir_entry);

    return 0;
}


/**
 * 函式: osfs_create
 * 描述: 在目錄中建立新檔案。
 */
static int osfs_create(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, umode_t mode, bool excl)
{   
    // Step 1: 取得父目錄 Inode
    struct osfs_inode *parent_inode = dir->i_private;
    struct osfs_inode *osfs_inode;
    struct inode *inode;
    int ret;

    // Step 2: 驗證檔名長度
    if (dentry->d_name.len > MAX_FILENAME_LEN) {
        pr_err("osfs_create: Filename too long\n");
        return -ENAMETOOLONG;
    }

    // Step 3: 建立新 Inode
    inode = osfs_new_inode(dir, mode);
    if (IS_ERR(inode)) {
        pr_err("osfs_create: Failed to create new inode\n");
        return PTR_ERR(inode);
    }

    osfs_inode = inode->i_private;
    if (!osfs_inode) {
        pr_err("osfs_create: Failed to get osfs_inode for inode %lu\n", inode->i_ino);
        iput(inode);
        return -EIO;
    }

    // Step 4: 將新檔案加入父目錄
    ret = osfs_add_dir_entry(dir, inode->i_ino, dentry->d_name.name, dentry->d_name.len);
    if (ret) {
        pr_err("osfs_create: Failed to add directory entry\n");
        iput(inode);
        return ret;
    }

    // Step 5: 更新父目錄 Metadata
    dir->i_size = parent_inode->i_size;
    inode_set_mtime_to_ts(dir, current_time(dir));
    inode_set_ctime_to_ts(dir, current_time(dir));
    mark_inode_dirty(dir);
    
    // Step 6: 綁定 Dentry 與 Inode
    d_instantiate(dentry, inode);

    pr_info("osfs_create: File '%.*s' created with inode %lu\n",
            (int)dentry->d_name.len, dentry->d_name.name, inode->i_ino);

    return 0;
}

const struct inode_operations osfs_dir_inode_operations = {
    .lookup = osfs_lookup,
    .create = osfs_create,
};

const struct file_operations osfs_dir_operations = {
    .iterate_shared = osfs_iterate,
    .llseek = generic_file_llseek,
};