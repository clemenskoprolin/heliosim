TARGET      := heliosim
SRC         := main.cpp
BUILD_DIR   := build
HTML        := index.html
CXXFLAGS    := -Iexternal/glm -std=c++17 -O3
EMFLAGS     := -s USE_GLFW=3 -s FULL_ES3=1 -s ALLOW_MEMORY_GROWTH=1 -s WASM=1
PORT        := 8000
TOUCH_EMULATOR ?= 0

# Detect platform (macOS vs Linux)
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
    OPEN_CMD = xdg-open
else
    OPEN_CMD = open
endif

# Build
all: $(BUILD_DIR)/$(TARGET).js $(BUILD_DIR)/index.html serve

$(BUILD_DIR)/$(TARGET).js: $(SRC)
	@mkdir -p $(BUILD_DIR)
	emcc $(SRC) $(CXXFLAGS) -o $(BUILD_DIR)/$(TARGET).js $(EMFLAGS)
	@echo "Build complete."

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

.PHONY: all serve clean FORCE

serve:
	@cd $(BUILD_DIR) && \
	python3 -m http.server $(PORT) >/dev/null 2>&1 & \
	sleep 1 && \
	$(OPEN_CMD) "http://localhost:$(PORT)/index.html"

clean:
	rm -rf $(BUILD_DIR)
	@echo "Cleaned build directory."
