#include <linux/fs.h>
#include <linux/string.h>
#include <linux/slab.h>
#include "osfs.h"

/**
 * 函式: osfs_lookup
 * 描述: 在目錄中尋找特定檔名的檔案。
 * 輸入:
 * - dir: 父目錄的 Inode (我們要去哪裡找)。
 * - dentry: VFS 傳進來的 dentry 物件，裡面包含我們要找的檔名 (d_name)。
 * - flags: 查找操作的旗標。
 * 回傳:
 * - 找到時回傳對應的 dentry 指標。
 * - 沒找到回傳 NULL (讓 VFS 知道檔案不存在，這對 create 操作很重要)。
 */
static struct dentry *osfs_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags)
{
    struct osfs_sb_info *sb_info = dir->i_sb->s_fs_info; // 取得 Superblock 資訊
    struct osfs_inode *parent_inode = dir->i_private;    // 取得父目錄的 OSFS Inode
    void *dir_data_block;
    struct osfs_dir_entry *dir_entries;
    int dir_entry_count;
    int i;
    struct inode *inode = NULL;

    pr_info("osfs_lookup: Looking up '%.*s' in inode %lu\n",
            (int)dentry->d_name.len, dentry->d_name.name, dir->i_ino);

    // 1. 讀取父目錄的資料區塊 (這裡假設目錄只有一個 Block)
    // 計算記憶體位置：資料區起始點 + Block 索引 * Block 大小
    dir_data_block = sb_info->data_blocks + parent_inode->i_block * BLOCK_SIZE;

    // 2. 計算目錄裡有多少個檔案 (目錄項目)
    // 透過目錄的檔案大小 (i_size) 除以每個項目的結構大小
    dir_entry_count = parent_inode->i_size / sizeof(struct osfs_dir_entry);
    dir_entries = (struct osfs_dir_entry *)dir_data_block;

    // 3. 遍歷所有目錄項目，比對檔名
    for (i = 0; i < dir_entry_count; i++) {
        // 檢查檔名長度是否一樣，且內容是否相同
        if (strlen(dir_entries[i].filename) == dentry->d_name.len &&
            strncmp(dir_entries[i].filename, dentry->d_name.name, dentry->d_name.len) == 0) {
            
            // 找到了！根據記錄的 Inode 編號取得 VFS Inode
            inode = osfs_iget(dir->i_sb, dir_entries[i].inode_no);
            if (IS_ERR(inode)) {
                pr_err("osfs_lookup: Error getting inode %u\n", dir_entries[i].inode_no);
                return ERR_CAST(inode);
            }
            // 將找到的 Inode 與傳入的 dentry 連結起來並回傳
            return d_splice_alias(inode, dentry);
        }
    }

    // 沒找到檔案，回傳 NULL
    return NULL;
}

/**
 * 函式: osfs_iterate
 * 描述: 讀取目錄內容 (供 ls 指令使用)。
 * 輸入:
 * - filp: 代表開啟的目錄檔案指標。
 * - ctx: 目錄遍歷的上下文 (包含目前讀到第幾個位置 pos)。
 * 回傳:
 * - 0 表示成功。
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

    // 處理 "." (當前目錄) 和 ".." (上一層目錄)
    // ctx->pos == 0 代表剛開始讀取
    if (ctx->pos == 0) {
        if (!dir_emit_dots(filp, ctx))
            return 0;
    }

    // 取得目錄的資料區塊
    dir_data_block = sb_info->data_blocks + osfs_inode->i_block * BLOCK_SIZE;
    // 計算總共有多少個檔案
    dir_entry_count = osfs_inode->i_size / sizeof(struct osfs_dir_entry);
    dir_entries = (struct osfs_dir_entry *)dir_data_block;

    /* 調整索引：因為 pos 0 和 1 被 "." 和 ".." 用掉了，所以實際檔案從索引 0 開始對應 pos 2 */
    i = ctx->pos - 2;

    // 從上次停下來的地方繼續遍歷
    for (; i < dir_entry_count; i++) {
        struct osfs_dir_entry *entry = &dir_entries[i];
        unsigned int type = DT_UNKNOWN;

        // dir_emit 會把檔名、Inode 編號傳送給使用者空間 (User Space)
        if (!dir_emit(ctx, entry->filename, strlen(entry->filename), entry->inode_no, type)) {
            pr_err("osfs_iterate: dir_emit failed for entry '%s'\n", entry->filename);
            return -EINVAL;
        }

        // 更新讀取位置，下次從下一個開始
        ctx->pos++;
    }

    return 0;
}

/**
 * 函式: osfs_new_inode (輔助函式)
 * 描述: 在檔案系統中建立一個新的 Inode。
 * 輸入:
 * - dir: 父目錄 Inode。
 * - mode: 檔案模式 (包含權限和類型，如 S_IFREG)。
 * 回傳:
 * - 成功回傳新建立的 Inode 指標，失敗回傳錯誤指標。
 */
struct inode *osfs_new_inode(const struct inode *dir, umode_t mode)
{
    struct super_block *sb = dir->i_sb;
    struct osfs_sb_info *sb_info = sb->s_fs_info;
    struct inode *inode;
    struct osfs_inode *osfs_inode;
    int ino, ret;

    /* 檢查是否支援該檔案類型 (只支援目錄、一般檔案、符號連結) */
    if (!S_ISDIR(mode) && !S_ISREG(mode) && !S_ISLNK(mode)) {
        pr_err("File type not supported (only directory, regular file and symlink supported)\n");
        return ERR_PTR(-EINVAL);
    }

    /* 檢查是否還有空閒的 Inode 和 Block */
    if (sb_info->nr_free_inodes == 0 || sb_info->nr_free_blocks == 0)
        return ERR_PTR(-ENOSPC);

    /* 1. 從 Bitmap 分配一個新的 Inode 編號 */
    ino = osfs_get_free_inode(sb_info);
    if (ino < 0 || ino >= sb_info->inode_count)
        return ERR_PTR(-ENOSPC);

    /* 2. 分配一個 VFS Inode 結構 */
    inode = new_inode(sb);
    if (!inode)
        return ERR_PTR(-ENOMEM);

    /* 3. 初始化 VFS Inode (擁有者、權限、時間) */
    inode_init_owner(&nop_mnt_idmap, inode, dir, mode);
    inode->i_ino = ino;
    inode->i_sb = sb;
    inode->i_blocks = 0;
    simple_inode_init_ts(inode); // 初始化 atime, mtime, ctime

    /* 根據檔案類型設定對應的操作函式 (Function Pointers) */
    // 目錄
    if (S_ISDIR(mode)) {
        // 設定目錄專用的 Inode 操作
        // 讓 VFS 知道：如果要「在這個目錄下找檔案」或「建立新檔案」，要呼叫 osfs_dir_inode_operations 裡的函式
        inode->i_op = &osfs_dir_inode_operations;

        // 設定目錄專用的 File 操作
        // 讓 VFS 知道：如果使用者對這個目錄下 ls 指令 (讀取目錄內容)，要呼叫 osfs_dir_operations 裡的 iterate
        inode->i_fop = &osfs_dir_operations;

        // 設定硬連結數 (Hard Link Count) 為 2
        // 為什麼是 2？
        // 1. 父目錄有一個項目指向它 (例如 /home/user 中的 'user')
        // 2. 它自己內部有一個 '.' (點) 目錄指向自己
        set_nlink(inode, 2); 

        inode->i_size = 0;
    } 
    // 一般檔案
    else if (S_ISREG(mode)) {
        // 設定檔案專用的 Inode 操作 (定義在 file.c，通常是空的或只有 getattr)
        inode->i_op = &osfs_file_inode_operations;

        // 設定檔案專用的 File 操作 (定義在 file.c)
        // 讓 VFS 知道：如果使用者對這個檔案做 read/write，要呼叫 osfs_file_operations 裡的 osfs_read/osfs_write
        inode->i_fop = &osfs_file_operations;

        // 設定硬連結數為 1
        // 因為一般檔案只有父目錄指向它，它自己內部沒有 '.' 指向自己
        set_nlink(inode, 1);

        inode->i_size = 0;
    } 
    // 符號連結 
    else if (S_ISLNK(mode)) {
        // 這次作業可能沒實作符號連結的具體操作，所以被註解掉了
        // inode->i_op = &osfs_symlink_inode_operations;
        set_nlink(inode, 1);
        inode->i_size = 0;
    }

    /* 4. 取得並初始化底層的 OSFS Inode */
    osfs_inode = osfs_get_osfs_inode(sb, ino);
    if (!osfs_inode) {
        pr_err("osfs_new_inode: Failed to get osfs_inode for inode %d\n", ino);
        iput(inode);
        return ERR_PTR(-EIO);
    }
    memset(osfs_inode, 0, sizeof(*osfs_inode));

    /* 填入 OSFS Inode 屬性 */
    osfs_inode->i_ino = ino;
    osfs_inode->i_mode = inode->i_mode;
    osfs_inode->i_uid = i_uid_read(inode);
    osfs_inode->i_gid = i_gid_read(inode);
    osfs_inode->i_size = inode->i_size;
    osfs_inode->i_blocks = 1; // 簡化版：預設使用 1 個 Block
    osfs_inode->__i_atime = osfs_inode->__i_mtime = osfs_inode->__i_ctime = current_time(inode);
    inode->i_private = osfs_inode; // 連結 VFS inode 和 OSFS inode

    /* 5. 立即分配一個data block 給這個新檔案 */
    ret = osfs_alloc_data_block(sb_info, &osfs_inode->i_block);
    if (ret) {
        pr_err("osfs_new_inode: Failed to allocate data block\n");
        iput(inode); // 失敗則釋放 Inode
        return ERR_PTR(ret);
    }

    /* 更新 Superblock 的空閒計數 */
    sb_info->nr_free_inodes--;

    /* 標記 Inode 為 Dirty (需要寫回) */
    mark_inode_dirty(inode);

    return inode;
}

/**
 * 函式: osfs_add_dir_entry (輔助函式)
 * 描述: 將新檔案的資訊加入到父目錄的資料區塊中。
 */
static int osfs_add_dir_entry(struct inode *dir, uint32_t inode_no, const char *name, size_t name_len)
{
    struct osfs_sb_info *sb_info = dir->i_sb->s_fs_info;
    struct osfs_inode *parent_inode = dir->i_private;
    void *dir_data_block;
    struct osfs_dir_entry *dir_entries;
    int dir_entry_count;
    int i;

    // 取得父目錄的資料區塊
    dir_data_block = sb_info->data_blocks + parent_inode->i_block * BLOCK_SIZE;

    // 計算目前已有的目錄項目數量
    dir_entry_count = parent_inode->i_size / sizeof(struct osfs_dir_entry);
    
    // 檢查目錄是否已滿 (這個實作假設目錄只有一個 Block)
    if (dir_entry_count >= MAX_DIR_ENTRIES) {
        pr_err("osfs_add_dir_entry: Parent directory is full\n");
        return -ENOSPC;
    }

    dir_entries = (struct osfs_dir_entry *)dir_data_block;

    // 再次檢查是否有重複檔名 (雖然 lookup 應該檢查過了，但多一層保護)
    for (i = 0; i < dir_entry_count; i++) {
        if (strlen(dir_entries[i].filename) == name_len &&
            strncmp(dir_entries[i].filename, name, name_len) == 0) {
            pr_warn("osfs_add_dir_entry: File '%.*s' already exists\n", (int)name_len, name);
            return -EEXIST;
        }
    }

    // 在陣列尾端新增一個目錄項目
    strncpy(dir_entries[dir_entry_count].filename, name, name_len);
    dir_entries[dir_entry_count].filename[name_len] = '\0'; // 確保字串結尾
    dir_entries[dir_entry_count].inode_no = inode_no;       // 紀錄新檔案的 Inode 編號

    // 更新父目錄的大小 (因為多了一個項目)
    parent_inode->i_size += sizeof(struct osfs_dir_entry);

    return 0;
}

/**
 * 函式: osfs_create
 * 描述: 在目錄中建立一個新檔案 (touch 指令會用到)。
 * 輸入:
 * - idmap: 掛載命名空間 ID 映射 (通常用不到，傳給 helper 即可)。
 * - dir: 父目錄的 Inode。
 * - dentry: 代表新檔案的 dentry (此時還沒有 inode，狀態為 negative)。
 * - mode: 檔案模式。
 * - excl: 是否為獨佔建立。
 */
static int osfs_create(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, umode_t mode, bool excl)
{   
    // Step 1: 取得父目錄的 OSFS Inode
    struct osfs_inode *parent_inode = dir->i_private;
    struct osfs_inode *osfs_inode;
    struct inode *inode;
    int ret;

    // Step 2: 驗證檔名長度是否合法
    if (dentry->d_name.len > MAX_FILENAME_LEN) {
        pr_err("osfs_create: Filename too long\n");
        return -ENAMETOOLONG;
    }

    // Step 3: 分配並初始化新的 VFS Inode 與 OSFS Inode
    // 這邊會呼叫上面的 osfs_new_inode 函式
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
    
    // 初始化新檔案屬性 (其實 osfs_new_inode 已經做大部分了，這裡再次確認)
    // 注意：osfs_new_inode 已經幫它分配了一個 Block，所以 i_block 不應為 0，
    // 但這裡為了 Lab 要求或防呆，有時會重置一些狀態，視具體邏輯而定。
    // 在這份 code 中，new_inode 已經設為 1 並分配了 block，這裡的設定看似有點多餘但無害。
    // osfs_inode->i_block = ...; // 已經在 new_inode 分配
    osfs_inode->i_size = 0;
    // osfs_inode->i_blocks = 0; // 小心！new_inode 設為 1，這裡設為 0 可能會導致狀態不一致。
                                  // 如果 new_inode 已經 alloc block，這裡應該維持 1。
                                  // 根據你提供的 code，這裡被設為 0，這可能是一個 Bug 或者是
                                  // 預期在 write 時才真的使用該 block。但 new_inode 確實 alloc 了。

    // Step 4: 將新檔案的資訊 (檔名、Inode 號) 加入父目錄
    ret = osfs_add_dir_entry(dir, inode->i_ino, dentry->d_name.name, dentry->d_name.len);
    if (ret) {
        pr_err("osfs_create: Failed to add directory entry\n");
        iput(inode); // 如果加入目錄失敗，要把剛剛建立的 inode 刪掉
        return ret;
    }

    // Step 5: 更新父目錄的 Metadata (大小、時間)
    dir->i_size = parent_inode->i_size; // 同步大小
    inode_set_mtime_to_ts(dir, current_time(dir));
    inode_set_ctime_to_ts(dir, current_time(dir));
    mark_inode_dirty(dir); // 標記父目錄為 Dirty
    
    // Step 6: 綁定 Inode 到 VFS Dentry
    // 這是告訴 VFS：這個 dentry 現在對應到這個 inode 了 (檔案建立成功)
    d_instantiate(dentry, inode);

    pr_info("osfs_create: File '%.*s' created with inode %lu\n",
            (int)dentry->d_name.len, dentry->d_name.name, inode->i_ino);

    return 0;
}

// 定義目錄的 Inode 操作，告訴 VFS 遇到目錄時要用這些函式
const struct inode_operations osfs_dir_inode_operations = {
    .lookup = osfs_lookup, // 尋找檔案
    .create = osfs_create, // 建立檔案
    // 如果要支援 mkdir, rmdir, unlink 等，也要加在這裡
};

// 定義目錄的檔案操作，告訴 VFS 開啟目錄時要用這些函式
const struct file_operations osfs_dir_operations = {
    .iterate_shared = osfs_iterate, // 遍歷目錄 (ls)
    .llseek = generic_file_llseek,  // 定位
};