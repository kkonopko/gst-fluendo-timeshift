plugin_LTLIBRARIES = libgsttstimeshift.la

# sources used to compile this plugin
libgsttstimeshift_la_SOURCES = \
  gsttsindexer.c \
  gsttsseeker.c \
  gsttsshifter.c \
  gsttsshifterbin.c \
  gsttsstamper.c \
  gsttstimeshift.c \
  tscache.c \
  tsindex.c \
  tsmemindex.c

libgsttstimeshift_la_CFLAGS = \
 -I$(top_srcdir)/common \
  $(GST_CFLAGS) \
  $(GST_BASE_CFLAGS) \
  $(GST_PLUGINS_BASE_CFLAGS) \
  $(CPU_TUNE_CFLAGS)

libgsttstimeshift_la_LDFLAGS = \
  $(GST_PLUGIN_LDFLAGS) \
  $(CPU_TUNE_LDFLAGS)

libgsttstimeshift_la_LIBADD = \
  $(GST_LIBS) \
  $(GST_BASE_LIBS) \
  $(GST_PLUGINS_BASE_LIBS)

# headers we need but don't want installed
noinst_HEADERS = \
  gsttsindexer.h \
  gsttsseeker.h \
  gsttsshifter.h \
  gsttsshifterbin.h \
  gsttsstamper.h \
  tscache.h \
  tsindex.h
