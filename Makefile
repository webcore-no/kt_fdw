##############################################################################
#
# Kt Foreign Data Wrapper for PostgreSQL
#
# Copyright (c) 2013 Andrew Dunstan
#
# This software is released under the PostgreSQL Licence
#
# Author: Andrew Dunstan <andrew@dunslane.net>
#
# IDENTIFICATION
#        kt_fdw/Makefile
#
##############################################################################


EXTENSION     = kt_fdw
EXTVERSION    = $(shell grep default_version $(EXTENSION).control | sed -e \
                "s/default_version[[:space:]]*=[[:space:]]*'\([^']*\)'/\1/")

DATA          = $(filter-out $(wildcard sql/*--*.sql),$(wildcard sql/*.sql))
DOCS          = $(wildcard doc/*.md)
USE_MODULE_DB = 1
TESTS         = $(wildcard test/sql/*.sql)
REGRESS       = $(patsubst test/sql/%.sql,%,$(TESTS))
REGRESS_OPTS  = --inputdir=test --outputdir=test \
                --load-extension=$(EXTENSION)

MODULE_big    = $(EXTENSION)
OBJS          = $(patsubst %.c, %.o, $(wildcard src/*.c)) src/ktlangc.o
PG_CONFIG    ?= pg_config
SHLIB_LINK    = -lkyototycoon -lsasl2 -lkyotocabinet

FLAGS         = -march=native -m64 -g -O2 -Wall -Wextra -Werror -fPIC -fsigned-char \
                -Wno-error=unused-parameter \
                -Wno-unused-but-set-variable -Wno-unused-but-set-parameter \
                -DUSE_TRANSACTIONS -DKTLOGVERBOSE -O
PG_CXXFLAGS   = $(CXXFLAGS) $(FLAGS)
PG_CFLAGS     = $(CFLAGS) $(FLAGS)

all: sql/$(EXTENSION)--$(EXTVERSION).sql

sql/$(EXTENSION)--$(EXTVERSION).sql: sql/$(EXTENSION).sql
	cp $< $@

DATA_built    = sql/$(EXTENSION)--$(EXTVERSION).sql
DATA          = $(filter-out sql/$(EXTENSION)--$(EXTVERSION).sql, \
                $(wildcard sql/*--*.sql))

EXTRA_CLEAN   = sql/$(EXTENSION)--$(EXTVERSION).sql

PGXS         := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# we put all the tests in a test subdir, but pgxs expects us not to, darn it
override pg_regress_clean_files = test/results/ test/regression.diffs test/regression.out tmp_check/ log/
