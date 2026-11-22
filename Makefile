CXX      := clang++
CXXFLAGS := -std=c++20 -O2 -Wall -Wextra -Wpedantic -I src
BUILD    := build
Q_PATH   ?= ../Q
Q_FLAGS  := -isystem $(Q_PATH)/q_lib/include -isystem $(Q_PATH)/infra/include

.PHONY: test q-test demo q-demo clean

test: $(BUILD)/test_converter $(BUILD)/hz_convert
	./$(BUILD)/test_converter
	python3 tests/test_cli.py ./$(BUILD)/hz_convert

q-test: $(BUILD)/test_q_converter $(BUILD)/hz_convert_q
	./$(BUILD)/test_q_converter
	python3 tests/test_q_cli.py ./$(BUILD)/hz_convert_q
	python3 tests/test_q_real.py ./$(BUILD)/hz_convert_q $(Q_PATH)

$(BUILD)/test_converter: tests/test_converter.cpp src/hz/converter.hpp
	mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ tests/test_converter.cpp

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
