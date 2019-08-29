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
#include "../shell/shell.h"
#include "../lib/user/assert.h"

void init(void);

int main(void) {
    put_str("I am kernel\n");
    init_all();
    cls_screen();
    console_put_str("[rabbit@localhost /]$ ");
    while(1);
    return 0;
}

/* init进程 */
void init(void) {
    uint32_t ret_pid = fork();
    if(ret_pid) {  // 父进程
        while(1);
    } else {	  // 子进程
        my_shell();
    }
    panic("init: should not be here");
}