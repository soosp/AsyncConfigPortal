# Makefile — builds and runs the host tests (JsonReadUtils, origin check,
# result answer).
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
ORIGIN  := origin_harness
RESULT  := result_harness
MARKER  := marker_harness

.PHONY: all run origin result marker san clean
all: run

$(HARNESS): json_harness.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< -o $@

run: $(HARNESS) origin result marker
	@echo "------------------------------------------------------------"
	@echo "JsonReadUtils suite:"
	@./$(HARNESS)

$(ORIGIN): origin_harness.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< -o $@

origin: $(ORIGIN)
	@echo "------------------------------------------------------------"
	@echo "Origin check suite:"
	@./$(ORIGIN)

$(RESULT): result_harness.cpp ../src/detail/result_json.h
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< -o $@

result: $(RESULT)
	@echo "------------------------------------------------------------"
	@echo "Result answer suite:"
	@./$(RESULT)

$(MARKER): marker_harness.cpp ../src/FirmwareMarker.h
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< -o $@

marker: $(MARKER)
	@echo "------------------------------------------------------------"
	@echo "FirmwareMarker suite:"
	@./$(MARKER)
	@echo "plain magic occurrences in the binary (must be 1, g_fwMarker):"
	@grep -c FWMARK01 $(MARKER)

# The parser eats uploaded, untrusted input, so the sanitized run is the one
# that actually justifies hand-writing it rather than pulling in a dependency.
san: json_harness.cpp
	@$(CXX) $(CXXFLAGS) $(SANFLAGS) $(INCLUDES) $< -o $(HARNESS)_san
	@echo "------------------------------------------------------------"
	@echo "JsonReadUtils suite (ASan + UBSan):"
	@./$(HARNESS)_san

clean:
	@rm -f $(HARNESS) $(HARNESS)_san $(HARNESS).exe $(HARNESS)_san.exe $(ORIGIN) $(ORIGIN).exe $(RESULT) $(RESULT).exe $(MARKER) $(MARKER).exe
