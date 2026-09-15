CC ?= cc
PKG_CONFIG ?= pkg-config
TARGET := hdmirx-kms-lowlat
SRC := src/hdmirx-kms-lowlat.c

CPPFLAGS += $(shell $(PKG_CONFIG) --cflags libdrm 2>/dev/null)
CFLAGS ?= -O2 -g
CFLAGS += -std=gnu11 -Wall -Wextra -Wpedantic
LDLIBS += $(shell $(PKG_CONFIG) --libs libdrm 2>/dev/null)

.PHONY: all clean check

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(LDLIBS)

check:
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsyntax-only $(SRC)
	python3 -m py_compile tools/analyze.py
	bash -n scripts/run-baseline.sh scripts/run-v35-phase.sh

clean:
	rm -f $(TARGET)
	rm -rf tools/__pycache__
