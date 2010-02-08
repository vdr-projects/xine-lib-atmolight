#
# Makefile for xine atmo post plugin
#
INSTALL ?= install
XINEPLUGINDIR ?= $(shell pkg-config --variable=plugindir libxine)
CFLAGS_XINE ?= $(shell pkg-config --cflags libxine )
LIBS_XINE ?= $(shell pkg-config --libs libxine)
CFLAGS_USB ?= $(shell pkg-config --cflags libusb-1.0)
LIBS_USB ?= $(shell pkg-config --libs libusb-1.0)
LDFLAGS_SO ?= -shared -fvisibility=hidden
CFLAGS ?= -O3 -pipe -Wall -fPIC -g

XINEPOSTATMO = xineplug_post_atmo.so

.PHONY: all install clean

all: $(XINEPOSTATMO)

install: all
	@echo Installing $(XINEPLUGINDIR)/post/$(XINEPOSTATMO)
	@-rm -rf $(XINEPLUGINDIR)/post/$(XINEPOSTATMO)
	@$(INSTALL) -m 0644 $(XINEPOSTATMO) $(XINEPLUGINDIR)/post/$(XINEPOSTATMO)

clean:
	@-rm -f *.so* *.o

xine_post_atmo.o: xine_post_atmo.c output_driver.h
	$(CC) $(CFLAGS) $(CFLAGS_XINE) $(CFLAGS_USB) -c -o $@ $<

$(XINEPOSTATMO): xine_post_atmo.o
	$(CC) $(CFLAGS) $(LDFLAGS_SO) $(LIBS_XINE) $(LIBS_USB) -lm -o $@ $<

	
