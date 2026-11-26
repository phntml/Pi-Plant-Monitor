CC      ?= cc
AR      ?= ar
ARFLAGS := rcs

CFLAGS   := -std=c17 -Wall -Wextra -Werror -Wvla
DEPFLAGS := -MMD -MP
CPPFLAGS := -Isrc/common -Isrc/sensors -Isrc/store

SANFLAGS ?=
CFLAGS   += $(SANFLAGS)
LDFLAGS  += $(SANFLAGS)

ASAN_FLAGS := -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -g

VALGRIND := valgrind --leak-check=full --track-fds=yes --trace-children=yes --error-exitcode=1

BUILD ?= build
OBJ   := $(BUILD)/obj
LIB   := $(BUILD)/lib
BIN   := $(BUILD)/bin

COMMON_SRCS     := $(wildcard src/common/*.c)
SENSOR_MAINS    := src/sensors/light_proc.c src/sensors/soil_proc.c
SENSOR_SRCS     := $(filter-out $(SENSOR_MAINS),$(wildcard src/sensors/*.c))
STORE_SRCS      := $(wildcard src/store/*.c)
HUB_SRCS        := src/hub/hub.c
SUPERVISOR_SRCS := src/supervisor/supervisor.c

objs = $(patsubst src/%.c,$(OBJ)/%.o,$(1))

COMMON_OBJS     := $(call objs,$(COMMON_SRCS))
SENSOR_OBJS     := $(call objs,$(SENSOR_SRCS))
STORE_OBJS      := $(call objs,$(STORE_SRCS))
HUB_OBJS        := $(call objs,$(HUB_SRCS))
SUPERVISOR_OBJS := $(call objs,$(SUPERVISOR_SRCS))

LIBCOMMON := $(LIB)/libcommon.a
LIBSENSOR := $(LIB)/libsensor.a
LIBSTORE  := $(LIB)/libstore.a
PROGS     := $(BIN)/light_proc $(BIN)/soil_proc $(BIN)/hub $(BIN)/supervisor

TEST_SRCS := $(wildcard tests/*.c)
TEST_BINS := $(patsubst tests/%.c,$(BIN)/tests/%,$(TEST_SRCS))
TEST_OBJS := $(patsubst tests/%.c,$(OBJ)/tests/%.o,$(TEST_SRCS))

.SECONDARY: $(TEST_OBJS)

.PHONY: all sensors hub supervisor test asan memcheck clean

all: $(PROGS)

sensors: $(BIN)/light_proc $(BIN)/soil_proc
hub: $(BIN)/hub
supervisor: $(BIN)/supervisor

$(OBJ)/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(LIBCOMMON): $(COMMON_OBJS)
	@mkdir -p $(@D)
	$(AR) $(ARFLAGS) $@ $^

$(LIBSENSOR): $(SENSOR_OBJS)
	@mkdir -p $(@D)
	$(AR) $(ARFLAGS) $@ $^

$(LIBSTORE): $(STORE_OBJS)
	@mkdir -p $(@D)
	$(AR) $(ARFLAGS) $@ $^

$(BIN)/light_proc: $(OBJ)/sensors/light_proc.o $(LIBSENSOR) $(LIBCOMMON)
$(BIN)/soil_proc: $(OBJ)/sensors/soil_proc.o $(LIBSENSOR) $(LIBCOMMON)
$(BIN)/light_proc $(BIN)/soil_proc: PROG_LIBS := -lsensor
$(BIN)/hub: $(HUB_OBJS) $(LIBSTORE) $(LIBCOMMON)
$(BIN)/hub: PROG_LIBS := -lstore
$(BIN)/supervisor: $(SUPERVISOR_OBJS) $(LIBCOMMON)

$(PROGS):
	@mkdir -p $(@D)
	$(CC) $(LDFLAGS) -o $@ $(filter %.o,$^) -L$(LIB) $(PROG_LIBS) -lcommon $(LDLIBS)

$(OBJ)/tests/%.o: tests/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(BIN)/tests/%: $(OBJ)/tests/%.o $(LIBSENSOR) $(LIBSTORE) $(LIBCOMMON)
	@mkdir -p $(@D)
	$(CC) $(LDFLAGS) -o $@ $< -L$(LIB) -lsensor -lstore -lcommon $(LDLIBS)

# Some tests run the daemons, so build them first.
test: $(TEST_BINS) $(PROGS)
	@fail=0; \
	for t in $(TEST_BINS); do \
		if ./$$t; then echo "PASS $$t"; \
		else echo "FAIL $$t"; fail=1; fi; \
	done; \
	exit $$fail

asan:
	rm -rf build-asan
	$(MAKE) BUILD=build-asan SANFLAGS="$(ASAN_FLAGS)" all test

memcheck: $(TEST_BINS)
	@fail=0; \
	for t in $(TEST_BINS); do \
		if $(VALGRIND) ./$$t; then echo "PASS $$t"; \
		else echo "FAIL $$t"; fail=1; fi; \
	done; \
	exit $$fail

clean:
	rm -rf $(BUILD) build-asan

-include $(shell find $(OBJ) -name '*.d' 2>/dev/null)
