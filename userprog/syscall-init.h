#ifndef __USERPROG_SYSCALLINIT_H
#define __USERPROG_SYSCALLINIT_H
#include "../lib/stdint.h"
void syscall_init(void);
uint32_t sys_getpid(void);
#endif
