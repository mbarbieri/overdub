# Thin wrapper over the CMake presets (docs/building.md). No build logic lives here.
RELEASE_BUNDLE := build/release/src/app/Overdub_artefacts/Release/Overdub.app
DIST_BUNDLE    := dist/Overdub.app
PREFIX         ?= /Applications
DEST           ?= $(PREFIX)

.PHONY: app install test verify-app clean submodules separation-model

submodules:
	@if [ -z "$$(ls -A external/JUCE 2>/dev/null)" ] || [ -z "$$(ls -A external/Catch2 2>/dev/null)" ] || [ -z "$$(ls -A external/json 2>/dev/null)" ]; then \
		echo "make: initialising submodules"; \
		git submodule update --init --recursive; \
	fi

app: submodules
	cmake --preset release
	cmake --build --preset release --target Overdub
	rm -rf $(DIST_BUNDLE)
	mkdir -p dist
	ditto $(RELEASE_BUNDLE) $(DIST_BUNDLE)
	tests/packaging/verify_app_bundle.sh $(DIST_BUNDLE) "$(CURDIR)"
	@echo "Bundle: $(CURDIR)/$(DIST_BUNDLE)"
	@codesign -dvv $(DIST_BUNDLE) 2>&1 | grep -E '^(Signature=adhoc|Authority=)' | head -1 | sed 's/^/Signed with: /'

install: app
	mkdir -p "$(DEST)"
	rm -rf "$(DEST)/Overdub.app"
	ditto $(DIST_BUNDLE) "$(DEST)/Overdub.app"
	@echo "Installed: $(DEST)/Overdub.app"

test: submodules
	cmake --preset debug
	cmake --build --preset debug
	ctest --preset debug

verify-app:
	tests/packaging/verify_app_bundle.sh $(DIST_BUNDLE) "$(CURDIR)"

separation-model:
	scripts/separation-model/install.sh

clean:
	rm -rf build dist
