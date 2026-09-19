/*
 * cs9711_libusb.c - Protocol validation tool for Chipsailing CS9711 (2541:9711)
 *
 * Userspace libusb tool. Confirms the vendor bulk protocol recovered from the
 * reference libfprint driver (archeYR/libfprint-CS9711) on the actual hardware,
 * before any kernel work.
 *
 * Flow:  claim iface0 -> INIT (verify magic) -> RESET (verify) -> INIT ->
 *        SCAN (read 8000 + 24 bytes = 34x236 raw frame) -> write out.pgm
 *
 * Build:  cc -O2 -Wall tools/cs9711_libusb.c -o tools/cs9711_libusb $(pkg-config --cflags --libs libusb-1.0)
 * Run:    ./tools/cs9711_libusb [out.pgm]
 *
 * Needs device access: run as root or install udev/99-cs9711.rules.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <libusb-1.0/libusb.h>

#define CS9711_VID            0x2541
#define CS9711_PID            0x9711

#define CS9711_EP_OUT         0x01
#define CS9711_EP_IN          0x81

#define CS9711_CMD_LEN        8
#define CS9711_FRAME_SIZE     8024   /* 8000 + 24 = 34 * 236 */
#define CS9711_BLOCK1         8000
#define CS9711_BLOCK2         24

#define CS9711_SENSOR_WIDTH   34
#define CS9711_SENSOR_HEIGHT  236

#define CS9711_WAIT_TIMEOUT   300    /* ms */
#define CS9711_RESET_SLEEP    250    /* ms */

#define CMD_INIT              0x01
#define CMD_RESET             0x02
#define CMD_SCAN              0x04

/* Expected 8-byte reply to INIT/RESET. */
static const uint8_t EXPECTED_REPLY[CS9711_CMD_LEN] = {
    0xea, 0x01, 0x62, 0xa0, 0x00, 0x00, 0xc3, 0xea
};

static void build_cmd(uint8_t *buf, uint8_t type)
{
    memset(buf, 0, CS9711_CMD_LEN);
    buf[0] = 0xEA;
    buf[CS9711_CMD_LEN - 1] = 0xEA;
    buf[1] = type;
    buf[CS9711_CMD_LEN - 2] = type;
}

static int cmd_send(libusb_device_handle *h, uint8_t type)
{
    uint8_t cmd[CS9711_CMD_LEN];
    int tx = 0, rc;

    build_cmd(cmd, type);
    rc = libusb_bulk_transfer(h, CS9711_EP_OUT, cmd, CS9711_CMD_LEN, &tx,
                              CS9711_WAIT_TIMEOUT);
    if (rc) {
        fprintf(stderr, "send cmd %u: %s (rc=%d)\n", type, libusb_strerror(rc), rc);
        return -1;
    }
    if (tx != CS9711_CMD_LEN) {
        fprintf(stderr, "send cmd %u: short write %d\n", type, tx);
        return -1;
    }
    return 0;
}

static int reply_recv(libusb_device_handle *h, uint8_t type, int verify)
{
    uint8_t buf[CS9711_CMD_LEN];
    int rx = 0, rc;

    rc = libusb_bulk_transfer(h, CS9711_EP_IN, buf, CS9711_CMD_LEN, &rx,
                              CS9711_WAIT_TIMEOUT);
    if (rc) {
        fprintf(stderr, "recv reply (cmd %u): %s (rc=%d)\n", type, libusb_strerror(rc), rc);
        return -1;
    }
    if (rx != CS9711_CMD_LEN) {
        fprintf(stderr, "recv reply (cmd %u): short read %d\n", type, rx);
        return -1;
    }
    if (verify && memcmp(buf, EXPECTED_REPLY, CS9711_CMD_LEN)) {
        fprintf(stderr, "recv reply (cmd %u): mismatch:\n", type);
        fprintf(stderr, "  got:      ");
        for (int i = 0; i < CS9711_CMD_LEN; i++) fprintf(stderr, "%02x ", buf[i]);
        fprintf(stderr, "\n  expected: ");
        for (int i = 0; i < CS9711_CMD_LEN; i++) fprintf(stderr, "%02x ", EXPECTED_REPLY[i]);
        fprintf(stderr, "\n");
        return -1;
    }
    return 0;
}

/* Run a full INIT or RESET handshake: send cmd, verify reply. */
static int handshake(libusb_device_handle *h, uint8_t type)
{
    if (cmd_send(h, type)) return -1;
    if (reply_recv(h, type, 1)) return -1;
    return 0;
}

/*
 * Reference (async libfprint) order: arm 8000-byte IN read (timeout 0), send
 * SCAN, wait for block1, then read 24-byte block2 (timeout 300ms), then send
 * RESET (no reply). Bulk transfers are host-flow-controlled, so for the
 * synchronous libusb port we send SCAN first, then issue the IN reads; the
 * device holds the data until we poll, so nothing is lost.
 *
 * Block1 uses a generous timeout: the reference uses 0 (wait indefinitely for
 * the capture). We use SCAN_BLOCK1_TIMEOUT so a missing finger fails cleanly
 * instead of hanging forever.
 */
#define SCAN_BLOCK1_TIMEOUT  30000  /* ms; device waits for finger/capture */

static int do_scan(libusb_device_handle *h, uint8_t *frame)
{
    uint8_t cmd[CS9711_CMD_LEN];
    int rx = 0, rc;

    build_cmd(cmd, CMD_SCAN);

    rc = libusb_bulk_transfer(h, CS9711_EP_OUT, cmd, CS9711_CMD_LEN, &rx,
                              CS9711_WAIT_TIMEOUT);
    if (rc || rx != CS9711_CMD_LEN) {
        fprintf(stderr, "scan send: %s (rc=%d, tx %d)\n",
                libusb_strerror(rc), rc, rx);
        return -1;
    }

    /* Block1: 8000 bytes, arrives once the sensor has captured. */
    rc = libusb_bulk_transfer(h, CS9711_EP_IN, frame, CS9711_BLOCK1, &rx,
                              SCAN_BLOCK1_TIMEOUT);
    if (rc) {
        fprintf(stderr, "scan block1: %s (rc=%d, got %d)\n",
                libusb_strerror(rc), rc, rx);
        return -1;
    }
    if (rx != CS9711_BLOCK1) {
        fprintf(stderr, "scan block1: short read %d / %d\n", rx, CS9711_BLOCK1);
        return -1;
    }

    /* Block2: 24-byte tail. */
    rc = libusb_bulk_transfer(h, CS9711_EP_IN, frame + CS9711_BLOCK1,
                              CS9711_BLOCK2, &rx, CS9711_WAIT_TIMEOUT);
    if (rc) {
        fprintf(stderr, "scan block2: %s (rc=%d, got %d)\n",
                libusb_strerror(rc), rc, rx);
        return -1;
    }
    if (rx != CS9711_BLOCK2) {
        fprintf(stderr, "scan block2: short read %d / %d\n", rx, CS9711_BLOCK2);
        return -1;
    }

    /*
     * Post-scan RESET (command only). The reference sends this without reading
     * a reply; the reply is drained lazily by the init SSM's recovery path on
     * the next session. We drain it here so the endpoint stays clean for a
     * possible second scan in the same run.
     */
    if (cmd_send(h, CMD_RESET)) {
        fprintf(stderr, "scan post-reset send failed\n");
        return -1;
    }
    {
        uint8_t drain[CS9711_CMD_LEN];
        int drx = 0;
        libusb_bulk_transfer(h, CS9711_EP_IN, drain, CS9711_CMD_LEN, &drx,
                             CS9711_WAIT_TIMEOUT);  /* best-effort drain */
    }
    return 0;
}

static void write_pgm(const char *path, const uint8_t *frame)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror("fopen"); return; }
    fprintf(f, "P5\n%d %d\n255\n", CS9711_SENSOR_WIDTH, CS9711_SENSOR_HEIGHT);
    fwrite(frame, 1, CS9711_FRAME_SIZE, f);
    fclose(f);
    printf("wrote %dx%d raw frame to %s (%d bytes)\n",
           CS9711_SENSOR_WIDTH, CS9711_SENSOR_HEIGHT, path, CS9711_FRAME_SIZE);
}

int main(int argc, char **argv)
{
    const char *out = (argc > 1) ? argv[1] : "out.pgm";
    libusb_device_handle *h = NULL;
    libusb_device *dev = NULL;
    struct libusb_device_descriptor ddesc;
    uint8_t *frame = NULL;
    int rc;

    frame = malloc(CS9711_FRAME_SIZE);
    if (!frame) { perror("malloc"); return 1; }

    rc = libusb_init_context(NULL, NULL, 0);
    if (rc) { fprintf(stderr, "libusb_init: %s\n", libusb_strerror(rc)); goto out; }

    h = libusb_open_device_with_vid_pid(NULL, CS9711_VID, CS9711_PID);
    if (!h) {
        fprintf(stderr, "device %04x:%04x not found (permissions?)\n",
                CS9711_VID, CS9711_PID);
        rc = 1;
        goto out;
    }

    dev = libusb_get_device(h);
    libusb_get_device_descriptor(dev, &ddesc);
    printf("opened %04x:%04x bcdDevice=%04x\n", ddesc.idVendor, ddesc.idProduct, ddesc.bcdDevice);

    rc = libusb_set_auto_detach_kernel_driver(h, 1);
    if (rc) fprintf(stderr, "warn: auto_detach: %s\n", libusb_strerror(rc));

    rc = libusb_claim_interface(h, 0);
    if (rc) {
        fprintf(stderr, "claim_interface: %s\n", libusb_strerror(rc));
        goto out;
    }

    /*
     * Normal init path (m_init SSM happy path): send INIT, read+verify the
     * 8-byte magic reply. The RESET/re-read sequence in the reference is only
     * a recovery path for when the INIT send itself times out (stale data),
     * so we do NOT issue RESET here.
     */
    printf("INIT ... ");
    fflush(stdout);
    if (handshake(h, CMD_INIT)) { printf("FAIL\n"); goto release; }
    printf("OK (magic verified)\n");

    printf("SCAN (place finger on sensor) ...\n");
    usleep(CS9711_RESET_SLEEP * 1000);   /* M_SCAN_INIT_SLEEP */
    if (do_scan(h, frame)) {
        fprintf(stderr, "scan failed\n");
        rc = 1;
        goto release;
    }
    printf("scan OK\n");
    write_pgm(out, frame);
    rc = 0;

release:
    libusb_release_interface(h, 0);
out:
    if (h) libusb_close(h);
    libusb_exit(NULL);
    free(frame);
    return rc ? 1 : 0;
}
