/*
 * ws_conn.c —— WebSocket 协议处理
 *
 * ============================================================
 *  整合 stage12_websocket 的核心逻辑：
 *    1. Sec-WebSocket-Accept 计算: SHA1(key + GUID) → Base64
 *    2. 帧解析: FIN/opcode/mask/payload
 *    3. 帧生成: 服务器帧不掩码
 *    4. 消息处理: echo / ping-pong / close
 *
 *  WebSocket 帧格式：
 *     0                   1                   2                   3
 *     0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 *    +-+-+-+-+-------+-+-------------+-------------------------------+
 *    |F|R|R|R| opcode|M| Payload len |    Extended payload length    |
 *    |I|S|S|S|  (4)  |A|     (7)     |             (16/64)           |
 *    |N|V|V|V|       |S|             |   (if payload len==126/127)   |
 *    +-+-+-+-+-------+-+-------------+ - - - - - - - - - - - - - - - +
 *    |     Extended payload length continued, if payload len == 127  |
 *    + - - - - - - - - - - - - - - - +-------------------------------+
 *    |                               |Masking-key, if MASK set to  1|
 *    +-------------------------------+-------------------------------+
 *    | Masking-key (continued)       |          Payload Data         |
 *    +-------------------------------+ - - - - - - - - - - - - - - - +
 * ============================================================
 */

#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <strings.h>

#include <openssl/sha.h>

/* WebSocket Opcodes */
enum {
    WS_OPCODE_CONTINUATION = 0x0,
    WS_OPCODE_TEXT         = 0x1,
    WS_OPCODE_BINARY       = 0x2,
    WS_OPCODE_CLOSE        = 0x8,
    WS_OPCODE_PING         = 0x9,
    WS_OPCODE_PONG         = 0xA,
};

/* WebSocket GUID */
static const char WS_GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

/* ---------- Base64 编码 ---------- */

static char *base64_encode(const unsigned char *data, size_t len)
{
    static const char b64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    size_t out_len = 4 * ((len + 2) / 3);
    char *out = malloc(out_len + 1);
    if (!out) return NULL;

    size_t i = 0, j = 0;
    while (i < len) {
        uint32_t b0 = data[i++];
        uint32_t b1 = i < len ? data[i++] : 0;
        uint32_t b2 = i < len ? data[i++] : 0;

        uint32_t triple = (b0 << 16) | (b1 << 8) | b2;

        out[j++] = b64[(triple >> 18) & 0x3F];
        out[j++] = b64[(triple >> 12) & 0x3F];
        out[j++] = (i > len + 1) ? '=' : b64[(triple >> 6) & 0x3F];
        out[j++] = (i > len)     ? '=' : b64[triple & 0x3F];
    }
    out[j] = '\0';
    return out;
}

/* ---------- 计算 Sec-WebSocket-Accept ---------- */

char *ws_compute_accept_key(const char *client_key)
{
    /* accept = Base64(SHA1(client_key + GUID)) */
    char combined[256];
    snprintf(combined, sizeof(combined), "%s%s", client_key, WS_GUID);

    unsigned char sha1_result[SHA_DIGEST_LENGTH];
    SHA1((unsigned char *)combined, strlen(combined), sha1_result);

    return base64_encode(sha1_result, SHA_DIGEST_LENGTH);
}

/* ---------- WebSocket 握手 ---------- */

int ws_do_handshake(conn_t *conn, const char *buf, int len)
{
    /* 在 connection.c 中直接处理，这里不需要 */
    (void)conn; (void)buf; (void)len;
    return 0;
}

/* ---------- 帧解析 ---------- */

int ws_parse_frame(const char *buf, int buf_len,
                   int *opcode, char **payload, int *payload_len)
{
    if (buf_len < 2) return 0;

    int pos = 0;
    uint8_t b0 = (uint8_t)buf[pos++];
    uint8_t b1 = (uint8_t)buf[pos++];

    int fin    = (b0 >> 7) & 1;
    int op     = b0 & 0x0F;
    int masked = (b1 >> 7) & 1;
    int len7   = b1 & 0x7F;

    *opcode = op;

    /* 扩展长度 */
    int payload_start;
    if (len7 == 126) {
        if (buf_len < 4) return 0;
        *payload_len = ((uint8_t)buf[2] << 8) | (uint8_t)buf[3];
        payload_start = 4;
    } else if (len7 == 127) {
        if (buf_len < 10) return 0;
        *payload_len = 0;
        for (int i = 0; i < 8; i++) {
            *payload_len = (*payload_len << 8) | (uint8_t)buf[2 + i];
        }
        payload_start = 10;
    } else {
        *payload_len = len7;
        payload_start = 2;
    }

    /* 掩码键 */
    uint8_t mask[4];
    if (masked) {
        if (buf_len < payload_start + 4) return 0;
        memcpy(mask, buf + payload_start, 4);
        payload_start += 4;
    }

    /* 检查是否有足够的数据 */
    if (buf_len < payload_start + *payload_len) return 0;

    /* 提取 payload 并解掩码 */
    *payload = malloc(*payload_len + 1);
    if (!*payload) return -1;

    for (int i = 0; i < *payload_len; i++) {
        (*payload)[i] = buf[payload_start + i];
        if (masked) {
            (*payload)[i] ^= mask[i % 4];
        }
    }
    (*payload)[*payload_len] = '\0';

    (void)fin;
    return payload_start + *payload_len;  /* 帧总长度 */
}

/* ---------- 帧发送 ---------- */

int ws_send_frame(int fd, int opcode, const char *data, int len)
{
    uint8_t header[14];
    int hlen = 0;

    /* FIN=1, opcode */
    header[hlen++] = 0x80 | (opcode & 0x0F);

    /* 长度（服务器不掩码，MASK=0） */
    if (len <= 125) {
        header[hlen++] = len;
    } else if (len <= 65535) {
        header[hlen++] = 126;
        header[hlen++] = (len >> 8) & 0xFF;
        header[hlen++] = len & 0xFF;
    } else {
        header[hlen++] = 127;
        for (int i = 7; i >= 0; i--) {
            header[hlen++] = (len >> (8 * i)) & 0xFF;
        }
    }

    /* 先发头部 */
    if (write(fd, header, hlen) < 0) return -1;

    /* 再发 payload */
    if (len > 0 && data) {
        if (write(fd, data, len) < 0) return -1;
    }

    return hlen + len;
}

/* ---------- 消息处理 ---------- */

void ws_handle_message(conn_t *conn, int opcode, char *payload, int payload_len)
{
    switch (opcode) {
    case WS_OPCODE_TEXT:
    case WS_OPCODE_BINARY:
        /* Echo：原样返回 */
        ws_send_frame(conn->fd, opcode, payload, payload_len);
        break;

    case WS_OPCODE_PING:
        /* PING → PONG */
        ws_send_frame(conn->fd, WS_OPCODE_PONG, payload, payload_len);
        break;

    case WS_OPCODE_CLOSE:
        /* CLOSE → 回 CLOSE 并关闭 */
        ws_send_frame(conn->fd, WS_OPCODE_CLOSE, NULL, 0);
        break;

    default:
        break;
    }

    if (payload) free(payload);
}