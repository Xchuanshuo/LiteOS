# 编译
gcc -m32 -I lib/kernel -c -o build/timer.o device/timer.c
gcc -m32 -I lib/kernel/ -I lib/ -I kernel/ -c -fno-builtin -o build/main.o kernel/main.c
nasm -f elf -o build/print.o lib/kernel/print.S
nasm -f elf -o build/kernel.o kernel/kernel.S
gcc -m32 -I lib/kernel/ -I lib/ -I kernel/ -c -fno-stack-protector -o build/interrupt.o kernel/interrupt.c
gcc -m32 -I lib/kernel/ -I lib/ -I kernel/ -c -fno-builtin -o build/init.o kernel/init.c
# 链接
ld -m elf_i386 -Ttext 0xc0001500 -e main -o build/kernel.bin build/main.o build/init.o build/interrupt.o build/print.o build/kernel.o
# 写入磁盘
dd if=build/kernel.bin of=/home/work/my_workspace/bochs/hd60M.img bs=512 count=200 seek=9 conv=notrunc