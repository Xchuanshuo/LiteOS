#ifndef __FS_INODE_H
#define __FS_INODE_H
#include "../lib/stdint.h"
#include "../lib/kernel/list.h"

/** inode结构 */
struct inode {
    uint32_t i_no;  // inode编号
    // 当inode是文件时,i_size是指文件大小;
    // 当inode是目录时,i_size是目录下所有页目录项大小之和
    uint32_t i_size;

    uint32_t i_open_cnts; // 记录此文件被打开的次数
    bool write_deny;      // 写文件不能并行,进程写文件时进行此标识
    // i_sectors[0-11]是直接块,i_sectors[12]用来存储一级间接块指针
    uint32_t i_sectors[13];
    struct list_elem inode_tag;
};

#endif
