ACLOCAL_AMFLAGS = ${ACLOCAL_FLAGS}

bin_PROGRAMS = spice-vdagent
sbin_PROGRAMS = spice-vdagentd

spice_vdagent_CFLAGS = $(X_CFLAGS) $(SPICE_CFLAGS)
spice_vdagent_LDADD = $(X_LIBS) $(SPICE_LIBS)
spice_vdagent_SOURCES = src/vdagent.c src/vdagent-x11.c src/udscs.c

spice_vdagentd_CFLAGS = $(DBUS_CFLAGS) $(SPICE_CFLAGS)
spice_vdagentd_LDADD = $(DBUS_LIBS) $(SPICE_LIBS)
spice_vdagentd_SOURCES = src/vdagentd.c \
                         src/vdagentd-uinput.c \
                         src/vdagent-virtio-port.c \
                         src/console-kit.c \
                         src/udscs.c

noinst_HEADERS = src/console-kit.h \
                 src/vdagentd-proto-strings.h \
                 src/vdagent-x11.h \
                 src/udscs.h \
                 src/vdagentd-uinput.h \
                 src/vdagentd-proto.h \
                 src/vdagent-virtio-port.h
