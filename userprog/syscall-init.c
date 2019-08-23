#include "syscall-init.h"
#include "../lib/user/syscall.h"
#include "../lib/stdint.h"
#include "../lib/kernel/print.h"
#include "../thread/thread.h"

#define syscall_nr 32
typedef void* syscall;
syscall syscall_table[syscall_nr];

/** 返回当前任务额pid */
uint32_t sys_getpid(void) {
    return running_thread()->pid;
}

void syscall_init(void) {
    put_str("syscall_init start\n");
    syscall_table[SYS_GETPID] = sys_getpid;
    put_str("syscall_init done\n");
}