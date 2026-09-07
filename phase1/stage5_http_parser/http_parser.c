/*
 * http_parser.c —— HTTP/1.1 请求状态机解析器实现
 *
 * 解析流程：
 *   1. 把新数据追加到缓冲区
 *   2. 根据当前状态尝试解析
 *   3. 解析请求行：method SP uri SP version CRLF
 *   4. 解析头部：key: value CRLF，空行表示头部结束
 *   5. 解析 body：根据 Content-Length 读取
 *
 * 关键处理：
 *   - 分包：一次数据不够一行，返回 NEED_MORE
 *   - 粘包：一次数据包含多个请求，解析完一个后保留剩余
 */

#include "http_parser.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ---------- 内部辅助函数 ---------- */

/*
 * find_crlf —— 在 buf[pos..len) 中找 \r\n
 * 返回 \r 的位置，找不到返回 -1
 */
static int find_crlf(const char *buf, int pos, int len)
{
    for (int i = pos; i < len - 1; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n') {
            return i;
        }
    }
    return -1;
}

/*
 * parse_method —— 字符串转方法枚举
 */
static http_method_t parse_method(const char *str)
{
    if (strcmp(str, "GET") == 0)    return HTTP_METHOD_GET;
    if (strcmp(str, "POST") == 0)   return HTTP_METHOD_POST;
    if (strcmp(str, "HEAD") == 0)   return HTTP_METHOD_HEAD;
    if (strcmp(str, "PUT") == 0)    return HTTP_METHOD_PUT;
    if (strcmp(str, "DELETE") == 0) return HTTP_METHOD_DELETE;
    return HTTP_METHOD_UNKNOWN;
}

/*
 * parse_request_line —— 解析请求行 "GET /path HTTP/1.1"
 *   line: 不含 CRLF 的请求行
 *   req:  输出解析结果
 * 返回 0 成功，-1 失败
 */
static int parse_request_line(const char *line, http_request_t *req)
{
    /*
     * 请求行格式：method SP request-target SP HTTP-version
     * 例：       GET /index.html HTTP/1.1
     *
     * 用 sscanf 简单解析（教学版）
     * 生产版应该手写解析，处理空格、URI 编码等
     */
    char method[16], uri[512], version[16];
    if (sscanf(line, "%15s %511s %15s", method, uri, version) != 3) {
        return -1;
    }

    req->method = parse_method(method);
    strncpy(req->method_str, method, sizeof(req->method_str) - 1);
    strncpy(req->uri, uri, sizeof(req->uri) - 1);
    strncpy(req->version, version, sizeof(req->version) - 1);

    return 0;
}

/*
 * parse_header_line —— 解析头部行 "Host: localhost"
 *   line: 不含 CRLF 的头部行
 *   req:  输出解析结果
 */
static void parse_header_line(const char *line, http_request_t *req)
{
    /*
     * 头部格式：field-name ":" OWS field-value
     * OWS = 可选空白（空格/制表符）
     *
     * 我们只关心几个关键头部：
     *   Host：主机名
     *   Content-Length：body 长度
     */
    char key[256], value[1024];

    /* 找冒号分隔 */
    const char *colon = strchr(line, ':');
    if (!colon) {
        return;
    }

    int key_len = colon - line;
    if (key_len >= (int)sizeof(key)) {
        key_len = sizeof(key) - 1;
    }
    strncpy(key, line, key_len);
    key[key_len] = '\0';

    /* 跳过冒号和可能的空格 */
    const char *val_start = colon + 1;
    while (*val_start == ' ' || *val_start == '\t') {
        val_start++;
    }
    strncpy(value, val_start, sizeof(value) - 1);
    value[sizeof(value) - 1] = '\0';

    /* 处理已知头部 */
    if (strcasecmp(key, "Host") == 0) {
        strncpy(req->host, value, sizeof(req->host) - 1);
    } else if (strcasecmp(key, "Content-Length") == 0) {
        req->content_length = atoi(value);
    }
    /*
     * 其他头部（User-Agent, Accept, Cookie 等）这里忽略
     * 生产版应该存到一个 key-value 列表里
     */
}

/* ---------- 公开接口 ---------- */

void http_parser_init(http_parser_t *parser)
{
    memset(parser, 0, sizeof(*parser));
    parser->state = HTTP_STATE_START;
    parser->request.content_length = -1;  /* -1 表示没有 Content-Length */
}

http_parse_result_t http_parser_feed(http_parser_t *parser,
                                      const char *data, size_t len)
{
    /* 1. 追加新数据到缓冲区 */
    if (parser->buf_len + (int)len > (int)sizeof(parser->buf)) {
        parser->state = HTTP_STATE_ERROR;
        return HTTP_PARSE_ERROR;
    }
    memcpy(parser->buf + parser->buf_len, data, len);
    parser->buf_len += len;

    /* 2. 根据状态解析 */
    while (parser->state != HTTP_STATE_DONE &&
           parser->state != HTTP_STATE_ERROR) {

        if (parser->state == HTTP_STATE_START ||
            parser->state == HTTP_STATE_HEADER) {

            /*
             * 请求行和头部都是按行的，找 \r\n
             */
            int crlf_pos = find_crlf(parser->buf, parser->parse_pos,
                                     parser->buf_len);
            if (crlf_pos < 0) {
                /* 没找到完整的一行，需要更多数据 */
                return HTTP_PARSE_NEED_MORE;
            }

            /* 提取这一行（不含 CRLF） */
            int line_start = parser->parse_pos;
            int line_len   = crlf_pos - line_start;
            char line[1024];
            if (line_len >= (int)sizeof(line)) {
                parser->state = HTTP_STATE_ERROR;
                return HTTP_PARSE_ERROR;
            }
            memcpy(line, parser->buf + line_start, line_len);
            line[line_len] = '\0';

            /* 移动解析位置到 CRLF 之后 */
            parser->parse_pos = crlf_pos + 2;

            if (parser->state == HTTP_STATE_START) {
                /* 解析请求行 */
                if (parse_request_line(line, &parser->request) < 0) {
                    parser->state = HTTP_STATE_ERROR;
                    return HTTP_PARSE_ERROR;
                }
                parser->state = HTTP_STATE_HEADER;

            } else {
                /* 解析头部行 */
                if (line_len == 0) {
                    /*
                     * 空行 = 头部结束
                     * 接下来是 body（如果有）
                     */
                    if (parser->request.content_length > 0) {
                        parser->state = HTTP_STATE_BODY;
                        parser->request.body_len = 0;
                    } else {
                        /* 没有 body，解析完成 */
                        parser->state = HTTP_STATE_DONE;
                    }
                } else {
                    parse_header_line(line, &parser->request);
                    /* 继续在 HTTP_STATE_HEADER 状态 */
                }
            }

        } else if (parser->state == HTTP_STATE_BODY) {
            /*
             * 读取 body
             * 根据 Content-Length 确定要读多少
             */
            int need = parser->request.content_length;
            int have = parser->buf_len - parser->parse_pos;

            if (have >= need) {
                /* body 数据够了 */
                memcpy(parser->request.body,
                       parser->buf + parser->parse_pos, need);
                parser->request.body_len = need;
                parser->parse_pos += need;
                parser->state = HTTP_STATE_DONE;
            } else {
                /* body 数据不够，需要更多 */
                return HTTP_PARSE_NEED_MORE;
            }
        }
    }

    if (parser->state == HTTP_STATE_ERROR) {
        return HTTP_PARSE_ERROR;
    }
    return HTTP_PARSE_DONE;
}

int http_parser_consumed(const http_parser_t *parser)
{
    return parser->parse_pos;
}

void http_parser_reset(http_parser_t *parser)
{
    /*
     * 重置解析器，处理下一个请求
     * 保留缓冲区中未消耗的数据（粘包）
     */
    int consumed = parser->parse_pos;
    int remain   = parser->buf_len - consumed;

    if (remain > 0) {
        /* 把剩余数据移到缓冲区开头 */
        memmove(parser->buf, parser->buf + consumed, remain);
    }

    parser->buf_len   = remain;
    parser->parse_pos = 0;
    parser->state     = HTTP_STATE_START;

    /* 清空请求结构体 */
    memset(&parser->request, 0, sizeof(parser->request));
    parser->request.content_length = -1;
}

const char *http_method_str(http_method_t method)
{
    switch (method) {
        case HTTP_METHOD_GET:    return "GET";
        case HTTP_METHOD_POST:   return "POST";
        case HTTP_METHOD_HEAD:   return "HEAD";
        case HTTP_METHOD_PUT:    return "PUT";
        case HTTP_METHOD_DELETE: return "DELETE";
        default:                 return "UNKNOWN";
    }
}