-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION jalali_utils UPDATE TO '0.0.4'" to load this file. \quit

CREATE FUNCTION is_jalali_leap(INTEGER) RETURNS BOOLEAN
  AS 'MODULE_PATHNAME', 'is_jalali_leap' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION jalali_days_in_month(INTEGER, INTEGER) RETURNS INTEGER
  AS 'MODULE_PATHNAME', 'jalali_days_in_month' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION make_jalali_date(INTEGER, INTEGER, INTEGER) RETURNS DATE
  AS 'MODULE_PATHNAME', 'make_jalali_date' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION jalali_to_date(TEXT) RETURNS DATE
  AS 'MODULE_PATHNAME', 'jalali_to_date' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION jalali_trunc(TEXT, TIMESTAMP WITH TIME ZONE) RETURNS TIMESTAMP WITH TIME ZONE
  AS 'MODULE_PATHNAME', 'jalali_trunc_timestamp' LANGUAGE C STABLE STRICT PARALLEL SAFE;
CREATE FUNCTION jalali_trunc(TEXT, TIMESTAMP WITHOUT TIME ZONE) RETURNS TIMESTAMP WITHOUT TIME ZONE
  AS 'MODULE_PATHNAME', 'jalali_trunc_timestamp_naive' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION jalali_trunc(TEXT, DATE) RETURNS DATE
  AS 'MODULE_PATHNAME', 'jalali_trunc_date' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION jalali_period_end(TEXT, TIMESTAMP WITH TIME ZONE) RETURNS TIMESTAMP WITH TIME ZONE
  AS 'MODULE_PATHNAME', 'jalali_period_end_timestamp' LANGUAGE C STABLE STRICT PARALLEL SAFE;
CREATE FUNCTION jalali_period_end(TEXT, TIMESTAMP WITHOUT TIME ZONE) RETURNS TIMESTAMP WITHOUT TIME ZONE
  AS 'MODULE_PATHNAME', 'jalali_period_end_timestamp_naive' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION jalali_period_end(TEXT, DATE) RETURNS DATE
  AS 'MODULE_PATHNAME', 'jalali_period_end_date' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- [start, end) of the Jalali period containing the value
CREATE FUNCTION jalali_period(unit TEXT, d DATE) RETURNS DATERANGE
  LANGUAGE SQL IMMUTABLE STRICT PARALLEL SAFE
  AS $$ SELECT daterange(jalali_trunc(unit, d), jalali_period_end(unit, d), '[)') $$;
CREATE FUNCTION jalali_period(unit TEXT, ts TIMESTAMP WITHOUT TIME ZONE) RETURNS TSRANGE
  LANGUAGE SQL IMMUTABLE STRICT PARALLEL SAFE
  AS $$ SELECT tsrange(jalali_trunc(unit, ts), jalali_period_end(unit, ts), '[)') $$;
CREATE FUNCTION jalali_period(unit TEXT, ts TIMESTAMP WITH TIME ZONE) RETURNS TSTZRANGE
  LANGUAGE SQL STABLE STRICT PARALLEL SAFE
  AS $$ SELECT tstzrange(jalali_trunc(unit, ts), jalali_period_end(unit, ts), '[)') $$;

CREATE FUNCTION to_char_jalali(TIMESTAMP WITH TIME ZONE, TEXT, TEXT DEFAULT 'en') RETURNS TEXT
  AS 'MODULE_PATHNAME', 'to_char_jalali_timestamp' LANGUAGE C STABLE STRICT PARALLEL SAFE;
CREATE FUNCTION to_char_jalali(TIMESTAMP WITHOUT TIME ZONE, TEXT, TEXT DEFAULT 'en') RETURNS TEXT
  AS 'MODULE_PATHNAME', 'to_char_jalali_timestamp_naive' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION to_char_jalali(DATE, TEXT, TEXT DEFAULT 'en') RETURNS TEXT
  AS 'MODULE_PATHNAME', 'to_char_jalali_date' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- Planner statistics for Jalali expressions. Without them the planner guesses
-- (about 1 row for "jalali_part('year', d) = 1404"); with them the estimate is accurate.
-- Statistics expressions must be immutable, so a timestamptz column is converted to
-- local wall-clock time in `tz` first (use the same zone your queries use).
CREATE FUNCTION jalali_create_statistics(tbl REGCLASS, col NAME, tz TEXT DEFAULT 'Asia/Tehran')
RETURNS TEXT LANGUAGE plpgsql AS $fn$
DECLARE
  typ TEXT;
  expr TEXT;
  stat TEXT := left('jalali_stats_' || (SELECT relname FROM pg_class WHERE oid = tbl) || '_' || col, 63);
BEGIN
  SELECT format_type(atttypid, NULL) INTO typ FROM pg_attribute
   WHERE attrelid = tbl AND attname = col AND NOT attisdropped;
  IF typ IS NULL THEN
    RAISE EXCEPTION 'column % not found in %', col, tbl;
  ELSIF typ = 'timestamp with time zone' THEN
    expr := format('(%I AT TIME ZONE %L)', col, tz);
  ELSIF typ IN ('date', 'timestamp without time zone') THEN
    expr := format('%I', col);
  ELSE
    RAISE EXCEPTION 'column % has type %, expected date or timestamp', col, typ;
  END IF;
  EXECUTE format('DROP STATISTICS IF EXISTS %I', stat);
  EXECUTE format('CREATE STATISTICS %I (ndistinct, mcv) ON jalali_part(''year'', %s), jalali_part(''month'', %s), jalali_part(''day'', %s) FROM %s',
                 stat, expr, expr, expr, tbl);
  EXECUTE format('ANALYZE %s', tbl);
  RETURN stat;
END $fn$;

-- ---------------------------------------------------------------------------
-- add months / age / last day / series
-- ---------------------------------------------------------------------------
CREATE FUNCTION jalali_add_months(DATE, INTEGER) RETURNS DATE
  AS 'MODULE_PATHNAME', 'jalali_add_months_date' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION jalali_add_months(TIMESTAMP WITHOUT TIME ZONE, INTEGER) RETURNS TIMESTAMP WITHOUT TIME ZONE
  AS 'MODULE_PATHNAME', 'jalali_add_months_timestamp_naive' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION jalali_add_months(TIMESTAMP WITH TIME ZONE, INTEGER) RETURNS TIMESTAMP WITH TIME ZONE
  AS 'MODULE_PATHNAME', 'jalali_add_months_timestamp' LANGUAGE C STABLE STRICT PARALLEL SAFE;

CREATE FUNCTION jalali_age(DATE, DATE) RETURNS INTERVAL
  AS 'MODULE_PATHNAME', 'jalali_age_date' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION jalali_age(TIMESTAMP WITHOUT TIME ZONE, TIMESTAMP WITHOUT TIME ZONE) RETURNS INTERVAL
  AS 'MODULE_PATHNAME', 'jalali_age_timestamp_naive' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION jalali_age(TIMESTAMP WITH TIME ZONE, TIMESTAMP WITH TIME ZONE) RETURNS INTERVAL
  AS 'MODULE_PATHNAME', 'jalali_age_timestamp' LANGUAGE C STABLE STRICT PARALLEL SAFE;
CREATE FUNCTION jalali_age(DATE) RETURNS INTERVAL
  LANGUAGE SQL STABLE STRICT PARALLEL SAFE AS $$ SELECT jalali_age(current_date, $1) $$;
CREATE FUNCTION jalali_age(TIMESTAMP WITH TIME ZONE) RETURNS INTERVAL
  LANGUAGE SQL STABLE STRICT PARALLEL SAFE AS $$ SELECT jalali_age(now(), $1) $$;

CREATE FUNCTION jalali_last_day_of_month(DATE) RETURNS DATE
  LANGUAGE SQL IMMUTABLE STRICT PARALLEL SAFE AS $$ SELECT jalali_period_end('month', $1) - 1 $$;
CREATE FUNCTION jalali_last_day_of_month(TIMESTAMP WITHOUT TIME ZONE) RETURNS DATE
  LANGUAGE SQL IMMUTABLE STRICT PARALLEL SAFE AS $$ SELECT jalali_period_end('month', $1::date) - 1 $$;
CREATE FUNCTION jalali_last_day_of_month(TIMESTAMP WITH TIME ZONE) RETURNS DATE
  LANGUAGE SQL STABLE STRICT PARALLEL SAFE AS $$ SELECT jalali_period_end('month', ($1)::timestamp::date) - 1 $$;

-- generate_series over Jalali calendar units. Months, quarters and years are computed from the
-- start value (start + i * step), so a clamped 30 Esfand does not drift.
CREATE FUNCTION jalali_series(from_d DATE, to_d DATE, unit TEXT DEFAULT 'month', step INTEGER DEFAULT 1)
RETURNS SETOF DATE LANGUAGE plpgsql IMMUTABLE STRICT PARALLEL SAFE AS $fn$
DECLARE
  u TEXT := lower(btrim(unit));
  i INTEGER := 0;
  v DATE;
BEGIN
  IF step = 0 THEN RAISE EXCEPTION 'step size cannot equal zero'; END IF;
  IF u NOT IN ('day', 'week', 'month', 'quarter', 'year') THEN
    RAISE EXCEPTION 'jalali_series units "%" not recognized or not supported', u;
  END IF;
  LOOP
    v := CASE u WHEN 'day' THEN from_d + i * step
                WHEN 'week' THEN from_d + 7 * i * step
                WHEN 'month' THEN jalali_add_months(from_d, i * step)
                WHEN 'quarter' THEN jalali_add_months(from_d, 3 * i * step)
                ELSE jalali_add_months(from_d, 12 * i * step) END;
    EXIT WHEN (step > 0 AND v > to_d) OR (step < 0 AND v < to_d);
    RETURN NEXT v;
    i := i + 1;
  END LOOP;
END $fn$;

CREATE FUNCTION jalali_series(from_t TIMESTAMP WITHOUT TIME ZONE, to_t TIMESTAMP WITHOUT TIME ZONE,
                              unit TEXT DEFAULT 'month', step INTEGER DEFAULT 1)
RETURNS SETOF TIMESTAMP WITHOUT TIME ZONE LANGUAGE plpgsql IMMUTABLE STRICT PARALLEL SAFE AS $fn$
DECLARE
  u TEXT := lower(btrim(unit));
  i INTEGER := 0;
  v TIMESTAMP;
BEGIN
  IF step = 0 THEN RAISE EXCEPTION 'step size cannot equal zero'; END IF;
  IF u NOT IN ('day', 'week', 'month', 'quarter', 'year', 'hour', 'minute', 'second') THEN
    RAISE EXCEPTION 'jalali_series units "%" not recognized or not supported', u;
  END IF;
  LOOP
    v := CASE u WHEN 'second' THEN from_t + i * step * interval '1 second'
                WHEN 'minute' THEN from_t + i * step * interval '1 minute'
                WHEN 'hour' THEN from_t + i * step * interval '1 hour'
                WHEN 'day' THEN from_t + i * step * interval '1 day'
                WHEN 'week' THEN from_t + 7 * i * step * interval '1 day'
                WHEN 'month' THEN jalali_add_months(from_t, i * step)
                WHEN 'quarter' THEN jalali_add_months(from_t, 3 * i * step)
                ELSE jalali_add_months(from_t, 12 * i * step) END;
    EXIT WHEN (step > 0 AND v > to_t) OR (step < 0 AND v < to_t);
    RETURN NEXT v;
    i := i + 1;
  END LOOP;
END $fn$;

CREATE FUNCTION jalali_series(from_t TIMESTAMP WITH TIME ZONE, to_t TIMESTAMP WITH TIME ZONE,
                              unit TEXT DEFAULT 'month', step INTEGER DEFAULT 1)
RETURNS SETOF TIMESTAMP WITH TIME ZONE LANGUAGE plpgsql STABLE STRICT PARALLEL SAFE AS $fn$
DECLARE
  u TEXT := lower(btrim(unit));
  i INTEGER := 0;
  v TIMESTAMPTZ;
BEGIN
  IF step = 0 THEN RAISE EXCEPTION 'step size cannot equal zero'; END IF;
  IF u NOT IN ('day', 'week', 'month', 'quarter', 'year', 'hour', 'minute', 'second') THEN
    RAISE EXCEPTION 'jalali_series units "%" not recognized or not supported', u;
  END IF;
  LOOP
    v := CASE u WHEN 'second' THEN from_t + i * step * interval '1 second'
                WHEN 'minute' THEN from_t + i * step * interval '1 minute'
                WHEN 'hour' THEN from_t + i * step * interval '1 hour'
                WHEN 'day' THEN from_t + i * step * interval '1 day'
                WHEN 'week' THEN from_t + 7 * i * step * interval '1 day'
                WHEN 'month' THEN jalali_add_months(from_t, i * step)
                WHEN 'quarter' THEN jalali_add_months(from_t, 3 * i * step)
                ELSE jalali_add_months(from_t, 12 * i * step) END;
    EXIT WHEN (step > 0 AND v > to_t) OR (step < 0 AND v < to_t);
    RETURN NEXT v;
    i := i + 1;
  END LOOP;
END $fn$;

-- ---------------------------------------------------------------------------
-- make_jalali_timestamp, to_date / to_timestamp with patterns
-- ---------------------------------------------------------------------------
CREATE FUNCTION make_jalali_timestamp(y INTEGER, mo INTEGER, d INTEGER, h INTEGER, mi INTEGER, s DOUBLE PRECISION)
RETURNS TIMESTAMP WITHOUT TIME ZONE LANGUAGE SQL IMMUTABLE STRICT PARALLEL SAFE
AS $$ SELECT make_timestamp(extract(year FROM make_jalali_date(y, mo, d))::int,
                            extract(month FROM make_jalali_date(y, mo, d))::int,
                            extract(day FROM make_jalali_date(y, mo, d))::int, h, mi, s) $$;
-- the zone argument is the zone the Jalali wall-clock time is in
CREATE FUNCTION make_jalali_timestamptz(y INTEGER, mo INTEGER, d INTEGER, h INTEGER, mi INTEGER,
                                        s DOUBLE PRECISION, tz TEXT DEFAULT NULL)
RETURNS TIMESTAMP WITH TIME ZONE LANGUAGE SQL STABLE PARALLEL SAFE
AS $$ SELECT CASE WHEN tz IS NULL THEN make_jalali_timestamp(y, mo, d, h, mi, s)::timestamptz
                  ELSE make_jalali_timestamp(y, mo, d, h, mi, s) AT TIME ZONE tz END $$;

CREATE FUNCTION jalali_to_date(TEXT, TEXT) RETURNS DATE
  AS 'MODULE_PATHNAME', 'jalali_to_date_fmt' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION jalali_to_timestamp(TEXT, TEXT) RETURNS TIMESTAMP WITHOUT TIME ZONE
  AS 'MODULE_PATHNAME', 'jalali_to_timestamp_fmt' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION jalali_to_timestamptz(TEXT, TEXT, TEXT DEFAULT NULL) RETURNS TIMESTAMP WITH TIME ZONE
  LANGUAGE SQL STABLE PARALLEL SAFE
  AS $$ SELECT CASE WHEN $3 IS NULL THEN jalali_to_timestamp($1, $2)::timestamptz
                    ELSE jalali_to_timestamp($1, $2) AT TIME ZONE $3 END $$;

-- ---------------------------------------------------------------------------
-- jalali_bin: n-unit bins aligned to the Jalali calendar (date_bin has an arbitrary origin)
-- ---------------------------------------------------------------------------
CREATE FUNCTION jalali_bin(INTEGER, TEXT, DATE) RETURNS DATE
  AS 'MODULE_PATHNAME', 'jalali_bin_date' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION jalali_bin(INTEGER, TEXT, TIMESTAMP WITHOUT TIME ZONE) RETURNS TIMESTAMP WITHOUT TIME ZONE
  AS 'MODULE_PATHNAME', 'jalali_bin_timestamp_naive' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION jalali_bin(INTEGER, TEXT, TIMESTAMP WITH TIME ZONE) RETURNS TIMESTAMP WITH TIME ZONE
  AS 'MODULE_PATHNAME', 'jalali_bin_timestamp' LANGUAGE C STABLE STRICT PARALLEL SAFE;

-- ---------------------------------------------------------------------------
-- explicit time zone argument, like date_trunc(unit, ts, zone)
-- ---------------------------------------------------------------------------
CREATE FUNCTION jalali_part(TEXT, TIMESTAMP WITH TIME ZONE, TEXT) RETURNS INTEGER
  LANGUAGE SQL IMMUTABLE STRICT PARALLEL SAFE AS $$ SELECT jalali_part($1, $2 AT TIME ZONE $3) $$;
CREATE FUNCTION jalali_trunc(TEXT, TIMESTAMP WITH TIME ZONE, TEXT) RETURNS TIMESTAMP WITH TIME ZONE
  LANGUAGE SQL IMMUTABLE STRICT PARALLEL SAFE AS $$ SELECT jalali_trunc($1, $2 AT TIME ZONE $3) AT TIME ZONE $3 $$;
CREATE FUNCTION jalali_period_end(TEXT, TIMESTAMP WITH TIME ZONE, TEXT) RETURNS TIMESTAMP WITH TIME ZONE
  LANGUAGE SQL IMMUTABLE STRICT PARALLEL SAFE AS $$ SELECT jalali_period_end($1, $2 AT TIME ZONE $3) AT TIME ZONE $3 $$;
CREATE FUNCTION jalali_period(TEXT, TIMESTAMP WITH TIME ZONE, TEXT) RETURNS TSTZRANGE
  LANGUAGE SQL IMMUTABLE STRICT PARALLEL SAFE
  AS $$ SELECT tstzrange(jalali_trunc($1, $2, $3), jalali_period_end($1, $2, $3), '[)') $$;

-- ---------------------------------------------------------------------------
-- in_jalali_period(col, unit, ref): true when col is in the Jalali period of ref.
-- The support function turns it into col >= start AND col < end, so a plain index on col is used.
-- ---------------------------------------------------------------------------
CREATE FUNCTION jalali_period_support(INTERNAL) RETURNS INTERNAL
  AS 'MODULE_PATHNAME', 'jalali_period_support' LANGUAGE C;
CREATE FUNCTION in_jalali_period(DATE, TEXT, DATE) RETURNS BOOLEAN
  AS 'MODULE_PATHNAME', 'in_jalali_period_date' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE
  SUPPORT jalali_period_support;
CREATE FUNCTION in_jalali_period(TIMESTAMP WITHOUT TIME ZONE, TEXT, TIMESTAMP WITHOUT TIME ZONE) RETURNS BOOLEAN
  AS 'MODULE_PATHNAME', 'in_jalali_period_timestamp_naive' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE
  SUPPORT jalali_period_support;
CREATE FUNCTION in_jalali_period(TIMESTAMP WITH TIME ZONE, TEXT, TIMESTAMP WITH TIME ZONE) RETURNS BOOLEAN
  AS 'MODULE_PATHNAME', 'in_jalali_period_timestamp' LANGUAGE C STABLE STRICT PARALLEL SAFE
  SUPPORT jalali_period_support;

-- ---------------------------------------------------------------------------
-- jalali_date: stored exactly like date; reads and prints as 1404/01/01. Sorts and compares
-- as a date, casts to and from date for free.
-- ---------------------------------------------------------------------------
CREATE TYPE jalali_date;
CREATE FUNCTION jalali_date_in(CSTRING) RETURNS jalali_date
  AS 'MODULE_PATHNAME', 'jalali_date_in' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION jalali_date_out(jalali_date) RETURNS CSTRING
  AS 'MODULE_PATHNAME', 'jalali_date_out' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION jalali_date_recv(INTERNAL) RETURNS jalali_date
  AS 'date_recv' LANGUAGE internal IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION jalali_date_send(jalali_date) RETURNS BYTEA
  AS 'date_send' LANGUAGE internal IMMUTABLE STRICT PARALLEL SAFE;
CREATE TYPE jalali_date (
  INPUT = jalali_date_in, OUTPUT = jalali_date_out, RECEIVE = jalali_date_recv, SEND = jalali_date_send,
  INTERNALLENGTH = 4, PASSEDBYVALUE, ALIGNMENT = int4, CATEGORY = 'D'
);
CREATE CAST (jalali_date AS DATE) WITHOUT FUNCTION AS IMPLICIT;
CREATE CAST (DATE AS jalali_date) WITHOUT FUNCTION AS ASSIGNMENT;
CREATE FUNCTION jalali_date_lt(jalali_date, jalali_date) RETURNS BOOLEAN AS 'date_lt' LANGUAGE internal IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION jalali_date_le(jalali_date, jalali_date) RETURNS BOOLEAN AS 'date_le' LANGUAGE internal IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION jalali_date_eq(jalali_date, jalali_date) RETURNS BOOLEAN AS 'date_eq' LANGUAGE internal IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION jalali_date_ge(jalali_date, jalali_date) RETURNS BOOLEAN AS 'date_ge' LANGUAGE internal IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION jalali_date_gt(jalali_date, jalali_date) RETURNS BOOLEAN AS 'date_gt' LANGUAGE internal IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION jalali_date_ne(jalali_date, jalali_date) RETURNS BOOLEAN AS 'date_ne' LANGUAGE internal IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION jalali_date_cmp(jalali_date, jalali_date) RETURNS INTEGER AS 'date_cmp' LANGUAGE internal IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION jalali_date_hash(jalali_date) RETURNS INTEGER AS 'hashint4' LANGUAGE internal IMMUTABLE STRICT PARALLEL SAFE;
CREATE OPERATOR < (LEFTARG = jalali_date, RIGHTARG = jalali_date, FUNCTION = jalali_date_lt, COMMUTATOR = >, NEGATOR = >=, RESTRICT = scalarltsel, JOIN = scalarltjoinsel);
CREATE OPERATOR <= (LEFTARG = jalali_date, RIGHTARG = jalali_date, FUNCTION = jalali_date_le, COMMUTATOR = >=, NEGATOR = >, RESTRICT = scalarlesel, JOIN = scalarlejoinsel);
CREATE OPERATOR = (LEFTARG = jalali_date, RIGHTARG = jalali_date, FUNCTION = jalali_date_eq, COMMUTATOR = =, NEGATOR = <>, RESTRICT = eqsel, JOIN = eqjoinsel, HASHES, MERGES);
CREATE OPERATOR >= (LEFTARG = jalali_date, RIGHTARG = jalali_date, FUNCTION = jalali_date_ge, COMMUTATOR = <=, NEGATOR = <, RESTRICT = scalargesel, JOIN = scalargejoinsel);
CREATE OPERATOR > (LEFTARG = jalali_date, RIGHTARG = jalali_date, FUNCTION = jalali_date_gt, COMMUTATOR = <, NEGATOR = <=, RESTRICT = scalargtsel, JOIN = scalargtjoinsel);
CREATE OPERATOR <> (LEFTARG = jalali_date, RIGHTARG = jalali_date, FUNCTION = jalali_date_ne, COMMUTATOR = <>, NEGATOR = =, RESTRICT = neqsel, JOIN = neqjoinsel);
CREATE OPERATOR CLASS jalali_date_ops DEFAULT FOR TYPE jalali_date USING btree AS
  OPERATOR 1 <, OPERATOR 2 <=, OPERATOR 3 =, OPERATOR 4 >=, OPERATOR 5 >, FUNCTION 1 jalali_date_cmp(jalali_date, jalali_date);
CREATE OPERATOR CLASS jalali_date_ops DEFAULT FOR TYPE jalali_date USING hash AS
  OPERATOR 1 =, FUNCTION 1 jalali_date_hash(jalali_date);
