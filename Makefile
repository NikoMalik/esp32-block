ENV  ?= esp32dev
PORT ?= /dev/ttyUSB0
BAUD ?= 115200
BLOB ?= blocklist.bin
BLOFF ?= 0x170000   # 'blocklist' partition offset, must match partitions.csv

.PHONY: help build upload flash blobflash monitor dev clean erase compiledb blocklist

help:
	@echo "targets:"
	@echo "  build      - compile firmware"
	@echo "  upload     - compile + flash firmware over USB"
	@echo "  blocklist  - rebuild $(BLOB) from sources (v2 bucketed)"
	@echo "  blobflash  - write $(BLOB) into the raw 'blocklist' partition"
	@echo "  flash      - upload + blobflash (full flash; LittleFS self-formats)"
	@echo "  monitor    - open serial monitor (Ctrl+C to exit)"
	@echo "  dev        - upload then monitor"
	@echo "  clean      - remove build artifacts"
	@echo "  erase      - erase whole flash"
	@echo "  compiledb  - regenerate compile_commands.json for clangd"
	@echo "vars: ENV=$(ENV) PORT=$(PORT) BAUD=$(BAUD)  (override like: make upload PORT=/dev/ttyUSB1)"

build:
	pio run -e $(ENV)

upload:
	pio run -e $(ENV) -t upload --upload-port $(PORT)

blobflash:
	esptool --chip esp32 --port $(PORT) --baud 460800 write-flash $(BLOFF) $(BLOB)

flash: upload blobflash

monitor:
	pio device monitor -p $(PORT) -b $(BAUD)

dev: upload monitor

clean:
	pio run -e $(ENV) -t clean

erase:
	pio run -e $(ENV) -t erase --upload-port $(PORT)

compiledb:
	pio run -e $(ENV) -t compiledb

blocklist: tools/build_blocklist
	./tools/build_blocklist $(BLOB)

tools/build_blocklist: tools/build_blocklist.cpp src/fxhash.h src/blfmt.h
	c++ -O2 -Wall -o $@ tools/build_blocklist.cpp
