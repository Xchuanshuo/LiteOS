#include "thread.h"
#include "../lib/stdint.h"
#include "../lib/string.h"
#include "../lib/kernel/print.h"
#include "../kernel/global.h"
#include "../kernel/debug.h"
#include "../kernel/interrupt.h"
#include "../kernel/memory.h"

#define PG_SIZE 4096

struct task_struct* main_thread;     // 主线程PCB
struct list thread_ready_list;       // 就绪队列
struct list thread_all_list;         // 所有任务队列
static struct list_elem* thread_tag; // 用于保存队列中的线程结点

extern void switch_to(struct task_struct* cur, struct task_struct* next);

/* 获取当前线程pcb指针 */
struct task_struct* running_thread() {
    uint32_t esp;
    asm("mov %%esp, %0" : "=g" (esp));
    // 取esp整数部分,即pcb起始地址
    return (struct task_struct*)(esp & 0xfffff000);
}

/* 由kernel_thread去执行function(func_arg) */
static void kernel_thread(thread_func* function, void* func_arg) {
    // 执行function前要开中断,避免后面的时钟中断被屏蔽而无法调度其它线程
    intr_enable();
    function(func_arg);
}

/* 初始化线程栈thread_stack,将待执行的函数和参数放到thread_stack中相应的位置 */
void thread_create(struct task_struct* pthread, thread_func function, void* func_arg) {
    // 先预留中断使用栈的空间
    pthread->self_kstack -= sizeof(struct intr_stack);
    // 再留出线程栈空间
    pthread->self_kstack -= sizeof(struct thread_stack);
    struct thread_stack* kthread_stack = (struct thread_stack*) pthread->self_kstack;
    kthread_stack->eip = kernel_thread;
    kthread_stack->function = function;
    kthread_stack->func_arg = func_arg;
    kthread_stack->ebp = kthread_stack->ebx =
            kthread_stack->esi = kthread_stack->edi = 0;
}

/* 初始化线程基本信息 */
void init_thread(struct task_struct* pthread, char* name, int prio) {
    memset(pthread, 0, sizeof(*pthread));
    strcpy(pthread->name, name);

    if(pthread == main_thread) {
        pthread->status = TASK_RUNNING;
    } else {
        pthread->status = TASK_READY;
    }
    // self_kstack是线程自己在内核态下使用的栈顶地址
    pthread->self_kstack = (uint32_t*)((uint32_t)pthread + PG_SIZE);
    pthread->priority = (uint8_t) prio;
    pthread->ticks = 0;
    pthread->elapsed_ticks = 0;
    pthread->pgdir = NULL;
    pthread->stack_magic = 0x19870916;   // 自定义的魔数
}

/* 创建一优先级为prio的线程,线程名为name,线程所执行的函数是function(func_arg)*/
struct task_struct* thread_start(char* name, int prio,
        thread_func function, void* func_arg) {
    // pcb都位于内核空间,包括用户进程的pcb也是在内核空间
    struct task_struct* thread = get_kernel_pages(1);

    init_thread(thread, name, prio);
    thread_create(thread, function, func_arg);

    // 确保之前不在队列中
    ASSERT(!elem_find(&thread_ready_list, &thread->general_tag));
    // 加入到就绪线程队列中
    list_append(&thread_ready_list, &thread->general_tag);
    // 确保之前不在全部线程队列中
    ASSERT(!elem_find(&thread_all_list, &thread->all_list_tag));
    // 加入全部线程队列
    list_append(&thread_all_list, &thread->all_list_tag);

//    asm volatile ("movl %0, %%esp; pop %%ebp; pop %%ebx; pop %%edi; pop %%esi; "
//                  "ret" : : "g" (thread->self_kstack) : "memory");
    return thread;
}

/* 将kernel中的main函数完善为主线程 */
static void make_main_thread(void) {
    // main线程早已运行,在loader.S中进入内核时的mov esp,0xc009f000
    // 就是为其预留的pcb空间,因此pcb地址为0xc009e000
    // 不需要通过get_kernel_page另分配一个内存
    main_thread = running_thread();
    init_thread(main_thread, "main", 31);

    // 直接将main函数所在的线程加入到thread_all_list
    ASSERT(!elem_find(&thread_all_list, &main_thread->all_list_tag));
    list_append(&thread_all_list, &main_thread->all_list_tag);
}

/* 实现任务调度 */
void schedule() {
    ASSERT(intr_get_status() == INTR_OFF);

    struct task_struct* cur = running_thread();
    if (cur->status == TASK_RUNNING) {
        // 若此线程只是cpu时间片到了,将其加入到就绪队列队尾
        ASSERT(!elem_find(&thread_ready_list, &cur->general_tag));
        list_append(&thread_ready_list, &cur->general_tag);
        // 重新设置当前线程的tick及其状态
        cur->ticks = cur->priority;
        cur->status = TASK_READY;
    } else {
        // todo
        // 若此线程需要某事件发生后才能继续上cpu运行,不需要将其加入队列
        // 因为当前线程不在就绪队列中
    }
    ASSERT(!list_empty(&thread_ready_list));
    thread_tag = NULL; // 清空thread_tag
    // 将thread_ready_list队列中的第一个就绪线程弹出,准备将其调度上cpu
    thread_tag = list_pop(&thread_ready_list);
    // 将general_tag地址转换为pcb所在地址
    struct task_struct* next = elem2entry(struct task_struct, general_tag, thread_tag);
    next->status = TASK_RUNNING;
    switch_to(cur, next);
}

/* 初始化线程环境 */
void thread_init(void) {
    put_str("thread_init start\n");
    list_init(&thread_ready_list);
    list_init(&thread_all_list);
    // 将当前main函数创建为线程
    make_main_thread();
    put_str("thread_init done\n");
}




