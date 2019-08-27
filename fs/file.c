#include "file.h"
#include "fs.h"
#include "super_block.h"
#include "inode.h"
#include "../lib/kernel/stdio-kernel.h"
#include "../kernel/memory.h"
#include "../kernel/debug.h"
#include "../kernel/interrupt.h"
#include "../lib/string.h"
#include "../thread/thread.h"
#include "../kernel/global.h"

#define DEFAULT_SETS 1

/** 文件表 */
struct file file_table[MAX_FILE_OPEN];

/***
 * 从文件表file_table中获取一个空闲位
 * @return 返回空闲位的下标
 */
int32_t get_free_slot_in_global(void) {
    uint32_t fd_idx = 3;
    while (fd_idx < MAX_FILE_OPEN) {
        if (file_table[fd_idx].fd_inode == NULL) {
            break;
        }
        fd_idx++;
    }
    if (fd_idx == MAX_FILE_OPEN) {
        printk("exceed max open files\n");
        return -1;
    }
    return fd_idx;
}

/**
 * 将全局描述符下标安装到进程或线程自己的文件描述符数组fd_table中
 * @param global_fd_idx 文件描述符下标
 * @return 成功则返回文件描述符数组的下标
 */
int32_t pcb_fd_install(int32_t global_fd_idx) {
    struct task_struct* cur = running_thread();
    uint8_t local_fd_idx = 3; // 跨过stdin, stdout, stderr
    while (local_fd_idx < MAX_FILES_OPEN_PER_PROC) {
        if (cur->fd_table[local_fd_idx] == -1) {
            cur->fd_table[local_fd_idx] = global_fd_idx;
            break;
        }
        local_fd_idx++;
    }
    if (local_fd_idx == MAX_FILES_OPEN_PER_PROC) {
        printk("exceed max open files_per_proc\n");
        return -1;
    }
    return local_fd_idx;
}

/**
 * 分配一个inode
 * @param part 分区
 * @return 返回分配好的inode的编号
 */
int32_t inode_bitmap_alloc(struct partition* part) {
    int32_t bit_idx = bitmap_scan(&part->inode_bitmap, 1);
    if (bit_idx == -1) {
        return -1;
    }
    bitmap_set(&part->inode_bitmap, bit_idx, 1);
    return bit_idx;
}

/**
 * 分配一个数据块(这里是1扇区)
 * @param part 分区
 * @return 返回数据块地址
 */
int32_t block_bitmap_alloc(struct partition* part) {
    int32_t bit_idx = bitmap_scan(&part->block_bitmap, 1);
    if (bit_idx == -1) {
        return -1;
    }
    bitmap_set(&part->block_bitmap, bit_idx, 1);
    // 数据块起始地址 + 分配的数据块号
    return part->sb->data_start_lba + bit_idx;
}

/**
 * 将内存中bitmap第bit_idx位所在的512字节(扇区)同步到硬盘
 * @param part 分区
 * @param bit_idx bit位
 * @param btmp_type 位图类型
 */
void bitmap_sync(struct partition* part, uint32_t bit_idx, uint8_t btmp_type) {
    uint32_t off_sec = bit_idx / 4096; // 本inode相对于位图的扇区偏移量
    uint32_t off_size = off_sec * BLOCK_SIZE; // 本inode相对于位图的字节偏移量
    uint32_t sec_lba = 0;
    uint8_t *bitmap_off = NULL;
    switch (btmp_type) {
        case INODE_BITMAP:
            sec_lba = part->sb->inode_bitmap_lba + off_sec;
            bitmap_off = part->inode_bitmap.bits + off_size;
            break;
        case BLOCK_BITMAP:
            sec_lba = part->sb->block_bitmap_lba + off_sec;
            bitmap_off = part->block_bitmap.bits + off_size;
            break;
        default:break;
    }
    ide_write(part->my_disk, sec_lba, bitmap_off, 1);
}

/**
 * 创建文件
 * @param parent_dir 父目录
 * @param filename 文件名
 * @return 返回文件描述符
 */
int32_t file_create(struct dir* parent_dir, char* filename, uint8_t flag) {
    // 后续操作的公共缓冲区
    void* io_buf = sys_malloc(1024);
    if (io_buf == NULL) {
        printk("in file_create: sys_malloc for io_buf failed!\n");
        return -1;
    }
    uint8_t rollback_step = 0; // 用于操作失败时回滚各资源状态
    // 为新文件分配inode
    int32_t inode_no = inode_bitmap_alloc(cur_part);
    if (inode_no == -1) {
        printk("in file_create: allocate inode failed!\n");
        return -1;
    }
    // inode要从堆内存中申请,不可生成局部变量(函数退出时会释放)
    // 因为file_table数组中的文件描述符的inode指针要指向它
    struct inode* new_file_node = (struct inode*)sys_malloc(sizeof(struct inode));
    if (new_file_node == NULL) {
        printk("file_create: sys_malloc for inode failded\n");
        rollback_step = 1;
        goto rollback;
    }
    inode_init(inode_no, new_file_node); // 初始化inode
    // 返回的是file_table数组的下标
    int fd_idx = get_free_slot_in_global();
    if (fd_idx == -1) {
        printk("exceed max open files\n");
        rollback_step = 2;
        goto rollback;
    }
    file_table[fd_idx].fd_inode = new_file_node;
    file_table[fd_idx].fd_pos = 0;
    file_table[fd_idx].fd_flag = flag;
    file_table[fd_idx].fd_inode->write_deny = false;

    struct dir_entry new_dir_entry;
    memset(&new_dir_entry, 0, sizeof(struct dir_entry));

    create_dir_entry(filename, inode_no, FT_REGULAR, &new_dir_entry);
    /** 同步数据到硬盘 */
    // 1.在目录parent_dir下安装目录项new_dir_entry, 写入硬盘后返回true,否则false
    if (!sync_dir_entry(parent_dir, &new_dir_entry, io_buf)) {
        printk("sync dir_entry to disk failed!\n");
        rollback_step = 3;
        goto rollback;
    }
    // 2. 将父目录inode的内容同步到硬盘
    memset(io_buf, 0, 1024);
    inode_sync(cur_part, parent_dir->inode, io_buf);
    // 3.将新创建文件的inode同步到硬盘
    memset(io_buf, 0, 1024);
    inode_sync(cur_part, new_file_node, io_buf);
    // 4.将inode_bitmap位同步到硬盘
    bitmap_sync(cur_part, inode_no, INODE_BITMAP);
    // 5.将创建的文件inode添加到open_inodes链表,便于后续使用
    list_push(&cur_part->open_inodes, &new_file_node->inode_tag);
    new_file_node->i_open_cnts = 1;

    sys_free(io_buf);
    // 将文件结构表下标安装到pcb的文件描述符表中,并返回安装的位置
    return pcb_fd_install(fd_idx);
rollback:
    switch (rollback_step) {
        case 3:
            memset(&file_table[fd_idx], 0, sizeof(struct file));
        case 2:
            sys_free(new_file_node);
        case 1:
            // 如果新文件的inode创建失败,之前位图分配的inode_no也要恢复
            bitmap_set(&cur_part->inode_bitmap, inode_no, 0);
            break;
        default:break;
    }
    sys_free(io_buf);
    return -1;
}

/** 打开编号为inode_no的inode对应的文件, 若成功则返回文件描述符, 否则返回-1 */
int32_t file_open(uint32_t inode_no, uint8_t flag) {
    int fd_idx = get_free_slot_in_global();
    if (fd_idx == -1) {
        printk("exceed max open files\n");
        return -1;
    }
    file_table[fd_idx].fd_inode = inode_open(cur_part, inode_no);
    file_table[fd_idx].fd_pos = 0;
    file_table[fd_idx].fd_flag = flag;
    bool* write_deny = &file_table[fd_idx].fd_inode->write_deny;
    // 写文件,读文件不需要判断write_deny
    if (flag & O_WRONLY || flag & O_RDWR) {
        enum intr_status old_status = intr_disable();
        if (!(*write_deny)) {
            // 当前没有其它进程或线程在写该文件
            *write_deny = true;
            intr_set_status(old_status);
        } else {
            intr_set_status(old_status);
            printk("file can`t be write now, try again later\n");
            return -1;
        }
    }
    return pcb_fd_install(fd_idx);
}

/** 关闭文件 */
int32_t file_close(struct file* file) {
    if (file == NULL) {
        return -1;
    }
    file->fd_inode->write_deny = false;
    inode_close(file->fd_inode);
    file->fd_inode = NULL; // 使文件结构可用
    return 0;
}

/***
 * 将buf中的count个字节写入file
 * @param file 文件
 * @param buf 数据源缓冲区
 * @param count 要写入的字节数量
 * @return 成功则返回写入的字节数,失败返回-1
 */
int32_t file_write(struct file* file, const void* buf, uint32_t count) {
    if ((file->fd_inode->i_size + count) > BLOCK_SIZE*140) {
        // 文件目前最大只支持512*140=71680字节
        printk("exceed max file_size 71680 bytes, write file failed!\n");
        return -1;
    }
    uint8_t* io_buf = sys_malloc(BLOCK_SIZE);
    if (io_buf == NULL) {
        printk("file_write: sys_malloc for io_buf failed!\n");
        return -1;
    }
    // 用来记录文件所有块的位置
    uint32_t* all_blocks = sys_malloc(BLOCK_SIZE + 48);
    if (all_blocks == NULL) {
        printk("file_write: sys_malloc for all_blocks failed!\n");
        return -1;
    }
    const uint8_t* src = buf; // src指向buf中待写入的数据
    uint32_t bytes_written = 0; // 记录已写入的数据大小
    uint32_t size_left = count; // 记录未写入的数据大小
    int32_t block_lba = -1; // 块地址
    uint32_t block_bitmap_idx = 0; // 记录block对应于块位图中的索引
    uint32_t sec_idx; // 用来索引扇区
    uint32_t sec_lba; // 扇区地址
    uint32_t sec_off_bytes; // 扇区内字节偏移量
    uint32_t sec_left_bytes; // 扇区内剩余字节量
    int32_t chunk_size; // 每次写入硬盘的数据块大小
    int32_t indirect_block_table; // 用来获取一级间接块表地址
    uint32_t block_idx; // 块索引
    if (file->fd_inode->i_sectors[0] == 0) {
        // 文件是第一次写则先为其分配一个块
        block_lba = block_bitmap_alloc(cur_part);
        if (block_lba == -1) {
            printk("file_write: block_bitmap_alloc failed\n");
            return -1;
        }
        file->fd_inode->i_sectors[0] = block_lba;

        // 每分配一个块就将位图同步到硬盘
        block_bitmap_idx = block_lba - cur_part->sb->data_start_lba;
        ASSERT(block_bitmap_idx != 0);
        bitmap_sync(cur_part, block_bitmap_idx, BLOCK_BITMAP);
    }
    // 写入count个字节前,该文件已经占用的块数
    uint32_t file_has_used_blocks = file->fd_inode->i_size / BLOCK_SIZE + 1;
    // 存储count字节后该文件将占用的块数
    uint32_t file_will_use_blocks = (file->fd_inode->i_size + count) / BLOCK_SIZE + 1;
    ASSERT(file_will_use_blocks <= 140);
    // 通过此增量判断是否需要分配扇区,如增量为0,表示原扇区够用
    uint32_t add_blocks = file_will_use_blocks - file_has_used_blocks;
    if (add_blocks == 0) {
        // 在同一扇区内写数据,不涉及到分配新扇区
        if (file_has_used_blocks <= 12) {
            block_idx = file_has_used_blocks - 1;
            all_blocks[block_idx] = file->fd_inode->i_sectors[block_idx];
        } else {
            // 未写入数据之前已经占用了间接块,需要将间接块地址读出来
            ASSERT(file->fd_inode->i_sectors[12] != 0);
            indirect_block_table = file->fd_inode->i_sectors[12];
            ide_read(cur_part->my_disk, indirect_block_table, all_blocks + 12, 1);
        }
    } else {
        // 若有增量,涉及到分配新扇区以及是否分配一级间接块表,分三种情况
        if (file_will_use_blocks <= 12) {
            // 第一种情况: 12个直接块够用
            // 先将有剩余空间的可继续用的扇区地址写入all_blocks
            block_idx = file_has_used_blocks - 1;
            ASSERT(file->fd_inode->i_sectors[block_idx] != 0);
            all_blocks[block_idx] = file->fd_inode->i_sectors[block_idx];
            // 再将后续要用的扇区分配好后写入all_blocks
            block_idx = file_has_used_blocks; // 指向第一个要分配的新扇区
            while (block_idx < file_will_use_blocks) {
                block_lba = block_bitmap_alloc(cur_part);
                if (block_lba == -1) {
                    printk("file_write: block_bitmap_alloc for situation 1 failed\n");
                    return -1;
                }
                // 写文件时,不应该存在块未使用但已经分配扇区的情况,当文件删除时,就会把块地址清0
                ASSERT(file->fd_inode->i_sectors[block_idx] == 0);
                file->fd_inode->i_sectors[block_idx] = all_blocks[block_idx] = block_lba;
                // 每分配一个块就将位图同步到硬盘
                block_bitmap_idx = block_lba - cur_part->sb->data_start_lba;
                bitmap_sync(cur_part, block_bitmap_idx, BLOCK_BITMAP);
                block_idx++; // 下一个分配的新扇区
            }
        } else if (file_has_used_blocks <= 12 && file_will_use_blocks > 12) {
            // 第二种情况: 旧数据在12个直接块内, 新数据将使用间接块
            // 先将有剩余空间的可继续用的扇区地址收集到all_blocks
            block_idx = file_has_used_blocks - 1;
            all_blocks[block_idx] = file->fd_inode->i_sectors[block_idx];

            // 创建一级间接块表
            block_lba = block_bitmap_alloc(cur_part);
            if (block_lba == -1) {
                printk("file_write: block_bitmap_alloc for situation 2 failed\n");
                return -1;
            }
            ASSERT(file->fd_inode->i_sectors[12] == 0);  // 确保一级间接块表未分配
            indirect_block_table = file->fd_inode->i_sectors[12] = block_lba;
            // 第一个未使用的块
            block_idx = file_has_used_blocks;
            while (block_idx < file_will_use_blocks) {
                block_lba = block_bitmap_alloc(cur_part);
                if (block_lba == -1) {
                    printk("file_write: block_bitmap_alloc for situation 2 failed\n");
                    return -1;
                }
                if (block_idx < 12) {
                    ASSERT(file->fd_inode->i_sectors[block_idx] == 0); // 确保尚未分配扇区地址
                    file->fd_inode->i_sectors[block_idx] = all_blocks[block_idx] = block_lba;
                } else {
                    // 间接块只写入到all_block数组中,待全部分配完成后一次性同步到硬盘
                    all_blocks[block_idx] = block_lba;
                }
                // 每分配一个块就将位图同步到硬盘
                block_bitmap_idx = block_lba - cur_part->sb->data_start_lba;
                bitmap_sync(cur_part, block_bitmap_idx, BLOCK_BITMAP);
                block_idx++; // 下一个新扇区
            }
            // 同步一级间接块表到硬盘
            ide_write(cur_part->my_disk, indirect_block_table, all_blocks + 12, 1);
        } else if (file_has_used_blocks > 12) {
            // 第三种情况: 新数据占用间接块
            ASSERT(file->fd_inode->i_sectors[12] != 0);
            // 获取一级间接块地址
            indirect_block_table = file->fd_inode->i_sectors[12];
            // 已经使用的间接块也将被读入all_blocks
            ide_read(cur_part->my_disk, indirect_block_table, all_blocks + 12, 1);
            // 第一个未使用的间接块
            block_idx = file_has_used_blocks;
            while (block_idx < file_will_use_blocks) {
                block_lba = block_bitmap_alloc(cur_part);
                if (block_lba == -1) {
                    printk("file_write: block_bitmap_alloc for situation 3 failed\n");
                    return -1;
                }
                all_blocks[block_idx++] = block_lba;
                // 每分配一个块就将位图同步到硬盘
                block_bitmap_idx = block_lba - cur_part->sb->data_start_lba;
                bitmap_sync(cur_part, block_bitmap_idx, BLOCK_BITMAP);
            }
            // 将间接块表同步到硬盘
            ide_write(cur_part->my_disk, indirect_block_table, all_blocks + 12, 1);
        }
    }
    bool first_write_block = true; // 含有剩余空间的扇区标识
    // 置fd_pos为文件大小-1,下面在写数据时随时更新
    file->fd_pos = file->fd_inode->i_size - 1;
    while (bytes_written < count) {
        memset(io_buf, 0, BLOCK_SIZE);
        sec_idx = file->fd_inode->i_size / BLOCK_SIZE;
        sec_lba = all_blocks[sec_idx];
        sec_off_bytes = file->fd_inode->i_size % BLOCK_SIZE;
        sec_left_bytes = BLOCK_SIZE - sec_off_bytes;

        // 判断此次写入硬盘的数据大小
        chunk_size = size_left < sec_left_bytes ? size_left : sec_left_bytes;
        if (first_write_block) {
            ide_read(cur_part->my_disk, sec_lba, io_buf, 1);
            first_write_block = false;
        }
        memcpy(io_buf + sec_off_bytes, src, chunk_size);
        ide_write(cur_part->my_disk, sec_lba, io_buf, 1);
        printk("file write at lba 0x%x\n", sec_lba);    //调试,完成后去掉

        src += chunk_size;  // 将指针推移到下个新数据
        file->fd_inode->i_size += chunk_size;
        file->fd_pos += chunk_size;
        bytes_written += chunk_size;
        size_left -= chunk_size;
    }
    // 同步inode信息到硬盘, 回收写文件时分配的空间
    inode_sync(cur_part, file->fd_inode, io_buf);
    sys_free(all_blocks);
    sys_free(io_buf);
    return bytes_written;
}

