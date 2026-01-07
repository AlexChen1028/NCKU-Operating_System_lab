#ifndef _OSFS_H
#define _OSFS_H

#include <linux/types.h>
#include <linux/fs.h>
#include <linux/bitmap.h>
#include <linux/time.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/string.h>
#include <linux/module.h>

#define OSFS_MAGIC 0x051AB520
//#define BLOCK_SIZE 4096
#define INODE_COUNT 20
#define DATA_BLOCK_COUNT 20
#define MAX_FILENAME_LEN 255
#define MAX_DIR_ENTRIES (BLOCK_SIZE / sizeof(struct osfs_dir_entry))

// Multi-level indexing constants
#define OSFS_N_DIRECT 12      // Number of direct blocks
#define OSFS_N_INDIRECT 1     // Number of indirect blocks
#define OSFS_N_DINDIRECT 1    // Number of double indirect blocks
#define OSFS_N_BLOCKS (OSFS_N_DIRECT + OSFS_N_INDIRECT + OSFS_N_DINDIRECT)

// Number of block pointers per indirect block
#define OSFS_ADDR_PER_BLOCK (BLOCK_SIZE / sizeof(uint32_t))

#define BITMAP_SIZE(bits) (((bits) + BITS_PER_LONG - 1) / BITS_PER_LONG)

#define INODE_BITMAP_SIZE BITMAP_SIZE(INODE_COUNT)
#define BLOCK_BITMAP_SIZE BITMAP_SIZE(DATA_BLOCK_COUNT)

#define ROOT_INODE 1

/**
 * Struct: osfs_sb_info
 * Description: Superblock information for the osfs filesystem.
 */
struct osfs_sb_info {
    uint32_t magic;
    uint32_t block_size;
    uint32_t inode_count;
    uint32_t block_count;
    uint32_t nr_free_inodes;
    uint32_t nr_free_blocks;
    unsigned long *inode_bitmap;
    unsigned long *block_bitmap;
    void *inode_table;
    void *data_blocks;
};

/**
 * Struct: osfs_dir_entry
 * Description: Directory entry structure.
 */
struct osfs_dir_entry {
    char filename[MAX_FILENAME_LEN];
    uint32_t inode_no;
};

/**
 * Struct: osfs_inode
 * Description: Filesystem-specific inode structure with multi-level indexing.
 */
struct osfs_inode {
    uint32_t i_ino;
    uint32_t i_size;
    uint32_t i_blocks;
    uint16_t i_mode;
    uint16_t i_links_count;
    uint32_t i_uid;
    uint32_t i_gid;
    struct timespec64 __i_atime;
    struct timespec64 __i_mtime;
    struct timespec64 __i_ctime;
    
    // Multi-level indexing structure
    uint32_t i_block[OSFS_N_BLOCKS];  // [0-11]: direct blocks
                                       // [12]: indirect block
                                       // [13]: double indirect block
};

struct inode *osfs_iget(struct super_block *sb, unsigned long ino);
struct osfs_inode *osfs_get_osfs_inode(struct super_block *sb, uint32_t ino);
int osfs_get_free_inode(struct osfs_sb_info *sb_info);
int osfs_alloc_data_block(struct osfs_sb_info *sb_info, uint32_t *block_no);
int osfs_fill_super(struct super_block *sb, void *data, int silent);
struct inode *osfs_new_inode(const struct inode *dir, umode_t mode);
void osfs_free_data_block(struct osfs_sb_info *sb_info, uint32_t block_no);
void osfs_destroy_inode(struct inode *inode);

// New helper functions for multi-level indexing
int osfs_get_block(struct inode *inode, sector_t block, uint32_t *phys_block, int create);
void osfs_free_inode_blocks(struct inode *inode);

extern const struct inode_operations osfs_file_inode_operations;
extern const struct file_operations osfs_file_operations;
extern const struct inode_operations osfs_dir_inode_operations;
extern const struct file_operations osfs_dir_operations;
extern const struct super_operations osfs_super_ops;

#endif /* _osfs_H */