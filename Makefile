BUILDDIR = build
BUILD_TYPE ?= release

.PHONY: all
all: clean configure build install
	@echo ""
	@echo "To run tests; Have a look at the Makefile helpers"
	@echo "================================================="

.PHONY: clean
clean:
	rm -rf $(BUILDDIR)

.PHONY: configure
configure:
	meson setup $(BUILDDIR) --buildtype=$(BUILD_TYPE)

.PHONY: build
build:
	meson compile -C $(BUILDDIR)
	
.PHONY: install
install:
	meson install -C $(BUILDDIR)

.PHONY: test-using-nvme
test-using-nvme:
	cd cijoe && cijoe workflows/prep_and_test.yaml \
		--config configs/localhost-nvme.toml \
		--monitor

.PHONY: test-using-nbd
test-using-nbd:
	cd cijoe && cijoe workflows/prep_and_test.yaml \
		--config configs/localhost-nbd.toml \
		--monitor

.PHONY: test-using-loop
test-using-loop:
	cd cijoe && cijoe workflows/prep_and_test.yaml \
		--config configs/localhost-loop.toml \
		--monitor

.PHONY: test-using-zram
test-using-zram:
	cd cijoe && cijoe workflows/prep_and_test.yaml \
		--config configs/localhost-zram.toml \
		--monitor

.PHONY: test-reflink-using-loop
test-reflink-using-loop:
	cd cijoe && cijoe workflows/prep_and_test_reflink.yaml \
		--config configs/localhost-loop.toml \
		--monitor

.PHONY: test-reflink-using-zram
test-reflink-using-zram:
	cd cijoe && cijoe workflows/prep_and_test_reflink_zram.yaml \
		--config configs/localhost-zram.toml \
		--monitor

# DESTRUCTIVE: formats configs/localhost-nvme.toml's dev_path. Point it at a spare device first.
.PHONY: test-reflink-using-nvme
test-reflink-using-nvme:
	cd cijoe && cijoe workflows/prep_and_test_reflink_nvme.yaml \
		--config configs/localhost-nvme.toml \
		--monitor

.PHONY: test
test: test-using-loop