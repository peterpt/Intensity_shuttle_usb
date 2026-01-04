PKG_CONFIG ?= pkg-config
CXXFLAGS ?= -O2 -g
CXXFLAGS := -std=gnu++14 -Wall -I. $(shell $(PKG_CONFIG) libusb-1.0 --cflags) -pthread $(CXXFLAGS)
LDFLAGS := $(shell $(PKG_CONFIG) libusb-1.0 --libs) -pthread $(LDFLAGS)
AR := ar
LN := ln
RANLIB := ranlib
INSTALL := install
PREFIX := /usr/local
LIBDIR := $(PREFIX)/lib
UDEVDIR ?= $(shell $(PKG_CONFIG) --variable udevdir udev)
UDEVDIR ?= /lib/udev
LIB := libbmusb.a
SODEV := libbmusb.so
SONAME := libbmusb.so.6
SOLIB := libbmusb.so.6.0.4

all: $(LIB) $(SOLIB) main bmusb-v4l2proxy

%.pic.o : %.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -fPIC -o $@ -c $^

main: bmusb.o main.o
	$(CXX) -o $@ $^ $(LDFLAGS)

bmusb-v4l2proxy: bmusb.o v4l2proxy.o
	$(CXX) -o $@ $^ $(LDFLAGS)

# Static library.
$(LIB): bmusb.o fake_capture.o
	$(AR) rc $@ $^
	$(RANLIB) $@

# Shared library.
$(SOLIB): bmusb.pic.o fake_capture.pic.o
	$(CXX) -shared -Wl,-soname,$(SONAME) -o $@ $^ $(LDFLAGS)

clean:
	$(RM) bmusb.o main.o v4l2proxy.o fake_capture.o bmusb.pic.o fake_capture.pic.o $(LIB) $(SOLIB) main bmusb-v4l2proxy

install: all
	$(INSTALL) -m 755 -d \
		$(DESTDIR)$(LIBDIR) \
		$(DESTDIR)$(LIBDIR)/pkgconfig \
		$(DESTDIR)$(PREFIX)/include/bmusb \
		$(DESTDIR)$(UDEVDIR)/rules.d
	$(INSTALL) -m 755 $(LIB) $(SOLIB) $(DESTDIR)$(LIBDIR)
	$(LN) -sf $(SOLIB) $(DESTDIR)$(LIBDIR)/$(SONAME)
	$(LN) -sf $(SOLIB) $(DESTDIR)$(LIBDIR)/$(SODEV)
	$(INSTALL) -m 755 bmusb/bmusb.h bmusb/fake_capture.h $(DESTDIR)$(PREFIX)/include/bmusb
	$(INSTALL) -m 644 bmusb.pc $(DESTDIR)$(LIBDIR)/pkgconfig
	$(INSTALL) -m 644 70-bmusb.rules $(DESTDIR)$(UDEVDIR)/rules.d

