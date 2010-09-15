
DESTDIR	?= 
sbindir	?= /sbin
udevdir	?= /lib/udev/rules.d

CFLAGS	?= -O2
CFLAGS	+= -g -Wall
CFLAGS	+= -g -Wall
CFLAGS  += $(shell pkg-config --cflags spice-protocol)

TARGETS	:= vdagentd vdagent

build: $(TARGETS)

install: build
	install -d $(DESTDIR)$(sbindir)
	install -s $(TARGETS) $(DESTDIR)$(sbindir)
	install -d $(DESTDIR)$(udevdir)
	install -m 644 *.rules $(DESTDIR)$(udevdir)

clean:
	rm -f $(TARGETS) *.o *~

vdagentd: vdagentd.o udscs.o
	$(CC) -o $@ $^

vdagent: vdagent.o vdagent-x11.o udscs.o
	$(CC) -o $@ $^ -lX11
