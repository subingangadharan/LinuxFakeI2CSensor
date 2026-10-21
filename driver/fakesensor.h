/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * fakesensor: record returned by read() on /dev/fakesensor.
 * Shared by the kernel driver and the user-space test.
 */
#ifndef _FAKESENSOR_H
#define _FAKESENSOR_H

#include <linux/types.h>

struct fs_sample {
	__s16 val;	/* signed 16-bit sample */
	__u8  seq;	/* COUNT register: gaps = missed samples */
	__u8  status;	/* STATUS register (bit1 = device-side overrun) */
	__u32 rsvd;
	__u64 ts_ns;	/* ktime_get_ns() taken in the hard IRQ handler */
};

#endif
