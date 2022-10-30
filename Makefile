# If RACK_DIR is not defined when calling the Makefile, default to two directories above
RACK_DIR ?= ../..

# FLAGS will be passed to both the C and C++ compiler
FLAGS += -std=c++14 -Idep/chowdsp_wdf/include -Idep/slime4rack/dep/slime4rack/include
CFLAGS +=
CXXFLAGS +=

# Careful about linking to shared libraries, since you can't assume much about the user's environment and library search path.
# Static libraries are fine, but they should be added to this plugin's build system.
LDFLAGS +=

# Add .cpp files to the build
SOURCES += $(wildcard src/*.cpp)

slime4rack := dep/slime4rack
DEPS += $(slime4rack)
$(slime4rack):
	git clone https://gitlab.com/slimechild/substation-opensource dep/slime4rack

chowdsp_wdf := dep/chowdsp_wdf
DEPS += $(chowdsp_wdf)
$(chowdsp_wdf):
	git clone https://github.com/Chowdhury-DSP/chowdsp_wdf dep/chowdsp_wdf

# Add files to the ZIP package when running `make dist`
# The compiled plugin and "plugin.json" are automatically added.
DISTRIBUTABLES += res
DISTRIBUTABLES += $(wildcard LICENSE*)
DISTRIBUTABLES += $(wildcard presets)

# Include the Rack plugin Makefile framework
include $(RACK_DIR)/plugin.mk
