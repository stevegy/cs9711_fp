/*
 * cs9711.h - Header for the Chipsailing CS9711 fingerprint USB character device driver.
 */

#ifndef _CS9711_H_
#define _CS9711_H_

#include <linux/ioctl.h>

#define CS9711_VID 0x2541
#define CS9711_PID 0x9711

/* Protocol constants. */
#define CS9711_CMD_LEN        8
#define CS9711_FRAME_SIZE     8024   /* 8000 + 24 = 34 * 236 */
#define CS9711_BLOCK1         8000
#define CS9711_BLOCK2         24

#define CS9711_SENSOR_WIDTH   34
#define CS9711_SENSOR_HEIGHT  236

#define CMD_INIT              0x01
#define CMD_RESET             0x02
#define CMD_SCAN              0x04

/* Expected 8-byte reply to INIT/RESET. */
static const unsigned char cs9711_expected_reply[CS9711_CMD_LEN] = {
    0xea, 0x01, 0x62, 0xa0, 0x00, 0x00, 0xc3, 0xea
};

/* Ioctl commands. */
#define CS9711_IOC_MAGIC 'C'

#define CS9711_IOC_SCAN     _IO(CS9711_IOC_MAGIC, 1)
#define CS9711_IOC_RESET    _IO(CS9711_IOC_MAGIC, 2)
#define CS9711_IOC_GETINFO  _IOR(CS9711_IOC_MAGIC, 3, struct cs9711_info)

struct cs9711_info {
	__u32 width;
	__u32 height;
	__u32 frame_size;
};

#endif /* _CS9711_H_ */
