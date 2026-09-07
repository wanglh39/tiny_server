/*
 * uring_echo_server.c —— 阶段 9：io_uring 异步 IO
 *
 * ============================================================
 * io_uring 是 Linux 5.1+ 的异步 IO 接口，彻底替代 epoll 的范式。
 *
 * epoll 的问题：
 *   1. "就绪通知"——告诉你 fd 可读了，你还得自己调 read()
 *   2. 每次 read/write 都是一次系统调用
 *   3. 本质是"同步等就绪 + 同步读写"
 *
 * io_uring 的革命：
 *   1. "完成通知"——你提交读请求，内核读完了通知你
 *   2. 提交和完成都通过共享内存环形队列，无系统调用
 *   3. 真正的异步——提交后立刻返回，不等完成
 *
 * 核心数据结构：
 *   SQ (Submission Queue)   —— 用户往里放 IO 请求
 *   CQ (Completion Queue)   —— 内核往里放完成结果
 *   SQE (Submission Entry)  —— 一个 IO 请求
 *   CQE (Completion Entry)  —— 一个完成结果
 *
 * 流程：
 *   用户: 从 SQ 取一个空 SQE → 填写请求 → 提交
 *   内核: 从 SQ 取请求 → 执行 → 把结果放 CQ
 *   用户: 从 CQ 取 CQE → 处理结果
 *
 * 运行：
 *   build/bin/uring_echo_server 8080
 *   另一个终端: echo "hello" | nc localhost 8080
 * ============================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* liburing 头文件 */
#include <liburing.h>

#include "error.h"
#include "log.h"
#include "wrap_posix.h"

#define ENTRIES    256     /* SQ/CQ 队列大小 */
#define BUF_SIZE   4096    /* 每个连接的读写缓冲区 */
#define BACKLOG    512

/*
 * 操作类型标识
 * io_uring 的 user_data 字段可以存任意 8 字节
 * 我们用它存操作类型，SQE 完成后能知道是哪个操作的完成
 *
 * 更高级的做法：用 user_data 存一个指向 conn_t 的指针，
 * 在 conn_t 里记录操作类型。这里简化：直接编码。
 */
enum {
    OP_ACCEPT = 0,
    OP_READ   = 1,
    OP_WRITE  = 2,
};

/*
 * 连接信息
 * 每个连接关联一个 buffer，用于 read/write
 *
 * user_data 编码方式：
 *   高 32 位 = 操作类型 (OP_xxx)
 *   低 32 位 = 连接 fd
 * 这样从 CQE 的 user_data 一次取出操作类型和 fd
 */
typedef struct {
    int  fd;
    char buf[BUF_SIZE];
} conn_t;

/* 全局 io_uring 实例 */
static struct io_uring ring;

/* 连接表：fd → conn_t*
 * 教学简化：用数组索引 = fd
 * 生产代码用哈希表或更高效的结构
 */
static conn_t *conns[65536];

/* ---------- user_data 编码/解码 ---------- */

/*
 * 编码 user_data：操作类型 + fd
 * io_uring 的 user_data 是 __u64（8 字节）
 * 高 32 位存操作类型，低 32 位存 fd
 */
static unsigned long encode_ud(int op, int fd)
{
    return ((unsigned long)op << 32) | (unsigned long)(unsigned int)fd;
}

static int decode_op(unsigned long ud)
{
    return (int)(ud >> 32);
}

static int decode_fd(unsigned long ud)
{
    return (int)(ud & 0xFFFFFFFF);
}

/* ---------- 提交 accept 请求 ---------- */
static void submit_accept(int listen_fd)
{
    /*
     * 从 SQ 取一个空闲 SQE
     * io_uring_get_sqe 返回 NULL 表示 SQ 满了
     */
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
        log_error("SQ 满了，无法提交 accept");
        return;
    }

    /*
     * 填写 SQE：accept 操作
     * io_uring_prep_accept(sqe, fd, addr, addrlen, flags)
     */
    io_uring_prep_accept(sqe, listen_fd, NULL, NULL, 0);

    /*
     * 设置 user_data：完成时能知道这是 accept 的结果
     */
    io_uring_sqe_set_data(sqe, (void *)encode_ud(OP_ACCEPT, listen_fd));

    /*
     * 提交到内核
     * io_uring_submit 把 SQ 中的 SQE 提交给内核
     * 可以攒多个 SQE 一次 submit（减少系统调用）
     */
    io_uring_submit(&ring);
}

/* ---------- 提交 read 请求 ---------- */
static void submit_read(int fd)
{
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
        log_error("SQ 满了，无法提交 read");
        return;
    }

    conn_t *conn = conns[fd];
    if (!conn) return;

    /*
     * 填写 SQE：read 操作
     * io_uring_prep_read(sqe, fd, buf, len, offset)
     * offset = 0 表示从文件头读（socket 忽略 offset）
     */
    io_uring_prep_read(sqe, fd, conn->buf, BUF_SIZE, 0);
    io_uring_sqe_set_data(sqe, (void *)encode_ud(OP_READ, fd));

    io_uring_submit(&ring);
}

/* ---------- 提交 write 请求 ---------- */
static void submit_write(int fd, int len)
{
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
        log_error("SQ 满了，无法提交 write");
        return;
    }

    conn_t *conn = conns[fd];
    if (!conn) return;

    /*
     * 填写 SQE：write 操作
     * echo：把读到的数据原样写回
     */
    io_uring_prep_write(sqe, fd, conn->buf, len, 0);
    io_uring_sqe_set_data(sqe, (void *)encode_ud(OP_WRITE, fd));

    io_uring_submit(&ring);
}

/* ---------- 处理完成事件 ---------- */
static void handle_completion(struct io_uring_cqe *cqe, int listen_fd)
{
    /*
     * CQE 字段：
     *   res  —— 结果（>= 0 成功，< 0 错误码）
     *   user_data —— 提交时设置的数据
     */
    unsigned long ud = (unsigned long)io_uring_cqe_get_data(cqe);
    int op = decode_op(ud);
    int fd = decode_fd(ud);
    int res = cqe->res;

    switch (op) {

    case OP_ACCEPT: {
        /*
         * accept 完成
         * res = 新连接的 fd（>= 0）或错误码（< 0）
         */
        if (res < 0) {
            log_error("io_uring accept 失败: %s", strerror(-res));
        } else {
            int conn_fd = res;

            /* 创建连接信息 */
            conn_t *conn = calloc(1, sizeof(conn_t));
            conn->fd = conn_fd;
            conns[conn_fd] = conn;

            log_debug("新连接 fd=%d", conn_fd);

            /* 提交 read 请求：异步读这个连接的数据 */
            submit_read(conn_fd);
        }

        /* 无论成功失败，继续提交 accept 等下一个连接 */
        submit_accept(listen_fd);
        break;
    }

    case OP_READ: {
        /*
         * read 完成
         * res = 读到的字节数
         *   > 0: 读到数据
         *   = 0: 对端关闭
         *   < 0: 错误
         */
        if (res > 0) {
            /* 读到数据，提交 write 请求（echo 回去） */
            submit_write(fd, res);
        } else if (res == 0) {
            /* 对端关闭 */
            log_debug("连接关闭 fd=%d", fd);
            close(fd);
            if (conns[fd]) {
                free(conns[fd]);
                conns[fd] = NULL;
            }
        } else {
            /* 错误 */
            log_error("read 错误 fd=%d: %s", fd, strerror(-res));
            close(fd);
            if (conns[fd]) {
                free(conns[fd]);
                conns[fd] = NULL;
            }
        }
        break;
    }

    case OP_WRITE: {
        /*
         * write 完成
         * res = 写的字节数
         * 写完后继续提交 read，等下一批数据
         */
        if (res > 0) {
            submit_read(fd);
        } else {
            log_error("write 错误 fd=%d: %s", fd, strerror(-res));
            close(fd);
            if (conns[fd]) {
                free(conns[fd]);
                conns[fd] = NULL;
            }
        }
        break;
    }

    default:
        log_error("未知操作类型: %d", op);
    }
}

/* ============================================================
 * main
 * ============================================================
 */
int main(int argc, char *argv[])
{
    int port = 8080;
    if (argc >= 2) port = atoi(argv[1]);

    signal(SIGPIPE, SIG_IGN);

    log_info("=== 阶段 9：io_uring 异步 IO ===");
    log_info("端口: %d", port);

    /*
     * 初始化 io_uring
     * io_uring_queue_init(entries, ring, flags)
     *   entries: SQ/CQ 队列大小
     *   flags: 0 = 默认（中断模式）
     *          IORING_SETUP_SQPOLL = 内核轮询 SQ（更高性能，需要 root）
     */
    int ret = io_uring_queue_init(ENTRIES, &ring, 0);
    if (ret < 0) {
        fprintf(stderr, "io_uring_queue_init 失败: %s\n", strerror(-ret));
        return 1;
    }

    log_info("io_uring 初始化完成（队列大小=%d）", ENTRIES);

    /* 创建监听 socket */
    int listen_fd = Socket(AF_INET, SOCK_STREAM, 0);

    int reuse = 1;
    Setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    Bind(listen_fd, (SA *)&addr, sizeof(addr));
    Listen(listen_fd, BACKLOG);

    log_info("监听 0.0.0.0:%d", port);
    printf("\n>>> echo \"hello\" | nc localhost %d <<<\n\n", port);

    /*
     * 提交第一个 accept 请求
     * 之后每次 accept 完成都会自动提交下一个
     */
    submit_accept(listen_fd);

    /*
     * 主循环：等待并处理完成事件
     *
     * 和 epoll 的对比：
     *   epoll: epoll_wait → 返回就绪 fd → 自己 read/write
     *   io_uring: io_uring_wait_cqe → 返回完成结果 → 直接用
     *
     * io_uring 省掉了 read/write 的系统调用！
     */
    for (;;) {
        struct io_uring_cqe *cqe;

        /*
         * 等待一个完成事件
         * io_uring_wait_cqe 会阻塞直到 CQ 有数据
         */
        ret = io_uring_wait_cqe(&ring, &cqe);
        if (ret < 0) {
            if (ret == -EINTR) continue;
            log_error("io_uring_wait_cqe: %s", strerror(-ret));
            break;
        }

        /* 处理所有已完成的 CQE */
        unsigned head;
        unsigned count = 0;

        io_uring_for_each_cqe(&ring, head, cqe) {
            handle_completion(cqe, listen_fd);
            count++;
        }

        /*
         * 推进 CQ 环头
         * 一次处理了 count 个 CQE，告诉 io_uring 我们消费了这么多
         */
        io_uring_cq_advance(&ring, count);
    }

    /* 清理 */
    close(listen_fd);
    io_uring_queue_exit(&ring);
    return 0;
}