/*
 * uvc_iso.c — 判定 ZS600B 上 usbfs ISO 收不到数据的确切原因
 *
 * 已知现象：URB 完成时 actual_length 是真实视频长度（12~3070 字节，波动），
 * 但用户态缓冲区里预填的 0xAA 哨兵一个字节都没被覆盖 → 内核没把数据交回用户态。
 *
 * 三种可能，本工具逐一区分：
 *   1) 缓存别名/未做 cache invalidate：DMA 已写 DRAM，CPU 读到陈旧 cache line
 *      → 传输后执行 cacheflush（__builtin___clear_cache）再看，数据应立刻出现
 *   2) 分包数/对齐导致 DMA 映射走 bounce buffer 且未拷回
 *      → 改 packets-per-URB（1/8/32）与缓冲对齐（32/4096）应出现差异
 *   3) 硬件/控制器真的不发数据 → 无论怎么改都只有哨兵
 *
 * 用法：uvc_iso [秒/组]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <libusb.h>

#define VID 0x534d
#define PID 0x2109

#define VS_PROBE_CONTROL   0x01
#define VS_COMMIT_CONTROL  0x02
#define SENTINEL           0xAA

struct ctrl {
    uint16_t bmHint;
    uint8_t  bFormatIndex, bFrameIndex;
    uint32_t dwFrameInterval;
    uint16_t wKeyFrameRate, wPFrameRate, wCompQuality, wCompWindowSize, wDelay;
    uint32_t dwMaxVideoFrameSize, dwMaxPayloadTransferSize, dwClockFrequency;
    uint8_t  bmFramingInfo, bPreferedVersion, bMinVersion, bMaxVersion;
} __attribute__((packed));

static libusb_device_handle *h;
static int vs_if = 1, vs_ep = 0x83, ctrl_len = 26;
static int alt_sel = 3, mf_size = 3072;

/* 按 alt 查该 alt 的每微帧字节数 */
static int mf_for_alt(int alt)
{
    libusb_device *d = libusb_get_device(h);
    struct libusb_config_descriptor *c = NULL;
    int r = 0;
    if (libusb_get_active_config_descriptor(d, &c) != 0) return 0;
    for (int i = 0; i < c->bNumInterfaces && !r; i++) {
        const struct libusb_interface *it = &c->interface[i];
        if (it->altsetting[0].bInterfaceClass != 0x0e || it->altsetting[0].bInterfaceSubClass != 0x02) continue;
        for (int a = 0; a < it->num_altsetting; a++) {
            const struct libusb_interface_descriptor *ad = &it->altsetting[a];
            if (ad->bAlternateSetting != alt || !ad->bNumEndpoints) continue;
            int mps = ad->endpoint[0].wMaxPacketSize & 0x7ff;
            int mult = (ad->endpoint[0].wMaxPacketSize >> 11) & 3;
            r = mps * (mult + 1);
        }
    }
    libusb_free_config_descriptor(c);
    return r;
}

/* ---- 传输上下文 ---- */
#define MAXPK 32
struct xfer {
    struct libusb_transfer *t;
    uint8_t *buf;
    int len;
};
static struct xfer X[8];
static int NX = 8;
static int g_pk, g_stop;
static unsigned long long g_bytes, g_pkts;

static void iso_cb(struct libusb_transfer *t)
{
    if (t->status == LIBUSB_TRANSFER_COMPLETED) {
        for (int i = 0; i < t->num_iso_packets; i++) {
            g_pkts++;
            g_bytes += t->iso_packet_desc[i].actual_length;
        }
    }
    if (!g_stop) libusb_submit_transfer(t);
}

/* 统计某块缓冲区里“不是哨兵”的字节数，以及首个非哨兵偏移 */
static void scan(const uint8_t *b, int len, const char *tag, unsigned long long *nz, int *first)
{
    *nz = 0; *first = -1;
    for (int i = 0; i < len; i++) {
        if (b[i] != SENTINEL) { (*nz)++; if (*first < 0) *first = i; }
    }
    printf("      %s: 非哨兵 %llu 字节，首个偏移 %d", tag, *nz, *first);
    if (*first >= 0) {
        printf("，样本 %02x %02x %02x %02x %02x %02x",
               b[*first], *first + 1 < len ? b[*first + 1] : 0,
               *first + 2 < len ? b[*first + 2] : 0,
               *first + 3 < len ? b[*first + 3] : 0,
               *first + 4 < len ? b[*first + 4] : 0,
               *first + 5 < len ? b[*first + 5] : 0);
    }
    printf("\n");
}

/* 跑一组：pk 分包数、align 缓冲对齐、flush 是否在收完后清 cache */
static void run_group(int pk, int align, int flush, int secs)
{
    printf("\n=== 分包=%d 对齐=%d flush=%d ===\n", pk, align, flush);
    g_pk = pk; g_stop = 0; g_bytes = 0; g_pkts = 0;

    if (libusb_set_interface_alt_setting(h, vs_if, alt_sel) != 0) {
        printf("    set alt%d 失败\n", alt_sel);
        return;
    }

    for (int i = 0; i < NX; i++) {
        int len = mf_size * pk;
        void *p = NULL;
        if (align > 0) {
            if (posix_memalign(&p, align, len) != 0) { printf("    对齐分配失败\n"); return; }
        } else {
            p = malloc(len);
        }
        memset(p, SENTINEL, len);
        X[i].buf = (uint8_t *)p; X[i].len = len;
        X[i].t = libusb_alloc_transfer(pk);
        libusb_fill_iso_transfer(X[i].t, h, vs_ep, X[i].buf, len, pk, iso_cb, NULL, 2000);
        libusb_set_iso_packet_lengths(X[i].t, mf_size);
        libusb_submit_transfer(X[i].t);
    }

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        struct timeval tv = { 0, 100000 };
        libusb_handle_events_timeout(NULL, &tv);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        if ((t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9 >= secs) break;
    }
    g_stop = 1;
    for (int i = 0; i < NX; i++) libusb_cancel_transfer(X[i].t);
    for (int i = 0; i < 40; i++) { struct timeval tv = { 0, 20000 }; libusb_handle_events_timeout(NULL, &tv); }

    printf("    收回 %llu 包 / %llu 字节（%.2f MB/s）\n", g_pkts, g_bytes, g_bytes / (double)secs / 1e6);

    if (flush) {
        for (int i = 0; i < NX; i++)
            __builtin___clear_cache((char *)X[i].buf, (char *)X[i].buf + X[i].len);
        printf("    已对 %d 块缓冲区执行 __builtin___clear_cache\n", NX);
    }

    unsigned long long nz; int first;
    scan(X[0].buf, X[0].len, "URB0", &nz, &first);

    for (int i = 0; i < NX; i++) { free(X[i].buf); libusb_free_transfer(X[i].t); }
    libusb_set_interface_alt_setting(h, vs_if, 0);
}

static int set_cur(uint16_t sel, struct ctrl *c)
{
    return libusb_control_transfer(h, 0x21, 0x01, sel << 8, vs_if, (unsigned char *)c, ctrl_len, 1000);
}
static int get_cur(uint16_t sel, struct ctrl *c)
{
    return libusb_control_transfer(h, 0xa1, 0x81, sel << 8, vs_if, (unsigned char *)c, ctrl_len, 1000);
}

static int setup_stream(int fmt, int frame, int alt)
{
    struct ctrl c;
    memset(&c, 0, sizeof(c));
    c.bmHint = 1;
    c.bFormatIndex = fmt;
    c.bFrameIndex = frame;
    if (set_cur(VS_PROBE_CONTROL, &c) < 0) { printf("PROBE 失败\n"); return -1; }
    if (get_cur(VS_PROBE_CONTROL, &c) < 0) { printf("GET 失败\n"); return -1; }
    printf("协商：fmt=%u frame=%u int=%u maxFrame=%u maxPay=%u\n",
           c.bFormatIndex, c.bFrameIndex, c.dwFrameInterval,
           c.dwMaxVideoFrameSize, c.dwMaxPayloadTransferSize);

    alt_sel = alt;
    if (libusb_set_interface_alt_setting(h, vs_if, alt) != 0) { printf("set alt%d 失败\n", alt); return -1; }
    if (set_cur(VS_COMMIT_CONTROL, &c) < 0) { printf("COMMIT 失败\n"); return -1; }
    printf("COMMIT OK\n");
    return 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    int secs = argc > 1 ? atoi(argv[1]) : 2;
    int fmt = argc > 2 ? atoi(argv[2]) : 1;
    int frame = argc > 3 ? atoi(argv[3]) : 1;

    libusb_init(NULL);
    h = libusb_open_device_with_vid_pid(NULL, VID, PID);
    if (!h) { printf("打不开设备（App 占用？）\n"); return 1; }
    libusb_set_configuration(h, 1);
    if (libusb_claim_interface(h, vs_if) != 0) { printf("claim 失败\n"); return 1; }
    libusb_set_interface_alt_setting(h, vs_if, 0);

    int want_alt = argc > 4 ? atoi(argv[4]) : 3;
    int m = mf_for_alt(want_alt);
    if (m <= 0) { printf("取 alt%d 的包长失败\n", want_alt); return 1; }
    mf_size = m;
    printf("alt%d 每微帧 %d 字节\n", want_alt, mf_size);

    if (setup_stream(fmt, frame, want_alt) != 0) return 1;

    /* 第一组：原始路径，不 flush */
    run_group(32, 0,    0, secs);
    /* 第二组：同样路径，收完清 cache */
    run_group(32, 0,    1, secs);
    /* 第三组：页对齐 + 清 cache */
    run_group(32, 4096, 1, secs);
    /* 第四组：单包含 URB + 清 cache（绕开多包 stride） */
    run_group(1,  4096, 1, secs);
    /* 第五组：8 包 + 清 cache */
    run_group(8,  4096, 1, secs);

    return 0;
}
