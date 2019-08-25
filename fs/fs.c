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

struct partition* cur_part;   // 默认情况下操作的分区

/** 在分区链表中找到名为part_name的分区,并将其指针赋值给cur_part */
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
    p_de->f_type = FILE_DIRECTORY;
    p_de++;

    // 初始化当前父目录
    memcpy(p_de->filename, "..", 2);
    p_de->i_no = 0; // 根目录的父目录依然是根目录自己
    p_de->f_type = FILE_DIRECTORY;

    // 往根目录所在的数据块里面写入根目录的目录项
    ide_write(hd, sb.data_start_lba, buf, 1);
    printk("   root_dir_lba:0x%x\n", sb.data_start_lba);
    printk("%s format done\n", part->name);
    sys_free(buf);
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
    list_traversal(&partition_list, mount_partition, (int) default_part);
}
