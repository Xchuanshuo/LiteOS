#include "exec.h"
#include "../thread/thread.h"
#include "../lib/kernel/stdio-kernel.h"
#include "../fs/fs.h"
#include "../lib/string.h"
#include "../kernel/global.h"
#include "../kernel/memory.h"

extern void intr_exit(void);
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/** 32位elf头 */
struct Elf32_Ehdr {
    unsigned char e_ident[16]; // elf字符等信息,作为elf文件的标识
    Elf32_Half e_type; // elf目标文件的类型
    Elf32_Half e_machine; // 目标文件的体系结构类型
    Elf32_Word e_version; // 版本信息
    Elf32_Addr e_entry; // 操作系统运行该程序时的入口地址
    Elf32_Off e_phoff; // 程序头表在文件内的字节偏移
    Elf32_Off e_shoff; // 节头表在文件内的字节偏移
    Elf32_Word e_flags; // 与处理器相关的标志
    Elf32_Half e_ehsize; // elf header的字节大小
    Elf32_Half e_phentsize; // 程序头表中每个条目的字节大小
    Elf32_Half e_phnum; // 程序头表中条目的数量
    Elf32_Half e_shentsize; // 字节头表中条目的大小
    Elf32_Half e_shnum; // 字节头表中条目的数量
    Elf32_Half e_shstrndx; // string name table在节头表中的索引index
};

/** 程序头表Program header.就是段描述头 */
struct Elf32_Phdr {
    Elf32_Word p_type;   // 程序中该段的类型
    Elf32_Off p_offset;  // 本段在文件内的起始偏移地址
    Elf32_Addr p_vaddr;  // 本段在内存中的起始虚拟地址
    Elf32_Addr p_paddr;  // 用于与物理地址相关的系统中(这里保留)
    Elf32_Word p_filesz; // 本段在文件中的大小
    Elf32_Word p_memsz;  // 本段在内存中的大小
    Elf32_Word p_flags;  // 指明与本段相关的标志,如读写权限,与处理器、程序系统有关
    Elf32_Word p_align;  // 本段在文件和内存中的对齐方式
};

/** 段类型 */
enum segment_type {
    PT_NULL,    // 忽略
    PT_LOAD,    // 可加载程序段
    PT_DYNAMIC, // 动态加载信息
    PT_INTERP,  // 动态加载器名称
    PT_NOTE,    // 一些辅助信息
    PT_SHLTB,   // 保留
    PT_PHDR     // 程序头表
};

/**
 * 将文件描述符fd指向的文件中,偏移offset,大小filesz
 * 的段加载到虚拟地址为vaddr的内存
 * @param fd 文件描述符
 * @param offset 相对于文件起始位置的偏移量
 * @param filesz 该段在文件中的大小
 * @param vaddr 虚拟地址
 * @return 成功返回true
 */
static bool segment_load(int32_t fd, uint32_t offset,
                         uint32_t filesz, uint32_t vaddr) {
    // vaddr地址所在页框的位置
    uint32_t vaddr_first_page = vaddr & 0xfffff000;
    // 加载到内存后,文件在第一个页框内的字节大小
    uint32_t size_in_first_page = PG_SIZE - (vaddr & 0x00000fff);
    uint32_t occupy_pages = 0;
    // 若一个页容不下该段
    if (filesz > size_in_first_page) {
        uint32_t left_size = filesz - size_in_first_page;
        occupy_pages = DIV_ROUND_UP(left_size, PG_SIZE) + 1;
    } else {
        occupy_pages = 1;
    }
    // 为进程分配内存
    uint32_t page_idx = 0;
    uint32_t vaddr_page = vaddr_first_page;
    while (page_idx < occupy_pages) {
        uint32_t* pde = pde_ptr(vaddr_page);
        uint32_t* pte = pte_ptr(vaddr_page);

        // 如果pde不存在,或pte不存在就分配内存,pde的判断要在pte
        // 之前,否则pde若不存在就会导致判断pte时缺页异常
        if (!(*pde & 0x00000001) || !(*pte & 0x00000001)) {
            if (get_a_page(PF_USER, vaddr_page) == NULL) {
                return false;
            }
        } // 如果原进程的页表已经分配了,利用现有的物理页,直接覆盖进程体
        vaddr_page += PG_SIZE;
        page_idx++;
    }
    sys_lseek(fd, offset, SEEK_SET);
    sys_read(fd, (void*) vaddr, filesz);
    return true;
}

/** 从文件系统上加载用户程序pathname,成功则返回程序的起始地址,否则返回-1 */
static int32_t load(const char* pathname) {
    int32_t ret = -1;
    struct Elf32_Ehdr elf_header;
    struct Elf32_Phdr prog_header;
    memset(&elf_header, 0, sizeof(struct Elf32_Ehdr));

    int32_t fd = sys_open(pathname, O_RDONLY);
    if (fd == -1) return -1;
    if (sys_read(fd, &elf_header, sizeof(struct Elf32_Ehdr))
            != sizeof(struct Elf32_Ehdr)) {
        ret = -1;
        goto done;
    }
    // 校验elf头
    /* 校验elf头 */
    if (memcmp(elf_header.e_ident, "\177ELF\1\1\1", 7) != 0
        || elf_header.e_type != 2
        || elf_header.e_machine != 3
        || elf_header.e_version != 1
        || elf_header.e_phnum > 1024
        || elf_header.e_phentsize != sizeof(struct Elf32_Phdr)) {
        ret = -1;
        goto done;
    }
    // 程序头表在文件内的偏移量
    Elf32_Off prog_header_offset = elf_header.e_phoff;
    // 程序头表中每个条目的字节大小
    Elf32_Half prog_header_size = elf_header.e_phentsize;
    // 遍历所有程序头
    uint32_t prog_idx = 0;
    while (prog_idx < elf_header.e_phnum) {
        memset(&prog_header, 0, prog_header_size);
        // 将文件的指针定位到程序头
        sys_lseek(fd, prog_header_offset, SEEK_SET);
        // 只获取程序头
        if (sys_read(fd, &prog_header, prog_header_size) != prog_header_size) {
            ret = -1;
            goto done;
        }
        // 如果是可加载段就调用segment_load加载到内存
        if (PT_LOAD == prog_header.p_type) {
            if (!segment_load(fd, prog_header.p_offset, prog_header.p_filesz, prog_header.p_vaddr)) {
                ret = -1;
                goto done;
            }
        }
        // 更新下一个程序头的偏移
        prog_header_offset += elf_header.e_phentsize;
        prog_idx++;
    }
    ret = elf_header.e_entry;
done:
    sys_close(fd);
    return ret;
}

/* 用path指向的程序替换当前进程 */
int32_t sys_execv(const char* path, const char* argv[]) {
   uint32_t argc = 0;
    while (argv[argc]) argc++;
    int32_t entry_point = load(path); // 加载程序文件
    if (entry_point == -1) return -1; // 加载失败返回-1

    struct task_struct* cur = running_thread();
    // 修改进程名
    memcpy(cur->name, path, TASK_NAME_LEN);
    struct intr_stack* intr_0_stack = (struct intr_stack*) ((uint32_t)cur
            + PG_SIZE - sizeof(struct intr_stack));
    // 参数传递给用户进程
    intr_0_stack->ebx = (uint32_t) argv;
    intr_0_stack->ecx = argc;
    intr_0_stack->eip = (void*) entry_point;
    // 使用户进程的栈地址为最高用户空间地址
    intr_0_stack->esp = (void*) 0xc0000000;

    // exec不同于fork,为使新进程更快被执行,直接从中断返回
    asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (intr_0_stack) : "memory");
    return 0;
}
