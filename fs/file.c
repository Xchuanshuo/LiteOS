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

