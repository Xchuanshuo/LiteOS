#include "shell.h"
#include "../lib/stdint.h"
#include "../fs/fs.h"
#include "../fs/file.h"
#include "../lib/user/syscall.h"
#include "../kernel/global.h"
#include "../lib/user/assert.h"
#include "../lib/string.h"
#include "buildin_cmd.h"

#define cmd_len 128 // 最大支持键入128个字符的命令行输入
#define MAX_ARG_NR 16 // 加上命令外,最多支持15个参数

/** 存储输入的命令 */
static char cmd_line[cmd_len] = {0};
char final_path[MAX_PATH_LEN] = {0}; // 用于洗路径时的缓冲

/** 用来记录当前目录,是当前目录的缓存,每次执行cd命令时会更新此内容 */
char cwd_cache[64] = {0};

/** 输出提示符 */
void print_prompt(void) {
    printf("[rabbit@localhost %s]$ ", cwd_cache);
}

/** 从键盘缓冲区中最多读入count个字节到buf */
static void readline(char* buf, int32_t count) {
    assert(buf != NULL && count > 0);
    char* pos = buf;
    while (read(stdin_no, pos, 1) != -1 && (pos - buf) < count) {
        switch (*pos) {
            // 找到回车或换行符后认为键入的命令结束,直接返回
            case '\n':
            case '\r':
                *pos = 0; // 添加cmd_line的终止符0
                putchar('\n');
                return;
            case '\b':
                if (buf[0] != '\b') { // 阻止删除本次输入的信息
                    --pos; // 退回到缓冲区cmd_line中上一个字符
                    putchar('\b');
                }
                break;
            case 'l' - 'a':
                // 1.先将当前的字符l-a置为0
                *pos = 0;
                // 2.再将屏幕清空
                clear();
                // 3.打印提示符
                print_prompt();
                // 4.将之前键入的内容再次打印
                printf("%s", buf);
                break;
            case 'u' - 'a':
                while (buf != pos) {
                    putchar('\b');
                    *(pos--) = 0;
                }
                break;
            default:
                // 非控制键则输出字符
                putchar(*pos);
                pos++;
        }
    }
    printf("readline: cant't find enter_key in the cmd_line, max num of char is 128!\n");
}

/** 分析字符串cmd_str中以token为分隔符的单词,将各单词的指针存入argv数组 */
static int32_t cmd_parse(char* cmd_str, char** argv, char token) {
    assert(cmd_str != NULL);
    int32_t arg_idx = 0;
    while (arg_idx < MAX_ARG_NR) {
        argv[arg_idx] = NULL;
        arg_idx++;
    }
    char* next = cmd_str;
    int32_t argc = 0;
    // 外层循环去处理整个命令行
    while (*next) {
        // 去除命令行字或参数之间的空格
        while (*next == token) {
            next++;
        }
        // 处理最后一个参数后接空格的情况,如"ls dir2 "
        if (*next == 0) {
            break;
        }
        argv[argc] = next;
        // 内层循环处理命令行中的每个命令字及参数
        while (*next && *next != token) {
            // 在字符串结束前找单词分隔符
            next++;
        }
        if (*next) {
            // 将token字符替换为字符串结束符0,做为一个单词的结束,
            // 并将字符指针next指向下一个字符
            *next++ = 0;
        }
        // 避免argv数组访问越界,参数过多则返回0
        if(argc > MAX_ARG_NR) {
            return -1;
        }
        argc++;
    }
    return argc;
}

char* argv[MAX_ARG_NR];    // argv必须为全局变量，为了以后exec的程序可访问参数
int32_t argc = -1;

/** 简单的shell */
void my_shell(void) {
    cwd_cache[0] = '/';
    while (1) {
        print_prompt();
        memset(final_path, 0, MAX_PATH_LEN);
        memset(cmd_line, 0, MAX_PATH_LEN);
        readline(cmd_line, MAX_PATH_LEN);
        if (cmd_line[0] == 0) {	 // 若只键入了一个回车
            continue;
        }
        argc = -1;
        argc = cmd_parse(cmd_line, argv, ' ');
        if (argc == -1) {
            printf("num of arguments exceed %d\n", MAX_ARG_NR);
            continue;
        }
        char buf[MAX_PATH_LEN] = {0};
        int32_t arg_idx = 0;
        while(arg_idx < argc) {
            make_clear_abs_path(argv[arg_idx], buf);
            printf("%s -> %s\n", argv[arg_idx], buf);
            arg_idx++;
        }
        printf("\n");
    }
    panic("my_shell: should not be here");
}