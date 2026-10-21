# fakesensor — Pico 2 fake I2C sensor + Linux driver with a data-ready IRQ

The Pico 2 pretends to be an I2C sensor. A Raspberry Pi (4 or 5) running Linux is the host,
and you write the kernel driver for it.

```
Pico 2 (firmware/)                      Raspberry Pi (driver/)
  timer IRQ every ODR -> new sample
  DRDY pin high  --------------------->  GPIO17 level-high IRQ
                                          hard IRQ: timestamp, IRQ_WAKE_THREAD
  I2C target @0x42 <-------------------   IRQ thread: burst read STATUS..DATA_H
  read of DATA_H drops DRDY                kfifo -> wake_up -> read()/poll()
```

## Wiring (3.3 V on both sides, no level shifter needed)

| Signal | Pico 2 | Raspberry Pi header |
|---|---|---|
| SDA  | GP4 (pin 6) | GPIO2 / SDA1 (pin 3) |
| SCL  | GP5 (pin 7) | GPIO3 / SCL1 (pin 5) |
| DRDY | GP6 (pin 9) | GPIO17 (pin 11) |
| GND  | GND (pin 8) | GND (pin 6) |

The Pi already has 1.8 kΩ pull-ups on SDA1/SCL1. Power the Pico from its own USB, which also
gives you its debug prints on `/dev/ttyACM0`.

### Orange Pi 5 (RK3588S, 26-pin header) instead of a Raspberry Pi

| Signal | Pico 2 | Orange Pi 5 header | SoC pin |
|---|---|---|---|
| SDA  | GP4 (pin 6) | pin 3 | GPIO1_B7 = I2C5_SDA_M3 |
| SCL  | GP5 (pin 7) | pin 5 | GPIO1_B6 = I2C5_SCL_M3 |
| DRDY | GP6 (pin 9) | pin 7 | GPIO1_C6 (Linux gpio 54) |
| GND  | GND (pin 8) | pin 6 | — |

- Add **4.7 kΩ pull-ups from SDA and SCL to 3.3 V (OPi pin 1)**. The Pico's internal pull-ups are
  too weak to rely on.
- Use `driver/fakesensor-opi5-overlay.dts` instead of the Pi overlay. It enables I2C5, routes it to
  pins 3/5 (`i2c5m3_xfer`), and puts the sensor at 0x42 with its IRQ on `&gpio1` pin 22 (RK_PC6),
  level high. It was checked by merging it into the mainline v7.2 `rk3588s-orangepi-5.dtb` with `fdtoverlay`.
- Install it with `sudo orangepi-add-overlay fakesensor-opi5-overlay.dts` (Orange Pi OS) or
  `sudo armbian-add-overlay fakesensor-opi5-overlay.dts` (Armbian), then reboot. Don't also
  enable the vendor `i2c5-m3` overlay; this one already does that.
- Kernel headers: on Armbian use `armbian-config` → install headers. On Orange Pi OS, install
  the `linux-headers-*` .deb that ships with the image (check `/opt`).
- Check it: `i2cdetect -y 5` should show 42, and `grep fakesensor /proc/interrupts` should show the
  `rockchip_gpio_irq` chip.
- Bonus for interviews: on this board the IRQ path is GPIO bank → **GICv3** (SPI). Compare it with
  the x86 IOAPIC → LAPIC path you studied: `cat /proc/interrupts`, and
  `/sys/kernel/debug/irq/irqs/<N>` shows the domain hierarchy.

## Register map

| Addr | Name | Access | Meaning |
|---|---|---|---|
| 0x00 | WHO_AM_I | RO | 0x5A |
| 0x01 | CTRL | RW | bit0 EN, bit1 INT_EN (drive DRDY) |
| 0x02 | ODR | RW | period in 10 ms units (default 10 = 100 ms) |
| 0x03 | STATUS | RO | bit0 DRDY, bit1 OVR; reading it snapshots 0x03..0x06 |
| 0x04 | COUNT | RO | sample sequence number |
| 0x05 | DATA_L | RO | sample low byte (s16) |
| 0x06 | DATA_H | RO | sample high byte; reading it clears DRDY and drops the pin |

The register pointer auto-increments, so a single write-then-read transfer reads a burst.

## Step 1 — firmware (on your PC)

```sh
# needs pico-sdk 2.x + arm-none-eabi-gcc (or the VS Code Pico extension)
cd firmware
cp $PICO_SDK_PATH/external/pico_sdk_import.cmake .
mkdir build && cd build
cmake .. && make -j
# hold BOOTSEL, plug the Pico in, copy fakesensor.uf2 to the RP2350 drive
```

## Step 2 — prove the bus works before writing any driver code (on the Pi)

```sh
sudo raspi-config nonint do_i2c 0        # enable i2c-1
sudo apt install i2c-tools
i2cdetect -y 1                           # expect 42 in the grid
i2cget -y 1 0x42 0x00                    # expect 0x5a
i2cset -y 1 0x42 0x01 0x03               # EN | INT_EN -> DRDY starts toggling
i2cget -y 1 0x42 0x03                    # STATUS, bit0 set
i2cset -y 1 0x42 0x01 0x00               # stop again before loading the driver
```

Put the logic analyzer on SDA/SCL/DRDY in PulseView (I2C decoder) and watch the
write-pointer → repeated START → 4-byte read, and DRDY falling right after DATA_H.

## Step 3 — overlay + driver (on the Pi)

```sh
# Pi 4, 64-bit Raspberry Pi OS: linux-headers-rpi-v8 ; Pi 5: linux-headers-rpi-2712
sudo apt install linux-headers-rpi-v8 device-tree-compiler
cd driver
make                                     # builds fakesensor.ko and fakesensor.dtbo
sudo insmod fakesensor.ko
sudo dtoverlay -d . fakesensor           # creates the i2c client -> probe runs
dmesg | tail                             # "fakesensor ready, irq NN, /dev/fakesensor"
```

## Step 4 — check it

```sh
grep fakesensor /proc/interrupts         # count rising ~10/s, "Level"
ps -eo pid,cls,rtprio,comm | grep irq/   # irq/NN-fakesensor  FF  50
cat /sys/bus/i2c/devices/1-0042/stats
echo 20 | sudo tee /sys/bus/i2c/devices/1-0042/odr_ms    # 50 Hz
cd ../test && gcc -O2 -I../driver -o fs_read fs_read.c && sudo ./fs_read 100
```

## Step 5 — experiments (this is where the interview material comes from)

1. **Drop `IRQF_ONESHOT`** while keeping `fs_irq_hard`. The level line is unmasked at EOI while
   DRDY is still high, so it fires again immediately. Watch `/proc/interrupts` explode and
   `hardirqs` ≫ `threads` in `stats`. Then pass `NULL` as the primary handler without ONESHOT and
   see `request_threaded_irq` refuse it ("handler=NULL and !ONESHOT").
2. **Do the I2C read in the hard handler.** With `CONFIG_DEBUG_ATOMIC_SLEEP=y` you get
   "BUG: sleeping function called from invalid context" — regmap-i2c takes a mutex.
3. **Switch the DT to edge** (`interrupts = <17 1>`). Set ODR to 10 ms and load the CPU so a
   read is late: DRDY is already high when the next sample comes, so there's no new edge and the stream
   **stalls forever**. Fix it by re-reading STATUS at the end of the thread and looping while DRDY.
   That's the classic lost-edge bug.
4. **Latency.** `sudo trace-cmd record -e irq:irq_handler_entry -e irq:irq_handler_exit -e sched:sched_wakeup -e i2c:i2c_read`
   and measure hard IRQ → thread wakeup → I2C done. Repeat under `stress-ng --cpu 4` and with
   `chrt` changing the IRQ thread priority.
5. **Overruns.** Set ODR to 10 ms, stop reading, and watch `fifo_drops`, then `dev_overruns` and
   `MISSED` in `fs_read`.
6. **Unbind while reading** (`echo 1-0042 | sudo tee /sys/bus/i2c/drivers/fakesensor/unbind` while
   `fs_read` blocks). That's a use-after-free, marked EXERCISE in the driver. Fix it with a kref plus a
   "gone" flag that wakes readers.
7. **Stretch goals:** enable the sensor only while `/dev/fakesensor` is open (runtime PM), convert
   to IIO with a triggered buffer, or replace the triangle wave with the RP2350's on-chip
   temperature sensor.

## Gotchas

- On a Pi 3 or 4, the BCM I2C controller handles clock stretching badly, and the Pico stretches
  SCL while it fills its TX FIFO. If you see NAKs or garbage, lower the speed with
  `dtparam=i2c_arm_baudrate=50000`, or use the `i2c-gpio` overlay. A Pi 5 (RP1 controller)
  doesn't have this problem.
- On a Pi 5 the header GPIOs belong to RP1. Check that `&gpio` in the overlay resolves to the
  header GPIO controller there, or retarget it.
- Clear the stale DRDY: if the Pico was left enabled from Step 2, the line may already be high
  when the IRQ is requested. The driver writes CTRL=0 first for this reason.
