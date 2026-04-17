CC = gcc
BUILD_TIMESTAMP := $(shell date +"%Y%m%d%H%M%S")
BASE_CFLAGS = -Wall -Wextra -O3 -I./include -I/usr/include/libpng16 -pthread \
              -fstack-protector-strong -D_FORTIFY_SOURCE=2 \
              -DBUILD_VERSION_STRING=\"$(BUILD_TIMESTAMP)-64\"
EXTRA_CFLAGS =
CFLAGS = $(BASE_CFLAGS) $(EXTRA_CFLAGS)
LDFLAGS = -lm -lpthread -lssl -lcrypto -lasound -lfftw3f -lpng16
USE_VULKAN ?= 0
USE_SOAPYSDR ?= 0

ifeq ($(USE_VULKAN),1)
CFLAGS += -DUSE_VULKAN -DVKFFT_BACKEND=0
LDFLAGS += -lvulkan \
           -lglslang -lSPIRV -lMachineIndependent -lGenericCodeGen -lOSDependent \
           -lglslang-default-resource-limits \
           -lSPIRV-Tools-opt -lSPIRV-Tools -lSPIRV-Tools-link -lSPIRV-Tools \
           -lstdc++
endif

ifeq ($(USE_SOAPYSDR),1)
CFLAGS += -DUSE_SOAPYSDR
LDFLAGS += -lSoapySDR
endif

SRC_DIR = src
OBJ_DIR = obj
INC_DIR = include

SRCS = $(wildcard $(SRC_DIR)/*.c)
OBJS = $(patsubst $(SRC_DIR)/%.c, $(OBJ_DIR)/%.o, $(SRCS))
DEPS = $(OBJS:.o=.d)

TARGET = vertexsdr

BUILD_FLAGS := $(CC) $(CFLAGS) $(LDFLAGS)
FLAG_FILE := $(OBJ_DIR)/.build_flags

all: $(TARGET)

profile:
	$(MAKE) clean
	$(MAKE) EXTRA_CFLAGS="-g1 -fno-omit-frame-pointer" all

profile-vulkan:
	$(MAKE) clean
	$(MAKE) USE_VULKAN=1 EXTRA_CFLAGS="-g1 -fno-omit-frame-pointer" all

profile-soapysdr:
	$(MAKE) clean
	$(MAKE) USE_SOAPYSDR=1 EXTRA_CFLAGS="-g1 -fno-omit-frame-pointer" all

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c $(FLAG_FILE) | $(OBJ_DIR)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

$(FLAG_FILE): FORCE | $(OBJ_DIR)
	@echo '$(BUILD_FLAGS)' | cmp -s - $@ || echo '$(BUILD_FLAGS)' > $@

FORCE:

shaders: shaders/wf_power.comp shaders/wf_downsample.comp shaders/wf_expand.comp
	glslangValidator -V shaders/wf_power.comp -o shaders/wf_power.spv
	glslangValidator -V shaders/wf_downsample.comp -o shaders/wf_downsample.spv
	glslangValidator -V shaders/wf_expand.comp -o shaders/wf_expand.spv
	xxd -i shaders/wf_power.spv > shaders/wf_power_spv.h
	xxd -i shaders/wf_downsample.spv > shaders/wf_downsample_spv.h
	xxd -i shaders/wf_expand.spv > shaders/wf_expand_spv.h

clean:
	rm -rf $(OBJ_DIR) $(TARGET)

.PHONY: all clean shaders profile profile-vulkan

-include $(DEPS)
