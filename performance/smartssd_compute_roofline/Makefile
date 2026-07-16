SHELL := /bin/bash

PLATFORM ?= xilinx_u2_gen3x4_xdma_gc_2_202110_1
TARGET ?= hw
LANES ?= 320
MEM_TAG ?= bank0
FREQ_MHZ ?= 300

VPP ?= v++
CXX ?= g++

PLATFORM_ID := $(basename $(notdir $(PLATFORM)))
sanitize = $(subst /,_,$(subst :,_,$(subst [,_,$(subst ],_,$(1)))))
CONFIG_ID := l$(LANES).m$(call sanitize,$(MEM_TAG)).f$(FREQ_MHZ).frp
TEMP_DIR := _x.$(TARGET).$(PLATFORM_ID).$(CONFIG_ID)
BUILD_DIR := build_dir.$(TARGET).$(PLATFORM_ID).$(CONFIG_ID)
PACKAGE_DIR := package.$(TARGET).$(PLATFORM_ID).$(CONFIG_ID)

KERNEL := peak_compute
XO := $(TEMP_DIR)/$(KERNEL).xo
LINK_XCLBIN := $(BUILD_DIR)/$(KERNEL).link.xclbin
XCLBIN := $(BUILD_DIR)/$(KERNEL).xclbin
HOST_EXE := smartssd_peak_compute

CXXFLAGS += -O2 -g -Wall -Wextra -std=c++17
CXXFLAGS += -I$(XILINX_XRT)/include
CXXFLAGS += -DLANES=$(LANES)
CXXFLAGS += -DKERNEL_CLOCK_MHZ=$(FREQ_MHZ)
LDFLAGS += -L$(XILINX_XRT)/lib -Wl,-rpath,$(XILINX_XRT)/lib
LDFLAGS += -pthread -lrt -luuid -lxrt_coreutil

VPP_COMMON := -t $(TARGET) --platform $(PLATFORM) --save-temps
VPP_DEFINES := -D LANES=$(LANES)
VPP_CONNECTIVITY := --connectivity.nk $(KERNEL):1:$(KERNEL)_1

.PHONY: all host build xclbin run emconfig report help clean cleanall check-env

all: host xclbin $(if $(filter hw_emu,$(TARGET)),emconfig)

host: check-env
	$(CXX) src/host.cpp -o $(HOST_EXE) $(CXXFLAGS) $(LDFLAGS)

build xclbin: $(XCLBIN)

$(XO): src/peak_compute.cpp Makefile | check-env
	mkdir -p $(TEMP_DIR)
	$(VPP) -c $(VPP_COMMON) $(VPP_FLAGS) $(VPP_DEFINES) \
		-k $(KERNEL) --temp_dir $(TEMP_DIR) -o $@ $<

$(LINK_XCLBIN): $(XO) Makefile
	mkdir -p $(BUILD_DIR)
	$(VPP) -l $(VPP_COMMON) $(VPP_FLAGS) --kernel_frequency $(FREQ_MHZ) \
		$(VPP_CONNECTIVITY) --temp_dir $(TEMP_DIR) -o $@ $<

$(XCLBIN): $(LINK_XCLBIN)
	$(VPP) -p $(LINK_XCLBIN) $(VPP_COMMON) $(VPP_FLAGS) \
		--package.out_dir $(PACKAGE_DIR) -o $@

emconfig: $(TEMP_DIR)/emconfig.json

$(TEMP_DIR)/emconfig.json: | check-env
	mkdir -p $(TEMP_DIR)
	emconfigutil --platform $(PLATFORM) --od $(TEMP_DIR)

run: all
ifeq ($(TARGET),hw_emu)
	cp $(TEMP_DIR)/emconfig.json .
	XCL_EMULATION_MODE=hw_emu ./$(HOST_EXE) -x $(XCLBIN) $(RUN_ARGS)
else
	./$(HOST_EXE) -x $(XCLBIN) $(RUN_ARGS)
endif

report:
	@find $(TEMP_DIR) $(BUILD_DIR) -type f \
		\( -name '*csynth.rpt' -o -name '*system_estimate*.xtxt' \
		-o -name '*utilization*.rpt' -o -name '*timing_summary*.rpt' \) \
		-print 2>/dev/null || true

check-env:
	@test -n "$(XILINX_VITIS)" || \
		(echo "ERROR: XILINX_VITIS is not set; source Vitis settings64.sh"; exit 1)
	@test -n "$(XILINX_XRT)" || \
		(echo "ERROR: XILINX_XRT is not set; source XRT setup.sh"; exit 1)

help:
	@echo "SmartSSD peak-compute benchmark"
	@echo ""
	@echo "Build:"
	@echo "  make PLATFORM=xilinx_u2_gen3x4_xdma_gc_2_202110_1 LANES=320 MEM_TAG=bank0"
	@echo ""
	@echo "Run:"
	@echo "  make run PLATFORM=xilinx_u2_gen3x4_xdma_gc_2_202110_1 LANES=320 MEM_TAG=bank0"
	@echo "  make run ... RUN_ARGS='--iterations 200000000 --repeats 5'"
	@echo ""
	@echo "Tunable parameters: TARGET, LANES, MEM_TAG, FREQ_MHZ"

clean:
	rm -rf $(HOST_EXE) emconfig.json *.log *.jou *.csv *.run_summary .Xil

cleanall: clean
	rm -rf _x.* build_dir.* package.*
