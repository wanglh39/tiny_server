#ifndef SERVER_STATIC_FILE_H
#define SERVER_STATIC_FILE_H

/*
 * static_file.h —— 静态文件服务模块
 */

#include <stddef.h>

int serve_static_file(int fd, const char *uri, const char *www_root);
const char *get_mime_type(const char *path);

#endif /* SERVER_STATIC_FILE_H */