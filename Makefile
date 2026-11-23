CXX      := clang++
CXXFLAGS := -std=c++20 -O2 -Wall -Wextra -Wpedantic -I src
BUILD    := build
Q_PATH   ?= ../Q
Q_FLAGS  := -isystem $(Q_PATH)/q_lib/include -isystem $(Q_PATH)/infra/include
LIVE_FLAGS := -fblocks
LIVE_FRAMEWORKS := -framework AudioToolbox -framework CoreAudio -framework CoreMIDI -framework CoreFoundation

.PHONY: test live-test q-test midi2-test midi2-check-q demo q-demo clean

test: $(BUILD)/test_converter $(BUILD)/hz_convert
	./$(BUILD)/test_converter
	python3 tests/test_cli.py ./$(BUILD)/hz_convert

live-test: $(BUILD)/test_live_unit $(BUILD)/hz_live
	./$(BUILD)/test_live_unit
	python3 tests/test_live_list.py ./$(BUILD)/hz_live
	python3 tests/test_live_self_test.py ./$(BUILD)/hz_live

q-test: $(BUILD)/test_q_converter $(BUILD)/hz_convert_q
	./$(BUILD)/test_q_converter
	python3 tests/test_q_cli.py ./$(BUILD)/hz_convert_q
	python3 tests/test_q_real.py ./$(BUILD)/hz_convert_q $(Q_PATH)
	python3 tests/test_q_multichannel.py ./$(BUILD)/hz_convert_q $(Q_PATH)

midi2-check-q:
	@test -f "$(Q_PATH)/q_lib/include/q/midi/translate.hpp" || { \
		echo "midi2-test requires Q_PATH=/path/to/Q with q/midi/translate.hpp" >&2; exit 2; }

midi2-test: midi2-check-q $(BUILD)/test_midi2 $(BUILD)/hz_midi2_test
	./$(BUILD)/test_midi2
	python3 tests/test_midi2_virtual.py ./$(BUILD)/hz_midi2_test

$(BUILD)/test_midi2: tests/test_midi2.cpp src/hz/midi2.hpp src/hz/converter.hpp
	mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $(Q_FLAGS) -o $@ tests/test_midi2.cpp

$(BUILD)/hz_midi2_test: tools/hz_midi2_test.cpp src/hz/midi2_virtual.cpp src/hz/midi2_virtual.hpp src/hz/midi2.hpp src/hz/converter.hpp
	mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $(Q_FLAGS) $(LIVE_FLAGS) -o $@ tools/hz_midi2_test.cpp src/hz/midi2_virtual.cpp -framework CoreMIDI -framework CoreFoundation

$(BUILD)/test_converter: tests/test_converter.cpp src/hz/converter.hpp
	mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ tests/test_converter.cpp

$(BUILD)/test_live_unit: tests/test_live_unit.cpp src/hz/live.hpp src/hz/live_stream.hpp src/hz/converter.hpp
	mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $(LIVE_FLAGS) -o $@ tests/test_live_unit.cpp

$(BUILD)/hz_live: tools/hz_live.cpp src/hz/live.cpp src/hz/live.hpp src/hz/live_stream.hpp src/hz/converter.hpp
	mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $(LIVE_FLAGS) -o $@ tools/hz_live.cpp src/hz/live.cpp $(LIVE_FRAMEWORKS)

$(BUILD)/test_q_converter: tests/test_q_converter.cpp src/hz/converter.hpp src/hz/q_pitch_estimator.hpp
	mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $(Q_FLAGS) -o $@ tests/test_q_converter.cpp

demo: $(BUILD)/hz_convert
	./$(BUILD)/hz_convert --tone 440 --seconds 0.5 --out $(BUILD)/a440.mid

q-demo: $(BUILD)/hz_convert_q
	./$(BUILD)/hz_convert_q --tone 440 --seconds 0.5 --out $(BUILD)/a440_q.mid

$(BUILD)/hz_convert: tools/hz_convert.cpp src/hz/converter.hpp
	mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ tools/hz_convert.cpp

$(BUILD)/hz_convert_q: tools/hz_convert.cpp src/hz/converter.hpp src/hz/q_pitch_estimator.hpp
	mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $(Q_FLAGS) -DHZ_WITH_Q=1 -o $@ tools/hz_convert.cpp

clean:
	rm -rf $(BUILD)
