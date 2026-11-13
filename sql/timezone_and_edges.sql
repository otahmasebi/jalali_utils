CREATE EXTENSION jalali_utils;

-- timestamptz follows the session time zone
SET TIME ZONE 'UTC';
SELECT format_jalali('2026-11-12 19:44:17+00'::timestamptz);
SET TIME ZONE 'America/New_York';
SELECT format_jalali('2026-11-12 19:44:17+00'::timestamptz);
SET TIME ZONE 'Asia/Tehran';
SELECT format_jalali('2026-11-12 19:44:17+00'::timestamptz);
SELECT jalali_part('day', '2026-11-12 20:30:00+00'::timestamptz);

-- timestamp without time zone is read as it is, whatever the session time zone
SET TIME ZONE 'America/New_York';
SELECT format_jalali('2026-11-12 23:14:17'::timestamp);
SELECT format_jalali('2026-11-12 23:14:17'::timestamp, false);
SELECT jalali_part('day', '2026-11-12 23:14:17'::timestamp);
SELECT jalali_part('month', '2026-03-20 23:30:00'::timestamp);
SELECT jalali_part('month', '2026-03-21 00:15:00'::timestamp);
SET TIME ZONE 'UTC';

-- a date has no time of day
SELECT jalali_part('hour', '2019-07-07'::date), jalali_part('minute', '2019-07-07'::date),
       jalali_part('second', '2019-07-07'::date);

-- weekdays
SELECT jalali_part('dow', '2026-11-14'::date) AS saturday_dow,
       jalali_part('isodow', '2026-11-15'::date) AS sunday_isodow,
       jalali_part('isodow', '2026-11-16'::date) AS monday_isodow;

-- infinity and dates before 622-03-21 have no Jalali date
SELECT format_jalali('infinity'::timestamptz) IS NULL AS inf_tz,
       format_jalali('infinity'::timestamp) IS NULL AS inf_ts,
       jalali_part('year', '-infinity'::date) IS NULL AS minf_date,
       format_jalali('0622-03-20'::date) IS NULL AS before_epoch;
SELECT format_jalali('0622-03-21'::date), format_jalali('1599-03-19'::date), format_jalali('1600-06-15'::date);
SELECT format_jalali('5874897-12-31'::date);

-- unsupported units
SELECT jalali_part('week', '2026-11-12'::date);

-- unit parsing: case-insensitive, surrounding whitespace ignored
SELECT jalali_part('  YEAR ', '2026-11-12'::date);
SELECT jalali_part('', '2026-11-12'::date);

-- volatility / parallel safety
SELECT p.oid::regprocedure AS func, p.provolatile, p.proparallel
FROM pg_proc p JOIN pg_depend d ON d.objid = p.oid AND d.deptype = 'e'
JOIN pg_extension e ON e.oid = d.refobjid AND e.extname = 'jalali_utils'
ORDER BY p.oid::regprocedure::text;

-- immutable overloads are indexable
CREATE TEMP TABLE ju_idx(d date);
CREATE INDEX ON ju_idx (jalali_part('year', d));

DROP TABLE ju_idx;
DROP EXTENSION jalali_utils;
