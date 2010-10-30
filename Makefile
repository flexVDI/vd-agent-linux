
DESTDIR	?= 
sbindir	?= /sbin
bindir	?= /usr/bin
udevdir	?= /lib/udev/rules.d
xdgautostartdir ?= /etc/xdg/autostart
gdmautostartdir ?= /usr/share/gdm/autostart/LoginWindow

CFLAGS	 ?= -O2 -g -Wall
CPPFLAGS  = $(shell pkg-config --cflags spice-protocol)
CPPFLAGS += $(shell pkg-config --cflags dbus-1)
CPPFLAGS += -D_GNU_SOURCE

TARGETS	:= spice-vdagentd spice-vdagent

build: $(TARGETS)

install: build
	install -d $(DESTDIR)$(bindir)
	install -d $(DESTDIR)$(sbindir)
	install -p -m 755 spice-vdagent $(DESTDIR)$(bindir)
	install -p -m 755 spice-vdagentd $(DESTDIR)$(sbindir)
	install -d $(DESTDIR)$(udevdir)
	install -p -m 644 *.rules $(DESTDIR)$(udevdir)
	install -d $(DESTDIR)$(xdgautostartdir)
	install -d $(DESTDIR)$(gdmautostartdir)
	desktop-file-install --dir=$(DESTDIR)$(xdgautostartdir) \
		spice-vdagent.desktop
	desktop-file-install --dir=$(DESTDIR)$(gdmautostartdir) \
		spice-vdagent.desktop

clean:
	rm -f $(TARGETS) *.o *~

spice-vdagentd: vdagentd.o vdagentd-uinput.o udscs.o vdagent-virtio-port.o console-kit.o
	$(CC) -o $@ $^ $(shell pkg-config --libs dbus-1)

spice-vdagent: vdagent.o vdagent-x11.o udscs.o
	$(CC) -o $@ $^ -lX11 -lXrandr -lXfixes
