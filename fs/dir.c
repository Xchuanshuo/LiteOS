#include "dir.h"
#include "../lib/stdint.h"
#include "inode.h"
#include "file.h"
#include "fs.h"
#include "../lib/kernel/stdio-kernel.h"
#include "../kernel/global.h"
#include "../kernel/debug.h"
#include "../kernel/memory.h"
#include "../lib/string.h"
#include "super_block.h"

struct dir root_dir; // 根目录

/** 打开根目录 */
void open_root_dir(struct partition* part) {
    root_dir.inode = inode_open(part, part->sb->root_inode_no);
    root_dir.dir_pos = 0;
}

/**
 * 在分区part上打开编号为inode_no的目录
 * @param part 分区
 * @param inode_no inode编号
 * @return 返回目录指针
 */
struct dir* dir_open(struct partition* part, uint32_t inode_no) {
    struct dir* pdir = (struct dir*)sys_malloc(sizeof(struct dir));
    pdir->inode = inode_open(part, inode_no);
    pdir->dir_pos = 0;
    return pdir;
}

/**
 * 在part分区内的pdir目录下查找名为name的文件或目录
 * @param part 分区
 * @param pdir 目录
 * @param name 文件名
 * @param dir_e 找到后对象存储的目标位置
 * @return 是否找到名为name的文件或目录
 */
bool search_dir_entry(struct partition* part, struct dir* pdir,
                      const char* name, struct dir_entry* dir_e) {
    uint32_t block_cnt = 140; // 12个直接块 + 128个1级间接块 = 140块
    // 存储所有的数据块信息占用的字节大小
    uint32_t* all_blocks = (uint32_t*) sys_malloc(48 + 512);
    if (all_blocks == NULL) {
        printk("search_dir_entry: sys_malloc for all_blocks failed");
        return false;
    }
    uint32_t block_idx = 0;
    while (block_idx < 12) {
        all_blocks[block_idx] = pdir->inode->i_sectors[block_idx];
        block_idx++;
    }
    block_idx = 0;
    if (pdir->inode->i_sectors[12] != 0) {
        // 若含有1级间接块表
        ide_read(part->my_disk, pdir->inode->i_sectors[12], all_blocks + 12, 1);
    }
    // 写目录项的时候已保证目录项不跨扇区,这样读目录项时容易
    // 处理, 只申请容纳1个扇区的内存
    uint8_t* buf = (uint8_t*)sys_malloc(SECTOR_SIZE);
    struct dir_entry* p_de = (struct dir_entry*) buf;
    uint32_t dir_entry_size = part->sb->dir_entry_size;
    // 1扇区可容纳的目录项个数
    uint32_t dir_entry_cnt = SECTOR_SIZE / dir_entry_size;

    // 开始在所有块中查找目录项
    while (block_idx < block_cnt) {
        if (all_blocks[block_idx] == 0) {
            block_idx++;
            continue;
        }
        ide_read(part->my_disk, all_blocks[block_idx], buf, 1);
        uint32_t dir_entry_idx = 0;
        while (dir_entry_idx < dir_entry_cnt) {
            // 若找到了,就复制整个目录项
            if (!strcmp(p_de->filename, name)) {
                memcpy(dir_e, p_de, dir_entry_size);
                sys_free(buf);
                sys_free(all_blocks);
                return true;
            }
            dir_entry_idx++;
            p_de++;
        }
        block_idx++;
        // 此时p_de已经指向一个扇区最后一个完整目录项了,恢复p_de指向为buf
        p_de = (struct dir_entry *) buf;
        memset(buf, 0, SECTOR_SIZE);
    }
    sys_free(buf);
    sys_free(all_blocks);
    return false;
}

/** 关闭目录 */
void dir_close(struct dir* dir) {
    if (dir == &root_dir) {
        return;
    }
    inode_close(dir->inode);
    sys_free(dir);
}

/**
 * 在内存中初始化目录项p_de
 * @param filename  文件名
 * @param inode_no  inode编号
 * @param file_type  文件类型
 * @param p_de  目标对象
 */
void create_dir_entry(char* filename, uint32_t inode_no,
                      uint8_t file_type, struct dir_entry* p_de) {
    ASSERT(strlen(filename) <= MAX_FILE_NAME_LEN);
    // 初始化目录项
    memcpy(p_de->filename, filename, strlen(filename));
    p_de->i_no = inode_no;
    p_de->f_type = file_type;
}

/**
 * 将目录项p_de写入父目录中,io_buf由主调函数提供
 * @param parent_dir 父目录
 * @param p_de 目录项
 * @param io_buf io缓冲区
 * @return
 */
bool sync_dir_entry(struct dir* parent_dir, struct dir_entry* p_de, void* io_buf) {
    struct inode* dir_inode = parent_dir->inode;
    uint32_t dir_size = dir_inode->i_size;
    uint32_t dir_entry_size = cur_part->sb->dir_entry_size;

    ASSERT(dir_size % dir_entry_size == 0);

    uint32_t dir_entry_per_sec = 512 / dir_entry_size;

    int32_t block_lba = -1;
    // 将该目录的所有扇区地址(12个直接块 + 128个间接块)存入all_blocks
    uint8_t block_idx = 0;
    uint32_t all_blocks[140] = {0};
    // 将12个直接块直接存入all_blocks
    while (block_idx < 12) {
        all_blocks[block_idx] = dir_inode->i_sectors[block_idx];
        block_idx++;
    }
    // dir_e用来在io_buf中遍历目录项
    struct dir_entry* dir_e = (struct dir_entry*)io_buf;
    int32_t block_bitmap_idx = -1;
    // 开始遍历所有块以寻找目录空位,若已有扇区中没有空闲位,
    // 在不超过文件大小的情况下申请扇区来存储新目录项
    block_idx = 0;
    // 文件(包括目录)最大支持12个直接块+128个间接块＝140个块
    while (block_idx < 140) {
        block_bitmap_idx = -1;
        if (all_blocks[block_idx] == 0) {
            block_lba = block_bitmap_alloc(cur_part);
            if (block_lba == -1) {
                printk("alloc block bitmap for sync_dir_entry failed\n");
                return false;
            }
            // 每分配一个块就同步一次block_bitmap
            block_bitmap_idx = block_lba - cur_part->sb->data_start_lba;
            ASSERT(block_bitmap_idx != -1);
            bitmap_sync(cur_part, block_bitmap_idx, BLOCK_BITMAP);

            block_bitmap_idx = -1;
            if (block_idx < 12) { // 若是直接块
                dir_inode->i_sectors[block_idx] = all_blocks[block_idx] = block_lba;
            } else if (block_idx == 12) {
                // 若是尚未分配1级间接块表(block_idx等于12表示第0个间接块地址为0)
                // 将上面分配的块作为一级间接块表地址
                dir_inode->i_sectors[12] = block_lba;
                block_lba = -1;
                // 再分间接块
                block_lba = block_bitmap_alloc(cur_part);

                if (block_lba == -1) {
                    block_bitmap_idx = dir_inode->i_sectors[12] - cur_part->sb->data_start_lba;
                    bitmap_set(&cur_part->block_bitmap, block_bitmap_idx, 0);
                    dir_inode->i_sectors[12] = 0;
                    printk("alloc block bitmap for sync_dir_entry failed\n");
                    return false;
                }
                // 每分配一个块就同步一次block_bitmap
                block_bitmap_idx = block_lba - cur_part->sb->data_start_lba;
                ASSERT(block_bitmap_idx != -1);
                bitmap_sync(cur_part, block_bitmap_idx, BLOCK_BITMAP);

                all_blocks[12] = block_lba;
                // 把新分配的第0个间接块地址写入一级间接块表
                ide_write(cur_part->my_disk, dir_inode->i_sectors[12], all_blocks + 12, 1);
            } else { // 若是间接块未分配
                all_blocks[block_idx] = block_lba;
                // 把新分配的第(block_idx-12)个间接块地址写入一级间接块表
                ide_write(cur_part->my_disk, dir_inode->i_sectors[12], all_blocks + 12, 1);
            }
            // 再将新目录项p_de写入新分配的间接块
            memset(io_buf, 0, 512);
            memcpy(io_buf, p_de, dir_entry_size);
            ide_write(cur_part->my_disk, all_blocks[block_idx], io_buf, 1);
            dir_inode->i_size += dir_entry_size;
            return true;
        }
        // 若block_idx块已存在,将其读入内存,然后在该块中查找空目录项
        ide_read(cur_part->my_disk, all_blocks[block_idx], io_buf, 1);
        uint8_t dir_entry_idx = 0;
        while (dir_entry_idx < dir_entry_per_sec) {
            if ((dir_e + dir_entry_idx)->f_type == FT_UNKNOWN) {
                // FT_UNKNOWN为0,无论是初始化或是删除文件后,都会将f_type置为FT_UNKNOWN.
                memcpy(dir_e + dir_entry_idx, p_de, dir_entry_size);
                ide_write(cur_part->my_disk, all_blocks[block_idx], io_buf, 1);

                dir_inode->i_size += dir_entry_size;
                return true;
            }
            dir_entry_idx++;
        }
        block_idx++;
    }
    printk("directory is full!\n");
    return false;
}

/** 把分区part目录pdir中编号为inode_no的目录项删除 */
bool delete_dir_entry(struct partition* part, struct dir* pdir,
                      uint32_t inode_no, void* io_buf) {
    struct inode* dir_inode = pdir->inode;
    uint32_t block_idx = 0, all_blocks[140] = {0};
    // 收集目录全部块地址
    while (block_idx < 12) {
        all_blocks[block_idx] = dir_inode->i_sectors[block_idx];
        block_idx++;
    }
    if (dir_inode->i_sectors[12]) {
        ide_read(part->my_disk, dir_inode->i_sectors[12], all_blocks + 12, 1);
    }
    // 目录项在存储时保证不会跨扇区
    uint32_t dir_entry_size = part->sb->dir_entry_size;
    uint32_t dir_entry_per_sec = (SECTOR_SIZE / dir_entry_size); // 每扇区最大目录项数目

    struct dir_entry* dir_e = (struct dir_entry*)io_buf;
    struct dir_entry* dir_entry_found = NULL;
    uint8_t dir_entry_idx, dir_entry_cnt;
    bool is_dir_first_block = false; // 目录的第1个块
    // 遍历所有块,寻找目录项
    block_idx = 0;
    while (block_idx < 140) {
        is_dir_first_block = false;
        if (all_blocks[block_idx] == 0) {
            block_idx++;
            continue;
        }
        dir_entry_idx = dir_entry_cnt = 0;
        memset(io_buf, 0, SECTOR_SIZE);
        // 读取扇区,获得目录项
        ide_read(part->my_disk, all_blocks[block_idx], io_buf, 1);
        // 遍历所有目录项,统计该扇区的目录项数量以及是否有待删除的目录项
        while (dir_entry_idx < dir_entry_per_sec) {
            struct dir_entry* temp = (dir_e + dir_entry_idx);
            if (temp->f_type != FT_UNKNOWN) {
                if (!strcmp(temp->filename, ".")) {
                    is_dir_first_block = true;
                } else if (strcmp(temp->filename, ".") &&
                           strcmp(temp->filename, "..")) {
                    dir_entry_cnt++; // 统计此扇区内的目录项个数,用来判断删除目录项后是否回收该扇区
                    if (temp->i_no == inode_no) { // 如果找到此inode
                        ASSERT(dir_entry_found == NULL);
                        dir_entry_found = temp;
                    }
                }
            }
            dir_entry_idx++;
        }
        // 若此扇区未找到该目录项,继续在下个扇区中找
        if (dir_entry_found == NULL) {
            block_idx++;
            continue;
        }
        // 在此扇区中找到目录项后,清除该目录项并判断是否回收扇区,随后退出循环直接返回
        ASSERT(dir_entry_cnt >= 1);
        // 除目录第1个扇区外,若该扇区上只有该目录项自己,则将整个扇区回收
        if (dir_entry_cnt == 1 && !is_dir_first_block) {
            // a.在块位图中回收该块
            uint32_t block_bitmap_idx = all_blocks[block_idx] - part->sb->data_start_lba;
            bitmap_set(&part->block_bitmap, block_bitmap_idx, 0);
            bitmap_sync(cur_part, block_bitmap_idx, BLOCK_BITMAP);
            // b.将块地址从数组i_sectors或索引表中去掉
            if (block_idx < 12) {
                dir_inode->i_sectors[block_idx] = 0;
            } else { // 在一级间接索引表中擦除该间接块地址
                uint32_t indirect_blocks = 0;
                uint32_t indirect_block_idx = 12;
                while (indirect_block_idx < 140) {
                    if (all_blocks[indirect_block_idx] != 0) {
                        indirect_blocks++;
                    }
                    indirect_block_idx++;
                }
                ASSERT(indirect_blocks > 1);
                if (indirect_blocks > 1) {
                    // 间接索引表中还包括其它间接块,仅在索引表中擦除当前这个间接块地址
                    all_blocks[block_idx] = 0;
                    ide_write(part->my_disk, dir_inode->i_sectors[12], all_blocks + 12, 1);
                } else {
                    // 间接索引表中就当前这1个间接块,直接把间接索引表所在的块回收,然后擦除间接索引表块地址
                    // 回收间接索引表所在的块
                    block_bitmap_idx = dir_inode->i_sectors[12] - part->sb->data_start_lba;
                    bitmap_set(&part->block_bitmap, block_bitmap_idx, 0);
                    bitmap_sync(cur_part, block_bitmap_idx, BLOCK_BITMAP);

                    // 将间接索引表地址清0
                    dir_inode->i_sectors[12] = 0;
                }
            }
        } else { // 仅将该目录清空
            memset(dir_entry_found, 0, dir_entry_size);
            ide_write(part->my_disk, all_blocks[block_idx], io_buf, 1);
        }
        // 更新i结点信息并同步到硬盘
        ASSERT(dir_inode->i_size >= dir_entry_size);
        dir_inode->i_size -= dir_entry_size;
        memset(io_buf, 0, SECTOR_SIZE * 2);
        inode_sync(part, dir_inode, io_buf);

        return true;
    }
    // 所有块中未找到则返回false,若出现这种情况应该是serarch_file出错了
    return false;
}