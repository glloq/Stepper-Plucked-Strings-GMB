# Native (host) build of the pure-C++ firmware core + unit tests.
# Usage:  make        -> build and run tests
#         make build  -> build only
CXX ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O1 -g

ROOT := ..
CORE := $(ROOT)/src/core

CORE_SRC := \
  $(CORE)/Types.cpp \
  $(CORE)/board/BoardProfile.cpp \
  $(CORE)/board/PinManager.cpp \
  $(CORE)/midi/StringFretSelector.cpp \
  $(CORE)/midi/MidiParser.cpp \
  $(CORE)/instrument/NoteAllocator.cpp \
  $(CORE)/instrument/StringController.cpp \
  $(CORE)/instrument/InstrumentController.cpp \
  $(CORE)/gmb/GmbSysExService.cpp \
  $(CORE)/motion/StepperAxis.cpp \
  $(CORE)/motion/HomingController.cpp \
  $(CORE)/configuration/Profile.cpp \
  $(CORE)/configuration/ProfileValidator.cpp \
  $(CORE)/gmb/Capabilities.cpp \
  $(CORE)/gmb/GmbSysEx.cpp \
  $(CORE)/safety/SafetyManager.cpp

TEST_SRC := \
  test_main.cpp \
  test_board.cpp \
  test_selector.cpp \
  test_allocator.cpp \
  test_motion.cpp \
  test_string_fsm.cpp \
  test_profile.cpp \
  test_sysex.cpp \
  test_integration.cpp \
  test_midiparser.cpp

BIN := run_tests

.PHONY: all build clean
all: build
	./$(BIN)

build: $(BIN)

$(BIN): $(CORE_SRC) $(TEST_SRC)
	$(CXX) $(CXXFLAGS) $^ -o $(BIN)

clean:
	rm -f $(BIN)
