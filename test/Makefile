# Makefile — builds and runs the JsonReadUtils host tests.
#
# Requires a C++17 compiler (g++ or clang++) and GNU make. From this folder:
#   make            build and run the suite
#   make san        build and run with Address/UB sanitizers (recommended)
#   make clean      remove the built binaries
#
# The library headers are searched one level up (..) and in ../src, so this
# works whether the headers sit in the project root or under src/.

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O0 -Wall -Wextra
INCLUDES := -I. -I.. -I../src
SANFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer

HARNESS := json_harness

.PHONY: all run san clean
all: run

$(HARNESS): json_harness.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< -o $@

run: $(HARNESS)
	@echo "------------------------------------------------------------"
	@echo "JsonReadUtils suite:"
	@./$(HARNESS)

# The parser eats uploaded, untrusted input, so the sanitized run is the one
# that actually justifies hand-writing it rather than pulling in a dependency.
san: json_harness.cpp
	@$(CXX) $(CXXFLAGS) $(SANFLAGS) $(INCLUDES) $< -o $(HARNESS)_san
	@echo "------------------------------------------------------------"
	@echo "JsonReadUtils suite (ASan + UBSan):"
	@./$(HARNESS)_san

clean:
	@rm -f $(HARNESS) $(HARNESS)_san $(HARNESS).exe $(HARNESS)_san.exe
