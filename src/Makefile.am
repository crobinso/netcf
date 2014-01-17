
GNULIB= ../gnulib/lib/libgnu.la
GNULIB_CFLAGS= -I$(top_srcdir)/gnulib/lib -I$(top_builddir)/gnulib/lib

POD2MAN = pod2man -c "Network Configuration" -r "$(PACKAGE)-$(VERSION)"

dist_man1_MANS = ncftool.1

AM_CFLAGS = $(NETCF_CFLAGS) $(GNULIB_CFLAGS) $(WARN_CFLAGS) \
  $(LIBXML_CFLAGS) $(LIBXSLT_CFLAGS) $(LIBAUGEAS_CFLAGS) $(LIBNL_CFLAGS) $(LIBNL_ROUTE3_CFLAGS)

AM_LDFLAGS = $(LIBNL_LIBS) $(LIBNL_ROUTE3_LIBS)

include_HEADERS = netcf.h

lib_LTLIBRARIES = libnetcf.la

bin_PROGRAMS = ncftool
if NETCF_TRANSACTION_SUPPORT
libexec_SCRIPTS = netcf-transaction.sh
endif
if ! NETCF_DRIVER_MSWINDOWS
noinst_PROGRAMS = ncftransform
endif

DRIVER_SOURCES_COMMON = dutil.h dutil.c
DRIVER_SOURCES_LINUX = dutil_linux.h dutil_linux.c
DRIVER_SOURCES_MSWINDOWS = dutil_mswindows.h dutil_mswindows.c drv_mswindows.c
DRIVER_SOURCES_POSIX = dutil_posix.h dutil_posix.c
DRIVER_SOURCES_REDHAT = drv_redhat.c
DRIVER_SOURCES_DEBIAN = drv_debian.c
DRIVER_SOURCES_SUSE = drv_suse.c

EXTRA_DIST = netcf_public.syms \
	netcf_private.syms \
	$(DRIVER_SOURCES_COMMON) \
	$(DRIVER_SOURCES_POSIX) \
	$(DRIVER_SOURCES_LINUX) \
	$(DRIVER_SOURCES_MSWINDOWS) \
	$(DRIVER_SOURCES_REDHAT) \
        $(DRIVER_SOURCES_DEBIAN) \
	ncftool.pod \
        $(DRIVER_SOURCES_SUSE)

if NETCF_DRIVER_REDHAT
DRIVER_SOURCES = \
	$(DRIVER_SOURCES_COMMON) \
	$(DRIVER_SOURCES_POSIX) \
	$(DRIVER_SOURCES_LINUX) \
	$(DRIVER_SOURCES_REDHAT)
endif
if NETCF_DRIVER_DEBIAN
DRIVER_SOURCES = \
	$(DRIVER_SOURCES_COMMON) \
	$(DRIVER_SOURCES_POSIX) \
	$(DRIVER_SOURCES_LINUX) \
	$(DRIVER_SOURCES_DEBIAN)
endif
if NETCF_DRIVER_SUSE
DRIVER_SOURCES = \
	$(DRIVER_SOURCES_COMMON) \
	$(DRIVER_SOURCES_POSIX) \
	$(DRIVER_SOURCES_LINUX) \
	$(DRIVER_SOURCES_SUSE)
endif
if NETCF_DRIVER_MSWINDOWS
DRIVER_SOURCES = \
	$(DRIVER_SOURCES_COMMON) \
	$(DRIVER_SOURCES_MSWINDOWS)
endif

BUILT_SOURCES = datadir.h netcf.syms

DISTCLEANFILES = datadir.h

libnetcf_la_SOURCES = netcf.h netcf.c internal.h \
     ref.h list.h \
     xslt_ext.c $(DRIVER_SOURCES)
libnetcf_la_LDFLAGS = -Wl,--version-script=netcf.syms \
     -version-info $(LIBNETCF_VERSION_INFO) -no-undefined $(LIBNL_CFLAGS) $(LIBNL_ROUTE3_CFLAGS)
libnetcf_la_LIBADD = $(NETCF_LIBDEPS) $(GNULIB) $(LIBNL_LIBS) $(LIBNL_ROUTE3_LIBS)
if NETCF_DRIVER_MSWINDOWS
libnetcf_la_LIBADD += -liphlpapi
endif
libnetcf_la_DEPENDENCIES = $(GNULIB) netcf.syms

ncftool_SOURCES = ncftool.c
ncftool_LDADD = libnetcf.la $(READLINE_LIBS) $(GNULIB) $(LIBNL_LIBS) $(LIBNL_ROUTE3_LIBS)

ncftool.1: ncftool.pod
	$(AM_V_GEN)$(POD2MAN) $< $(srcdir)/$@

if ! NETCF_DRIVER_MSWINDOWS
ncftransform_SOURCES = ncftransform.c
ncftransform_LDADD = libnetcf.la $(GNULIB)
endif

netcf.syms: netcf_public.syms netcf_private.syms
	rm -f $@-tmp $@
	printf '# WARNING: generated from the following files:\n# $^\n\n' >$@-tmp
	cat $(srcdir)/netcf_public.syms >>$@-tmp
	printf '\n\n# Private symbols\n\n' >>$@-tmp
	printf 'NETCF_PRIVATE_$(VERSION) {\n\n'  >>$@-tmp
	printf 'global:\n\n' >>$@-tmp
	cat $(srcdir)/netcf_private.syms >>$@-tmp
	printf '\n\nlocal:\n*;\n\n};' >>$@-tmp
	chmod a-w $@-tmp
	mv $@-tmp $@

# Generate datadir.h. That's where we look for stylesheets
internal.h: datadir.h

datadir.h: $(top_builddir)/config.status
	echo '#define NETCF_DATADIR "$(datadir)"' > datadir.h

EXTRA_DIST += netcf-transaction.sh.in \
	      netcf-transaction.init.in \
	      netcf-transaction.service.in

if NETCF_DRIVER_REDHAT

install-data-local: install-sysinit

uninstall-local: uninstall-sysinit

# This is for the shell script that handles network config change
# transactions. It is used by both the initscripts and systemd
# flavors, as well as by libnetcf.so itself
netcf-transaction.sh: netcf-transaction.sh.in $(top_builddir)/config.status
	$(AM_V_GEN)sed					\
	    -e 's![@]localstatedir[@]!$(localstatedir)!g'	\
	    -e 's![@]sysconfdir[@]!$(sysconfdir)!g'	\
	    < $< > $@-t &&				\
	    chmod a+x $@-t &&				\
	    mv $@-t $@

BUILT_SOURCES += netcf-transaction.sh

if NETCF_USE_INITSCRIPTS
install-sysinit: netcf-transaction.init
	$(MKDIR_P) $(DESTDIR)$(sysconfdir)/rc.d/init.d
	$(INSTALL_SCRIPT) netcf-transaction.init \
	  $(DESTDIR)$(sysconfdir)/rc.d/init.d/netcf-transaction

uninstall-sysinit:
	rm -f $(DESTDIR)$(sysconfdir)/rc.d/init.d/netcf-transaction \
	  $(DESTDIR)$(sysconfdir)/sysconfig/netcf-transaction

netcf-transaction.init: netcf-transaction.init.in \
                        $(top_builddir)/config.status
	$(AM_V_GEN)sed \
		-e 's![@]localstatedir[@]!$(localstatedir)!g' \
		-e 's|[@]libexecdir[@]|$(libexecdir)|g'     \
	< $< > $@-t &&                                      \
	chmod a+x $@-t &&                                   \
	mv $@-t $@

BUILT_SOURCES += netcf-transaction.init

else ! NETCF_USE_INITSCRIPTS

if NETCF_USE_SYSTEMD
SYSTEMD_UNIT_DIR = $(prefix)/lib/systemd/system

install-sysinit: netcf-transaction.service
	$(MKDIR_P) $(DESTDIR)$(SYSTEMD_UNIT_DIR)
	$(INSTALL_DATA) netcf-transaction.service \
	  $(DESTDIR)$(SYSTEMD_UNIT_DIR)/netcf-transaction.service

uninstall-sysinit:
	rm -f $(DESTDIR)$(SYSTEMD_UNIT_DIR)/netcf-transaction.service
	rmdir $(DESTDIR)$(SYSTEMD_UNIT_DIR) ||:

netcf-transaction.service: netcf-transaction.service.in \
                           $(top_builddir)/config.status
	$(AM_V_GEN)sed						\
	    -e 's|[@]libexecdir[@]|$(libexecdir)|g'		\
	    < $< > $@-t &&					\
	    mv $@-t $@

BUILT_SOURCES += netcf-transaction.service

else ! NETCF_USE_SYSTEMD
install-sysinit:
uninstall-sysinit:
endif ! NETCF_USE_SYSTEMD
endif ! NETCF_USE_INITSCRIPTS
endif NETCF_DRIVER_REDHAT

DISTCLEANFILES += $(BUILT_SOURCES)
