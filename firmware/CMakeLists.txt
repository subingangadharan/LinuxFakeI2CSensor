cmake_minimum_required(VERSION 3.13)

# Pico 2 (RP2350). For the Arm cores this selects rp2350-arm-s.
set(PICO_BOARD pico2 CACHE STRING "Board type")

# Copy pico_sdk_import.cmake from $PICO_SDK_PATH/external/ next to this file.
include(pico_sdk_import.cmake)

project(fakesensor C CXX ASM)
set(CMAKE_C_STANDARD 11)

pico_sdk_init()

add_executable(fakesensor main.c)

target_link_libraries(fakesensor
	pico_stdlib
	pico_i2c_slave
	hardware_i2c
)

# Debug prints over USB serial (ttyACM on the host); keep UART free.
pico_enable_stdio_usb(fakesensor 1)
pico_enable_stdio_uart(fakesensor 0)

pico_add_extra_outputs(fakesensor)	# produces fakesensor.uf2
