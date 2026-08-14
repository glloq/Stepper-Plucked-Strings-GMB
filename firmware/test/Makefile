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
  $(CORE)/motion/MotionPlanner.cpp \
  $(CORE)/configuration/Profile.cpp \
  $(CORE)/configuration/ProfileValidator.cpp \
  $(CORE)/gmb/Capabilities.cpp \
  $(CORE)/gmb/GmbSysEx.cpp \
  $(CORE)/safety/SafetyManager.cpp \
  $(ROOT)/src/platform/esp32/ServoBank.cpp

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
  test_midiparser.cpp \
  test_servos.cpp \
  test_planner.cpp \
  test_audit.cpp \
  test_debounce.cpp \
  test_safety.cpp \
  test_app_phase.cpp \
  test_readiness.cpp \
  test_diagnostics.cpp \
  test_holdbutton.cpp \
  test_command_ring.cpp \
  test_udp_source_gate.cpp \
  test_governor.cpp \
  test_actuator_manager.cpp \
  test_transport.cpp \
  test_device_instrument.cpp \
  test_profile_activation.cpp \
  test_servobank_park.cpp

BIN := run_tests

.PHONY: all build clean
all: build
	./$(BIN)

build: $(BIN)

$(BIN): $(CORE_SRC) $(TEST_SRC)
	$(CXX) $(CXXFLAGS) $^ -o $(BIN)

clean:
	rm -f $(BIN)
