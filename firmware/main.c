// SPDX-License-Identifier: MIT
/*
 * fakesensor firmware for Raspberry Pi Pico 2 (RP2350)
 *
 * Emulates a simple I2C sensor at address 0x42 with a data-ready (DRDY) line.
 *
 *   GP4 = I2C0 SDA, GP5 = I2C0 SCL, GP6 = DRDY (active high, level)
 *
 * Register map (8-bit registers, auto-incrementing pointer like real sensors):
 *
 *   0x00 WHO_AM_I  RO  0x5A
 *   0x01 CTRL      RW  bit0 EN (sampling on), bit1 INT_EN (drive DRDY pin)
 *   0x02 ODR       RW  sample period in 10 ms units (1..255), default 10 = 100 ms
 *   0x03 STATUS    RO  bit0 DRDY (new sample), bit1 OVR (previous sample never read)
 *   0x04 COUNT     RO  sample sequence number, wraps at 256
 *   0x05 DATA_L    RO  sample, low byte  (signed 16-bit)
 *   0x06 DATA_H    RO  sample, high byte
 *
 * Read semantics (what makes it behave like a real part):
 *   - Reading STATUS takes a snapshot of STATUS/COUNT/DATA, so a burst read
 *     0x03..0x06 is never torn even if a new sample lands mid-transfer.
 *   - Reading DATA_H clears DRDY/OVR and drops the DRDY pin, unless a newer
 *     sample arrived after the snapshot (then DRDY stays asserted).
 */

#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/i2c_slave.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"

#define I2C_PORT	i2c0
#define I2C_ADDR	0x42
#define I2C_BAUD	100000
#define SDA_PIN		4
#define SCL_PIN		5
#define DRDY_PIN	6

enum {
	REG_WHOAMI,
	REG_CTRL,
	REG_ODR,
	REG_STATUS,
	REG_COUNT,
	REG_DATA_L,
	REG_DATA_H,
	NUM_REGS
};

#define WHOAMI_VAL	0x5A
#define CTRL_EN		(1u << 0)
#define CTRL_INT_EN	(1u << 1)
#define STATUS_DRDY	(1u << 0)
#define STATUS_OVR	(1u << 1)

/*
 * Both the I2C IRQ and the timer IRQ run on core 0 at the default priority,
 * so on Cortex-M33 they cannot preempt each other: each handler runs to
 * completion. They can still interleave *between* I2C bytes, which is what
 * the snapshot logic below handles.
 */
static volatile uint8_t regs[NUM_REGS];
static volatile bool odr_changed;

static struct {
	uint8_t ptr;		/* register pointer */
	bool ptr_written;	/* first byte of a write = pointer */
	bool snap_valid;	/* snapshot taken in this read transfer */
	uint8_t snap[NUM_REGS];
} ctx;

/* stats, printed on USB serial */
static volatile uint32_t samples, host_reads, overruns;

static void drdy_update(void)
{
	bool on = (regs[REG_STATUS] & STATUS_DRDY) && (regs[REG_CTRL] & CTRL_INT_EN);

	gpio_put(DRDY_PIN, on);
}

static void reg_write(uint8_t reg, uint8_t val)
{
	switch (reg) {
	case REG_CTRL:
		regs[REG_CTRL] = val & (CTRL_EN | CTRL_INT_EN);
		drdy_update();
		break;
	case REG_ODR:
		regs[REG_ODR] = val ? val : 1;
		odr_changed = true;	/* main loop restarts the timer */
		break;
	default:
		break;			/* read-only / out of range: ignore */
	}
}

static uint8_t reg_read(uint8_t reg)
{
	uint8_t v;

	if (reg >= NUM_REGS)
		return 0;

	if (reg == REG_STATUS) {
		for (int i = 0; i < NUM_REGS; i++)
			ctx.snap[i] = regs[i];
		ctx.snap_valid = true;
	}

	v = (ctx.snap_valid && reg >= REG_STATUS) ? ctx.snap[reg] : regs[reg];

	if (reg == REG_DATA_H) {
		host_reads++;
		/* clear DRDY only if no newer sample arrived since the snapshot */
		if (!ctx.snap_valid || ctx.snap[REG_COUNT] == regs[REG_COUNT]) {
			regs[REG_STATUS] &= ~(STATUS_DRDY | STATUS_OVR);
			drdy_update();
		}
	}
	return v;
}

static void i2c_handler(i2c_inst_t *i2c, i2c_slave_event_t event)
{
	switch (event) {
	case I2C_SLAVE_RECEIVE:		/* controller wrote a byte */
		if (!ctx.ptr_written) {
			ctx.ptr = i2c_read_byte_raw(i2c);
			ctx.ptr_written = true;
		} else {
			reg_write(ctx.ptr++, i2c_read_byte_raw(i2c));
		}
		break;
	case I2C_SLAVE_REQUEST:		/* controller wants a byte */
		i2c_write_byte_raw(i2c, reg_read(ctx.ptr++));
		break;
	case I2C_SLAVE_FINISH:		/* STOP or repeated START */
		ctx.ptr_written = false;
		ctx.snap_valid = false;
		break;
	default:
		break;
	}
}

/* Synthetic signal: triangle wave between -1000 and +1000 */
static int16_t next_sample(void)
{
	static int16_t s;
	static int16_t step = 50;

	s += step;
	if (s >= 1000 || s <= -1000)
		step = -step;
	return s;
}

static bool sample_cb(repeating_timer_t *t)
{
	int16_t s;

	(void)t;
	if (!(regs[REG_CTRL] & CTRL_EN))
		return true;

	if (regs[REG_STATUS] & STATUS_DRDY) {	/* host never read the last one */
		regs[REG_STATUS] |= STATUS_OVR;
		overruns++;
	}

	s = next_sample();
	regs[REG_DATA_L] = (uint8_t)(s & 0xff);
	regs[REG_DATA_H] = (uint8_t)((uint16_t)s >> 8);
	regs[REG_COUNT]++;
	regs[REG_STATUS] |= STATUS_DRDY;
	samples++;
	drdy_update();
	return true;				/* keep repeating */
}

static void start_timer(repeating_timer_t *t)
{
	/* negative period = measured start-to-start, so no drift */
	add_repeating_timer_ms(-(int32_t)regs[REG_ODR] * 10, sample_cb, NULL, t);
}

int main(void)
{
	repeating_timer_t timer;
	absolute_time_t next_print;

	stdio_init_all();

	gpio_init(DRDY_PIN);
	gpio_set_dir(DRDY_PIN, GPIO_OUT);
	gpio_put(DRDY_PIN, 0);

	gpio_init(SDA_PIN);
	gpio_set_function(SDA_PIN, GPIO_FUNC_I2C);
	gpio_pull_up(SDA_PIN);
	gpio_init(SCL_PIN);
	gpio_set_function(SCL_PIN, GPIO_FUNC_I2C);
	gpio_pull_up(SCL_PIN);

	regs[REG_WHOAMI] = WHOAMI_VAL;
	regs[REG_ODR] = 10;			/* 100 ms */

	i2c_init(I2C_PORT, I2C_BAUD);
	i2c_slave_init(I2C_PORT, I2C_ADDR, &i2c_handler);

	start_timer(&timer);
	next_print = make_timeout_time_ms(1000);

	for (;;) {
		if (odr_changed) {
			odr_changed = false;
			cancel_repeating_timer(&timer);
			start_timer(&timer);
		}
		if (time_reached(next_print)) {
			next_print = make_timeout_time_ms(1000);
			printf("ctrl=%02x odr=%ums samples=%lu reads=%lu ovr=%lu drdy=%d\n",
			       regs[REG_CTRL], regs[REG_ODR] * 10u,
			       (unsigned long)samples, (unsigned long)host_reads,
			       (unsigned long)overruns, gpio_get(DRDY_PIN));
		}
		__wfi();			/* sleep until the next IRQ */
	}
}
