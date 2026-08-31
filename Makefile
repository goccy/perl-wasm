# Container image shipping every toolchain wasmify needs (wasi-sdk + binaryen +
# buf + the wasmify CLI itself). Override locally to pin a SHA or to iterate on a
# wasmify branch — e.g. `make wasm IMAGE=localhost:5001/wasmify:local`.
IMAGE ?= ghcr.io/goccy/wasmify:v0.6.12

# Resource limits for the container that runs the pipeline. CPUS bounds make's
# parallelism; a Perl wasm build peaks well under MEMORY.
MEMORY ?= 8g
CPUS   ?= 8

# The wasmify image is published linux/amd64 only. On an arm64 host (Apple
# Silicon) set DOCKER_PLATFORM=linux/amd64 to run it under emulation:
#   make wasm DOCKER_PLATFORM=linux/amd64
DOCKER_PLATFORM ?=
PLATFORM_FLAG := $(if $(DOCKER_PLATFORM),--platform=$(DOCKER_PLATFORM),)

# Bundle module path / go directive stamped into the wasm2go bundle's go.mod.
# The bundle is released as a self-contained Go module, so it needs a go.mod
# declaring the import path its own `import` / `//go:linkname` sites embed.
# wasmify writes that path into wasmify.json's bridge.Wasm2GoImportPath, which
# the codegen reads back; derive it from the JSON so the two never drift.
# The generated tree nests under the GoPackage's package directory
# ("internal", from bridge.GoPackage = github.com/goccy/go-perl/internal):
# build/wasm2go/internal/perl.go is the service binding for go-perl, and the
# self-contained wasm2go bundle module sits one level below it.
WASM2GO_BUNDLE_DIR    := build/wasm2go/internal/internal/wasm2go
WASM2GO_BUNDLE_GO_VER := 1.25.0

# Exported into every pipeline phase. Perl is NOT wasi-native: it wants the
# implicit -D__EMSCRIPTEN__ and the POSIX-compat overlay (its socket/subprocess
# code compiles against the host-capability stubs), so — unlike CPython — this
# does NOT set WASMIFY_NO_EMSCRIPTEN_DEFINE / WASMIFY_NO_POSIX_COMPAT.
BUILD_ENV = WASMIFY_NON_INTERACTIVE=1

# Full pipeline replayed inside the container, top-to-bottom. Perl needs a
# configure phase first: scripts/wasi-configure.sh builds the HOST miniperl /
# generate_uudmap (Perl runs miniperl for build-time codegen; the wasm one can't
# run on the host), runs the wasm cross-configure, and assembles perl5/lib.
# `wasmify build` then replays the captured `make` under the compiler-wrapper;
# generate-build emits build.json, parse-headers/gen-proto emit the api spec +
# proto + bridge, wasm-build links perl.wasm (the bridge in perl.cc is injected
# automatically), and buf generate + bundle-gomod produce the wasm2go bundle as
# a self-contained Go module.
WASMIFY_PIPELINE = \
	make tools && \
	$(BUILD_ENV) bash scripts/wasi-configure.sh && \
	$(BUILD_ENV) wasmify build --non-interactive && \
	wasmify generate-build && \
	wasmify parse-headers --header pl.h --clang $(WASI_CLANG) && \
	wasmify gen-proto && \
	$(BUILD_ENV) wasmify wasm-build --optimize --non-interactive && \
	rm -rf build && \
	buf generate --timeout 0 && \
	make bundle-gomod
# `buf generate --timeout 0`: buf's default two-minute plugin timeout SIGKILLs
# protoc-gen-wasmify-go mid-transpile on a wasm this size and reports only
# "signal: killed".

# parse-headers runs clang over pl.h with the flags the build captured, which
# include the wasm target, --sysroot, and -mllvm -wasm-enable-sjlj. A host
# clang (Apple clang in particular) rejects those, so point it at wasi-sdk's
# clang — the same compiler that built the archives. Works both in the
# container and on a bare host (wasmify ensure-tools installs the sdk there).
WASI_CLANG = $${WASI_SDK_PATH:-$$HOME/.config/wasmify/bin/wasi-sdk}/bin/clang
# `rm -rf build` so `buf generate` writes the wasm2go bundle into a CLEAN tree:
# protoc-gen-wasmify-go overwrites the files it emits but never deletes stale
# ones, and the bundle's file SET depends on the wasm's size (a sub-threshold
# wasm yields a single-package layout; a larger one yields the base/ + pN
# multi-package layout). Mixing two leftover sets in one package won't compile.

.PHONY: all wasm wasm-host wasm-clean tools bundle-gomod stdlib-zip image-pull help

all: wasm

# Install the tools wasmify.json declares (wasi-sdk; pre-baked in the image).
# Safe to re-run; already-installed tools are skipped.
tools:
	wasmify ensure-tools ./perl5 --output-dir .

# Build perl.wasm + the wasm2go bundle from a clean checkout, exactly the way
# .github/workflows/build.yml does. Outputs:
#   .wasmify/wasm-build/output/perl.wasm
#   build/wasm2go/                                <- wasm2go bridge + bundle
#   build/wasm2go/internal/wasm2go/go.mod         <- bundle module manifest
wasm:
	docker run --rm $(PLATFORM_FLAG) \
		-v $(CURDIR):/work -w /work \
		--memory=$(MEMORY) --cpus=$(CPUS) \
		-e WASMIFY_NON_INTERACTIVE=1 \
		$(IMAGE) \
		bash -c '$(WASMIFY_PIPELINE)'

# Same pipeline, run directly on the host — much faster than the container on
# an Apple Silicon machine (the image is linux/amd64 and runs emulated there).
# Requirements, matching what the image ships:
#   * the wasmify CLI and protoc-gen-wasmify-go on PATH, built from the SAME
#     wasmify version as $(IMAGE):
#       go install github.com/goccy/wasmify/cmd/wasmify@vX.Y.Z
#       go install github.com/goccy/wasmify/protoc-plugins/protoc-gen-wasmify-go@vX.Y.Z
#   * buf, python3, make on PATH
#   * wasi-sdk — `make tools` (the pipeline's first step) installs it under
#     ~/.config/wasmify/bin/wasi-sdk if missing.
wasm-host:
	bash -c '$(WASMIFY_PIPELINE)'

# Build the embeddable stdlib zip from the ASSEMBLED perl5/lib tree. CI's
# build.yml/release.yml run this same script when staging artifacts; this target
# is for local parity. NOTE: run `make wasm` (or at least the configure + make)
# first — a pristine perl5/lib holds only the ~73 core .pm; the dual-life
# modules and the generated Config.pm are written there during the build.
stdlib-zip:
	python3 scripts/make-stdlib-zip.py perl5/lib perl_stdlib.zip

# Write go.mod into the wasm2go bundle so the released tarball is a
# self-contained Go module. Parses bridge.Wasm2GoImportPath out of wasmify.json
# with grep+sed (no jq in the image; no Go toolchain needed — a literal manifest).
bundle-gomod:
	@if [ ! -d "$(WASM2GO_BUNDLE_DIR)" ]; then \
		echo "$(WASM2GO_BUNDLE_DIR) does not exist — run 'make wasm' first" >&2; \
		exit 1; \
	fi
	@path=$$(grep -E '"Wasm2GoImportPath"[[:space:]]*:' wasmify.json \
		| head -1 \
		| sed -E 's/.*"Wasm2GoImportPath"[[:space:]]*:[[:space:]]*"([^"]+)".*/\1/'); \
	if [ -z "$$path" ]; then \
		echo "wasmify.json bridge.Wasm2GoImportPath is empty; cannot stamp bundle go.mod" >&2; \
		exit 1; \
	fi; \
	printf 'module %s\n\ngo %s\n' "$$path" "$(WASM2GO_BUNDLE_GO_VER)" \
		> $(WASM2GO_BUNDLE_DIR)/go.mod; \
	echo "wrote $(WASM2GO_BUNDLE_DIR)/go.mod (module $$path)"

# Drop everything wasmify regenerates so the next `make wasm` runs from scratch.
# The committed inputs (wasmify.json, buf.{yaml,gen.yaml}, proto/wasmify, perl.cc,
# pl.h, scripts/, the perl5 submodule) survive.
wasm-clean:
	rm -rf .wasmify api-spec.json build.json proto/perl.proto bridge build \
	       perl_stdlib.zip

# Refresh the cached toolchain image.
image-pull:
	docker pull $(PLATFORM_FLAG) $(IMAGE)

help:
	@echo "Targets:"
	@echo "  wasm         build perl.wasm + wasm2go bundle in the wasmify container"
	@echo "  stdlib-zip   zip the assembled perl5/lib into perl_stdlib.zip"
	@echo "  bundle-gomod stamp go.mod into the wasm2go bundle"
	@echo "  tools        install wasi-sdk + declared tools"
	@echo "  wasm-clean   remove every wasmify-generated artefact"
	@echo "  image-pull   docker pull the wasmify toolchain image"
