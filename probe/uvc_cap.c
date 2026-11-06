/*
 * uvc_cap.c — ZS600B 上的最小 UVC 抓流工具（MJPEG）
 *
 * 顺序 A：alt0 -> PROBE -> GET -> COMMIT -> SET_INTERFACE(alt) -> 收流
 * 顺序 B：alt0 -> PROBE -> GET -> SET_INTERFACE(alt) -> COMMIT -> 收流
 *          （uvcvideo 的实际顺序；部分固件只认这个）
 *
 * 用法：uvc_cap <fmtIdx> <frameIdx> <秒> [a|b]
 *       帧存到 /data/local/tmp/hdmi/frame_<n>.jpg
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <libusb.h>

#define VID 0x534d
#define PID 0x2109
#define VS_IF_DEFAULT 1

#define VS_PROBE_CONTROL   0x01
#define VS_COMMIT_CONTROL  0x02

struct ctrl {
    uint16_t bmHint;
    uint8_t  bFormatIndex, bFrameIndex;
    uint32_t dwFrameInterval;
    uint16_t wKeyFrameRate, wPFrameRate, wCompQuality, wCompWindowSize, wDelay;
    uint32_t dwMaxVideoFrameSize, dwMaxPayloadTransferSize, dwClockFrequency;
    uint8_t  bmFramingInfo, bPreferedVersion, bMinVersion, bMaxVersion;
} __attribute__((packed));

static libusb_device_handle *h;
static uint8_t  vs_if = VS_IF_DEFAULT, vs_ep = 0x83;
static struct { int alt; int bytes_per_mf; } alts[8];
static int nalts;
static int ctrl_len = 26;

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

/* 找 VS 接口、端点、alt 表、控制结构长度 */
static int scan_config(void)
{
    libusb_device *dev = libusb_get_device(h);
    struct libusb_config_descriptor *cfg = NULL;
    if (libusb_get_active_config_descriptor(dev, &cfg) != 0) return -1;

    for (int i = 0; i < cfg->bNumInterfaces; i++) {
        const struct libusb_interface *itf = &cfg->interface[i];
        const struct libusb_interface_descriptor *a0 = &itf->altsetting[0];
        if (a0->bInterfaceClass == 0x0e && a0->bInterfaceSubClass == 0x01) {
            const uint8_t *q = a0->extra, *qe = q + a0->extra_length;
            while (q + 2 <= qe && q[0]) {
                if (q[1] == 0x24 && q[2] == 0x01 && q[0] >= 5) {
                    uint16_t v = le16(q + 3);
                    ctrl_len = (v >= 0x0110) ? 34 : 26;
                }
                q += q[0];
            }
        }
        if (a0->bInterfaceClass != 0x0e || a0->bInterfaceSubClass != 0x02) continue;
        vs_if = a0->bInterfaceNumber;
        for (int a = 0; a < itf->num_altsetting; a++) {
            const struct libusb_interface_descriptor *ad = &itf->altsetting[a];
            if (!ad->bNumEndpoints) continue;
            const struct libusb_endpoint_descriptor *ep = &ad->endpoint[0];
            vs_ep = ep->bEndpointAddress;
            if (nalts < 8) {
                int mps = ep->wMaxPacketSize & 0x7ff;
                int mult = (ep->wMaxPacketSize >> 11) & 3;
                alts[nalts].alt = ad->bAlternateSetting;
                alts[nalts].bytes_per_mf = mps * (mult + 1);
                nalts++;
            }
        }
    }
    libusb_free_config_descriptor(cfg);
    return nalts ? 0 : -1;
}

static int set_cur(uint16_t sel, struct ctrl *c)
{
    return libusb_control_transfer(h, 0x21, 0x01, sel << 8, vs_if, (unsigned char *)c, ctrl_len, 1000);
}
static int get_cur(uint16_t sel, struct ctrl *c)
{
    return libusb_control_transfer(h, 0xa1, 0x81, sel << 8, vs_if, (unsigned char *)c, ctrl_len, 1000);
}

/* ---- 收流 ---- */
static FILE *g_fp;
static int g_frames, g_saved, g_in, g_buflen, g_bufcap, g_stop;
static unsigned long long g_bytes;
static FILE *g_rawf;
static unsigned long long g_rawlen, g_nonzero;
static unsigned long long g_pkts, g_fullpkts, g_shortpkts, g_zeropkts;
static unsigned long long g_sentinel, g_allzero;
static int g_lenmin = 1 << 30, g_lenmax = 0;
static uint8_t *g_buf;
static char g_path[128];

static void iso_cb(struct libusb_transfer *t)
{
    if (t->status == LIBUSB_TRANSFER_COMPLETED) {
        for (int i = 0; i < t->num_iso_packets; i++) {
            struct libusb_iso_packet_descriptor *p = &t->iso_packet_desc[i];
            g_pkts++;
            if (p->actual_length == p->length) g_fullpkts++;
            else if (p->actual_length == 0) g_zeropkts++;
            else g_shortpkts++;
            if ((int)p->actual_length < g_lenmin) g_lenmin = (int)p->actual_length;
            if ((int)p->actual_length > g_lenmax) g_lenmax = (int)p->actual_length;
            if (p->status != LIBUSB_TRANSFER_COMPLETED || !p->actual_length) continue;
            uint8_t *d = libusb_get_iso_packet_buffer_simple(t, i);
            g_bytes += p->actual_length;
            { int nz = 0, same = 1;
              for (int q = 0; q < (int)p->actual_length; q++) { if (d[q]) nz++; if (d[q] != 0xAA) same = 0; }
              if (nz) g_nonzero++;
              if (!nz) g_allzero++;
              if (!same) g_sentinel++; }
            if (g_rawf && g_rawlen < 2 * 1024 * 1024) {
                fwrite(d, 1, p->actual_length, g_rawf);
                g_rawlen += p->actual_length;
            }
            for (int k = 0; k + 1 < (int)p->actual_length; k++) {
                if (!g_in && d[k] == 0xFF && d[k + 1] == 0xD8) { g_in = 1; g_buflen = 0; }
                if (g_in && g_buflen + 1 < g_bufcap) g_buf[g_buflen++] = d[k];
                if (g_in && d[k] == 0xFF && d[k + 1] == 0xD9) {
                    g_in = 0;
                    g_frames++;
                    if (g_saved < 3 && g_buflen > 1000) {
                        snprintf(g_path, sizeof(g_path), "/data/local/tmp/hdmi/frame_%d.jpg", g_saved);
                        FILE *f = fopen(g_path, "wb");
                        if (f) { fwrite(g_buf, 1, g_buflen, f); fclose(f); }
                        printf("[+] 帧 %d：%d 字节 -> %s\n", g_saved, g_buflen, g_path);
                        g_saved++;
                    }
                }
            }
        }
    }
    if (!g_stop) libusb_submit_transfer(t);
}

static unsigned long long stream(int alt, int mf, int maxframe, int secs, int *frames)
{
    const int NX = 8, PK = 32;
    struct libusb_transfer *x[8];
    g_bytes = 0; g_nonzero = 0; g_sentinel = 0; g_allzero = 0;
    g_pkts = g_fullpkts = g_shortpkts = g_zeropkts = 0;
    g_lenmin = 1 << 30; g_lenmax = 0; g_rawlen = 0; g_frames = 0; g_saved = 0; g_in = 0; g_buflen = 0; g_stop = 0;
    g_bufcap = maxframe + 4096;
    if (g_bufcap > 6 * 1024 * 1024) g_bufcap = 6 * 1024 * 1024;
    g_buf = malloc(g_bufcap);
    g_rawf = fopen("/data/local/tmp/hdmi/raw.bin", "wb");

    if (libusb_set_interface_alt_setting(h, vs_if, alt) != 0) { printf("  set alt 失败\n"); return 0; }
    for (int i = 0; i < NX; i++) {
        x[i] = libusb_alloc_transfer(PK);
        uint8_t *b = malloc(mf * PK);
        memset(b, 0xAA, mf * PK);   /* 哨兵：若抓完仍是 0xAA 说明 DMA 没写进来 */
        libusb_fill_iso_transfer(x[i], h, vs_ep, b, mf * PK, PK, iso_cb, NULL, 2000);
        libusb_set_iso_packet_lengths(x[i], mf);
        libusb_submit_transfer(x[i]);
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
    for (int i = 0; i < NX; i++) libusb_cancel_transfer(x[i]);
    for (int i = 0; i < 40; i++) { struct timeval tv = { 0, 20000 }; libusb_handle_events_timeout(NULL, &tv); }
    for (int i = 0; i < NX; i++) { free(x[i]->buffer); libusb_free_transfer(x[i]); }
    if (g_rawf) { fclose(g_rawf); g_rawf = NULL; }
    free(g_buf);
    *frames = g_frames;
    return g_bytes;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    int fmt = argc > 1 ? atoi(argv[1]) : 1;
    int frame = argc > 2 ? atoi(argv[2]) : 1;
    int secs = argc > 3 ? atoi(argv[3]) : 4;
    char order = argc > 4 ? argv[4][0] : 'b';

    libusb_init(NULL);
    h = libusb_open_device_with_vid_pid(NULL, VID, PID);
    if (!h) { printf("打不开设备（App 占用？）\n"); return 1; }
    libusb_set_configuration(h, 1);
    if (scan_config() != 0) { printf("找不到 VS 接口\n"); return 1; }
    printf("VS 接口=%d 端点=0x%02x 控制结构=%dB alt 表:", vs_if, vs_ep, ctrl_len);
    for (int i = 0; i < nalts; i++) printf(" alt%d=%dB/帧", alts[i].alt, alts[i].bytes_per_mf);
    printf("\n");

    int r = libusb_claim_interface(h, vs_if);
    if (r) { printf("claim 失败 %s\n", libusb_error_name(r)); return 1; }
    libusb_set_interface_alt_setting(h, vs_if, 0);

    /* 需要从描述符拿 maxFrame：这里直接用 frame 描述符不解析，交给设备回填 */
    struct ctrl c;
    memset(&c, 0, sizeof(c));
    c.bmHint = 1;
    c.bFormatIndex = fmt;
    c.bFrameIndex = frame;
    c.dwFrameInterval = 0;              /* 让设备选默认 */
    r = set_cur(VS_PROBE_CONTROL, &c);
    printf("PROBE -> %s\n", r < 0 ? libusb_error_name(r) : "OK");
    if (r < 0) return 1;
    r = get_cur(VS_PROBE_CONTROL, &c);
    printf("GET   -> %d 字节：fmt=%u frame=%u int=%u maxFrame=%u maxPay=%u\n",
           r, c.bFormatIndex, c.bFrameIndex, c.dwFrameInterval,
           c.dwMaxVideoFrameSize, c.dwMaxPayloadTransferSize);

    int sel = 0;
    for (int i = 0; i < nalts; i++)
        if (alts[i].bytes_per_mf >= (int)c.dwMaxPayloadTransferSize) { sel = i; break; }
    printf("选 alt%d（%d B/微帧），顺序 %c\n", alts[sel].alt, alts[sel].bytes_per_mf, order);

    if (order == 'a') {
        r = set_cur(VS_COMMIT_CONTROL, &c);
        printf("COMMIT -> %s\n", r < 0 ? libusb_error_name(r) : "OK");
        if (r < 0) return 1;
    } else {
        r = libusb_set_interface_alt_setting(h, vs_if, alts[sel].alt);
        printf("SET_INTERFACE alt%d -> %s\n", alts[sel].alt, r ? libusb_error_name(r) : "OK");
        r = set_cur(VS_COMMIT_CONTROL, &c);
        printf("COMMIT -> %s\n", r < 0 ? libusb_error_name(r) : "OK");
        if (r < 0) return 1;
    }

    int frames = 0;
    unsigned long long n = stream(alts[sel].alt, alts[sel].bytes_per_mf,
                                  (int)c.dwMaxVideoFrameSize, secs, &frames);
    printf("\n结果：%llu 字节（%.2f MB/s），完整帧 %d（%.1f fps），存 %d 帧\n",
           n, n / (double)secs / 1e6, frames, frames / (double)secs, g_saved);
    printf("ISO 包 %llu：满长 %llu、短包 %llu、零长 %llu；长度范围 %d~%d\n",
           g_pkts, g_fullpkts, g_shortpkts, g_zeropkts, g_lenmin == (1 << 30) ? 0 : g_lenmin, g_lenmax);
    printf("非零字节的 ISO 包：%llu 个；全零包：%llu 个；含非 0xAA 数据（内核确实写过）的包：%llu 个\n",
           g_nonzero, g_allzero, g_sentinel);
    printf("raw 落盘 %llu 字节\n", g_rawlen);
    {
        FILE *f = fopen("/data/local/tmp/hdmi/raw.bin", "rb");
        if (f) {
            unsigned char b[32];
            size_t got = fread(b, 1, 32, f);
            printf("raw 头 %zu 字节：", got);
            for (size_t i = 0; i < got; i++) printf("%02x ", b[i]);
            printf("\n");
            fseek(f, 0, SEEK_END);
            printf("raw 文件大小 %ld\n", ftell(f));
            fclose(f);
        }
    }

    /* 机读结论：OK=有完整帧；PARTIAL=内核写进去过数据但没凑出完整帧；NOFRAME=缓冲区未被写过 */
    printf("VERDICT %s\n", frames > 0 ? "OK" : (g_sentinel ? "PARTIAL" : "NOFRAME"));

    libusb_set_interface_alt_setting(h, vs_if, 0);
    libusb_release_interface(h, vs_if);
    libusb_close(h);
    libusb_exit(NULL);
    return frames > 0 ? 0 : 2;
}
