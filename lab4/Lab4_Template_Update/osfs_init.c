#include <linux/init.h>
#include <linux/module.h>
#include "osfs.h"

// 前置宣告函式原型
static struct dentry *osfs_mount(struct file_system_type *fs_type,
                                 int flags,
                                 const char *dev_name,
                                 void *data);

static void osfs_kill_superblock(struct super_block *sb);

/**
 * 結構: osfs_type
 * 描述: 定義 osfs 檔案系統的描述結構，讓 Kernel 認識它。
 * 這是在 register_filesystem 時傳給 Kernel 的名片。
 */
struct file_system_type osfs_type = {
    .owner = THIS_MODULE,             // 擁有此結構的模組 (通常是 THIS_MODULE)
    .name = "osfs",                   // 檔案系統名稱 (mount -t osfs 時用的名字)
    .mount = osfs_mount,              // 掛載時要執行的函式
    .kill_sb = osfs_kill_superblock,  // 卸載/銷毀 Superblock 時要執行的函式
    .fs_flags = FS_USERNS_MOUNT,      // 允許在 User Namespace 中掛載 (容器化常用)
};

/**
 * 函式: osfs_init
 * 描述: 模組初始化函式 (對應 insmod 指令)。
 * 輸入: 無。
 * 回傳:
 * - 0 表示註冊成功。
 * - 負數表示失敗 (錯誤代碼)。
 */
static int __init osfs_init(void)
{
    int ret;

    // 向 Linux Kernel 註冊我們的檔案系統
    ret = register_filesystem(&osfs_type);
    if (ret) {
        pr_err("Failed to register filesystem\n"); // 如果註冊失敗，印出錯誤訊息
        return ret;
    }

    pr_info("osfs: Successfully registered\n"); // dmesg 會看到這行
    return 0;
}

/**
 * 函式: osfs_exit
 * 描述: 模組卸載函式 (對應 rmmod 指令)。
 * 輸入: 無。
 * 回傳: 無。
 */
static void __exit osfs_exit(void)
{
    int ret;

    // 從 Linux Kernel 註銷檔案系統
    ret = unregister_filesystem(&osfs_type);
    if (ret)
        pr_err("Failed to unregister filesystem\n");
    else
        pr_info("osfs: Successfully unregistered\n");
}

/**
 * 函式: osfs_mount
 * 描述: 處理掛載 (mount) 請求。
 * 輸入:
 * - fs_type: 指向 osfs_type 的指標。
 * - flags: 掛載旗標 (如唯讀等)。
 * - dev_name: 裝置名稱 (因為我們是 nodev，所以這個參數通常被忽略或設為 none)。
 * - data: 掛載時傳入的額外選項字串。
 * 回傳:
 * - 成功回傳指向根目錄的 dentry 指標。
 */
static struct dentry *osfs_mount(struct file_system_type *fs_type,
                                 int flags,
                                 const char *dev_name,
                                 void *data)
{
    // mount_nodev 用於「不需要實體區塊裝置」的檔案系統 (如記憶體檔案系統)。
    // 關鍵參數是 osfs_fill_super，這是我們在 super.c 定義的函式，用來初始化 Superblock。
    return mount_nodev(fs_type, flags, data, osfs_fill_super);
}

/**
 * 函式: osfs_kill_superblock
 * 描述: 清理並釋放檔案系統的 Superblock (卸載時執行)。
 * 輸入:
 * - sb: 要銷毀的 Superblock 指標。
 */
static void osfs_kill_superblock(struct super_block *sb)
{
    // 取得我們自定義的 Superblock Info 結構 (存放記憶體配置資訊)
    struct osfs_sb_info *sb_info = sb->s_fs_info;

    pr_info("osfs_kill_superblock: Unmounting file system\n");

    if (sb_info) {
        pr_info("osfs_kill_superblock: free block \n");

        // 釋放當初在 osfs_fill_super 中用 vmalloc 分配的大塊記憶體
        vfree(sb_info);
        sb->s_fs_info = NULL; // 避免懸空指標 (Dangling Pointer)
    }

    pr_info("osfs_kill_superblock: File system unmounted successfully\n");
}

// 巨集：指定模組的初始化與結束函式
module_init(osfs_init);
module_exit(osfs_exit);

// 模組資訊
MODULE_LICENSE("GPL");            // 授權條款 (必須是 GPL 相容才能使用某些 Kernel API)
MODULE_AUTHOR("OSLAB");           // 作者
MODULE_DESCRIPTION("A simple memory-based file system kernel module"); // 描述