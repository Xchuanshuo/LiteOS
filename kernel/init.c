#include "init.h"
#include "../lib/kernel/print.h"
#include "interrupt.h"
#include "../device/timer.h"
#include "memory.h"
#include "../thread/thread.h"
#include "../device/console.h"
#include "../device/keyboard.h"
#include "../userprog/tss.h"
#include "../userprog/syscall-init.h"
#include "../device/ide.h"
#include "../fs/fs.h"

void init_all() {
    put_str("init_all\n");
    idt_init();    // 初始化中断
    mem_init();	  // 初始化内存管理系统
    thread_init(); // 初始化线程相关结构
    timer_init();  // 初始化PIT
    console_init(); // 控制台初始化
    keyboard_init();  // 键盘初始化
    tss_init();  // tss初始化
    syscall_init(); // 初始化系统调用
    intr_enable();    // 后面的ide_init需要打开中断
    ide_init();	     // 初始化硬盘
    filesys_init(); // 初始化文件系统
}

