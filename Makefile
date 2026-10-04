# FlightGear 0.9.10 for the PlayStation 3 (PSL1GHT).
#
#   ./fetch_sources.sh     once: download FlightGear, SimGear and PLIB and apply port/patches
#   ./dk make -j8          build/fgfs.run.elf (and the ps3gl self-test build/gltest.run.elf)
#   ./fetch_data.sh        once: the FlightGear base data, with the PS3 changes from port/fgdata
#   ./fetch_scenery.sh     once: the Vienna area from World Scenery 2.12
#   ./pkg.sh [--full]      fgfs-ps3.pkg (--full: with the data, FlightGear-0.9.10-PS3.pkg)
#
# ./dk runs the command in the Docker image with the PS3 toolchain.
# make PS3_DEBUG=1 builds in corruption guards and a big-allocation tracer (port/ps3_libc.c).

R       := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
B       := $(R)/build
PLIB    := $(R)/src/plib-1.8.4/src
SG      := $(R)/src/SimGear-0.3.10
FG      := $(R)/src/FlightGear-0.9.10
PORT    := /ps3dev/portlibs/ppu

CXX     := ppu-g++
CC      := ppu-gcc
AR      := ppu-ar
# -mminimal-toc: one TOC entry per object. Without it the TOC (220 KB) is split
# into several groups, and ld then misses r2 switches on some cross-group calls
# (fgReadAircraft -> SGPath ran on the wrong TOC and malloc read garbage locks).
MACH    := -mcpu=cell -mhard-float -mminimal-toc -ffunction-sections -fdata-sections
# 2006-era code: no optimisations that exploit undefined behaviour (the Morse
# ident loop in Sound/morse.cxx became an endless loop with GCC 7).
OPT     ?= -O2 -fno-strict-aliasing -fno-aggressive-loop-optimizations -fno-delete-null-pointer-checks
INC     := -I$(R)/ps3gl/include -I$(R)/compat -I$(B)/include -I$(B)/include/plib -I$(PORT)/include \
           -I/ps3dev/ppu/include -I/ps3dev/ppu/include/simdmath
DEFS    := $(if $(filter 1,$(PS3_DEBUG)),-DPS3_DEBUG) -D__PS3__ -DHAVE_CONFIG_H -DPKGLIBDIR='"/dev_hdd0/game/FGFS00910/USRDIR/fgdata"'
CXXFLAGS = $(MACH) $(OPT) -w -std=gnu++98 -fpermissive -include ps3_prefix.h $(DEFS) $(INC) $(EXTRA_$(LIBNAME))
CFLAGS   = $(MACH) $(OPT) -w -std=gnu99 $(DEFS) $(INC) $(EXTRA_$(LIBNAME))

all: run
.PHONY: all

ifeq ($(wildcard $(FG)/src/Main/main.cxx),)
$(error the FlightGear sources are missing: run ./fetch_sources.sh first)
endif

# $(call deflib,name,sources) -> $(B)/lib/libname.a from absolute source paths
define deflib
OBJS_$(1) := $$(patsubst $(R)/%,$(B)/obj/%.o,$(2))
$(B)/lib/lib$(1).a: LIBNAME := $(1)
$(B)/lib/lib$(1).a: $$(OBJS_$(1))
	@mkdir -p $$(dir $$@)
	@rm -f $$@
	@$(AR) rcs $$@ $$^
	@echo AR $$(notdir $$@)
$(1): $(B)/lib/lib$(1).a
.PHONY: $(1)
endef

# PLIB's headers are used as <plib/x.h> and as "x.h"
PLIB_HDR := $(B)/include/plib/.stamp
$(PLIB_HDR):
	@mkdir -p $(dir $@)
	@for d in util sg ssg ssgAux fnt pui puAux net js; do ln -sf $(PLIB)/$$d/*.h $(dir $@); done
	@touch $@

$(B)/obj/%.cxx.o: $(R)/%.cxx | $(PLIB_HDR)
	@mkdir -p $(dir $@)
	@echo CXX $*.cxx
	@$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@
$(B)/obj/%.cpp.o: $(R)/%.cpp | $(PLIB_HDR)
	@mkdir -p $(dir $@)
	@echo CXX $*.cpp
	@$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@
$(B)/obj/%.c.o: $(R)/%.c | $(PLIB_HDR)
	@mkdir -p $(dir $@)
	@echo CC $*.c
	@$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# ---- PLIB ----
PLIB_DIRS := util sg ssg ssgAux fnt pui puAux net
# PNG textures through libpng (port/ssgLoadPNG_ps3.cxx) instead of PLIB's glpng loader
PLIB_SRCS := $(filter-out $(PLIB)/ssg/ssgLoadPNG.cxx,$(foreach d,$(PLIB_DIRS),$(wildcard $(PLIB)/$(d)/*.cxx))) \
             $(PLIB)/js/js.cxx $(R)/port/jsPS3.cxx $(R)/port/ssgLoadPNG_ps3.cxx
$(eval $(call deflib,plib,$(PLIB_SRCS)))

# ---- SimGear and FlightGear: source lists come from their Makefile.am files ----
$(B)/sg_srcs.mk: $(R)/gen_srcs.py
	@mkdir -p $(B)
	@python3 $(R)/gen_srcs.py SG_SRCS $(SG)/simgear /threads/ screen/RenderTexture.cpp jpgfactory \
	    serial/serial.cxx thread-posix thread-win32 > $@
$(B)/fg_srcs.mk: $(R)/gen_srcs.py
	@mkdir -p $(B)
	@python3 $(R)/gen_srcs.py FG_SRCS $(FG)/src Main/fg_os Main/metar_main /FDM/SP/ > $@
include $(B)/sg_srcs.mk $(B)/fg_srcs.mk

SG_SRCS += $(R)/port/RenderTexture_ps3.cpp $(R)/port/serial_ps3.cxx $(R)/port/nasal_thread_ps3.c
$(eval $(call deflib,simgear,$(SG_SRCS)))
EXTRA_simgear := -I$(SG) -I$(SG)/simgear -I$(SG)/simgear/nasal

FG_SRCS += $(wildcard $(R)/port/fg/*.cxx)
$(eval $(call deflib,fgfs,$(FG_SRCS)))
EXTRA_fgfs := -I$(SG) -I$(FG)/src -I$(FG)/src/Include -I$(FG) -I$(FG)/src/FDM/JSBSim -I$(R)/ps3gl/include

# ---- ps3gl: OpenGL 1.x on RSX ----
# The compiled shaders are part of the repository (ps3gl/src/shaders.h). Changing
# them needs NVIDIA's Cg runtime, libCg.so, in tools/cg: then run make shaders.
SHD      := $(R)/ps3gl/shaders
SHD_BIN  := $(SHD)/ffp.vpo $(patsubst %.fcg,%.fpo,$(wildcard $(SHD)/*.fcg))
CGCOMP   := LD_LIBRARY_PATH=$(R)/tools/cg cgcomp
shaders:
	@for f in $(SHD)/*.vcg; do echo CG $$f; $(CGCOMP) -v $$f $${f%.vcg}.vpo || exit 1; done
	@for f in $(SHD)/*.fcg; do echo CG $$f; $(CGCOMP) -f $$f $${f%.fcg}.fpo || exit 1; done
	@python3 $(R)/ps3gl/mkshaders.py $(R)/ps3gl/src/shaders.h $(SHD_BIN)
.PHONY: shaders
$(eval $(call deflib,ps3gl,$(R)/ps3gl/src/ps3gl.c $(R)/ps3gl/src/glu.c $(R)/ps3gl/src/dxt.c \
    $(B)/gen/dxt_spu_bin.c))

# ps3gl's SPU program (DXT texture compression), embedded as a byte array
$(B)/spu/dxt_spu.elf: $(R)/ps3gl/spu/dxt_spu.c $(R)/ps3gl/src/stb_dxt.h $(R)/ps3gl/src/dxt.h
	@mkdir -p $(dir $@)
	@echo SPU dxt_spu.c
	@spu-gcc -O3 -Wall -I/ps3dev/spu/include -o $@ $< -L/ps3dev/spu/lib -lsputhread
$(B)/gen/dxt_spu_bin.c: $(B)/spu/dxt_spu.elf
	@mkdir -p $(dir $@)
	@python3 -c "import sys; d = open(sys.argv[1], 'rb').read(); \
	  print('const unsigned char dxt_spu_bin[] __attribute__((aligned(128))) = {' + ','.join(map(str, d)) + '};')" $< > $@

# ---- port glue: controller, OpenAL on the audio port, libc additions, the hangar ----
$(eval $(call deflib,port,$(R)/port/ps3pad.c $(R)/port/al_ps3.c $(R)/port/ps3_libc.c $(R)/port/btg_prefetch.cxx \
    $(R)/port/hangar/hangar.cxx $(R)/port/hangar/http.c $(R)/port/hangar/unzip.c))

EXTRA_port := -I$(SG)

# ---- the program ----
LIBS_ALL := $(B)/lib/libfgfs.a $(B)/lib/libsimgear.a $(B)/lib/libplib.a $(B)/lib/libps3gl.a $(B)/lib/libport.a
# Linked twice: the second pass hands the .got bounds of the first to the
# debug guard in port/ps3_libc.c (same code, so the same layout).
comma := ,
DEBUG_LD := $(if $(filter 1,$(PS3_DEBUG)),-Wl$(comma)--wrap=malloc$(comma)--wrap=calloc$(comma)--wrap=realloc)
# --wrap=exit: a fatal error while an aircraft loads returns to the hangar (port/fg/fg_os_ps3.cxx)
FGFS_LINK = $(CXX) $(MACH) -Wl,--gc-sections -Wl,--no-multi-toc -Wl,--wrap=exit $(DEBUG_LD) -Wl,-Map,$(B)/fgfs.map \
	  -Wl,--whole-archive $(B)/lib/libfgfs.a -Wl,--no-whole-archive \
	  -Wl,--start-group $(B)/lib/libsimgear.a $(B)/lib/libplib.a $(B)/lib/libps3gl.a $(B)/lib/libport.a -Wl,--end-group \
	  -L$(PORT)/lib -L/ps3dev/ppu/lib -lpng -lz -lnet -lio -laudio -lsysutil -lrsx -lgcm_sys -lsysmodule -lrt -llv2 -lm
$(B)/fgfs.elf: $(LIBS_ALL)
	@echo LD $(notdir $@)
	@$(FGFS_LINK) -Wl,--defsym=ps3_got_start=0 -Wl,--defsym=ps3_got_end=0 -o $@.pass1
	@set -- $$(ppu-objdump -h $@.pass1 | awk '$$2==".got"{print $$4, $$3}'); \
	  $(FGFS_LINK) -Wl,--defsym=ps3_got_start=0x$$1 \
	    -Wl,--defsym=ps3_got_end=$$(printf 0x%x $$((0x$$1 + 0x$$2))) -o $@
	@rm -f $@.pass1
elf: $(B)/fgfs.elf
.PHONY: elf

# ---- ps3gl self-test ----
$(B)/gltest.elf: $(R)/gltest/gltest.c $(B)/lib/libps3gl.a $(B)/lib/libport.a
	@echo LD $(notdir $@)
	@$(CC) $(CFLAGS) -I$(R)/ps3gl/include -o $@ $< -L$(B)/lib -lps3gl -lport \
	  -L/ps3dev/ppu/lib -lio -lsysutil -lrsx -lgcm_sys -lsysmodule -lrt -llv2 -lm \
	  -Wl,--defsym=ps3_got_start=0 -Wl,--defsym=ps3_got_end=0
gltest: $(B)/gltest.elf
.PHONY: gltest

# ---- runnable images: stripped + sprxlinker (fixes the import stub tables) ----
$(B)/%.run.elf: $(B)/%.elf
	@echo STRIP+SPRX $(notdir $@)
	@ppu-strip $< -o $@
	@sprxlinker $@ >/dev/null
run: $(B)/gltest.run.elf $(B)/fgfs.run.elf
.PHONY: run

clean:
	rm -rf $(B)
.PHONY: clean

-include $(shell find $(B)/obj -name '*.d' 2>/dev/null)
