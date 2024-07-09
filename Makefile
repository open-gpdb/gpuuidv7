# contrib/pguuidv7/Makefile

MODULE_big = pguuidv7
OBJS = pguuidv7.o

EXTENSION = pguuidv7
DATA = pguuidv7--1.0.sql

REGRESS = pguuidv7

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pguuidv7
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
