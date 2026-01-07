#include <linux/fs.h>
#include <linux/uaccess.h>
#include "osfs.h"

/**
 * 函式: osfs_read
 * 描述: 從檔案讀取資料。
 * 輸入:
 * - filp: 代表要讀取的檔案指標 (File Pointer)。
 * - buf: 使用者空間的 Buffer，讀到的資料要放在這。
 * - len: 使用者想要讀取的位元組數 (Bytes)。
 * - ppos: 目前檔案的讀取位置指標 (File Position Pointer)。
 * 回傳:
 * - 成功讀取的位元組數。
 * - 0 表示已讀到檔尾 (EOF) 或檔案為空。
 * - -EFAULT 表示記憶體複製失敗。
 */
static ssize_t osfs_read(struct file *filp, char __user *buf, size_t len, loff_t *ppos)
{
    // 取得檔案對應的 VFS Inode
    struct inode *inode = file_inode(filp);
    // 取得我們自定義的 OSFS Inode (存在 i_private 中)
    struct osfs_inode *osfs_inode = inode->i_private;
    // 取得 Superblock 資訊 (為了存取 data_blocks 記憶體區段)
    struct osfs_sb_info *sb_info = inode->i_sb->s_fs_info;
    void *data_block;
    ssize_t bytes_read;

    // 如果檔案還沒被分配資料區塊，代表是空檔案，直接回傳 0
    if (osfs_inode->i_blocks == 0)
        return 0;

    // 如果讀取位置已經超過檔案大小，代表讀完了
    if (*ppos >= osfs_inode->i_size)
        return 0;

    // 如果「目前位置 + 預計讀取長度」超過檔案實際大小，
    // 則將讀取長度修正為「剩餘可讀長度」
    if (*ppos + len > osfs_inode->i_size)
        len = osfs_inode->i_size - *ppos;

    // 計算資料在記憶體中的實體位置
    // 公式：資料區起始點 + (Block ID * Block 大小) + 檔案內偏移量
    data_block = sb_info->data_blocks + osfs_inode->i_block * BLOCK_SIZE + *ppos;
    
    // 將資料從 Kernel Space 複製到 User Space
    if (copy_to_user(buf, data_block, len))
        return -EFAULT;

    // 更新檔案讀取指標位置
    *ppos += len;
    bytes_read = len;

    return bytes_read;
}


/**
 * 函式: osfs_write
 * 描述: 寫入資料到檔案。
 * 輸入:
 * - filp: 代表要寫入的檔案指標。
 * - buf: 使用者空間的 Buffer，包含要寫入的資料。
 * - len: 想要寫入的長度。
 * - ppos: 目前檔案的寫入位置指標。
 * 回傳:
 * - 成功寫入的位元組數。
 * - 錯誤代碼 (如 -EFAULT, -ENOSPC 等)。
 */
static ssize_t osfs_write(struct file *filp, const char __user *buf, size_t len, loff_t *ppos)
{   
    // Step 1: 取得 Inode 和檔案系統資訊
    struct inode *inode = file_inode(filp);
    struct osfs_inode *osfs_inode = inode->i_private;
    struct osfs_sb_info *sb_info = inode->i_sb->s_fs_info;
    void *data_block;
    ssize_t bytes_written;
    int ret;

    // Step 2: 檢查是否已經分配了資料區塊 (Data Block)
    // 如果是新建立的檔案 (touch 出來的)，i_blocks 會是 0，這裡要幫它分配空間
    if (osfs_inode->i_blocks == 0) {
        // 從 Bitmap 中找一個空的 Block 並分配給它
        ret = osfs_alloc_data_block(sb_info, &osfs_inode->i_block);
        if (ret) {
            pr_err("osfs_write: Failed to allocate data block\n");
            return ret; // 如果空間滿了，回傳錯誤
        }
        osfs_inode->i_blocks = 1; // 標記已擁有 1 個 Block
    }

    // Step 3: 限制寫入長度 (因為這個版本的 OSFS 每個檔案只有一個 Block)
    // 檢查：目前位置 + 寫入長度 是否超過 Block 大小 (4096 bytes)
    if (*ppos + len > BLOCK_SIZE) {
        len = BLOCK_SIZE - *ppos; // 如果超過，截斷多餘的部分
    }
    
    // 如果沒有空間可寫了，回傳 0
    if (len <= 0) {
        return 0;
    }

    // Step 4: 將資料從使用者空間 (User Space) 寫入到資料區塊 (Kernel Space)
    // 計算寫入的記憶體位址
    data_block = sb_info->data_blocks + osfs_inode->i_block * BLOCK_SIZE + *ppos;
    
    if (copy_from_user(data_block, buf, len)) {
        pr_err("osfs_write: Failed to copy data from user space\n");
        return -EFAULT;
    }

    // Step 5: 更新 Inode 和 OSFS Inode 的屬性 (Metadata)
    *ppos += len;        // 更新寫入位置
    bytes_written = len; // 紀錄實際寫入長度
    
    // 如果寫入後，檔案變大了 (超過原本的 i_size)，則更新檔案大小
    if (*ppos > osfs_inode->i_size) {
        osfs_inode->i_size = *ppos;
        inode->i_size = osfs_inode->i_size; // 同步 VFS inode 大小
    }
    
    // 更新最後修改時間 (mtime) 和狀態改變時間 (ctime)
    osfs_inode->__i_mtime = current_time(inode);
    osfs_inode->__i_ctime = current_time(inode);
    inode_set_mtime_to_ts(inode, osfs_inode->__i_mtime);
    inode_set_ctime_to_ts(inode, osfs_inode->__i_ctime);
    
    // 標記 Inode 為 Dirty (告訴系統這個 Inode 變更過，需要寫回)
    mark_inode_dirty(inode);

    // Step 6: 回傳實際寫入的位元組數
    return bytes_written;
}

/**
 * 結構: osfs_file_operations
 * 描述: 定義一般檔案的操作函式表。
 * 當使用者對檔案呼叫 read/write 時，Kernel 會查這張表來決定執行哪個函式。
 */
const struct file_operations osfs_file_operations = {
    .open = generic_file_open, // 使用通用的開啟函式
    .read = osfs_read,         // 對應到我們實作的 osfs_read
    .write = osfs_write,       // 對應到我們實作的 osfs_write
    .llseek = default_llseek,  // 使用預設的定位函式 (lseek)
    // 如果需要其他操作 (如 mmap, fsync)，可以加在這裡
};

/**
 * 結構: osfs_file_inode_operations
 * 描述: 定義檔案 Inode 的操作函式表。
 * 目前為空，因為我們不需要特殊的 Inode 操作 (如設定屬性 setattr)。
 */
const struct inode_operations osfs_file_inode_operations = {
    // 可以在這裡加入 .getattr = osfs_getattr 等操作
};