MODULE_big = cxxmapam
OBJS = src/cxxmapam_handler.o

EXTENSION = cxxmapam
DATA = cxxmapam--1.0.sql
PGFILEDESC = "cxxmapam - table access method using C++ std::map"

REGRESS = cxxmapam

PG_CPPFLAGS = -std=c++20
PG_CXXFLAGS = -fPIC

UNAME := $(shell uname -s)
ifeq ($(UNAME), Darwin)
SHLIB_LINK = -lc++
endif
ifeq ($(UNAME), Linux)
SHLIB_LINK = -lstdc++
endif

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/cxxmapam
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
