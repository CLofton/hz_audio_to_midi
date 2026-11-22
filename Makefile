CXX      := clang++
CXXFLAGS := -std=c++20 -O2 -Wall -Wextra -Wpedantic -I src
BUILD    := build

.PHONY: test demo clean

test: $(BUILD)/test_converter $(BUILD)/hz_convert
	./$(BUILD)/test_converter
	python3 tests/test_cli.py ./$(BUILD)/hz_convert

$(BUILD)/test_converter: tests/test_converter.cpp src/hz/converter.hpp
	mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ tests/test_converter.cpp

demo: $(BUILD)/hz_convert
	./$(BUILD)/hz_convert --tone 440 --seconds 0.5 --out $(BUILD)/a440.mid

$(BUILD)/hz_convert: tools/hz_convert.cpp src/hz/converter.hpp
	mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ tools/hz_convert.cpp

clean:
	rm -rf $(BUILD)
