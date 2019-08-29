#include "buildin_cmd.h"
#include "../lib/user/syscall.h"
#include "../lib/stdint.h"
#include "../lib/string.h"
#include "../fs/fs.h"
#include "../kernel/global.h"
#include "../fs/dir.h"
#include "shell.h"
#include "../lib/user/assert.h"

/** 将路径old_abs_path中的..和..转换为实际路径后存入new_abs_path */
static void wash_path(char* old_abs_path, char* new_abs_path) {
    assert(old_abs_path[0] == '/');
    char name[MAX_FILE_NAME_LEN] = {0};
    char* sub_path = old_abs_path;
    sub_path = path_parse(sub_path, name);
    if (name[0] == 0) {
        // 若只键入了"/",直接将"/"存入new_abs_path后返回
        new_abs_path[0] = '/';
        new_abs_path[1] = 0;
        return;
    }
    new_abs_path[0] = 0; // 避免传给new_abs_path的缓冲区不干净
    strcat(new_abs_path, "/");
    while (name[0]) {
        if (!strcmp("..", name)) {
            // 如果是上一级目录
            char* slash_ptr = strrchr(new_abs_path, '/');
            if (slash_ptr != new_abs_path) {
                *slash_ptr = 0;
            } else {
                *(slash_ptr + 1) = 0;
            }
        } else if (strcmp(".", name) != 0) {
            if (strcmp(new_abs_path, "/") != 0) {
                strcat(new_abs_path, "/");
            }
            strcat(new_abs_path, name);
        }
        memset(name, 0, MAX_FILE_NAME_LEN);
        if (sub_path) {
            sub_path = path_parse(sub_path, name);
        }
    }
}

/** 将path处理成不含..和.的绝对路径,存储在final_path中*/
void make_clear_abs_path(char* path, char* final_path) {
    char abs_path[MAX_PATH_LEN] = {0};
    // 先判断是否输入的是绝对路径
    if (path[0] != '/') {
        memset(abs_path, 0, MAX_PATH_LEN);
        if (getcwd(abs_path, MAX_PATH_LEN) != NULL) {
            if (!(abs_path[0] == '/' && abs_path[1] == 0)) {
                // 若abs_path表示的当前目录不是根目录/
                strcat(abs_path, "/");
            }
        }
    }
    strcat(abs_path, path);
    wash_path(abs_path, final_path);
}

