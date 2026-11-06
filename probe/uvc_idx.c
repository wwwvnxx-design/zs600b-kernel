/*
 * uvc_idx.c — COMMIT 握手变体穷举
 * 覆盖：wValue 编码、wIndex 编码、零长度、连发两次、probe 后不 GET 直接 commit
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <libusb.h>

#define VID 0x534d
#define PID 0x2109
#define VS_IF 1
#define FMT 1
#define FRAME 6
#define INTERVAL 166666
#define MAXF 1843200

static libusb_device_handle *h;
static uint8_t cbuf[34];   /* commit 用的数据区，全 0 */

static int xfer(uint8_t rt, uint8_t req, uint16_t val, uint16_t idx, uint8_t *buf, int len)
{
    return libusb_control_transfer(h, rt, req, val, idx, buf, len, 1000);
}

static int do_probe(int len)
{
    uint8_t b[34];
    memset(b, 0, sizeof(b));
    b[0] = 1;
    b[2] = FMT; b[3] = FRAME;
    b[4] = INTERVAL & 0xff; b[5] = (INTERVAL >> 8) & 0xff;
    b[6] = (INTERVAL >> 16) & 0xff; b[7] = (INTERVAL >> 24) & 0xff;
    b[18] = MAXF & 0xff; b[19] = (MAXF >> 8) & 0xff;
    b[20] = (MAXF >> 16) & 0xff; b[21] = (MAXF >> 24) & 0xff;
    int r = xfer(0x21, 0x01, 0x0100, VS_IF, b, len);
    if (r < 0) printf("    probe 失败 %s\n", libusb_error_name(r));
    return r;
}

static void fresh(void)
{
    libusb_reset_device(h);
    libusb_set_configuration(h, 1);
    libusb_claim_interface(h, VS_IF);
    libusb_set_interface_alt_setting(h, VS_IF, 0);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    libusb_init(NULL);
    h = libusb_open_device_with_vid_pid(NULL, VID, PID);
    if (!h) { printf("打不开设备（App 占用？）\n"); return 1; }
    libusb_set_configuration(h, 1);
    printf("claim -> %s\n", libusb_error_name(libusb_claim_interface(h, VS_IF)));

    struct { const char *name; uint8_t req; uint16_t val; uint16_t idx; int len; int twice; int skipget; } T[] = {
        { "commit 0x0200 idx=1 len26",        0x01, 0x0200, 1,      26, 0, 0 },
        { "commit 0x0002 idx=1 len26",        0x01, 0x0002, 1,      26, 0, 0 },
        { "commit 0x0200 idx=0x0101 len26",   0x01, 0x0200, 0x0101, 26, 0, 0 },
        { "commit 0x0200 idx=0x0000 len26",   0x01, 0x0200, 0,      26, 0, 0 },
        { "commit 0x0200 idx=1 len0",         0x01, 0x0200, 1,      0,  0, 0 },
        { "commit 0x0200 idx=1 len26 x2",     0x01, 0x0200, 1,      26, 1, 0 },
        { "probe→commit 不 GET",              0x01, 0x0200, 1,      26, 0, 1 },
        { "commit 用 SET_CUR(probe 选择子)",   0x01, 0x0100, 1,      26, 0, 0 },
    };

    for (unsigned i = 0; i < sizeof(T)/sizeof(T[0]); i++) {
        printf("\n=== %s ===\n", T[i].name);
        fresh();
        if (!T[i].skipget) {
            int r = do_probe(26);
            if (r < 0) continue;
            uint8_t g[34]; memset(g, 0, sizeof(g));
            xfer(0xa1, 0x81, 0x0100, VS_IF, g, 26);   /* 正常流程带 GET */
        } else {
            /* 直接 commit，不 probe */
        }
        int r = xfer(0x21, T[i].req, T[i].val, T[i].idx, cbuf, T[i].len);
        printf("  commit -> %s\n", r < 0 ? libusb_error_name(r) : "OK");
        if (r < 0 && T[i].twice) {
            r = xfer(0x21, T[i].req, T[i].val, T[i].idx, cbuf, T[i].len);
            printf("  第二次   -> %s\n", r < 0 ? libusb_error_name(r) : "OK");
        }
    }
    libusb_release_interface(h, VS_IF);
    libusb_close(h);
    libusb_exit(NULL);
    return 0;
}
