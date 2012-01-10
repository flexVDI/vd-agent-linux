ACLOCAL_AMFLAGS = ${ACLOCAL_FLAGS}

bin_PROGRAMS = src/spice-vdagent
sbin_PROGRAMS = src/spice-vdagentd

src_spice_vdagent_CFLAGS = $(X_CFLAGS) $(SPICE_CFLAGS)
src_spice_vdagent_LDADD = $(X_LIBS) $(SPICE_LIBS)
src_spice_vdagent_SOURCES = src/vdagent.c src/vdagent-x11.c src/udscs.c

src_spice_vdagentd_CFLAGS = $(DBUS_CFLAGS) $(PCIACCESS_CFLAGS) $(SPICE_CFLAGS)
src_spice_vdagentd_LDADD = $(DBUS_LIBS) $(PCIACCESS_LIBS) $(SPICE_LIBS)
src_spice_vdagentd_SOURCES = src/vdagentd.c \
                             src/vdagentd-uinput.c \
                             src/vdagentd-xorg-conf.c \
                             src/vdagent-virtio-port.c \
                             src/udscs.c
if HAVE_CONSOLE_KIT
src_spice_vdagentd_SOURCES += src/console-kit.c
endif

noinst_HEADERS = src/console-kit.h \
                 src/vdagentd-proto-strings.h \
                 src/vdagent-x11.h \
                 src/udscs.h \
                 src/vdagentd-uinput.h \
                 src/vdagentd-xorg-conf.h \
                 src/vdagentd-proto.h \
                 src/vdagent-virtio-port.h

xdgautostartdir = $(sysconfdir)/xdg/autostart
xdgautostart_DATA = $(top_srcdir)/data/spice-vdagent.desktop

gdmautostartdir = $(datadir)/gdm/autostart/LoginWindow
gdmautostart_DATA = $(top_srcdir)/data/spice-vdagent.desktop

initdir = $(sysconfdir)/rc.d/init.d
init_SCRIPTS = $(top_srcdir)/data/spice-vdagentd

install-data-local:
	$(mkdir_p) $(DESTDIR)$(localstatedir)/log/spice-vdagentd
	$(mkdir_p) $(DESTDIR)$(localstatedir)/run/spice-vdagentd


tmpfilesdir = $(sysconfdir)/tmpfiles.d
tmpfiles_DATA = $(top_srcdir)/data/spice-vdagentd.conf

EXTRA_DIST = $(xdgautostart_DATA) $(gdmautostart_DATA) $(init_SCRIPTS) \
             $(tmpfiles_DATA) README.RHEL-5 data/xorg.conf.RHEL-5
