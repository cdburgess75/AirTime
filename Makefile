# AirTime — host build & unit tests for the platform-independent core.
#
# Requires only a C++17 compiler (no PlatformIO, no network). This is the
# canonical way to run the core test suite today:
#
#     make test      build and run all unit tests
#     make clean      remove build artifacts
#
# The same sources under lib/airtime_core/ are compiled unchanged into the
# ATS Mini firmware once Milestone 0 hardware verification is done.

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wshadow -g
CORE_DIR := lib/airtime_core
TEST_DIR := test
INCLUDES := -I$(CORE_DIR) -I$(TEST_DIR)

CORE_SRC := $(wildcard $(CORE_DIR)/*.cpp)
TEST_SRC := $(wildcard $(TEST_DIR)/*.cpp)

BUILD := build
BIN   := $(BUILD)/airtime_tests

.PHONY: all test clean

all: test

$(BIN): $(CORE_SRC) $(TEST_SRC) $(wildcard $(CORE_DIR)/*.h) $(wildcard $(TEST_DIR)/*.h)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(CORE_SRC) $(TEST_SRC) -o $(BIN)

test: $(BIN)
	@./$(BIN)

clean:
	@rm -rf $(BUILD)
