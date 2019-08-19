cmake_minimum_required(VERSION 3.10)
project(LiteOS C)

set(CMAKE_C_STANDARD 11)

add_executable(LiteOS main.c kernel/init.c lib kernel boot device thread kernel/main.c
        device/timer.h device/timer.c kernel/debug.h kernel/debug.c lib/string.h lib/string.c
        lib/kernel/bitmap.h lib/kernel/bitmap.c kernel/memory.h kernel/memory.c thread/thread.h thread/thread.c)