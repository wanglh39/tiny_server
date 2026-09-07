#ifndef HTTP_PARSER_H
#define HTTP_PARSER_H

/*
 * http_parser.h —— HTTP/1.1 请求的增量状态机解析器
 *
 * ============================================================
 * 为什么要状态机？为什么不能用 strstr 找 \r\n\r\n？
 * ============================================================
 *
 *   TCP 是字节流，没有消息边界。一次 read 可能返回：
 *     情况1：半个请求   "GET / HTTP/1.1\r\nHost: local"
 *     情况2：一个请求   "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n"
 *     情况3：一个半请求 "GET / HTTP/1.1\r\n\r\nGET /favicon HTTP/1.1\r\n"
 *
 *   如果用 strstr 找 "\r\n\r\n"：
 *     情况1：找不到，但数据已经收到一半了，怎么办？
 *     情况3：找到了，但后面还有半个请求，怎么处理？
 *
 *   状态机解决：
 *     每次收到数据就"喂"给解析器，解析器维护当前状态
 *     如果数据不够，返回 "需要更多"（NEED_MORE）
 *     如果解析完成，返回 "完成"（DONE），并告诉你消耗了多少字节
 *     剩余字节是下一个请求的开始（处理粘包）
 */

#include <stddef.h>

/* HTTP 请求方法 */
typedef enum {
    HTTP_METHOD_UNKNOWN = 0,
    HTTP_METHOD_GET,
    HTTP_METHOD_POST,
    HTTP_METHOD_HEAD,
    HTTP_METHOD_PUT,
    HTTP_METHOD_DELETE,
} http_method_t;

/* 解析状态 */
typedef enum {
    HTTP_STATE_START   = 0,  /* 初始状态，等待请求行 */
    HTTP_STATE_HEADER  = 1,  /* 正在解析头部 */
    HTTP_STATE_BODY    = 2,  /* 正在解析 body */
    HTTP_STATE_DONE    = 3,  /* 解析完成 */
    HTTP_STATE_ERROR   = 4,  /* 解析错误 */
} http_state_t;

/* 解析结果 */
typedef enum {
    HTTP_PARSE_NEED_MORE = 0,  /* 需要更多数据 */
    HTTP_PARSE_DONE      = 1,  /* 解析完成 */
    HTTP_PARSE_ERROR     = 2,  /* 解析错误 */
} http_parse_result_t;

/* HTTP 请求结构体 */
typedef struct {
    http_method_t  method;
    char           method_str[16];    /* "GET", "POST" 等原始字符串 */
    char           uri[512];          /* 请求 URI，如 "/index.html?name=abc" */
    char           version[16];       /* "HTTP/1.1" */
    char           host[256];         /* Host 头部值 */
    int            content_length;    /* Content-Length 值，-1 表示没有 */
    char           body[8192];        /* 请求体 */
    int            body_len;          /* 已收到的 body 字节数 */
} http_request_t;

/* 解析器结构体 */
typedef struct {
    http_state_t    state;            /* 当前状态 */
    http_request_t  request;          /* 解析结果 */
    char            buf[16384];       /* 接收缓冲区 */
    int             buf_len;          /* 缓冲区已有数据长度 */
    int             parse_pos;        /* 已解析到的位置 */
} http_parser_t;

/* ---------- 接口函数 ---------- */

/*
 * http_parser_init —— 初始化解析器
 */
void http_parser_init(http_parser_t *parser);

/*
 * http_parser_feed —— 喂入新数据
 *   data: 新收到的数据
 *   len:  数据长度
 *
 * 返回：
 *   HTTP_PARSE_NEED_MORE：需要更多数据，继续 read
 *   HTTP_PARSE_DONE：解析完成，可以处理请求了
 *   HTTP_PARSE_ERROR：解析错误，应该关闭连接
 *
 * 调用后，parser->request 里是解析出的请求
 */
http_parse_result_t http_parser_feed(http_parser_t *parser,
                                      const char *data, size_t len);

/*
 * http_parser_consumed —— 返回已消耗的字节数
 * 解析完成后，调用此函数知道消耗了多少字节
 * 剩余的（buf_len - consumed）是下一个请求的数据（粘包处理）
 */
int http_parser_consumed(const http_parser_t *parser);

/*
 * http_parser_reset —— 重置解析器，准备解析下一个请求
 * 保留缓冲区中未消耗的数据（粘包的下一个请求）
 */
void http_parser_reset(http_parser_t *parser);

/*
 * http_method_str —— 方法枚举转字符串
 */
const char *http_method_str(http_method_t method);

#endif /* HTTP_PARSER_H */