#include <linux/fs.h>
#include <linux/uaccess.h>
#include "osfs.h"

/**
 * 函式: osfs_read
 * 描述: 支援多層索引 (Multi-level Indexing) 的檔案讀取函式。
 * 這能夠處理跨越多個 Block 的大檔案讀取。
 */
static ssize_t osfs_read(struct file *filp, char __user *buf, size_t len, loff_t *ppos)
{
    struct inode *inode = file_inode(filp);
    struct osfs_inode *osfs_inode = inode->i_private;
    struct osfs_sb_info *sb_info = inode->i_sb->s_fs_info;
    void *data_block;
    ssize_t bytes_read = 0;
    uint32_t phys_block;
    loff_t offset;
    size_t to_read;
    int ret;

    // 檢查檔案是否為空 (沒有分配任何 Block)
    if (osfs_inode->i_blocks == 0)
        return 0;

    // 檢查讀取位置是否超過檔案大小
    if (*ppos >= osfs_inode->i_size)
        return 0;

    // 調整讀取長度，避免讀超過檔案結尾
    if (*ppos + len > osfs_inode->i_size)
        len = osfs_inode->i_size - *ppos;

    // --- 迴圈處理跨 Block 讀取 ---
    while (len > 0) {
        // 1. 計算目前的「邏輯」區塊編號 (例如：這是檔案的第幾個 Block)
        sector_t block = *ppos / BLOCK_SIZE;
        
        // 2. 計算在該 Block 內的偏移量 (Offset)
        offset = *ppos % BLOCK_SIZE;
        
        // 3. 計算這次迴圈能讀多少 (不能跨越 Block 邊界，取最小值)
        to_read = min_t(size_t, len, BLOCK_SIZE - offset);

        // 4. 呼叫核心函式取得「實體」區塊編號
        // create=0 表示如果不存則不要建立 (讀取模式)
        ret = osfs_get_block(inode, block, &phys_block, 0);
        
        if (ret || phys_block == 0) {
            // 如果發生錯誤，或是 phys_block 為 0 (代表這是檔案的「洞」，沒資料)
            // 填入 0 給使用者
            if (clear_user(buf + bytes_read, to_read))
                return bytes_read > 0 ? bytes_read : -EFAULT;
        } else {
            // 5. 計算實體記憶體位址並複製資料
            data_block = sb_info->data_blocks + phys_block * BLOCK_SIZE + offset;
            if (copy_to_user(buf + bytes_read, data_block, to_read))
                return bytes_read > 0 ? bytes_read : -EFAULT;
        }

        // 6. 更新指標與計數器，準備讀下一個 Block
        *ppos += to_read;
        bytes_read += to_read;
        len -= to_read;
    }

    return bytes_read;
}

/**
 * 函式: osfs_write
 * 描述: 支援多層索引的檔案寫入函式。
 * 這能夠處理跨越多個 Block 的大檔案寫入，並自動分配所需的 Direct/Indirect Blocks。
 */
static ssize_t osfs_write(struct file *filp, const char __user *buf, size_t len, loff_t *ppos)
{
    struct inode *inode = file_inode(filp);
    struct osfs_inode *osfs_inode = inode->i_private;
    struct osfs_sb_info *sb_info = inode->i_sb->s_fs_info;
    void *data_block;
    ssize_t bytes_written = 0;
    uint32_t phys_block;
    loff_t offset;
    size_t to_write;
    int ret;

    // --- 迴圈處理跨 Block 寫入 ---
    while (len > 0) {
        // 1. 計算邏輯區塊與偏移量
        sector_t block = *ppos / BLOCK_SIZE;
        offset = *ppos % BLOCK_SIZE;
        // 計算這次能寫多少 (填滿目前的 Block 為止)
        to_write = min_t(size_t, len, BLOCK_SIZE - offset);

        // 2. 取得或分配實體區塊
        // create=1 表示如果該區塊 (或中間的索引表) 不存在，請幫我建立！
        ret = osfs_get_block(inode, block, &phys_block, 1);
        if (ret) {
            pr_err("osfs_write: Failed to get/allocate block %llu, error %d\n", 
                   (unsigned long long)block, ret);
            // 如果前面已經有寫入部分資料，則回傳成功寫入的位元組數，不回傳錯誤
            if (bytes_written > 0)
                break;
            return ret;
        }

        // 防呆檢查
        if (phys_block == 0) {
            pr_err("osfs_write: Invalid physical block\n");
            if (bytes_written > 0)
                break;
            return -EIO;
        }

        // 3. 將資料寫入實體 Block
        data_block = sb_info->data_blocks + phys_block * BLOCK_SIZE + offset;
        if (copy_from_user(data_block, buf + bytes_written, to_write)) {
            pr_err("osfs_write: Failed to copy data from user space\n");
            if (bytes_written > 0)
                break;
            return -EFAULT;
        }

        // 4. 更新指標
        *ppos += to_write;
        bytes_written += to_write;
        len -= to_write;
    }

    // 5. 更新檔案 Metadata
    // 如果寫入後檔案變大了，更新檔案大小
    if (*ppos > osfs_inode->i_size) {
        osfs_inode->i_size = *ppos;
        inode->i_size = osfs_inode->i_size;
    }

    // 更新時間
    osfs_inode->__i_mtime = current_time(inode);
    osfs_inode->__i_ctime = current_time(inode);
    inode_set_mtime_to_ts(inode, osfs_inode->__i_mtime);
    inode_set_ctime_to_ts(inode, osfs_inode->__i_ctime);

    mark_inode_dirty(inode);

    return bytes_written;
}

/**
 * 結構: osfs_file_operations
 * 描述: 定義一般檔案的操作。
 */
const struct file_operations osfs_file_operations = {
    .open = generic_file_open,
    .read = osfs_read,
    .write = osfs_write,
    .llseek = default_llseek,
};

/**
 * 結構: osfs_file_inode_operations
 * 描述: 定義 Inode 操作 (目前為空)。
 */
const struct inode_operations osfs_file_inode_operations = {
};