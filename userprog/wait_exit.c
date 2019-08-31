#include "wait_exit.h"
#include "../kernel/global.h"
#include "../kernel/debug.h"
#include "../thread/thread.h"
#include "../lib/kernel/list.h"
#include "../lib/kernel/stdio-kernel.h"
#include "../kernel/memory.h"
#include "../lib/kernel/bitmap.h"
#include "../fs/fs.h"

/**
 * 释放用户进程资源:
 *  1.页表中对应的物理页
 *  2.虚拟内存池占用的物理页
 *  3.关闭打开的文件
 * @param release_thread
 */
static void release_prog_resource(struct task_struct* release_thread) {
    uint32_t* pgdir_vaddr = release_thread->pgdir;
    uint16_t user_pde_nr = 768, pde_idx = 0;
    uint16_t user_pte_nr = 1024, pte_idx = 0;
    uint32_t pde = 0, pte = 0;;
    uint32_t *v_pde_ptr = NULL, *v_pte_ptr = NULL;
    // 用来记录pde中第0个pte的位置
    uint32_t* first_pte_vaddr_in_pde = NULL;
    uint32_t pg_phy_addr = 0;

    // 1.回收页表中用户空间的页框
    while (pde_idx < user_pde_nr) {
        v_pde_ptr = pgdir_vaddr + pde_idx;
        pde = *v_pde_ptr;
        // 如果页目录项p位为1,表示该页目录下可能有页表项
        if (pde & 0x00000001) {
            // 一个页表表示的内存容量是4M,即0x400000
            first_pte_vaddr_in_pde = pte_ptr(pde_idx * 0x400000);
            pte_idx = 0;
            while (pte_idx < user_pte_nr) {
                v_pte_ptr = first_pte_vaddr_in_pde + pte_idx;
                pte = *v_pte_ptr;
                if (pte & 0x00000001) {
                    // 回收页表项(pte)对应的物理页框
                    pg_phy_addr = pte & 0xfffff000;
                    free_a_phy_page(pg_phy_addr);
                }
                pte_idx++;
            }
            // 回收页目录项(pde,也就是指向的页表本身)对应的物理页框
            pg_phy_addr = pde & 0xfffff000;
            free_a_phy_page(pg_phy_addr);
        }
        pde_idx++;
    }
    // 2.回收用户虚拟地址池所占用的物理内存
    uint32_t bitmap_pg_cnt = (release_thread->userprog_vaddr.vaddr_bitmap.btmp_bytes_len) / PG_SIZE;
    uint8_t* user_vaddr_pool_bitmap = release_thread->userprog_vaddr.vaddr_bitmap.bits;
    mfree_page(PF_KERNEL, user_vaddr_pool_bitmap, bitmap_pg_cnt);

    // 3.关闭进程打开的文件
    uint32_t fd_idx = 3;
    while (fd_idx < MAX_FILES_OPEN_PER_PROC) {
        if (release_thread->fd_table[fd_idx] != -1) {
            sys_close(fd_idx);
        }
        fd_idx++;
    }
}

/** list_traversal的回调函数,
 *  查找pelem的parent_pid是否是ppid,成功返回true,失败则返回false */
static bool find_child(struct list_elem* pelem, int32_t ppid) {
    struct task_struct* pthread = elem2entry(struct task_struct, all_list_tag, pelem);
    if (pthread->parent_pid == ppid) {
        return true;
    }
    return false;
}

/** list_traversal的回调函数, 查找状态为TASK_HANGING的任务 */
static bool find_hanging_child(struct list_elem* pelem, int32_t ppid) {
    struct task_struct* pthread = elem2entry(struct task_struct, all_list_tag, pelem);
    if (pthread->parent_pid == ppid && pthread->status ==TASK_HANGING) {
        return true;
    }
    return false;
}

/** list_traversal的回调函数, 将一个子进程过继给init */
static bool init_adopt_a_child(struct list_elem* pelem, int32_t pid) {
    struct task_struct* pthread = elem2entry(struct task_struct, all_list_tag, pelem);
    if (pthread->parent_pid == pid) {
        pthread->parent_pid = 1;
    }
    return false;
}

/** 等待子进程调用exit,将子进程的退出状态保存到status指向的变量.
 *  成功则返回子进程的pid,失败则返回-1 */
pid_t sys_wait(int32_t* status) {
    struct task_struct* parent_thread = running_thread();
    while (1) {
        // 优先处理已经是挂起状态的任务
        struct list_elem* child_elem = list_traversal(&thread_all_list,
                find_hanging_child, parent_thread->pid);
        // 若有子进程挂起
        if (child_elem != NULL) {
            struct task_struct* child_thread =
                    elem2entry(struct task_struct, all_list_tag, child_elem);
            *status = child_thread->exit_status;
            // thread_exit之后,pcb会回收,因此提取获取pid
            uint16_t child_pid = child_thread->pid;
            // 从就绪队列中移除, 并回收页表和pcb
            thread_exit(child_thread, false);
            return child_pid;
        }
        child_elem = list_traversal(&thread_all_list, find_child, parent_thread->pid);
        if (child_elem == NULL) { // 若没有子进程 出错返回
            return -1;
        } else {
            // 若子进程还未运行完,即未调用exit,则将自己挂起,直到子进程执行exit时将自己唤醒
            thread_block(TASK_WAITING);
        }
    }
}

/** 子进程用来结束自己时调用 */
void sys_exit(int32_t status) {
    struct task_struct* child_thread = running_thread();
    child_thread->exit_status = status;
    if (child_thread->parent_pid == -1) {
        PANIC("sys_exit: child_thread->parent_pid is -1");
    }
    // 将进程child_thread所有的子进程都过继给init进程
    list_traversal(&thread_all_list, init_adopt_a_child, child_thread->pid);

    // 回收进程child_thread的资源
    release_prog_resource(child_thread);

    // 如果父进程正在等待子进程,则将父进程唤醒
    struct task_struct* parent_thread = pid2thread(child_thread->parent_pid);
    if (parent_thread->status == TASK_WAITING) {
        thread_unblock(parent_thread);
    }
    // 将自己挂起,等待父进程获取其status,并回收其pcb
    thread_block(TASK_HANGING);
}