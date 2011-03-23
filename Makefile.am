ACLOCAL_AMFLAGS = ${ACLOCAL_FLAGS}

bin_PROGRAMS = spice-vdagent
sbin_PROGRAMS = spice-vdagentd

spice_vdagent_CFLAGS = $(X_CFLAGS) $(SPICE_CFLAGS)
spice_vdagent_LDADD = $(X_LIBS) $(SPICE_LIBS)
spice_vdagent_SOURCES = src/vdagent.c src/vdagent-x11.c src/udscs.c

spice_vdagentd_CFLAGS = $(DBUS_CFLAGS) $(SPICE_CFLAGS)
spice_vdagentd_LDADD = $(DBUS_LIBS) $(SPICE_LIBS)
spice_vdagentd_SOURCES = vdagentd.c \
                         vdagentd-uinput.c \
                         vdagent-virtio-port.c \
                         console-kit.c \
                         udscs.c

noinst_HEADERS = console-kit.h \
                 vdagentd-proto-strings.h \
                 vdagent-x11.h \
                 udscs.h \
                 vdagentd-uinput.h \
                 vdagentd-proto.h \
                 vdagent-virtio-port.h
