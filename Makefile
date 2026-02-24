BUILD_DIR ?= build
CMAKE ?= cmake
NPM ?= npm
NPM_CACHE_DIR ?= $(CURDIR)/.npm-cache
NPM_USERCONFIG ?= $(CURDIR)/.npmrc.local
VCPKG_ROOT ?= $(HOME)/vcpkg
VCPKG ?= $(VCPKG_ROOT)/vcpkg
VCPKG_TOOLCHAIN ?= $(VCPKG_ROOT)/scripts/buildsystems/vcpkg.cmake

UNAME_S := $(shell uname -s 2>/dev/null)
UNAME_M := $(shell uname -m 2>/dev/null)

ifeq ($(OS),Windows_NT)
	DEFAULT_VCPKG_TRIPLET := x64-windows-static
else ifeq ($(UNAME_S),Darwin)
	ifeq ($(UNAME_M),arm64)
		DEFAULT_VCPKG_TRIPLET := arm64-osx
	else
		DEFAULT_VCPKG_TRIPLET := x64-osx
	endif
else ifeq ($(UNAME_S),Linux)
	ifeq ($(UNAME_M),aarch64)
		DEFAULT_VCPKG_TRIPLET := arm64-linux
	else
		DEFAULT_VCPKG_TRIPLET := x64-linux
	endif
else
	DEFAULT_VCPKG_TRIPLET := x64-linux
endif

VCPKG_TRIPLET ?= $(DEFAULT_VCPKG_TRIPLET)
VCPKG_DOWNLOADS_DIR ?= $(CURDIR)/.vcpkg-downloads
VCPKG_BINARY_CACHE_DIR ?= $(CURDIR)/.vcpkg-binary-cache

CMAKE_DEBUG_ARGS := -DCMAKE_BUILD_TYPE=Debug -DFERRYMAN_BUILD_FRONTEND=OFF
CMAKE_RELEASE_ARGS := -DCMAKE_BUILD_TYPE=Release -DFERRYMAN_BUILD_FRONTEND=OFF

ifneq ("$(wildcard $(VCPKG_TOOLCHAIN))","")
	CMAKE_DEBUG_ARGS += -DCMAKE_TOOLCHAIN_FILE=$(VCPKG_TOOLCHAIN)
	CMAKE_RELEASE_ARGS += -DCMAKE_TOOLCHAIN_FILE=$(VCPKG_TOOLCHAIN)
endif

.PHONY: configure build run run-backend dev-backend clean frontend dev-frontend dev release deps deps-proxy

configure:
	$(CMAKE) -S . -B $(BUILD_DIR) $(CMAKE_DEBUG_ARGS)

build: configure
	$(CMAKE) --build $(BUILD_DIR) -j

run: build
	./$(BUILD_DIR)/Ferryman

run-backend: build
	./$(BUILD_DIR)/Ferryman

dev-backend: run-backend

frontend:
	@mkdir -p $(NPM_CACHE_DIR)
	@touch $(NPM_USERCONFIG)
	cd frontend && \
		if [ -d node_modules ]; then \
			echo "frontend: node_modules present, skipping npm install"; \
		else \
			npm_config_cache=$(NPM_CACHE_DIR) npm_config_userconfig=$(NPM_USERCONFIG) $(NPM) install; \
		fi && \
		npm_config_cache=$(NPM_CACHE_DIR) npm_config_userconfig=$(NPM_USERCONFIG) $(NPM) run build

dev-frontend:
	@mkdir -p $(NPM_CACHE_DIR)
	@touch $(NPM_USERCONFIG)
	cd frontend && npm_config_cache=$(NPM_CACHE_DIR) npm_config_userconfig=$(NPM_USERCONFIG) $(NPM) run dev -- --host

dev: frontend
	$(CMAKE) -S . -B $(BUILD_DIR) $(CMAKE_DEBUG_ARGS)
	$(CMAKE) --build $(BUILD_DIR) -j
	./$(BUILD_DIR)/Ferryman

release: frontend
	$(CMAKE) -S . -B $(BUILD_DIR) $(CMAKE_RELEASE_ARGS)
	$(CMAKE) --build $(BUILD_DIR) -j

deps:
	VCPKG_ROOT=$(VCPKG_ROOT) \
	VCPKG=$(VCPKG) \
	VCPKG_TRIPLET=$(VCPKG_TRIPLET) \
	VCPKG_DOWNLOADS=$(VCPKG_DOWNLOADS_DIR) \
	VCPKG_DEFAULT_BINARY_CACHE=$(VCPKG_BINARY_CACHE_DIR) \
	./scripts/make_deps.sh

deps-proxy:
	FERRYMAN_USE_PROXY=1 $(MAKE) deps

clean:
	$(CMAKE) -E rm -rf $(BUILD_DIR)
