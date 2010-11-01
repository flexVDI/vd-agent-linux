VERSION = 0.6.3

DESTDIR	?= 
sbindir	?= /sbin
bindir	?= /usr/bin
initdir	?= /etc/rc.d/init.d
xdgautostartdir ?= /etc/xdg/autostart
gdmautostartdir ?= /usr/share/gdm/autostart/LoginWindow
socketdir ?= /var/run/spice-vdagentd

CFLAGS	 ?= -O2 -g -Wall
CPPFLAGS  = $(shell pkg-config --cflags spice-protocol)
CPPFLAGS += $(shell pkg-config --cflags dbus-1)
CPPFLAGS += -D_GNU_SOURCE

TARGETS	:= spice-vdagentd spice-vdagent

build: $(TARGETS)

install: build
	install -d $(DESTDIR)$(bindir)
	install -d $(DESTDIR)$(sbindir)
	install -d $(DESTDIR)$(socketdir)
	install -p -m 755 spice-vdagent $(DESTDIR)$(bindir)
	install -p -m 755 spice-vdagentd $(DESTDIR)$(sbindir)
	install -d $(DESTDIR)$(initdir)
	install -p -m 755 spice-vdagentd.sh $(DESTDIR)$(initdir)/spice-vdagentd
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

tag:
	@git tag -a -m "Tag as spice-vdagent-$(VERSION)" spice-vdagent-$(VERSION)
	@echo "Tagged as spice-vdagent-$(VERSION)"

archive-no-tag:
	@git archive --format=tar --prefix=spice-vdagent-$(VERSION)/ spice-vdagent-$(VERSION) > spice-vdagent-$(VERSION).tar
	@bzip2 -f spice-vdagent-$(VERSION).tar

archive: clean tag archive-no-tag
