CREATE EXTENSION jalali_utils;
SET TIME ZONE 'Asia/Tehran';
SET DateStyle = 'ISO, YMD';
SET client_min_messages = warning;

-- calendar helpers
SELECT is_jalali_leap(1403), is_jalali_leap(1404), jalali_days_in_month(1403, 12), jalali_days_in_month(1404, 12), jalali_days_in_month(1404, 7);

-- reverse conversion
SELECT make_jalali_date(1403, 12, 30), make_jalali_date(1404, 1, 1), jalali_to_date('۱۴۰۴/۰۱/۰۱'), jalali_to_date(' 1404-1-1 ');
SELECT make_jalali_date(1404, 12, 30);
SELECT jalali_to_date('abc');

-- week of year, Saturday based
SELECT jalali_part('week', date '2025-03-21'), jalali_part('week', date '2025-03-28'), jalali_part('week', date '2026-03-20');

-- trunc and period
SELECT jalali_trunc('month', timestamptz '2025-04-25 10:30+03:30'), jalali_trunc('year', date '2025-04-25'), jalali_trunc('week', date '2025-04-25'), jalali_trunc('quarter', timestamp '2025-08-25 10:30:11');
SELECT jalali_period('month', date '2025-04-25'), jalali_period('year', timestamp '2025-04-25 10:00'), jalali_period('day', timestamptz '2025-04-25 10:00+03:30');
SELECT jalali_trunc('dow', date '2025-04-25');

-- to_char_jalali
SELECT to_char_jalali(timestamptz '2025-03-25 15:04:05.123456+03:30', 'YYYY/MM/DD HH24:MI:SS.US Day Mon Month DDD Q WW W AM');
SELECT to_char_jalali(date '2025-03-25', 'FMDD Month YYYY', 'fa');
SELECT to_char_jalali(date '2025-03-25', 'YYYY', 'de');

-- statistics helper: the planner estimate becomes accurate
CREATE TABLE ju_st AS SELECT date '2020-01-01' + (i % 2000) AS d FROM generate_series(1, 20000) i;
SELECT jalali_create_statistics('ju_st', 'd');
ANALYZE ju_st;
SELECT count(*) AS actual FROM ju_st WHERE jalali_part('year', d) = 1403;
DROP TABLE ju_st;

-- 0.0.4 extras: ISO-style week (Saturday first), add_months, age, last day
SELECT jalali_part('isoweek', date '2025-03-21'), jalali_part('isoyear', date '2025-03-21'), jalali_part('isoweek', date '2026-03-20'), jalali_part('isoyear', date '2026-03-20');
SELECT to_char_jalali(date '2025-03-25', 'IYYY-IW-ID J FMRM FMDDth SSSS');
SELECT jalali_add_months(make_jalali_date(1404, 1, 31), 6), jalali_add_months(make_jalali_date(1403, 12, 30), 12), jalali_add_months(timestamp '2025-03-25 10:11:12', -1);
SELECT jalali_age(make_jalali_date(1404, 3, 5), make_jalali_date(1402, 1, 20)), jalali_age(date '2025-03-21', date '2025-03-21');
SELECT jalali_last_day_of_month(make_jalali_date(1403, 12, 3)), jalali_last_day_of_month(make_jalali_date(1404, 12, 3)), jalali_last_day_of_month(make_jalali_date(1404, 7, 3));

-- series, bin
SELECT x FROM jalali_series(make_jalali_date(1404, 1, 31), make_jalali_date(1404, 6, 1), 'month') x;
SELECT jalali_series(make_jalali_date(1404, 3, 1), make_jalali_date(1404, 1, 1), 'month', -1);
SELECT jalali_series(date '2025-01-01', date '2025-02-01', 'day', 0);
SELECT jalali_bin(3, 'month', date '2025-06-10'), jalali_bin(2, 'week', date '2025-06-10'), jalali_bin(1, 'year', date '2025-06-10');

-- patterns and constructors
SELECT jalali_to_date('5 Farvardin 1404', 'DD Month YYYY'), jalali_to_date('۱۴۰۴/۰۱/۰۵', 'YYYY/MM/DD'), jalali_to_date('1403-366', 'YYYY-DDD'), jalali_to_date('1403-001', 'YYYY-DDD');
SELECT jalali_to_timestamp('1404/01/05 03:04:05 PM', 'YYYY/MM/DD HH12:MI:SS AM'), jalali_to_timestamptz('1404/01/05 12:00', 'YYYY/MM/DD HH24:MI'), jalali_to_timestamptz('1404/01/05 12:00', 'YYYY/MM/DD HH24:MI', 'UTC');
SELECT make_jalali_timestamp(1404, 1, 5, 13, 14, 15.25), make_jalali_timestamptz(1404, 1, 5, 0, 0, 0, 'Asia/Tehran'), make_jalali_timestamptz(1404, 1, 5, 0, 0, 0, 'UTC');
SELECT make_jalali_timestamp(1404, 12, 30, 0, 0, 0);
SELECT jalali_to_date('1404/13/01', 'YYYY/MM/DD');

-- zone argument
SELECT jalali_part('day', timestamptz '2025-04-20 21:00+00', 'America/New_York'), jalali_trunc('month', timestamptz '2025-04-20 21:00+00', 'UTC'), jalali_period('day', timestamptz '2025-04-20 21:00+00', 'Asia/Tehran');

-- in_jalali_period and index use
CREATE TABLE ju_ix AS SELECT date '2020-01-01' + i AS d FROM generate_series(0, 50000) i;
CREATE INDEX ON ju_ix (d);
ANALYZE ju_ix;
SELECT count(*) FROM ju_ix WHERE in_jalali_period(d, 'month', date '2025-04-25');
SET enable_seqscan = off;
SET enable_bitmapscan = off;
EXPLAIN (COSTS OFF) SELECT * FROM ju_ix WHERE in_jalali_period(d, 'month', date '2025-04-25');
RESET enable_seqscan;
RESET enable_bitmapscan;
DROP TABLE ju_ix;

-- jalali_date type
SELECT '1404/01/05'::jalali_date, ('1404/01/05'::jalali_date)::date, (date '2025-03-25')::jalali_date, ('1404/01/05'::jalali_date + 1)::jalali_date;
SELECT 'infinity'::jalali_date, '-infinity'::jalali_date, date '0500-01-01'::jalali_date;
SELECT '1404/12/30'::jalali_date;
SELECT 'abc'::jalali_date;
SELECT '1404/01/05'::jalali_date < '1404/01/06'::jalali_date, to_char_jalali('1404/01/05'::jalali_date, 'YYYY Month');
DROP EXTENSION jalali_utils;
