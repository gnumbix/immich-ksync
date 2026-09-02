# ImmichKSync — build, test and run.
#
# Everything here wraps CMake; there is no package manager step, because every
# dependency is a system library from Qt 6, KDE Frameworks 6 or the C library.

BUILD   ?= build
CONFIG  ?= Debug
PREFIX  ?= /usr/local
JOBS    ?= $(shell nproc)

.DEFAULT_GOAL := build

$(BUILD)/CMakeCache.txt:
	cmake -B $(BUILD) -G Ninja -DCMAKE_BUILD_TYPE=$(CONFIG) -DCMAKE_INSTALL_PREFIX=$(PREFIX)

.PHONY: configure
configure: ## Configure the build directory
	cmake -B $(BUILD) -G Ninja -DCMAKE_BUILD_TYPE=$(CONFIG) -DCMAKE_INSTALL_PREFIX=$(PREFIX)

.PHONY: build
build: $(BUILD)/CMakeCache.txt ## Build the app
	cmake --build $(BUILD) -j $(JOBS)

.PHONY: release
release: ## Build the Release configuration
	$(MAKE) build CONFIG=Release

.PHONY: test
test: build ## Run the hermetic unit and interface tests
	ctest --test-dir $(BUILD) --output-on-failure

# Live tests need a server.
#   make test-live IMMICH_TEST_SERVER=http://localhost:2283 IMMICH_TEST_API_KEY=…
.PHONY: test-live
test-live: build ## Run the end-to-end tests against a real Immich server
	@test -n "$(IMMICH_TEST_SERVER)" || (echo "Set IMMICH_TEST_SERVER and IMMICH_TEST_API_KEY"; exit 1)
	IMMICH_TEST_SERVER='$(IMMICH_TEST_SERVER)' IMMICH_TEST_API_KEY='$(IMMICH_TEST_API_KEY)' \
	  ctest --test-dir $(BUILD) --output-on-failure -R LiveServerTest

# Proves the TLS handshake, which the hermetic suite cannot: start the throwaway
# server with ./tools/run-tls-test-server.sh first.
.PHONY: test-tls
test-tls: build ## Run the mutual-TLS tests against ./tools/run-tls-test-server.sh
	IMMICH_TEST_TLS=1 ctest --test-dir $(BUILD) --output-on-failure -R TlsLiveTest

.PHONY: test-certificates
test-certificates: ## Regenerate the certificate fixtures used by the tests
	./tools/make-test-certificates.sh

.PHONY: snapshots
snapshots: build ## Render the settings tabs to PNGs in ./snapshots
	IMMICHKSYNC_SNAPSHOT_DIR="$(PWD)/snapshots" \
	  ctest --test-dir $(BUILD) --output-on-failure -R InterfaceRenderTest
	@echo "Snapshots written to $(PWD)/snapshots"

.PHONY: install
install: build ## Install into $(PREFIX)
	cmake --install $(BUILD)
	@echo "Installed into $(PREFIX). Run 'immichksync' or find it in your application menu."

.PHONY: run
run: build ## Build and launch the tray app
	@$(MAKE) --no-print-directory stop
	$(BUILD)/bin/immichksync &

.PHONY: stop
stop: ## Quit a running instance
	@pgrep -x immichksync >/dev/null && pkill -x immichksync || true
	@sleep 0.3

.PHONY: logs
logs: ## Follow the app log
	tail -f "$${XDG_STATE_HOME:-$$HOME/.local/state}/immichksync/immichksync.log"

.PHONY: clean
clean: ## Remove build products
	rm -rf $(BUILD) snapshots

.PHONY: reset-state
reset-state: stop ## Delete the local sync database (files and server are untouched)
	rm -rf "$${XDG_DATA_HOME:-$$HOME/.local/share}/immichksync"
	@echo "Local sync state removed."

.PHONY: help
help: ## List targets
	@grep -E '^[a-zA-Z_-]+:.*?## .*$$' $(MAKEFILE_LIST) \
	  | awk 'BEGIN {FS = ":.*?## "}; {printf "  \033[36m%-18s\033[0m %s\n", $$1, $$2}'
