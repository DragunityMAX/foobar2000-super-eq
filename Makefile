CXX ?= g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Werror -Iinclude
BUILD_DIR := build

.PHONY: all test cli clean

all: test cli

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

test: $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) tests/test_main.cpp -o $(BUILD_DIR)/super_eq_tests -lm
	./$(BUILD_DIR)/super_eq_tests

cli: $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) src/core/super_eq_cli.cpp -o $(BUILD_DIR)/super_eq_cli -lm

clean:
	rm -rf $(BUILD_DIR)
