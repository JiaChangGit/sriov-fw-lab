ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
OUT := $(ROOT)/build/guest
CC ?= gcc
CFLAGS := -std=gnu11 -O2 -g -Wall -Wextra -Werror -Wformat=2 -Wshadow -I$(ROOT)/include

.PHONY: all
all: $(OUT)/m1-spike $(OUT)/lab-exit $(OUT)/labd $(OUT)/labctl $(OUT)/tenant

WIRE := $(ROOT)/guest/common/lab_wire.c
RESET := $(ROOT)/guest/tenant/reset.c $(ROOT)/guest/tenant/recovery.c
HEADERS := $(ROOT)/guest/common/lab_wire.h $(ROOT)/guest/common/lab_ring.h $(ROOT)/include/toy_uapi.h $(ROOT)/include/toy_regs.h

$(OUT)/tenant: $(ROOT)/guest/tenant/tenant.c $(ROOT)/guest/tenant/owner.c $(ROOT)/guest/tenant/dma_fault.c $(ROOT)/guest/tenant/owner.h $(ROOT)/guest/tenant/vfio.c $(ROOT)/guest/tenant/vfio.h $(RESET) $(WIRE) $(HEADERS) | $(OUT)
	$(CC) $(CFLAGS) $(ROOT)/guest/tenant/tenant.c $(ROOT)/guest/tenant/owner.c $(ROOT)/guest/tenant/dma_fault.c $(RESET) $(ROOT)/guest/tenant/vfio.c $(WIRE) -o $@

$(OUT)/labd: $(ROOT)/guest/admin/labd.c $(ROOT)/guest/admin/t06.inc $(ROOT)/guest/admin/recovery.inc $(ROOT)/guest/admin/t02.c $(ROOT)/guest/admin/t02.h $(ROOT)/guest/tenant/vfio.c $(ROOT)/guest/tenant/vfio.h $(WIRE) $(HEADERS) | $(OUT)
	$(CC) $(CFLAGS) $(ROOT)/guest/admin/labd.c $(ROOT)/guest/admin/t02.c $(ROOT)/guest/tenant/vfio.c $(WIRE) -o $@

$(OUT)/labctl: $(ROOT)/guest/admin/labctl.c $(WIRE) $(HEADERS) | $(OUT)
	$(CC) $(CFLAGS) $(ROOT)/guest/admin/labctl.c $(WIRE) -o $@

$(OUT):
	mkdir -p $@

$(OUT)/m1-spike: $(ROOT)/guest/tenant/m1_spike.c $(ROOT)/guest/tenant/vfio.c $(ROOT)/guest/tenant/vfio.h $(ROOT)/include/toy_uapi.h $(ROOT)/include/toy_regs.h | $(OUT)
	$(CC) $(CFLAGS) $(ROOT)/guest/tenant/m1_spike.c $(ROOT)/guest/tenant/vfio.c -o $@

$(OUT)/lab-exit: $(ROOT)/guest/tenant/lab_exit.c | $(OUT)
	$(CC) $(CFLAGS) $< -o $@
