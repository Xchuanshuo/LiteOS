#include "../lib/kernel/print.h"
#include "init.h"
#include "../thread/thread.h"
#include "interrupt.h"
#include "../device/console.h"

void k_thread_a(void*);
void k_thread_b(void*);

int main(void) {
    put_str("I am kernel\n");
    init_all();
//
    thread_start("k_thread_a", 31, k_thread_a, "argA");
//    thread_start("k_thread_b", 8, k_thread_b, "argB");

    // 打开中断 使时钟中断起作用
    intr_enable();
    while(1) {
        console_put_str("Main");
    }
    return 0;
}

/* 在线程中运行的函数 */
void k_thread_a(void* arg) {
    // 用void*来通用表示参数, 被调用的函数知道自己需要什么类型的参数,自己转换再用
    char* para = arg;
    while (1) {
        console_put_str(para);
    }
}

void k_thread_b(void* arg) {
    // 用void*来通用表示参数, 被调用的函数知道自己需要什么类型的参数,自己转换再用
    char* para = arg;
    while (1) {
        console_put_str(para);
    }
}