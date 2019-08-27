#include "fs.h"
#include "super_block.h"
#include "inode.h"
#include "dir.h"
#include "../lib/stdint.h"
#include "../lib/kernel/stdio-kernel.h"
#include "../lib/kernel/list.h"
#include "../lib/string.h"
#include "../device/ide.h"
#include "../kernel/global.h"
#include "../kernel/debug.h"
#include "../kernel/memory.h"
#include "file.h"
#include "../device/console.h"

struct partition* cur_part;   // 默认情况下操作的分区

/** 分区挂载,在分区链表中找到名为part_name的分区,并将其指针赋值给cur_part */
static bool mount_partition(struct list_elem* pelem, int arg) {
    char* part_name = (char*) arg;
    struct partition* part = elem2entry(struct partition, part_tag, pelem);
    if (!strcmp(part->name, part_name)) {
        cur_part = part;
        struct disk* hd = cur_part->my_disk;
        // sb_buf用来存储从硬盘上读入的超级块
        struct super_block* sb_buf = (struct super_block*) sys_malloc (SECTOR_SIZE);
        // 在内存中创建分区cur_part的超级块
        cur_part->sb = (struct super_block*)sys_malloc(sizeof(struct super_block));

        memset(sb_buf, 0, SECTOR_SIZE);
        // 读入超级块
        ide_read(hd, cur_part->start_lba + 1,  sb_buf, 1);
        // 把sb_buf中超级块的信息复制到分区的超级块sb中
        memcpy(cur_part->sb, sb_buf, sizeof(struct super_block));

        /** 将硬盘的块位图读入内存 */
        cur_part->block_bitmap.bits = (uint8_t*) sys_malloc(sb_buf->block_bitmap_sects * SECTOR_SIZE);
        if (cur_part->block_bitmap.bits == NULL) {
            PANIC("alloc memory failed!");
        }
        cur_part->block_bitmap.btmp_bytes_len = sb_buf->block_bitmap_sects * SECTOR_SIZE;
        // 从硬盘读入块位图到分区的block_bitmap.bits
        ide_read(hd, sb_buf->block_bitmap_lba, cur_part->block_bitmap.bits, sb_buf->block_bitmap_sects);

        /** 将硬盘上的inode位图读入到内存 */
        cur_part->inode_bitmap.bits = (uint8_t*)sys_malloc(sb_buf->inode_bitmap_sects * SECTOR_SIZE);
        if (cur_part->inode_bitmap.bits == NULL) {
            PANIC("alloc memory failed!");
        }
        cur_part->inode_bitmap.btmp_bytes_len = sb_buf->inode_bitmap_sects * SECTOR_SIZE;
        // 从硬盘读入inode位图到分区的inode_bitmap.bits
        ide_read(hd, sb_buf->inode_bitmap_lba, cur_part->inode_bitmap.bits, sb_buf->inode_bitmap_sects);

        list_init(&cur_part->open_inodes);
        printk("mount %s done!\n", part->name);
        // 只有返回true是list_traversal才会停止遍历,减少后续无意义的遍历
        return true;
    }
    return false;
}

/** 格式化分区,也就是初始化分区的元信息,创建文件系统 */
static void partition_format(struct partition* part) {
    uint32_t boot_sector_sects = 1;
    uint32_t super_block_sects = 1;
    // inode位图占用的扇区数,最多支持4096个文件
    uint32_t inode_bitmap_sects = DIV_ROUND_UP(MAX_FILES_PER_PART, BITS_PER_SECTOR);
    // inode表需要占用的扇区数
    uint32_t inode_table_sects = DIV_ROUND_UP(((sizeof(struct inode) * MAX_FILES_PER_PART)), SECTOR_SIZE);
    // 已使用的扇区数
    uint32_t used_sects = boot_sector_sects + super_block_sects + inode_bitmap_sects + inode_table_sects;
    uint32_t free_sects = part->sec_cnt - used_sects; // 空闲的扇区数

    /** 简单处理块位图占据的扇区数 */
    uint32_t block_bitmap_sects;
    block_bitmap_sects = DIV_ROUND_UP(free_sects, BITS_PER_SECTOR);
    // 位图中位的长度,也就是可用块的数量
    uint32_t block_bitmap_bit_len = free_sects - block_bitmap_sects;
    // 块位图占用的扇区数
    block_bitmap_sects = DIV_ROUND_UP(block_bitmap_bit_len, BITS_PER_SECTOR);

    /** 超级块初始化 */
    struct super_block sb;
    sb.magic = 0x19590318;
    sb.sec_cnt = part->sec_cnt;
    sb.inode_cnt = MAX_FILES_PER_PART;
    sb.part_lba_base = part->start_lba;
    // 第0块是引导块,第1块是超级块
    sb.block_bitmap_lba = sb.part_lba_base + 2;
    sb.block_bitmap_sects = block_bitmap_sects;

    sb.inode_bitmap_lba = sb.block_bitmap_lba + sb.block_bitmap_sects;
    sb.inode_bitmap_sects = inode_bitmap_sects;

    sb.inode_table_lba = sb.inode_bitmap_lba +  sb.inode_bitmap_sects;
    sb.inode_table_sects = inode_table_sects;

    sb.data_start_lba = sb.inode_table_lba + sb.inode_table_sects;
    sb.root_inode_no = 0;
    sb.dir_entry_size = sizeof(struct dir_entry);

    printk("%s info:\n", part->name);
    printk("   magic:0x%x\n   part_lba_base:0x%x\n   all_sectors:0x%x\n   "
           "inode_cnt:0x%x\n   block_bitmap_lba:0x%x\n   block_bitmap_sectors:0x%x\n   "
           "inode_bitmap_lba:0x%x\n   inode_bitmap_sectors:0x%x\n   inode_table_lba:0x%x\n"
           "   inode_table_sectors:0x%x\n   data_start_lba:0x%x\n", sb.magic, sb.part_lba_base,
           sb.sec_cnt, sb.inode_cnt, sb.block_bitmap_lba, sb.block_bitmap_sects,
           sb.inode_bitmap_lba, sb.inode_bitmap_sects, sb.inode_table_lba,
           sb.inode_table_sects, sb.data_start_lba);

    struct disk* hd = part->my_disk;

    /*************************************
     * 1.将超级块写入本分区的1扇区(跨过引导扇区)*
     *************************************/
    ide_write(hd, part->start_lba + 1, &sb, 1);
    printk("   super_block_lba:0x%x\n", part->start_lba + 1);

    // 找出数据量最大的元信息,用其尺寸做存储缓冲区
    uint32_t buf_size = sb.block_bitmap_sects >= sb.inode_bitmap_sects
            ? sb.block_bitmap_sects : sb.inode_bitmap_sects;
    buf_size = (buf_size >= sb.inode_table_sects ? buf_size : sb.inode_table_sects) * SECTOR_SIZE;
    uint8_t* buf = (uint8_t*) sys_malloc(buf_size);

    /*****************************************
     * 2.将位图块初始化并写入sb.block_bitmap_lba *
     *****************************************/
    buf[0] |= 0x01;  // 第0个块预留给根目录,位图先占位
    uint32_t block_bitmap_last_byte = block_bitmap_bit_len / 8;
    uint8_t block_bitmap_last_bit = (uint8_t) (block_bitmap_bit_len % 8);
    // last_size是位图所在的最后一个扇区中,不足一扇区的其余部分
    uint32_t last_size = SECTOR_SIZE - (block_bitmap_last_byte % SECTOR_SIZE);

    // 1 先将位图最后一字节到其所在的扇区的结束位置全置为1,
    memset(&buf[block_bitmap_last_byte], 0xff, last_size);

     // 2 再将上一步中覆盖的最后一字节内的有效位重新置0
    uint8_t bit_idx = 0;
    while (bit_idx <= block_bitmap_last_bit) {
        buf[block_bitmap_last_byte] &= ~(1 << bit_idx++);
    }
    ide_write(hd, sb.block_bitmap_lba, buf, sb.block_bitmap_sects);

    /**********************************************
     * 3.将inode位图块初始化并写入sb.inode_bitmap_lba *
     **********************************************/
    // 先清空缓冲区
    memset(buf, 0, buf_size);
    buf[0] |= 0x01; // 第0个inode分给了根目录
    // inode_table中4096个inode,位图inode_bitmap刚好占用一个扇区
    ide_write(hd, sb.inode_bitmap_sects, buf, sb.inode_bitmap_sects);

    /********************************************
     * 4.将inode数组初始化并写入sb.inode_table_lba *
     *******************************************/
    memset(buf, 0, buf_size);
    struct inode* i = (struct inode*) buf;
    i->i_size = sb.dir_entry_size * 2; // .和..
    i->i_no = 0; // 根目录占inode数组中第0个node
    i->i_sectors[0] = sb.data_start_lba;
    ide_write(hd, sb.inode_table_lba, buf, sb.inode_table_sects);

    /***************************************
     * 5.将根目录初始化并写入sb.data_start_lba *
     ***************************************/
    // 写入根目录的两个目录项 .和..
    memset(buf, 0, buf_size);
    struct dir_entry* p_de = (struct dir_entry*) buf;
    // 初始化当前目录
    memcpy(p_de->filename, ".", 1);
    p_de->i_no = 0;
    p_de->f_type = FT_DIRECTORY;
    p_de++;

    // 初始化当前父目录
    memcpy(p_de->filename, "..", 2);
    p_de->i_no = 0; // 根目录的父目录依然是根目录自己
    p_de->f_type = FT_DIRECTORY;

    // 往根目录所在的数据块里面写入根目录的目录项
    ide_write(hd, sb.data_start_lba, buf, 1);
    printk("   root_dir_lba:0x%x\n", sb.data_start_lba);
    printk("%s format done\n", part->name);
    sys_free(buf);
}

/** 将最上层路径名称解析出来 */
static char* path_parse(char* pathname, char* name_store) {
    if (pathname[0] == '/') {
        // 跳过前面的 '/'
        while (*(++pathname) == '/');
    }
    // 开始一般的路径解析
    while (*pathname != '/' && *pathname != 0) {
        *name_store++ = *pathname++;
    }
    if (pathname[0] == 0) {
        return NULL;
    }
    return pathname;
}

/** 返回路径深度 如/a/b/c,深度为3 */
int32_t path_depth_cnt(char* pathname) {
    ASSERT(pathname != NULL);
    char* p = pathname;
    char name[MAX_FILE_NAME_LEN]; // 用于path_parse的参数做路径解析
    uint32_t depth = 0;
    // 解析路径,从中拆分出各级名称
    p = path_parse(p, name);
    while (name[0]) {
        depth++;
        memset(name, 0, MAX_FILE_NAME_LEN);
        if (p) {
            p = path_parse(p, name);
        }
    }
    return depth;
}

/** 搜索文件pathname,若找到则返回其inode号,否则返回-1 */
static int search_file(const char* pathname, struct path_search_record* searched_record) {
    // 如果待查找的是根目录,为避免下面无用的查找,直接返回已知根目录信息
    if (!strcmp(pathname, "/") || !strcmp(pathname, "/.") ||  !strcmp(pathname, "/..")) {
        searched_record->parent_dir = &root_dir;
        searched_record->file_type = FT_DIRECTORY;
        searched_record->searched_path[0] = 0; // 搜索路径置空
        return 0;
    }

    uint32_t path_len = strlen(pathname);
    // 保证pathname至少是这样的路径/x,且小于最大长度
    ASSERT(pathname[0] == '/' && path_len > 1 && path_len < MAX_PATH_LEN);
    char* sub_path = (char*) pathname;
    struct dir* parent_dir = &root_dir;
    struct dir_entry dir_e;
    // 记录路径解析出来的各级名称
    char name[MAX_FILE_NAME_LEN] = {0};

    searched_record->parent_dir = parent_dir;
    searched_record->file_type = FT_UNKNOWN;
    uint32_t parent_inode_no = 0; // 父目录的inode号

    sub_path = path_parse(sub_path, name);
    while (name[0]) {
        // 记录查找过的路径,但不能超过searched_path的长度512字节
        ASSERT(strlen(searched_record->searched_path) < 512);
        // 记录已存在的父目录
        strcat(searched_record->searched_path, "/");
        strcat(searched_record->searched_path, name);
        // 在所给的目录中查找文件或目录
        if (search_dir_entry(cur_part, parent_dir, name, &dir_e)) {
            memset(name, 0, MAX_FILE_NAME_LEN);
            if (sub_path) {
                sub_path = path_parse(sub_path, name);
            }
            if (dir_e.f_type == FT_DIRECTORY) {
                parent_inode_no = parent_dir->inode->i_no;
                dir_close(parent_dir);
                parent_dir = dir_open(cur_part, dir_e.i_no); // 更新父目录
                searched_record->parent_dir = parent_dir;
                continue;
            } else if (dir_e.f_type == FT_REGULAR) {
                searched_record->file_type = FT_REGULAR;
                return dir_e.i_no;
            }
        } else {
            // 找不到目录项时,留着parent_dir不关闭，
            // 若是创建新文件需要在parent_dir中创建
            return -1;
        }
    }
    // 执行到此,必然是遍历了完整路径并且查找的文件或目录只有同名目录存在
    dir_close(searched_record->parent_dir);
    // 保存被查找的直接父目录
    searched_record->parent_dir = dir_open(cur_part, parent_inode_no);
    searched_record->file_type = FT_DIRECTORY;
    return dir_e.i_no;
}

/** 打开或创建文件成功后,返回文件描述符,否则返回-1 */
int32_t sys_open(const char* pathname, uint8_t flags) {
    // 对目录要用dir_open,这里只有open文件
    if (pathname[strlen(pathname) - 1] == '/') {
        printk("cant't open a directory %s\n", pathname);
        return -1;
    }
    ASSERT(flags <= 7);
    int32_t fd = -1;

    struct path_search_record searched_record;
    memset(&searched_record, 0, sizeof(struct path_search_record));

    // 记录目录深度,帮助判断中间某个目录不存在的情况
    uint32_t pathname_depth = path_depth_cnt((char*) pathname);
    // 先检查文件是否存在
    int inode_no = search_file(pathname, &searched_record);
    bool found = inode_no != -1 ? true : false;
    if (searched_record.file_type == FT_DIRECTORY) {
        printk("can`t open a direcotry with open(), use opendir() to instead\n");
        dir_close(searched_record.parent_dir);
        return -1;
    }
    uint32_t path_searched_depth = path_depth_cnt(searched_record.searched_path);
    if (pathname_depth != path_searched_depth) {
        // 说明没有访问到全部路径,某个中间目录是不存在的
        printk("cannot access %s: Not a directory, subpath %s is`t exist\n",
               pathname, searched_record.searched_path);
        dir_close(searched_record.parent_dir);
        return -1;
    }
    // 若是在最后一个路径上没找到,并且不是要创建文件,直接返回-1
    if (!found && !(flags & O_CREATE)) {
        printk("in path %s, file %s is`t exist\n",
               searched_record.searched_path,
               (strrchr(searched_record.searched_path, '/') + 1));
        dir_close(searched_record.parent_dir);
        return -1;
    } else if (found && flags & O_CREATE) {
        // 若要创建的文件已经存在
        printk("%s has already exist!\n", pathname);
        dir_close(searched_record.parent_dir);
        return -1;
    }
    switch (flags & O_CREATE) {
        case O_CREATE:
            printk("creating file\n");
            fd = file_create(searched_record.parent_dir, (strrchr(pathname, '/') + 1), flags);
            dir_close(searched_record.parent_dir);
            break;
        default:
            // 其余情况为打开文件
            fd = file_open(inode_no, flags);
    }
    // 返回任务pcb->fd_table数组中的元素下标
    return fd;
}

/** 将文件描述符转化为文件表的下标 */
static uint32_t fd_local2global(uint32_t local_fd) {
    struct task_struct* cur = running_thread();
    int32_t global_fd = cur->fd_table[local_fd];
    ASSERT(global_fd >= 0 && global_fd < MAX_FILE_OPEN);
    return (uint32_t) global_fd;
}

/** 将buf中连续count个字节写入文件描述符fd,成功则返回写入的字节数,失败返回-1 */
int32_t sys_write(int32_t fd, const void* buf, uint32_t count) {
    if (fd < 0) {
        printk("sys_write: fd error!\n");
        return -1;
    }
    if (fd == stdout_no) {
        char tmp_buf[1024] = {0};
        memcpy(tmp_buf, buf, count);
        console_put_str(tmp_buf);
        return count;
    }
    uint32_t _fd = fd_local2global(fd);
    struct file* wr_file = &file_table[_fd];
    if (wr_file->fd_flag & O_WRONLY || wr_file->fd_flag & O_RDWR) {
        uint32_t bytes_written = file_write(wr_file, buf, count);
        return bytes_written;
    } else {
        console_put_str("sys_write: not allowed to write file without flag O_RDWR or O_WRONLY!\n");
        return -1;
    }
}

/** 关闭进程或线程的文件描述符fd指向的文件, 成功返回0, 否则返回-1 */
int32_t sys_close(int32_t fd) {
    int32_t ret = -1;
    if (fd > 2) {
        uint32_t _fd = fd_local2global(fd);
        ret = file_close(&file_table[_fd]);
        running_thread()->fd_table[fd] = -1; // 使该文件描述可用
    }
    return ret;
}

/** 在磁盘上搜索文件系统,若没有则格式化分区创建文件系统 */
void filesys_init() {
    uint8_t channel_no = 0, dev_no, part_idx = 0;
    // sb_buf用来存储从硬盘上读入的超级块
    struct super_block* sb_buf = (struct super_block*)sys_malloc(SECTOR_SIZE);

    if (sb_buf == NULL) {
        PANIC("alloc memory failed!");
    }
    printk("searching filesystem......\n");
    while (channel_no < channel_cnt) {
        dev_no = 0;
        while (dev_no < 2) {
            if (dev_no == 0) { // 跨过裸盘hd60M.img
                dev_no++;
                continue;
            }
            struct disk* hd = &channels[channel_no].devices[dev_no];
            struct partition* part = hd->prim_parts;
            while (part_idx < 12) { // 4个主分区+8个逻辑分区
                if (part_idx == 4) { // 开始处理逻辑分区
                    part = hd->logic_parts;
                }

                if (part->sec_cnt != 0) {
                    memset(sb_buf, 0, SECTOR_SIZE);
                    // 读出分区的超级块,根据魔数是否正确来判断是否存在文件系统
                    ide_read(hd, part->start_lba + 1, sb_buf, 1);
                    // 只支持自己的文件系统 其它的不能识别 直接格式化成自己的
                    if (sb_buf->magic == 0x19590318) {
                        printk("%s has filesystem\n", part->name);
                        printk("root: 0x%x\n", sb_buf->data_start_lba);
                    } else {
                        printk("formatting %s`s partition %s......\n", hd->name, part->name);
                        partition_format(part);
                    }
                }
                part_idx++;
                part++; // 下一分区
            }
            dev_no++; // 下一磁盘
        }
        channel_no++; // 下一通道
    }
    sys_free(sb_buf);

    // 确定默认操作的分区
    char default_part[8] = "sdb1";
    // 挂载分区
    list_traversal(&partition_list, mount_partition, (int) default_part);
    // 将当前分区的根目录打开
    open_root_dir(cur_part);
    // 初始化文件表
    uint32_t fd_idx = 0;
    while (fd_idx < MAX_FILE_OPEN) {
        file_table[fd_idx++].fd_inode = NULL;
    }
}

