/*
 * async_log.c —— 异步日志实现（双缓冲区）
 */

#include "async_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>

/* ---------- 固定大小缓冲区 ---------- */
typedef struct {
    char  data[LOG_BUF_SIZE];
    size_t used;
} log_buffer_t;

static log_buffer_t *buffer_create(void)
{
    log_buffer_t *b = (log_buffer_t *)malloc(sizeof(log_buffer_t));
    if (b) b->used = 0;
    return b;
}

static void buffer_append(log_buffer_t *b, const char *data, size_t len)
{
    if (b->used + len <= LOG_BUF_SIZE) {
        memcpy(b->data + b->used, data, len);
        b->used += len;
    }
}

static int buffer_available(log_buffer_t *b, size_t len)
{
    return b->used + len <= LOG_BUF_SIZE;
}

/* ---------- 异步日志器 ---------- */
typedef struct {
    /* 前端数据（受 mutex 保护） */
    pthread_mutex_t  mutex;
    pthread_cond_t   cond;
    log_buffer_t    *current_buffer;   /* 前端正在写的 buffer */
    log_buffer_t    *next_buffer;      /* 备用空 buffer */
    log_buffer_t   **buffers_to_write; /* 待写入文件的 buffer 数组 */
    int              num_to_write;
    int              capacity;         /* 数组容量 */

    /* 后端数据 */
    pthread_t        thread;
    int              fd;               /* 日志文件 fd */
    log_level_t      level;
    int              running;
} async_logger_t;

static async_logger_t g_logger;

/* ---------- 后端线程 ---------- */
static void *log_thread_func(void *arg)
{
    async_logger_t *lg = (async_logger_t *)arg;

    /* 后端的备用 buffer */
    log_buffer_t *new_buffer1 = buffer_create();
    log_buffer_t *new_buffer2 = buffer_create();

    /* 后端的本地 buffer 数组 */
    log_buffer_t **buffers = (log_buffer_t **)malloc(sizeof(log_buffer_t *) * 16);
    int num_buffers = 0;
    int cap_buffers = 16;

    while (lg->running) {
        /* 1. 锁内：交换 buffers_to_write 到本地 */
        pthread_mutex_lock(&lg->mutex);

        while (lg->num_to_write == 0 && lg->running) {
            pthread_cond_wait(&lg->cond, &lg->mutex);
        }

        /* 把 buffers_to_write 里的 buffer 搬到本地 */
        for (int i = 0; i < lg->num_to_write; i++) {
            if (num_buffers >= cap_buffers) {
                cap_buffers *= 2;
                buffers = (log_buffer_t **)realloc(buffers,
                    sizeof(log_buffer_t *) * cap_buffers);
            }
            buffers[num_buffers++] = lg->buffers_to_write[i];
        }
        lg->num_to_write = 0;

        /* 如果 current_buffer 也满了，一并搬走 */
        if (lg->current_buffer->used > 0) {
            if (num_buffers >= cap_buffers) {
                cap_buffers *= 2;
                buffers = (log_buffer_t **)realloc(buffers,
                    sizeof(log_buffer_t *) * cap_buffers);
            }
            buffers[num_buffers++] = lg->current_buffer;
            lg->current_buffer = new_buffer1;
            new_buffer1 = NULL;
        }

        /* 确保 current 和 next 都有 */
        if (!lg->next_buffer) {
            if (new_buffer1) {
                lg->next_buffer = new_buffer1;
                new_buffer1 = NULL;
            } else if (new_buffer2) {
                lg->next_buffer = new_buffer2;
                new_buffer2 = NULL;
            } else {
                lg->next_buffer = buffer_create();
            }
        }

        if (!new_buffer1) {
            if (new_buffer2) {
                new_buffer1 = new_buffer2;
                new_buffer2 = NULL;
            } else {
                new_buffer1 = buffer_create();
            }
        }
        if (!new_buffer2) {
            new_buffer2 = buffer_create();
        }

        pthread_mutex_unlock(&lg->mutex);

        /* 2. 锁外：写入文件 */
        if (num_buffers == 0) continue;

        for (int i = 0; i < num_buffers; i++) {
            write(lg->fd, buffers[i]->data, buffers[i]->used);
        }

        /* 3. 回收 buffer */
        if (num_buffers > 2) {
            /* 太多了，只保留 2 个 */
            for (int i = 2; i < num_buffers; i++) {
                free(buffers[i]);
            }
            num_buffers = 2;
        }

        /* 把 buffer 还给前端做备用 */
        if (num_buffers >= 1) {
            buffers[0]->used = 0;
            if (!new_buffer1) {
                new_buffer1 = buffers[0];
            } else if (!new_buffer2) {
                new_buffer2 = buffers[0];
            } else {
                free(buffers[0]);
            }
        }
        if (num_buffers >= 2) {
            buffers[1]->used = 0;
            if (!new_buffer1) {
                new_buffer1 = buffers[1];
            } else if (!new_buffer2) {
                new_buffer2 = buffers[1];
            } else {
                free(buffers[1]);
            }
        }
        num_buffers = 0;
    }

    /* 退出前刷出剩余数据 */
    pthread_mutex_lock(&lg->mutex);
    for (int i = 0; i < lg->num_to_write; i++) {
        write(lg->fd, lg->buffers_to_write[i]->data,
              lg->buffers_to_write[i]->used);
        free(lg->buffers_to_write[i]);
    }
    if (lg->current_buffer->used > 0) {
        write(lg->fd, lg->current_buffer->data, lg->current_buffer->used);
    }
    lg->num_to_write = 0;
    pthread_mutex_unlock(&lg->mutex);

    free(buffers);
    if (new_buffer1) free(new_buffer1);
    if (new_buffer2) free(new_buffer2);

    return NULL;
}

/* ---------- 前端 API ---------- */

int async_log_init(const char *filename, log_level_t level)
{
    memset(&g_logger, 0, sizeof(g_logger));

    g_logger.fd = open(filename, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (g_logger.fd < 0) {
        fprintf(stderr, "无法打开日志文件 %s: %s\n", filename, strerror(errno));
        return -1;
    }

    g_logger.current_buffer = buffer_create();
    g_logger.next_buffer    = buffer_create();
    g_logger.capacity       = 16;
    g_logger.buffers_to_write = (log_buffer_t **)malloc(
        sizeof(log_buffer_t *) * g_logger.capacity);
    g_logger.num_to_write = 0;
    g_logger.level        = level;
    g_logger.running      = 1;

    pthread_mutex_init(&g_logger.mutex, NULL);
    pthread_cond_init(&g_logger.cond, NULL);

    pthread_create(&g_logger.thread, NULL, log_thread_func, &g_logger);

    return 0;
}

void async_log_stop(void)
{
    if (!g_logger.running) return;

    g_logger.running = 0;
    pthread_cond_signal(&g_logger.cond);
    pthread_join(g_logger.thread, NULL);

    close(g_logger.fd);
    free(g_logger.current_buffer);
    free(g_logger.next_buffer);
    free(g_logger.buffers_to_write);
    pthread_mutex_destroy(&g_logger.mutex);
    pthread_cond_destroy(&g_logger.cond);
}

static const char *level_str(log_level_t level)
{
    switch (level) {
    case LOG_LEVEL_DEBUG: return "DEBUG";
    case LOG_LEVEL_INFO:  return "INFO ";
    case LOG_LEVEL_WARN:  return "WARN ";
    case LOG_LEVEL_ERROR: return "ERROR";
    case LOG_LEVEL_FATAL: return "FATAL";
    default:              return "?????";
    }
}

void async_log_write(log_level_t level, const char *fmt, ...)
{
    if (level < g_logger.level) return;

    /* 格式化日志行 */
    char line[LOG_MAX_LINE];

    /* 时间戳 */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);

    int header_len = snprintf(line, sizeof(line),
        "%04d-%02d-%02d %02d:%02d:%02d.%03ld [%s] ",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
        tm.tm_hour, tm.tm_min, tm.tm_sec,
        ts.tv_nsec / 1000000, level_str(level));

    va_list ap;
    va_start(ap, fmt);
    int body_len = vsnprintf(line + header_len, sizeof(line) - header_len - 1,
                             fmt, ap);
    va_end(ap);

    int total = header_len + body_len;
    line[total++] = '\n';

    /* 写入 current_buffer */
    pthread_mutex_lock(&g_logger.mutex);

    if (buffer_available(g_logger.current_buffer, total)) {
        buffer_append(g_logger.current_buffer, line, total);
    } else {
        /* current_buffer 满了，加入待写队列 */
        if (g_logger.num_to_write >= g_logger.capacity) {
            g_logger.capacity *= 2;
            g_logger.buffers_to_write = (log_buffer_t **)realloc(
                g_logger.buffers_to_write,
                sizeof(log_buffer_t *) * g_logger.capacity);
        }
        g_logger.buffers_to_write[g_logger.num_to_write++] = g_logger.current_buffer;

        /* 换上 next_buffer */
        if (g_logger.next_buffer) {
            g_logger.current_buffer = g_logger.next_buffer;
            g_logger.next_buffer = NULL;
        } else {
            g_logger.current_buffer = buffer_create();
        }

        buffer_append(g_logger.current_buffer, line, total);

        /* 唤醒后端线程 */
        pthread_cond_signal(&g_logger.cond);
    }

    pthread_mutex_unlock(&g_logger.mutex);
}