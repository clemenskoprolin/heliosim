TARGET      := heliosim
SRC         := main.cpp
HEADERS     := barnes_hut.hpp
BUILD_DIR   := build
HTML        := index.html
CXXFLAGS    := -I. -Iexternal/glm -std=c++17 -O3
NATIVE_CXX  ?= c++
EMXX        ?= em++
TEST_TARGET := $(BUILD_DIR)/barnes_hut_test
BENCHMARK_SRC := benchmarks/barnes_hut_benchmark.cpp
BENCHMARK_TARGET := $(BUILD_DIR)/barnes_hut_benchmark
BENCHMARK_JS := $(BUILD_DIR)/barnes_hut_benchmark.js
BENCHMARK_HTML := $(BUILD_DIR)/browser_benchmark.html
EMFLAGS     := -s USE_GLFW=3 -s FULL_ES3=1 -s ALLOW_MEMORY_GROWTH=1 -s WASM=1 -s EXPORTED_RUNTIME_METHODS=ccall,cwrap
PORT        := 8000
TOUCH_EMULATOR ?= 0

# Detect platform (macOS vs Linux)
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
    OPEN_CMD = xdg-open
else
    OPEN_CMD = open
    export PATH := /opt/homebrew/bin:$(PATH)
endif

# Build
all: $(BUILD_DIR)/$(TARGET).js $(BUILD_DIR)/index.html serve

$(BUILD_DIR)/$(TARGET).js: $(SRC) $(HEADERS)
	@mkdir -p $(BUILD_DIR)
	$(EMXX) $(SRC) $(CXXFLAGS) -o $(BUILD_DIR)/$(TARGET).js $(EMFLAGS)
	@echo "Build complete."

$(TEST_TARGET): tests/barnes_hut_test.cpp $(HEADERS)
	@mkdir -p $(BUILD_DIR)
	$(NATIVE_CXX) tests/barnes_hut_test.cpp $(CXXFLAGS) -o $(TEST_TARGET)

test: $(TEST_TARGET)
	./$(TEST_TARGET)

$(BENCHMARK_TARGET): $(BENCHMARK_SRC) $(HEADERS)
	@mkdir -p $(BUILD_DIR)
	$(NATIVE_CXX) $(BENCHMARK_SRC) $(CXXFLAGS) -o $(BENCHMARK_TARGET)

benchmark: $(BENCHMARK_TARGET)
	./$(BENCHMARK_TARGET)

$(BENCHMARK_JS): $(BENCHMARK_SRC) $(HEADERS)
	@mkdir -p $(BUILD_DIR)
	$(EMXX) $(BENCHMARK_SRC) $(CXXFLAGS) -o $(BENCHMARK_JS) \
		--no-entry -s ALLOW_MEMORY_GROWTH=1 -s WASM=1 -s EXPORTED_RUNTIME_METHODS=ccall

$(BENCHMARK_HTML): benchmarks/browser_benchmark.html
	@mkdir -p $(BUILD_DIR)
	cp $< $@

benchmark-wasm: $(BENCHMARK_JS) $(BENCHMARK_HTML)

$(BUILD_DIR)/index.html: $(HTML) FORCE
	@mkdir -p $(BUILD_DIR)
	cp $(HTML) $(BUILD_DIR)/index.html
ifeq ($(TOUCH_EMULATOR),1)
	@sed -i '' 's|<!-- TOUCH_EMULATOR -->|<script src="touch-emulator.js"></script>\
  <script> TouchEmulator(); </script>|' $(BUILD_DIR)/index.html
	@echo "Touch emulator enabled."
else
	@sed -i '' 's|<!-- TOUCH_EMULATOR -->||' $(BUILD_DIR)/index.html
endif
	@echo "Copied index.html to build."

FORCE:

.PHONY: all serve test benchmark benchmark-wasm clean FORCE

serve:
	@cd $(BUILD_DIR) && \
	python3 -m http.server $(PORT) >/dev/null 2>&1 & \
	sleep 1 && \
	$(OPEN_CMD) "http://localhost:$(PORT)/index.html"

clean:
	rm -rf $(BUILD_DIR)
	@echo "Cleaned build directory."
