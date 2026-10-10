CC ?= cc
# gnu11, not c11: the compass_* log macros use `, ##__VA_ARGS__`.
CFLAGS += -std=gnu11 -Wall -Wextra -Werror -pedantic
# src/ for the common private header, and every chip folder, because the
# tests include a chip's private headers by plain name.
CPPFLAGS += -I../include -I../src $(addprefix -I,$(wildcard ../src/*/))

# The chip drivers are the same C on both platforms, so their suites run once,
# through src/compass_port_stm.c against hal_stubs/ (stm/). esp/ checks only
# what src/compass_port_esp.c does differently, against idf_stubs/. The maths
# tests touch no bus or platform API; two of them include a public header,
# which wants a family header, so they build with the HAL stub's flags too.

# What stm-forge passes: the family header the code includes, the HAL module
# an app's hal_conf would enable, and every chip option on (COMPASS_<CHIP> = y).
STM_CPPFLAGS := -Ihal_stubs \
		-DCMSIS_device_header='"stm32_hal_stub.h"' \
		-DHAL_I2C_MODULE_ENABLED -DHAL_SPI_MODULE_ENABLED \
		-DHAL_UART_MODULE_ENABLED \
		-DCONFIG_COMPASS_QMC5883L=1 -DCONFIG_COMPASS_QMC5883P=1 \
		-DCONFIG_COMPASS_IST8310=1 -DCONFIG_COMPASS_LIS3MDL=1 \
		-DCONFIG_COMPASS_CMPS14=1 \
		-DCONFIG_COMPASS_DEBUG=1
# What ESP-IDF passes: ESP_PLATFORM, and the chips the test probes.
ESP_CPPFLAGS := -Iidf_stubs -DESP_PLATFORM -DCONFIG_COMPASS_QMC5883L=1 \
		-DCONFIG_COMPASS_LIS3MDL=1 -DCONFIG_COMPASS_CMPS14=1

# The core and the stubs every binary of a suite links.
CORE     := ../src/compass.c ../src/compass_log.c
STM_CORE := $(CORE) ../src/compass_port_stm.c hal_stubs/hal_stubs.c
ESP_CORE := $(CORE) ../src/compass_port_esp.c idf_stubs/idf_stubs.c

MATH_TESTS := test_qmc5883l_math test_qmc5883p_math test_ist8310_math \
	      test_lis3mdl_math test_cmps14_math
ESP_TESTS  := esp/test_port_esp
STM_TESTS  := $(addprefix stm/,test_core test_qmc5883l test_qmc5883p \
		test_ist8310 test_lis3mdl test_cmps14 test_port_stm)
TESTS      := $(MATH_TESTS) $(ESP_TESTS) $(STM_TESTS)

.PHONY: all clean test

all: $(TESTS)

test_qmc5883l_math: test_qmc5883l_math.c
test_qmc5883p_math: test_qmc5883p_math.c
test_ist8310_math: test_ist8310_math.c
test_lis3mdl_math: test_lis3mdl_math.c
test_cmps14_math: test_cmps14_math.c

esp/test_port_esp: esp/test_port_esp.c ../src/qmc5883l/qmc5883l.c \
		   ../src/lis3mdl/lis3mdl.c ../src/cmps14/cmps14.c $(ESP_CORE)

stm/test_core: stm/test_core.c $(STM_CORE)
stm/test_qmc5883l: stm/test_qmc5883l.c ../src/qmc5883l/qmc5883l.c \
		   $(STM_CORE)
stm/test_qmc5883p: stm/test_qmc5883p.c ../src/qmc5883p/qmc5883p.c \
		   $(STM_CORE)
stm/test_ist8310: stm/test_ist8310.c ../src/ist8310/ist8310.c $(STM_CORE)
stm/test_lis3mdl: stm/test_lis3mdl.c ../src/lis3mdl/lis3mdl.c $(STM_CORE)
stm/test_cmps14: stm/test_cmps14.c ../src/cmps14/cmps14.c $(STM_CORE)
stm/test_port_stm: stm/test_port_stm.c $(STM_CORE)

$(MATH_TESTS) $(STM_TESTS): CPPFLAGS += $(STM_CPPFLAGS)
$(ESP_TESTS): CPPFLAGS += $(ESP_CPPFLAGS)

# Every printf() in a binary that links the stubs also lands in
# stub_log_last, so a test can check what a driver logged. GNU ld.
stm/test_core stm/test_qmc5883l stm/test_qmc5883p stm/test_ist8310 \
	stm/test_lis3mdl stm/test_cmps14: LDFLAGS += -Wl,--wrap=printf
# Lets the driver tests count calloc() and free() calls, to prove a refused
# probe allocates nothing and a failed one leaks nothing. Only the drivers'
# own references are wrapped.
stm/test_qmc5883l stm/test_qmc5883p stm/test_ist8310 stm/test_lis3mdl \
	stm/test_cmps14: LDFLAGS += -Wl,--wrap=calloc -Wl,--wrap=free

$(TESTS):
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $(filter %.c,$^) -o $@ -lm

test: $(TESTS)
	@set -e; for t in $(TESTS); do echo "./$$t"; ./$$t; done

clean:
	rm -f $(TESTS)

# Headers are prerequisites so that editing one rebuilds. Without this, `make
# test` reruns a stale binary and reports green for code it never compiled.
# Recipes use $(filter %.c,$^) so the headers never reach the compiler.
HDRS  := $(wildcard ../include/*.h ../src/*.h ../src/*/*.h)
STUBS := $(wildcard hal_stubs/*.h idf_stubs/*.h idf_stubs/*/*.h)
$(TESTS): $(HDRS) $(STUBS)
