# LH-0 / LH-0.1 / LH-1 load harness — Makefile.
#
# Targets:
#   make lh0       build the lh0 binary at ./build/lh0
#   make fixtures  build fixture zips (driver.zip + lh1storm.zip)
#   make all       both of the above
#   make clean     remove ./build and fixture zips
#   make tidy      go mod tidy + go vet
#   make test      (placeholder; no Go tests yet — see ICD §Future work)

GO       ?= go
BUILD    := build
DRIVER   := fixtures/driver
DRIVERZIP := fixtures/driver.zip
STORM    := fixtures/lh1storm
STORMZIP := fixtures/lh1storm.zip
SIDECAR    := fixtures/lh2sidecar
SIDECARZIP := fixtures/lh2sidecar.zip

.PHONY: all lh0 fixtures clean tidy test

all: lh0 fixtures

lh0:
	mkdir -p $(BUILD)
	$(GO) build -trimpath -o $(BUILD)/lh0 ./cmd/lh0

# `cd && zip -rq` rather than `zip -rq ... $(DRIVER)` so the archive
# contains relative paths rooted at manifest.json, matching what the
# validator expects (it rejects a zip whose entries live under a
# synthetic top-level directory).
fixtures: $(DRIVERZIP) $(STORMZIP) $(SIDECARZIP)
$(DRIVERZIP): $(shell find $(DRIVER) -type f 2>/dev/null)
	cd $(DRIVER) && zip -rq ../../$(DRIVERZIP) .
$(STORMZIP): $(shell find $(STORM) -type f 2>/dev/null)
	cd $(STORM) && zip -rq ../../$(STORMZIP) .
$(SIDECARZIP): $(shell find $(SIDECAR) -type f 2>/dev/null)
	cd $(SIDECAR) && zip -rq ../../$(SIDECARZIP) .

clean:
	rm -rf $(BUILD) $(DRIVERZIP) $(STORMZIP) $(SIDECARZIP)

tidy:
	$(GO) mod tidy
	$(GO) vet ./...

test:
	@echo "lh0 has no Go unit tests yet; coverage is end-to-end via README § Smoke."
