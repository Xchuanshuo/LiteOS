#include "../lib/kernel/print.h"
#include "init.h"
#include "../thread/thread.h"
#include "interrupt.h"
#include "../device/console.h"
#include "../device/keyboard.h"
#include "../device/ioqueue.h"
#include "../userprog/process.h"
#include "../userprog/syscall-init.h"
#include "../lib/user/syscall.h"
#include "../lib/stdio.h"
#include "../fs/fs.h"
#include "../lib/string.h"
#include "../fs/dir.h"

void init(void);

int main(void) {
    put_str("I am kernel\n");
    init_all();
    while(1);
    return 0;
}

/** init进程 */
void init(void) {
    uint32_t ret_pid = (uint32_t) fork();
    if(ret_pid) {
        printf("i am father, my pid is %d, child pid is %d\n", getpid(), ret_pid);
    } else {
        printf("i am child, my pid is %d, ret pid is %d\n", getpid(), ret_pid);
    }
    while(1);
}