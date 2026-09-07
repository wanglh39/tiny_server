/*
 * h2_conn.c —— HTTP/2 协议处理
 *
 * ============================================================
 *  整合 stage14_http2 的核心逻辑：
 *    1. 连接前言验证 (24字节 magic)
 *    2. 帧解析 (9字节帧头 + payload)
 *    3. HPACK 解码 (静态表 61 项 + 字面头部)
 *    4. 帧处理 (SETTINGS/HEADERS/DATA/PING/GOAWAY)
 *    5. 响应生成 (HEADERS帧 + DATA帧)
 *
 *  HTTP/2 帧格式 (9字节帧头):
 *    +-----------------------------------------------+
 *    |                 Length (24)                   |
 *    +---------------+---------------+---------------+
 *    |   Type (8)    |   Flags (8)   |
 *    +-+-------------+---------------+-------------------------------+
 *    |R|                 Stream Identifier (31)                     |
 *    +=+=============================================================+
 *    |                   Frame Payload (0...)                       |
 *    +---------------------------------------------------------------+
 *
 *  HPACK 静态表 (前 5 项):
 *    1: :authority        ""
 *    2: :method           GET
 *    3: :method           POST
 *    4: :path             /
 *    5: :path             /index.html
 *    ...
 * ============================================================
 */

#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* HTTP/2 帧类型 */
enum {
    H2_DATA          = 0x0,
    H2_HEADERS       = 0x1,
    H2_PRIORITY      = 0x2,
    H2_RST_STREAM    = 0x3,
    H2_SETTINGS      = 0x4,
    H2_PUSH_PROMISE  = 0x5,
    H2_PING          = 0x6,
    H2_GOAWAY        = 0x7,
    H2_WINDOW_UPDATE = 0x8,
    H2_CONTINUATION  = 0x9,
};

/* 帧标志 */
enum {
    H2_FLAG_END_STREAM  = 0x1,
    H2_FLAG_ACK         = 0x1,
    H2_FLAG_END_HEADERS = 0x4,
    H2_FLAG_PADDED      = 0x8,
    H2_FLAG_PRIORITY    = 0x20,
};

/* 连接前言 */
static const char H2_PREFACE[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

/* ---------- HPACK 静态表 ---------- */

typedef struct {
    const char *name;
    const char *value;
} hpack_entry_t;

static const hpack_entry_t HPACK_STATIC_TABLE[] = {
    {":authority", ""},                  /* 1 */
    {":method", "GET"},                  /* 2 */
    {":method", "POST"},                 /* 3 */
    {":path", "/"},                      /* 4 */
    {":path", "/index.html"},            /* 5 */
    {":scheme", "http"},                 /* 6 */
    {":scheme", "https"},                /* 7 */
    {":status", "200"},                  /* 8 */
    {":status", "204"},                  /* 9 */
    {":status", "206"},                  /* 10 */
    {":status", "304"},                  /* 11 */
    {":status", "400"},                  /* 12 */
    {":status", "404"},                  /* 13 */
    {":status", "500"},                  /* 14 */
    {"accept-charset", ""},              /* 15 */
    {"accept-encoding", "gzip, deflate"},/* 16 */
    {"accept-language", ""},             /* 17 */
    {"accept-ranges", ""},               /* 18 */
    {"accept", ""},                      /* 19 */
    {"access-control-allow-origin", ""}, /* 20 */
    {"age", ""},                         /* 21 */
    {"allow", ""},                       /* 22 */
    {"authorization", ""},              /* 23 */
    {"cache-control", ""},               /* 24 */
    {"content-disposition", ""},         /* 25 */
    {"content-encoding", ""},            /* 26 */
    {"content-language", ""},            /* 27 */
    {"content-length", ""},              /* 28 */
    {"content-location", ""},            /* 29 */
    {"content-range", ""},               /* 30 */
    {"content-type", ""},                /* 31 */
    {"cookie", ""},                      /* 32 */
    {"date", ""},                        /* 33 */
    {"etag", ""},                        /* 34 */
    {"expect", ""},                      /* 35 */
    {"expires", ""},                     /* 36 */
    {"from", ""},                        /* 37 */
    {"host", ""},                        /* 38 */
    {"if-match", ""},                    /* 39 */
    {"if-modified-since", ""},           /* 40 */
    {"if-none-match", ""},               /* 41 */
    {"if-range", ""},                    /* 42 */
    {"if-unmodified-since", ""},         /* 43 */
    {"last-modified", ""},               /* 44 */
    {"link", ""},                        /* 45 */
    {"location", ""},                    /* 46 */
    {"max-forwards", ""},                /* 47 */
    {"proxy-authenticate", ""},          /* 48 */
    {"proxy-authorization", ""},         /* 49 */
    {"range", ""},                       /* 50 */
    {"referer", ""},                     /* 51 */
    {"refresh", ""},                     /* 52 */
    {"retry-after", ""},                 /* 53 */
    {"server", ""},                      /* 54 */
    {"set-cookie", ""},                  /* 55 */
    {"strict-transport-security", ""},   /* 56 */
    {"transfer-encoding", ""},           /* 57 */
    {"user-agent", ""},                  /* 58 */
    {"vary", ""},                        /* 59 */
    {"via", ""},                         /* 60 */
    {"www-authenticate", ""},            /* 61 */
};
#define HPACK_TABLE_SIZE 61

/* ---------- HPACK 整数解码 ---------- */

static const uint8_t *hpack_decode_int(const uint8_t *buf, int prefix_bits,
                                        uint32_t *value)
{
    uint8_t mask = (1 << prefix_bits) - 1;
    *value = *buf & mask;
    buf++;

    if (*value < mask) {
        return buf;
    }

    uint32_t m = 0;
    while (1) {
        *value += (*buf & 0x7F) << m;
        if (!(*buf & 0x80)) {
            buf++;
            return buf;
        }
        m += 7;
        buf++;
    }
}

/* ---------- HPACK Huffman 解码 ---------- */

#include <arpa/inet.h>

typedef struct { uint32_t code; int bits; int sym; } huff_dec_t;

static const huff_dec_t huff_table[] = {
    {0x1ff8,13,0},{0x7fffd8,23,1},{0xfffffe2,28,2},{0xfffffe3,28,3},
    {0xfffffe4,28,4},{0xfffffe5,28,5},{0xfffffe6,28,6},{0xfffffe7,28,7},
    {0xfffffe8,28,8},{0xffffea,24,9},{0x3ffffffc,30,10},{0xfffffe9,28,11},
    {0xfffffea,28,12},{0x3ffffffd,30,13},{0xfffffeb,28,14},{0xfffffec,28,15},
    {0xfffffed,28,16},{0xfffffee,28,17},{0xfffffef,28,18},{0xffffff0,28,19},
    {0xffffff1,28,20},{0xffffff2,28,21},{0x3ffffffe,30,22},{0xffffff3,28,23},
    {0xffffff4,28,24},{0xffffff5,28,25},{0xffffff6,28,26},{0xffffff7,28,27},
    {0xffffff8,28,28},{0xffffff9,28,29},{0xffffffa,28,30},{0xffffffb,28,31},
    {0x14,6,32},{0x3f8,10,33},{0x3f9,10,34},{0xffa,12,35},
    {0x1ff9,13,36},{0x15,6,37},{0xf8,8,38},{0x7fa,11,39},
    {0x3fa,10,40},{0x3fb,10,41},{0xf9,8,42},{0x7fb,11,43},
    {0xfa,8,44},{0x16,6,45},{0x17,6,46},{0x18,6,47},
    {0x0,5,48},{0x1,5,49},{0x2,5,50},{0x19,6,51},
    {0x1a,6,52},{0x1b,6,53},{0x1c,6,54},{0x1d,6,55},
    {0x1e,6,56},{0x1f,6,57},{0x5c,7,58},{0xfb,8,59},
    {0x7ffc,15,60},{0x20,6,61},{0xffb,12,62},{0x3fc,10,63},
    {0x1ffa,13,64},{0x21,6,65},{0x5d,7,66},{0x5e,7,67},
    {0x5f,7,68},{0x60,7,69},{0x61,7,70},{0x62,7,71},
    {0x63,7,72},{0x64,7,73},{0x65,7,74},{0x66,7,75},
    {0x67,7,76},{0x68,7,77},{0x69,7,78},{0x6a,7,79},
    {0x6b,7,80},{0x6c,7,81},{0x6d,7,82},{0x6e,7,83},
    {0x6f,7,84},{0x70,7,85},{0x71,7,86},{0x72,7,87},
    {0xfc,8,88},{0x73,7,89},{0xfd,8,90},{0x1ffb,13,91},
    {0x7fff0,19,92},{0x1ffc,13,93},{0x3ffc,14,94},{0x22,6,95},
    {0x7ffd,15,96},{0x3,5,97},{0x23,6,98},{0x4,5,99},
    {0x24,6,100},{0x5,5,101},{0x25,6,102},{0x26,6,103},
    {0x27,6,104},{0x6,5,105},{0x74,7,106},{0x75,7,107},
    {0x28,6,108},{0x29,6,109},{0x2a,6,110},{0x7,5,111},
    {0x2b,6,112},{0x76,7,113},{0x2c,6,114},{0x8,5,115},
    {0x9,5,116},{0x2d,6,117},{0x77,7,118},{0x78,7,119},
    {0x79,7,120},{0x7a,7,121},{0x7b,7,122},{0x7ffe,15,123},
    {0x7fc,11,124},{0x3ffd,14,125},{0x1ffd,13,126},{0xffffffc,28,127},
    {0xfffe6,20,128},{0x3fffd2,22,129},{0xfffe7,20,130},{0xfffe8,20,131},
    {0x3fffd3,22,132},{0x3fffd4,22,133},{0x3fffd5,22,134},{0x7fffd9,23,135},
    {0x3fffd6,22,136},{0x7fffda,23,137},{0x7fffdb,23,138},{0x7fffdc,23,139},
    {0x7fffdd,23,140},{0x7fffde,23,141},{0xffffeb,24,142},{0x7fffdf,23,143},
    {0xffffec,24,144},{0xffffed,24,145},{0x3fffd7,22,146},{0x7fffe0,23,147},
    {0xffffee,24,148},{0x7fffe1,23,149},{0x7fffe2,23,150},{0x7fffe3,23,151},
    {0x7fffe4,23,152},{0x1fffdc,21,153},{0x3fffd8,22,154},{0x7fffe5,23,155},
    {0x3fffd9,22,156},{0x7fffe6,23,157},{0x7fffe7,23,158},{0xffffef,24,159},
    {0x3fffda,22,160},{0x1fffdd,21,161},{0xfffe9,20,162},{0x3fffdb,22,163},
    {0x3fffdc,22,164},{0x7fffe8,23,165},{0x7fffe9,23,166},{0x1fffde,21,167},
    {0x7fffea,23,168},{0x3fffdd,22,169},{0x3fffde,22,170},{0xfffff0,24,171},
    {0x1fffdf,21,172},{0x3fffdf,22,173},{0x7fffeb,23,174},{0x7fffec,23,175},
    {0x1fffe0,21,176},{0x1fffe1,21,177},{0x3fffe0,22,178},{0x1fffe2,21,179},
    {0x7fffed,23,180},{0x3fffe1,22,181},{0x7fffee,23,182},{0x7fffef,23,183},
    {0xfffea,20,184},{0x3fffe2,22,185},{0x3fffe3,22,186},{0x3fffe4,22,187},
    {0x7ffff0,23,188},{0x3fffe5,22,189},{0x3fffe6,22,190},{0x7ffff1,23,191},
    {0x3ffffe0,26,192},{0x3ffffe1,26,193},{0xfffeb,20,194},{0x7fff1,19,195},
    {0x3fffe7,22,196},{0x7ffff2,23,197},{0x3fffe8,22,198},{0x1ffffec,25,199},
    {0x3ffffe2,26,200},{0x3ffffe3,26,201},{0x3ffffe4,26,202},{0x7ffffde,27,203},
    {0x7ffffdf,27,204},{0x3ffffe5,26,205},{0xfffff1,24,206},{0x1ffffed,25,207},
    {0x7fff2,19,208},{0x1fffe3,21,209},{0x3ffffe6,26,210},{0x7ffffe0,27,211},
    {0x7ffffe1,27,212},{0x3ffffe7,26,213},{0x7ffffe2,27,214},{0xfffff2,24,215},
    {0x1fffe4,21,216},{0x1fffe5,21,217},{0x3ffffe8,26,218},{0x3ffffe9,26,219},
    {0xffffffd,28,220},{0x7ffffe3,27,221},{0x7ffffe4,27,222},{0x7ffffe5,27,223},
    {0xfffec,20,224},{0xfffff3,24,225},{0xfffed,20,226},{0x1fffe6,21,227},
    {0x3fffe9,22,228},{0x1fffe7,21,229},{0x1fffe8,21,230},{0x7ffff3,23,231},
    {0x3fffea,22,232},{0x3fffeb,22,233},{0x1ffffee,25,234},{0x1ffffef,25,235},
    {0xfffff4,24,236},{0xfffff5,24,237},{0x3ffffea,26,238},{0x7ffff4,23,239},
    {0x3ffffeb,26,240},{0x7ffffe6,27,241},{0x3ffffec,26,242},{0x3ffffed,26,243},
    {0x7ffffe7,27,244},{0x7ffffe8,27,245},{0x7ffffe9,27,246},{0x7ffffea,27,247},
    {0x7ffffeb,27,248},{0xffffffe,28,249},{0x7ffffec,27,250},{0x7ffffed,27,251},
    {0x7ffffee,27,252},{0x7ffffef,27,253},{0x7fffff0,27,254},{0x3ffffee,26,255},
    {0x3fffffff,30,256},
};
#define HUFF_TABLE_SIZE (sizeof(huff_table)/sizeof(huff_table[0]))

static int huff_decode(const uint8_t *src, int src_len, char *dst, int dst_max)
{
    uint64_t bits = 0;
    int nbits = 0;
    int dout = 0;
    int i = 0;

    while (i < src_len || nbits > 0) {
        while (nbits < 32 && i < src_len) {
            bits = (bits << 8) | src[i++];
            nbits += 8;
        }

        if (nbits == 0) break;

        int found = 0;
        for (int b = 32; b >= 5; b--) {
            if (nbits < b) continue;
            uint64_t code = (bits >> (nbits - b)) & ((1ULL << b) - 1);
            for (int j = 0; j < (int)HUFF_TABLE_SIZE; j++) {
                if (huff_table[j].bits == b && huff_table[j].code == code) {
                    if (dout < dst_max - 1) {
                        dst[dout++] = (char)huff_table[j].sym;
                    }
                    nbits -= b;
                    bits &= (1ULL << nbits) - 1;
                    found = 1;
                    break;
                }
            }
            if (found) break;
        }

        if (!found) {
            if (nbits >= 8) {
                nbits -= 8;
                bits &= (1ULL << nbits) - 1;
            } else {
                break;
            }
        }
    }

    dst[dout] = '\0';
    return dout;
}

/* ---------- HPACK 字符串解码 ---------- */

static const uint8_t *hpack_decode_str(const uint8_t *buf, char *str, int max_len)
{
    int huffman = (*buf & 0x80) != 0;
    uint32_t len;
    const uint8_t *start = buf;
    buf = hpack_decode_int(buf, 7, &len);

    if (huffman) {
        huff_decode(buf, len, str, max_len);
    } else {
        int copy_len = len < (uint32_t)max_len - 1 ? len : (uint32_t)max_len - 1;
        strncpy(str, (const char *)buf, copy_len);
        str[copy_len] = '\0';
    }
    (void)start;
    return buf + len;
}

/* ---------- HPACK 头部块解码 ---------- */

static void hpack_decode(const uint8_t *buf, int len, h2_stream_t *stream)
{
    const uint8_t *end = buf + len;

    while (buf < end) {
        if (*buf & 0x80) {
            /* 索引引用（静态表） */
            uint32_t index;
            buf = hpack_decode_int(buf, 7, &index);

            if (index > 0 && index <= HPACK_TABLE_SIZE) {
                const hpack_entry_t *e = &HPACK_STATIC_TABLE[index - 1];
                if (strcmp(e->name, ":method") == 0) {
                    strncpy(stream->method, e->value, sizeof(stream->method) - 1);
                } else if (strcmp(e->name, ":path") == 0) {
                    strncpy(stream->path, e->value, sizeof(stream->path) - 1);
                } else if (strcmp(e->name, ":authority") == 0) {
                    strncpy(stream->host, e->value, sizeof(stream->host) - 1);
                }
            }
        } else if (*buf & 0x40) {
            /* 字面头部，带增量索引 (01 prefix) */
            uint32_t index;
            buf = hpack_decode_int(buf, 6, &index);

            char name[256] = "";
            char value[512];

            if (index == 0) {
                /* index=0: 后面跟 name 字符串 + value 字符串 */
                buf = hpack_decode_str(buf, name, sizeof(name));
            } else if (index <= HPACK_TABLE_SIZE) {
                /* index>0: name 从静态表查找，后面只跟 value 字符串 */
                strncpy(name, HPACK_STATIC_TABLE[index - 1].name, sizeof(name) - 1);
            }

            buf = hpack_decode_str(buf, value, sizeof(value));

            if (strcmp(name, ":method") == 0)
                strncpy(stream->method, value, sizeof(stream->method) - 1);
            else if (strcmp(name, ":path") == 0)
                strncpy(stream->path, value, sizeof(stream->path) - 1);
            else if (strcmp(name, ":authority") == 0)
                strncpy(stream->host, value, sizeof(stream->host) - 1);
        } else if (*buf & 0x20) {
            /* 动态表大小更新，跳过 */
            uint32_t size;
            buf = hpack_decode_int(buf, 5, &size);
        } else {
            /* 字面头部，不带索引 (0000 prefix) */
            uint32_t index;
            buf = hpack_decode_int(buf, 4, &index);

            char name[256] = "";
            char value[512];

            if (index == 0) {
                buf = hpack_decode_str(buf, name, sizeof(name));
            } else if (index <= HPACK_TABLE_SIZE) {
                strncpy(name, HPACK_STATIC_TABLE[index - 1].name, sizeof(name) - 1);
            }

            buf = hpack_decode_str(buf, value, sizeof(value));

            if (strcmp(name, ":method") == 0)
                strncpy(stream->method, value, sizeof(stream->method) - 1);
            else if (strcmp(name, ":path") == 0)
                strncpy(stream->path, value, sizeof(stream->path) - 1);
            else if (strcmp(name, ":authority") == 0)
                strncpy(stream->host, value, sizeof(stream->host) - 1);
        }
    }
}

/* ---------- HPACK 整数编码 ---------- */

static uint8_t *hpack_encode_int(uint8_t *buf, uint32_t value,
                                  uint8_t prefix, int prefix_bits)
{
    uint8_t mask = (1 << prefix_bits) - 1;

    if (value < mask) {
        *buf++ = prefix | value;
        return buf;
    }

    *buf++ = prefix | mask;
    value -= mask;

    while (value >= 128) {
        *buf++ = (value & 0x7F) | 0x80;
        value >>= 7;
    }
    *buf++ = value;
    return buf;
}

/* ---------- HPACK 字符串编码 ---------- */

static uint8_t *hpack_encode_str(uint8_t *buf, const char *str)
{
    int len = strlen(str);
    buf = hpack_encode_int(buf, len, 0x00, 7);
    memcpy(buf, str, len);
    return buf + len;
}

/* ---------- 帧头解析/生成 ---------- */

typedef struct {
    uint32_t length;     /* 24 位 */
    uint8_t  type;
    uint8_t  flags;
    uint32_t stream_id;  /* 31 位 */
} h2_frame_hdr_t;

static int parse_frame_hdr(const uint8_t *buf, h2_frame_hdr_t *hdr)
{
    hdr->length = ((uint32_t)buf[0] << 16) |
                  ((uint32_t)buf[1] << 8)  |
                  (uint32_t)buf[2];
    hdr->type      = buf[3];
    hdr->flags     = buf[4];
    hdr->stream_id = ((uint32_t)buf[5] << 24) |
                     ((uint32_t)buf[6] << 16) |
                     ((uint32_t)buf[7] << 8)  |
                     (uint32_t)buf[8];
    hdr->stream_id &= 0x7FFFFFFF;  /* 最高位 R 保留 */
    return 0;
}

static int build_frame_hdr(uint8_t *buf, uint32_t length, uint8_t type,
                           uint8_t flags, uint32_t stream_id)
{
    buf[0] = (length >> 16) & 0xFF;
    buf[1] = (length >> 8)  & 0xFF;
    buf[2] = length & 0xFF;
    buf[3] = type;
    buf[4] = flags;
    buf[5] = (stream_id >> 24) & 0x7F;
    buf[6] = (stream_id >> 16) & 0xFF;
    buf[7] = (stream_id >> 8)  & 0xFF;
    buf[8] = stream_id & 0xFF;
    return 9;
}

/* ---------- 发送帧 ---------- */

static int h2_send_frame(int fd, uint8_t type, uint8_t flags,
                         uint32_t stream_id,
                         const uint8_t *payload, uint32_t payload_len)
{
    uint8_t hdr[9];
    build_frame_hdr(hdr, payload_len, type, flags, stream_id);

    if (write(fd, hdr, 9) < 0) return -1;
    if (payload_len > 0 && payload) {
        if (write(fd, payload, payload_len) < 0) return -1;
    }
    return 0;
}

/* ---------- SETTINGS 帧 ---------- */

int h2_send_settings(int fd)
{
    /* 发送空 SETTINGS 帧 */
    return h2_send_frame(fd, H2_SETTINGS, 0, 0, NULL, 0);
}

int h2_send_settings_ack(int fd)
{
    return h2_send_frame(fd, H2_SETTINGS, H2_FLAG_ACK, 0, NULL, 0);
}

/* ---------- 检查连接前言 ---------- */

int h2_check_preface(const char *buf, int len)
{
    if (len < H2_PREFACE_LEN) return 0;
    return memcmp(buf, H2_PREFACE, H2_PREFACE_LEN) == 0;
}

/* ---------- 查找/创建流 ---------- */

static h2_stream_t *find_or_create_stream(conn_t *conn, uint32_t stream_id)
{
    for (int i = 0; i < conn->h2_stream_count; i++) {
        if (conn->h2_streams[i].id == stream_id) {
            return &conn->h2_streams[i];
        }
    }

    if (conn->h2_stream_count >= H2_MAX_STREAMS) return NULL;

    h2_stream_t *s = &conn->h2_streams[conn->h2_stream_count++];
    memset(s, 0, sizeof(*s));
    s->id    = stream_id;
    s->state = H2_STREAM_OPEN;
    return s;
}

/* ---------- 发送响应 ---------- */

int h2_send_response(int fd, uint32_t stream_id, int status,
                     const char *content_type, const char *body, int body_len)
{
    /*
     * HPACK 编码响应头部：
     *   :status 200         → 索引 8
     *   content-type xxx    → 字面头部
     *   content-length xxx  → 字面头部
     */
    uint8_t headers_buf[512];
    uint8_t *p = headers_buf;

    /* :status 用索引引用（8 = 200, 9 = 204, ...） */
    if (status == 200) {
        *p++ = 0x80 | 8;  /* 索引 8 */
    } else {
        /* 字面头部，索引名称 (:status 在表中没有直接索引名称) */
        *p++ = 0x00;  /* 字面头部，索引 0 */
        p = hpack_encode_str(p, ":status");
        char status_str[8];
        snprintf(status_str, sizeof(status_str), "%d", status);
        p = hpack_encode_str(p, status_str);
    }

    /* content-type */
    *p++ = 0x00;  /* 字面头部 */
    p = hpack_encode_str(p, "content-type");
    p = hpack_encode_str(p, content_type);

    /* content-length */
    *p++ = 0x00;
    p = hpack_encode_str(p, "content-length");
    char cl_str[16];
    snprintf(cl_str, sizeof(cl_str), "%d", body_len);
    p = hpack_encode_str(p, cl_str);

    int headers_len = p - headers_buf;

    /* 发送 HEADERS 帧 (END_HEADERS) */
    h2_send_frame(fd, H2_HEADERS, H2_FLAG_END_HEADERS, stream_id,
                  headers_buf, headers_len);

    /* 发送 DATA 帧 (END_STREAM) */
    h2_send_frame(fd, H2_DATA, H2_FLAG_END_STREAM, stream_id,
                  (const uint8_t *)body, body_len);

    return 0;
}

/* ---------- 帧处理 ---------- */

static void handle_settings(conn_t *conn, h2_frame_hdr_t *hdr,
                            const uint8_t *payload)
{
    if (hdr->flags & H2_FLAG_ACK) {
        /* 收到 ACK，不需要处理 */
        return;
    }
    /* 回复 ACK */
    h2_send_settings_ack(conn->fd);
}

static void handle_headers(conn_t *conn, h2_frame_hdr_t *hdr,
                           const uint8_t *payload)
{
    h2_stream_t *stream = find_or_create_stream(conn, hdr->stream_id);
    if (!stream) return;

    /* 跳过 pad 和 priority（如果有） */
    int offset = 0;
    if (hdr->flags & H2_FLAG_PADDED) {
        int pad_len = payload[0];
        offset = 1;
        (void)pad_len;
    }
    if (hdr->flags & H2_FLAG_PRIORITY) {
        offset += 5;  /* 4字节 stream dependency + 1字节 weight */
    }

    /* HPACK 解码 */
    int payload_len = hdr->length - offset;
    if (hdr->flags & H2_FLAG_PADDED) {
        payload_len -= payload[0] + 1;
    }

    hpack_decode(payload + offset, payload_len, stream);
    stream->headers_done = 1;


    /* END_STREAM → 请求完整，发响应 */
    if (hdr->flags & H2_FLAG_END_STREAM) {
        stream->state = H2_STREAM_HALF_CLOSED;

        /* 生成响应 */
        if (strcmp(stream->path, "/") == 0 || strcmp(stream->path, "/index.html") == 0) {
            const char *body = "<!DOCTYPE html><html><body>"
                "<h1>advanced_server (HTTP/2)</h1>"
                "<p>Hello from HTTP/2!</p>"
                "</body></html>";
            h2_send_response(conn->fd, stream->id, 200, "text/html",
                            body, strlen(body));
        } else if (strcmp(stream->path, "/api/info") == 0) {
            char body[256];
            int blen = snprintf(body, sizeof(body),
                "{\"status\":\"ok\",\"proto\":\"h2\",\"worker\":%d,"
                "\"method\":\"%s\",\"path\":\"%s\"}",
                conn->worker_id, stream->method, stream->path);
            h2_send_response(conn->fd, stream->id, 200, "application/json",
                            body, blen);
        } else {
            const char *body = "<!DOCTYPE html><html><body>"
                "<h1>404 Not Found</h1>"
                "</body></html>";
            h2_send_response(conn->fd, stream->id, 404, "text/html",
                            body, strlen(body));
        }

        stream->state = H2_STREAM_CLOSED;
    }
}

static void handle_data(conn_t *conn, h2_frame_hdr_t *hdr,
                        const uint8_t *payload)
{
    /* 简化：不处理请求 body */
    (void)conn; (void)hdr; (void)payload;
}

static void handle_ping(conn_t *conn, h2_frame_hdr_t *hdr,
                        const uint8_t *payload)
{
    if (hdr->flags & H2_FLAG_ACK) return;
    /* 回复 PING ACK */
    h2_send_frame(conn->fd, H2_PING, H2_FLAG_ACK, 0, payload, hdr->length);
}

static void handle_goaway(conn_t *conn, h2_frame_hdr_t *hdr,
                          const uint8_t *payload)
{
    (void)hdr; (void)payload;
    /* 收到 GOAWAY，准备关闭 */
}

static void handle_window_update(conn_t *conn, h2_frame_hdr_t *hdr,
                                  const uint8_t *payload)
{
    (void)conn; (void)hdr; (void)payload;
}

static void handle_frame(conn_t *conn, h2_frame_hdr_t *hdr,
                         const uint8_t *payload)
{
    switch (hdr->type) {
    case H2_SETTINGS:      handle_settings(conn, hdr, payload); break;
    case H2_HEADERS:       handle_headers(conn, hdr, payload); break;
    case H2_DATA:          handle_data(conn, hdr, payload); break;
    case H2_PING:          handle_ping(conn, hdr, payload); break;
    case H2_GOAWAY:        handle_goaway(conn, hdr, payload); break;
    case H2_WINDOW_UPDATE: handle_window_update(conn, hdr, payload); break;
    case H2_PRIORITY:      break;  /* 忽略 */
    case H2_RST_STREAM:    break;  /* 忽略 */
    default:               break;
    }
}

/* ---------- HTTP/2 数据处理入口 ---------- */

int h2_handle_data(conn_t *conn, const char *buf, int len)
{
    int pos = 0;

    /* 1. 验证连接前言 */
    if (!conn->h2_preface_read) {
        if (len < H2_PREFACE_LEN) return 0;  /* 数据不够 */

        if (memcmp(buf, H2_PREFACE, H2_PREFACE_LEN) != 0) {
            return -1;  /* 前言不匹配 */
        }

        conn->h2_preface_read = 1;
        pos = H2_PREFACE_LEN;

        /* 发送 SETTINGS 帧 */
        h2_send_settings(conn->fd);
    }

    /* 2. 循环解析帧 */
    while (pos + 9 <= len) {
        h2_frame_hdr_t hdr;
        parse_frame_hdr((const uint8_t *)(buf + pos), &hdr);

        /* 检查是否有完整的帧 */
        if (pos + 9 + (int)hdr.length > len) {
            break;  /* 帧不完整，等更多数据 */
        }

        const uint8_t *payload = (const uint8_t *)(buf + pos + 9);
        handle_frame(conn, &hdr, payload);

        pos += 9 + hdr.length;
    }

    return 0;
}