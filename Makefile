MODULE_big = pg_vault
OBJS = src/pg_vault.o

EXTENSION = pg_vault
DATA = sql/pg_vault--0.0.1.sql
MODULES = pg_vault

CFLAGS=`pg_config --includedir-server`

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

all: pg_vault.so

shared_ispell.so: $(OBJS)

%.o : src/%.c
