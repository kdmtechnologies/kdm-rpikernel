CC      ?= gcc
PKG_CFG ?= pkg-config

CFLAGS  := -O2 -Wall -Wextra -std=c11 -D_GNU_SOURCE $(shell $(PKG_CFG) --cflags libdrm freetype2 libpng)
LDFLAGS := $(shell $(PKG_CFG) --libs libdrm freetype2 libpng) -lm

TARGET  := kdm-osd
SRCS    := kdm_osd.c osd_canvas.c osd_icons.c osd_view.c osd_hw.c osd_touch.c osd_drm.c
OBJS    := $(SRCS:.c=.o)
HDRS    := osd_types.h osd_canvas.h osd_icons.h osd_view.h osd_hw.h osd_touch.h osd_drm.h

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) -c -o $@ $<

install: $(TARGET)
	install -Dm755 $(TARGET) /usr/local/bin/$(TARGET)
	install -Dm644 kdm-osd.service /etc/systemd/system/kdm-osd.service
	systemctl daemon-reload
	systemctl enable kdm-osd.service
	systemctl restart kdm-osd.service

clean:
	rm -f $(TARGET) $(OBJS)
