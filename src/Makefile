# Engine build: one static library + CLI tools. C++17, no deps.
# CMake is not installed in the analysis container, so this is a plain Makefile.
CXX      ?= g++
AR       ?= ar
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic
CPPFLAGS += -I.

BUILD := build

LIB_SRC  := script/classfile.cpp script/vm.cpp script/packages.cpp script/natives.cpp \
            script/compiler/lexer.cpp script/compiler/decl.cpp \
            script/compiler/body.cpp script/compiler/emit.cpp \
            resource/rpk.cpp resource/rdb.cpp resource/manager.cpp \
            parts/cfg.cpp physics/shape.cpp physics/world.cpp physics/mesh.cpp \
            physics/vclip.cpp \
            mesh/scx.cpp render/device.cpp render/scene.cpp render/ui.cpp \
            render/recording.cpp input/backend.cpp input/device.cpp \
            input/controller.cpp input/hotkeys.cpp input/cursor.cpp \
            input/platform_backend.cpp vehicle/vehicle.cpp vehicle/dyno.cpp app/boot.cpp \
            game/runtime.cpp game/car.cpp game/traffic.cpp game/pedestrians.cpp \
            platform/fs.cpp \
            audio/audio.cpp audio/sfx.cpp audio/music.cpp audio/mixer.cpp \
            audio/dsound.cpp audio/sound.cpp
LIB_OBJ  := $(LIB_SRC:%.cpp=$(BUILD)/%.o)
LIB      := $(BUILD)/libengine.a

TOOLS    := classdump rpkdump resman cfgdump vmboot scxcheck
TOOL_BIN := $(TOOLS:%=$(BUILD)/%)
TOOL_OBJ := $(TOOLS:%=$(BUILD)/tools/%.o)

DEPS := $(LIB_OBJ:.o=.d) $(TOOL_OBJ:.o=.d)

# Shipped samples (bin/ is never committed); used by `make check`.
GAME := ../bin/the game
SAMPLES := "$(GAME)/sl/Scripts/game/Init.class" \
           "$(GAME)/cars/racers/Axis_data/scripts/Axis_FL_door.class"
CORPUS := "$(GAME)/system/scripts/lang/Object.class" \
          "$(GAME)/system/scripts/lang/Math.class" \
          "$(GAME)/system/scripts/lang/Vector3.class" \
          "$(GAME)/system/scripts/lang/Ypr.class" \
          "$(GAME)/system/scripts/lang/System.class" \
          "$(GAME)/system/scripts/lang/String.class" \
          "$(GAME)/system/scripts/lang/Integer.class" \
          "$(GAME)/system/scripts/lang/Float.class"

# rpkdump --selfcheck takes the two shipped package pairs from the repo root.
ROOT_CHECK := --selfcheck "bin/the game/system.rpk" "bin/the game/system.rdb" \
              "bin/the game/cars/racers/Axis.rpk" "bin/the game/cars/racers/Axis.rdb"

all: $(TOOL_BIN)

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(LIB): $(LIB_OBJ)
	$(AR) rcs $@ $^

$(BUILD)/%: $(BUILD)/tools/%.o $(LIB)
	$(CXX) $(CXXFLAGS) $< $(LIB) -o $@

check: all
	$(BUILD)/classdump --selfcheck $(SAMPLES)
	$(BUILD)/classdump --corpus $(CORPUS)
	$(BUILD)/classdump --natives-check ../analysis/natives.tsv
	$(BUILD)/classdump --conformance "$(GAME)"
	cd .. && src/$(BUILD)/classdump --parse-java "bin/the game"
	# compiler check (skeleton, CONS order, TREE bodies): report only
	cd .. && src/$(BUILD)/classdump --compile-check "bin/the game"
	cd .. && src/$(BUILD)/rpkdump $(ROOT_CHECK)
	cd .. && src/$(BUILD)/resman --selfcheck
	cd .. && src/$(BUILD)/resman --sounds "bin/the game"
	cd .. && src/$(BUILD)/cfgdump --selfcheck
	# VM runaway once ate all RAM+swap and froze the CT: cap at 1.5 GB / 5 min
	cd .. && ulimit -v 1500000 && timeout 300 src/$(BUILD)/vmboot --selfcheck
	cd .. && src/$(BUILD)/scxcheck --selfcheck

clean:
	rm -rf $(BUILD)

-include $(DEPS)
.PHONY: all check clean
