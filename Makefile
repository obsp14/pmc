# pmc - host build of the management-controller core
#
#   make           core tests + simulator
#   make test      C unit tests + Python end-to-end tests against the simulator
#   make clean
#
# The Zephyr firmware is built with west, see app/README.md.

CC     ?= gcc
CFLAGS ?= -O2 -g
CFLAGS += -std=c99 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion \
          -Icore/include
B      := build
CORE   := core/src/crc.c core/src/mctp.c core/src/raslog.c core/src/monitor.c core/src/pldm.c

all: $(B)/test_core $(B)/pmc-sim

$(B):
	@mkdir -p $@

$(B)/test_core: tests/test_core.c $(CORE) | $(B)
	$(CC) $(CFLAGS) -fsanitize=address,undefined $^ -o $@

$(B)/pmc-sim: sim/pmc_sim.c $(CORE) | $(B)
	$(CC) $(CFLAGS) -Wno-conversion -DPMC_ALLOW_TEST_INJECT $^ -o $@ -lcrypto -lm

test: all
	$(B)/test_core
	cd tools/pmctl && python3 -m pytest -q tests

clean:
	rm -rf $(B)

.PHONY: all test clean
