#ifndef __FS_FS_H
#define __FS_FS_H
#include "../lib/stdint.h"
#include "../device/ide.h"

#define MAX_FILES_PER_PART 4096 // 每个分区所支持创建的最大文件数
#define BITS_PER_SECTOR 4096    // 每扇区的位数
#define SECTOR_SIZE 512         // 扇区字节大小
#define BLOCK_SIZE SECTOR_SIZE  // 块字节大小

/** 文件类型 */
enum file_types {
    FILE_UNKNOWN,   // 不支持的文件类型
    FILE_REGULAR,   // 普通文件
    FILE_DIRECTORY  // 目录
};
void filesys_init(void);
#endif
