CC      ?= gcc
PKG_CFG ?= pkg-config

CFLAGS  := -O2 -Wall -Wextra -std=c11 $(shell $(PKG_CFG) --cflags libdrm freetype2 libpng)
LDFLAGS := $(shell $(PKG_CFG) --libs libdrm freetype2 libpng) -lm

TARGET  := kdm-osd

all: $(TARGET)

$(TARGET): kdm_osd.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

install: $(TARGET)
	install -Dm755 $(TARGET) /usr/local/bin/$(TARGET)
	install -Dm644 kdm-osd.service /etc/systemd/system/kdm-osd.service
	systemctl daemon-reload
	systemctl enable kdm-osd.service
	systemctl restart kdm-osd.service

clean:
	rm -f $(TARGET)
