#include "timer.h"
#include "../lib/kernel/io.h"
#include "../lib/kernel/print.h"
#include "../kernel/interrupt.h"
#include "../kernel/debug.h"
#include "../thread/thread.h"

#define IRQ0_FREQUENCY	   100
#define INPUT_FREQUENCY	   1193180
#define COUNTER0_VALUE     (INPUT_FREQUENCY / IRQ0_FREQUENCY)
#define CONTRER0_PORT	   0x40
#define COUNTER0_NO	       0
#define COUNTER_MODE	   2
#define READ_WRITE_LATCH   3
#define PIT_CONTROL_PORT   0x43

uint32_t ticks;    // ticks是内核中断开启以来总共的滴答数

/** 把操作的计数器counter_on、读写锁属性rwl、计数器模式counter_mode写入控制寄存器
    并赋予初始值counter_value*/
static void frequency_set(uint8_t counter_port,
                          uint8_t counter_no,
                          uint8_t rwl,
                          uint8_t counter_mode,
                          uint16_t counter_value) {
    /* 往控制字寄存器端口0x43写入控制字 */
    outb(PIT_CONTROL_PORT,  (uint8_t)(counter_no << 6 | rwl << 4 | counter_mode << 1));
    /* 先写入counter_value的低8位 */
    outb(counter_port, (uint8_t)counter_value);
    /* 再写入counter_value的高8位 */
    outb(counter_port, (uint8_t)counter_value >> 8);
}

/* 时钟中断的处理函数 */
static void intr_timer_handler(void) {
    struct task_struct* cur_thread = running_thread();
    // 检查栈是否溢出
    ASSERT(cur_thread->stack_magic == 0x19870916);
    // 记录此线程占用的cpu时间
    cur_thread->elapsed_ticks++;
    // 从内核第一次处理时间中断后开始至今的滴哒数,内核态和用户态总共的嘀哒数
    ticks++;

    if (cur_thread->ticks == 0) {
        // 若进程时间片用完,就开始调度新的线程
        schedule();
    } else {
        // 将当前进程的时间片-1
        cur_thread->ticks++;
    }
}

/* 初始化PIT8253 */
void timer_init() {
    put_str("timer_init start\n");
    /* 设置8253的定时周期,也就是发中断的周期 */
    frequency_set(CONTRER0_PORT, COUNTER0_NO, READ_WRITE_LATCH, COUNTER_MODE,
                  COUNTER0_VALUE);
    register_handler(0x20, intr_timer_handler);
    put_str("timer_init done\n");
}
