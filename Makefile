CC ?= cc
BUILD_DIR ?= build
CHIP_AUDIO ?= ../chip-audio
CPPFLAGS += -Iinclude -I$(CHIP_AUDIO)/include
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Wno-unused-function
all: $(BUILD_DIR)/libchip-sequencer.a
$(BUILD_DIR)/%.o: src/%.c src/json.h $(wildcard include/*.h $(CHIP_AUDIO)/include/*.h)
	mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
$(BUILD_DIR)/libchip-sequencer.a: $(BUILD_DIR)/chip-sequencer.o $(BUILD_DIR)/compiler.o
	ar rcs $@ $^
CHIP_AUDIO_LIB ?= $(CHIP_AUDIO)/build/libchip-audio.a
$(BUILD_DIR)/sequence-test: tests/sequence-test.c $(BUILD_DIR)/libchip-sequencer.a $(CHIP_AUDIO_LIB)
	$(CC) $(CPPFLAGS) $(CFLAGS) -UNDEBUG $< $(BUILD_DIR)/libchip-sequencer.a $(CHIP_AUDIO_LIB) -lm -o $@
$(BUILD_DIR)/compile-test: tests/compile-test.c $(BUILD_DIR)/libchip-sequencer.a $(CHIP_AUDIO_LIB)
	$(CC) $(CPPFLAGS) $(CFLAGS) -UNDEBUG $< $(BUILD_DIR)/libchip-sequencer.a $(CHIP_AUDIO_LIB) -lm -o $@
$(BUILD_DIR)/chip-render: examples/render.c $(BUILD_DIR)/libchip-sequencer.a $(CHIP_AUDIO_LIB)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(BUILD_DIR)/libchip-sequencer.a $(CHIP_AUDIO_LIB) -lm -o $@
.PHONY: test
test: $(BUILD_DIR)/sequence-test $(BUILD_DIR)/compile-test
	$(BUILD_DIR)/sequence-test examples/sequence.json
	$(BUILD_DIR)/compile-test examples/performance.json $(BUILD_DIR)/compiled.json
	$(BUILD_DIR)/sequence-test $(BUILD_DIR)/compiled.json

$(BUILD_DIR)/model-probe: tests/model-probe.c $(BUILD_DIR)/libchip-sequencer.a $(CHIP_AUDIO_LIB)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(BUILD_DIR)/libchip-sequencer.a $(CHIP_AUDIO_LIB) -lm -o $@
test: test-controls
.PHONY: test-controls
test-controls: $(BUILD_DIR)/model-probe
	python3 tests/control-test.py $(BUILD_DIR)/model-probe

$(BUILD_DIR)/diagnostic-probe: tests/diagnostic-probe.c $(BUILD_DIR)/libchip-sequencer.a $(CHIP_AUDIO_LIB)
	$(CC) $(CPPFLAGS) $(CFLAGS) -UNDEBUG $< $(BUILD_DIR)/libchip-sequencer.a $(CHIP_AUDIO_LIB) -lm -o $@
test: test-diagnostics
.PHONY: test-diagnostics
test-diagnostics: $(BUILD_DIR)/diagnostic-probe
	python3 tests/diagnostic-test.py $(BUILD_DIR)/diagnostic-probe
