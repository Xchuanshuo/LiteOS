#ifndef __FS_DIR_H
#define __FS_DIR_H
#include "../lib/stdint.h"
#include "inode.h"
#include "../device/ide.h"
#include "../kernel/global.h"

#define MAX_FILE_NAME_LEN 16  // 最大文件名长度

/** 目录结构 */
struct dir {
    struct inode* inode;  // 目录对应的inode
    uint32_t dir_ops;     // 记录目录内的偏移
    uint8_t dir_buf[512]; // 目录的数据缓存
};

/** 目录项结构 */
struct dir_entry {
    char filename[MAX_FILE_NAME_LEN]; // 普通文件或目录名称
    uint32_t i_no;                    // 普通文件或目录对应的inode编号
    enum file_types f_type;           // 文件类型
};
#endif
