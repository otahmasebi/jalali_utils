MODULES = jalali_utils
EXTENSION = jalali_utils
TESTS = $(wildcard sql/*.sql)
REGRESS = $(patsubst sql/%.sql,%,$(TESTS))
DATA = jalali_utils--0.0.1.sql jalali_utils--0.0.1--0.0.2.sql jalali_utils--0.0.2--0.0.3.sql jalali_utils--0.0.3--0.0.4.sql
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
