#include "init.h"
#include "../lib/kernel/print.h"
#include "interrupt.h"
#include "../device/timer.h"
#include "../device/console.h"
#include "memory.h"
#include "../thread/thread.h"

void init_all() {
    put_str("init_all\n");
    idt_init();    // 初始化中断
    mem_init();	  // 初始化内存管理系统
    thread_init(); // 初始化线程相关结构
    timer_init();  // 初始化PIT
    console_init(); // 控制台初始化
}

