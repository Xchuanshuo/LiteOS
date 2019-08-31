cmake_minimum_required(VERSION 3.10)
project(LiteOS C)

set(CMAKE_C_STANDARD 11)

add_executable(LiteOS main.c kernel/init.c lib kernel boot device thread shell userprog fs command
        kernel/main.c device/timer.h device/timer.c kernel/debug.h kernel/debug.c lib/string.h lib/string.c
        lib/kernel/bitmap.h lib/kernel/bitmap.c kernel/memory.h kernel/memory.c thread/thread.h
        thread/thread.c lib/kernel/list.h lib/kernel/list.c thread/sync.h thread/sync.c device/console.h
        device/console.c device/keyboard.h device/keyboard.c device/ioqueue.h device/ioqueue.c userprog/tss.h
        userprog/tss.c userprog/process.h userprog/process.c lib/user/syscall.h lib/user/syscall.c userprog/syscall-init.h
        userprog/syscall-init.c lib/stdio.h lib/stdio.c lib/kernel/stdio-kernel.h lib/kernel/stdio-kernel.c
        device/ide.h device/ide.c fs/super_block.h fs/inode.h fs/dir.h fs/fs.h fs/fs.c fs/inode.c fs/file.h
        fs/file.c fs/dir.c userprog/fork.h userprog/fork.c shell/shell.h shell/shell.c lib/user/assert.h
        lib/user/assert.c shell/buildin_cmd.h shell/buildin_cmd.c userprog/exec.h userprog/exec.c userprog/wait_exit.h userprog/wait_exit.c shell/pipe.h shell/pipe.c)