
DESTDIR	?= 
sbindir	?= /sbin
udevdir	?= /lib/udev/rules.d

CFLAGS	?= -O2
CFLAGS	+= -g -Wall

TARGETS	:= vdagent

build: $(TARGETS)

install: build
	install -d $(DESTDIR)$(sbindir)
	install -s $(TARGETS) $(DESTDIR)$(sbindir)
	install -d $(DESTDIR)$(udevdir)
	install -m 644 *.rules $(DESTDIR)$(udevdir)

clean:
	rm -f $(TARGET) *.o *~

vdagent: vdagent.o
