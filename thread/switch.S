[bits 32]
section .text
global switch_to
switch_to:
   ;栈中此处是返回地址
   push esi
   push edi
   push ebx
   push ebp

   mov eax, [esp + 20] ;得到栈中的参数cur
   mov [eax], esp      ;保存栈顶指针esp task_struct的self_kstack字段
                       ;self_kstack在task_struct中的偏移为0
                   ;所以直接往thread开头处存4字节便可

   ;--------- 完成当前线程环境的备份,接下来恢复下一个线程的环境----------
   mov eax, [esp + 24] ;得到栈中的参数next
   mov esp, [eax]      ;pcb的第一个成员是self_kstack的成员
   ;它用来记录0级栈指针,被换上cpu时用来恢复0级栈
   ;0级栈中保存了进程或线程的所有信息,包括3级栈指针
   pop ebp
   pop ebx
   pop edi
   pop esi
   ret


