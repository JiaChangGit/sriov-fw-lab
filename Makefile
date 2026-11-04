SHELL := /bin/bash
.SHELLFLAGS := -euo pipefail -c
.NOTPARALLEL:
.DEFAULT_GOAL := help
ROOT := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
PYTHON ?= python3
JOBS ?= 2
PROFILE ?= stress
ACCEL ?= tcg

.PHONY: help env-check fetch-sources build-core verify-m0 build-qemu build-kernel build-initramfs build-edk2 verify-m1 verify-m2 verify-t02 verify-t03 verify-guest-native clean-build clean-cache
help:
	@echo 'Run inside WSL: make env-check; make fetch-sources; make verify-m0'
	@echo 'Build targets: build-core build-qemu build-kernel build-initramfs build-edk2'
	@echo 'Full supported M0-M3 project: make verify-project JOBS=2 (TCG).'
	@echo 'See docs/BUILD_TUTORIAL_ZH.md and docs/VALIDATION.md.'
env-check:
	$(PYTHON) $(ROOT)tools/env_check.py
fetch-sources:
	$(PYTHON) $(ROOT)tools/fetch_sources.py
build-core:
	$(PYTHON) $(ROOT)tools/build_core.py
verify-m0:
	$(PYTHON) $(ROOT)tools/verify_m0.py
build-qemu:
	$(PYTHON) $(ROOT)tools/build_qemu.py --jobs $(JOBS)
build-kernel:
	$(PYTHON) $(ROOT)tools/build_kernel.py --profile $(PROFILE) --jobs $(JOBS)
build-initramfs:
	$(PYTHON) $(ROOT)tools/build_initramfs.py --profile $(PROFILE) --jobs $(JOBS)
build-edk2:
	$(PYTHON) $(ROOT)tools/build_edk2.py --jobs $(JOBS)
verify-m1: build-qemu build-kernel build-initramfs
	$(PYTHON) $(ROOT)tools/run_vm.py --gate m1 --profile $(PROFILE) --accel $(ACCEL)
verify-m2:
	$(PYTHON) $(ROOT)tools/verify_m2.py --profile $(PROFILE) --jobs $(JOBS) --accel $(ACCEL)
verify-t03:
	$(PYTHON) $(ROOT)tools/verify_t03.py

.PHONY: verify-t01 verify-t04 verify-t05 verify-t06 verify-t09
verify-t01:
	$(PYTHON) $(ROOT)tools/verify_t01.py

verify-t04:
	$(PYTHON) $(ROOT)tools/verify_t04.py
verify-t05:
	$(PYTHON) $(ROOT)tools/verify_t05.py
verify-t06:
	$(PYTHON) $(ROOT)tools/verify_t06.py
.PHONY: verify-t07 verify-t08 verify-m3 build-handbook
verify-t07:
	$(PYTHON) $(ROOT)tools/verify_recovery.py --case T07
verify-t08:
	$(PYTHON) $(ROOT)tools/verify_recovery.py --case T08
verify-m3:
	$(PYTHON) $(ROOT)tools/verify_m3.py
build-handbook:
	$(PYTHON) $(ROOT)tools/build_handbook.py
	$(PYTHON) $(ROOT)tools/check_handbook.py
verify-t09:
	$(PYTHON) $(ROOT)tools/verify_t09.py
verify-t02:
	$(PYTHON) $(ROOT)tools/verify_t02.py
verify-guest-native:
	$(PYTHON) $(ROOT)tools/verify_guest_native.py
clean-build:
	$(PYTHON) $(ROOT)tools/clean.py build
clean-cache:
	$(PYTHON) $(ROOT)tools/clean.py cache
.PHONY: verify-project demo demo-reset
verify-project:
	$(PYTHON) $(ROOT)tools/verify_project.py --jobs $(JOBS)
demo:
	$(PYTHON) $(ROOT)tools/run_vm.py --gate m1 --profile stress --accel tcg
demo-reset:
	$(PYTHON) $(ROOT)tools/verify_recovery.py --case T07
