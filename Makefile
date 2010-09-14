
DESTDIR	?= 
sbindir	?= /sbin
udevdir	?= /lib/udev/rules.d

CFLAGS	?= -O2
CFLAGS	+= -g -Wall
CFLAGS	+= -g -Wall
CFLAGS  += $(shell pkg-config --cflags spice-protocol)

TARGETS	:= vdagent client

build: $(TARGETS)

install: build
	install -d $(DESTDIR)$(sbindir)
	install -s $(TARGETS) $(DESTDIR)$(sbindir)
	install -d $(DESTDIR)$(udevdir)
	install -m 644 *.rules $(DESTDIR)$(udevdir)

clean:
	rm -f $(TARGETS) *.o *~

vdagent: vdagent.o udscs.o
client: client.o udscs.o