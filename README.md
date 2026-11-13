# Jalali Utils (A Jalali extension for PostgreSQL)
Jalali Utils is a package for converting Gregorian dates to Jalali.
This package consists of the following functions:


```sql
format_jalali(TIMESTAMP WITH TIME ZONE, with_time BOOLEAN DEFAULT TRUE)
format_jalali(TIMESTAMP, with_time BOOLEAN DEFAULT TRUE)
format_jalali(DATE)
jalali_part(TEXT, TIMESTAMP WITH TIME ZONE)
jalali_part(TEXT, TIMESTAMP)
jalali_part(TEXT, DATE)
jalali_trunc(TEXT, TIMESTAMP WITH TIME ZONE | TIMESTAMP | DATE)        -- start of the Jalali period
jalali_period_end(TEXT, TIMESTAMP WITH TIME ZONE | TIMESTAMP | DATE)   -- exclusive end of the period
jalali_period(TEXT, DATE | TIMESTAMP | TIMESTAMP WITH TIME ZONE)       -- [start, end) as a range
make_jalali_date(year, month, day) -> DATE
jalali_to_date(TEXT) -> DATE                                           -- 'Y/M/D' or 'Y-M-D', Persian digits accepted
is_jalali_leap(year), jalali_days_in_month(year, month)
to_char_jalali(TIMESTAMP WITH TIME ZONE | TIMESTAMP | DATE, pattern TEXT, lang TEXT DEFAULT 'en')
jalali_create_statistics(table REGCLASS, column NAME, tz TEXT DEFAULT 'Asia/Tehran')

-- 0.0.4
jalali_part / jalali_trunc / jalali_period_end / jalali_period (..., TIMESTAMPTZ, zone TEXT)   -- explicit time zone
jalali_add_months(DATE | TIMESTAMP | TIMESTAMPTZ, n INT)               -- clamps the day (31 Farvardin + 6 months = 30 Mehr)
jalali_age(a, b) / jalali_age(a)                                       -- years/months/days in the Jalali calendar
jalali_last_day_of_month(DATE | TIMESTAMP | TIMESTAMPTZ) -> DATE
jalali_series(from, to, unit TEXT, step INT DEFAULT 1)                 -- generate_series by Jalali month/quarter/year/week/day...
jalali_bin(n INT, unit TEXT, ts [, origin])                            -- date_bin: bins of n Jalali units
jalali_to_date(text, pattern) / jalali_to_timestamp(text, pattern) / jalali_to_timestamptz(text, pattern [, zone])
make_jalali_timestamp(y, m, d, h, mi, s) / make_jalali_timestamptz(y, m, d, h, mi, s, zone)
in_jalali_period(col, unit, ref) -> boolean                            -- planner rewrites it to an index range condition
CREATE TYPE jalali_date                                                -- binary compatible with date, prints Y/MM/DD
```

### Examples
```sql
SELECT jalali_trunc('month', now());                  -- first instant of this Jalali month (session time zone)
SELECT * FROM sale_order WHERE date_order <@ jalali_period('month', now());   -- index friendly range filter
SELECT make_jalali_date(1404, 1, 1);                  -- 2025-03-21
SELECT to_char_jalali(now(), 'YYYY/MM/DD HH24:MI Day');
SELECT to_char_jalali(current_date, 'FMDD Month YYYY', 'fa');   -- ۵ فروردین ۱۴۰۴
```

`jalali_trunc` / `jalali_period` units: second, minute, hour, day, week (starts on Saturday), month, quarter,
year, decade, century, millennium. `dow`, `isodow`, `doy` and `isoyear` are rejected. Sub-day units on a `date` return it unchanged.
`jalali_part('week', d)` is the Saturday-based week of the Jalali year; week 1 is the week containing 1 Farvardin.
A `timestamptz` result is a real instant at local midnight of the period, so DST changes are respected.

```sql
SELECT jalali_add_months(make_jalali_date(1404,1,31), 6);              -- 2025-10-22 (30 Mehr 1404)
SELECT jalali_age(make_jalali_date(1404,3,5), make_jalali_date(1402,1,20));   -- 2 years 1 mon 16 days
SELECT * FROM jalali_series(make_jalali_date(1404,1,1), make_jalali_date(1404,12,29), 'month');
SELECT jalali_bin(3, 'month', date '2025-06-10');                      -- start of the 3-month Jalali bin
SELECT jalali_to_date('5 Farvardin 1404', 'DD Month YYYY');            -- 2025-03-25
SELECT jalali_to_timestamptz('1404/01/05 12:00', 'YYYY/MM/DD HH24:MI', 'Asia/Tehran');
SELECT jalali_trunc('month', now(), 'Asia/Tehran');                    -- zone independent of the session
SELECT * FROM sale_order WHERE in_jalali_period(date_order, 'month', now());   -- uses a plain btree index
SELECT '1404/01/05'::jalali_date::date;                                -- 2025-03-25
```

`jalali_part` also accepts `isoweek` and `isoyear`: a Saturday..Friday week belongs to the year that contains its 4th day
(the ISO 8601 rule for a Saturday-first week). `jalali_series`, `jalali_bin` accept second..year, week included. Dates before
1/1/1 print as `G:YYYY-MM-DD` as a `jalali_date`. `in_jalali_period` is `STABLE` for `timestamptz` and is rewritten to
`col >= jalali_trunc(...) AND col < jalali_period_end(...)`, so no expression index is needed.

Parsing patterns for `jalali_to_date` accept YYYY YY MM MON MONTH DD DDD HH HH12 HH24 MI SS MS US AM PM, DAY names (ignored),
Persian digits and Persian names; leftover characters and out of range values raise an error.

### to_char_jalali patterns
YYYY YYY YY Y, CC, MM, MONTH Month month, MON Mon mon, DD, DDD, D (1 = Saturday), DAY Day day, DY Dy dy, HH HH12 HH24,
MI, SS, SSSS, MS, US, AM PM am pm, W (week of month), WW (week of year), Q, IYYY IYY IY IW (ISO-style, Saturday-first),
ID (ISO day of week), RM rm (Roman month), TZ TZH TZM OF, J (Julian day), `TH`/`th` ordinal suffix, `FM` prefix (no padding),
`"quoted text"`, backslash escape. As in PostgreSQL, `W` and `WW` are plain 7-day blocks counted from the 1st of the month /
of the year; they are not aligned to Saturday. Use `jalali_part('week', d)` or `IW` for Saturday-aligned weeks. Language `'en'` gives Latin month/day names; `'fa'` gives Persian names, Persian digits
and requires a UTF8 database. Unknown characters are copied as they are.

### Planner statistics
The planner has no idea how `jalali_part('year', d)` is distributed, so it guesses (about 1 row for `= 1404`). Run
`SELECT jalali_create_statistics('sale_order', 'date_order', 'Asia/Tehran');` once. It creates extended statistics on year,
month and day and analyzes the table, and the estimates become accurate. For a `timestamptz` column the statistics (and
your queries) must use the same `AT TIME ZONE` expression, because statistics expressions have to be immutable.

## Time zones
* `TIMESTAMP WITH TIME ZONE`: the instant is rendered in the **session time zone** (`SET TIME ZONE ...`).
  Before 0.0.2 this was always `Asia/Tehran`.
* `TIMESTAMP` (without time zone): read as it is, no time zone conversion. Use it when the value is
  already in the user's time zone, e.g. `jalali_part('day', col AT TIME ZONE 'UTC' AT TIME ZONE 'Asia/Tehran')`.
* `DATE`: has no time of day, so `hour`, `minute` and `second` are 0.

`NULL` is returned for `infinity` / `-infinity` and for dates before 622-03-21 (1/1/1), which have no Jalali date.

Examples (run with `SET TIME ZONE 'Asia/Tehran';`; `timestamptz` results follow the session time zone):
```sql
SET TIME ZONE 'Asia/Tehran';
SELECT format_jalali(now()); -->   1399/07/08 23:45:37
SELECT format_jalali(now(), false); -->   1399/07/08
SELECT format_jalali('2019-07-07', false); -->  1398/04/16
SELECT format_jalali('2021-03-20'); -->  1399/12/30 00:00:00
SELECT format_jalali('2021-03-20'::date); -->  1399/12/30
SELECT jalali_part('year', '2019-07-07 14:10:52.84937+04:30'); --> 1398
SELECT jalali_part('minute', '2019-07-07 14:10:52.84937+04:30'); --> 10   -- Asia/Tehran session (+04:30 in July 2019)
SELECT jalali_part('doy', '2019-07-07 14:10:52.84937+04:30'); --> 10   -- Asia/Tehran session (+04:30 in July 2019)
SELECT jalali_part('dow', '2019-07-07'); --> 1   -- a Sunday; Saturday is 0
```

`jalali_part` function can accept these values as the first parameter:
* year
* month
* day
* hour
* minute
* second
* doy (day of year)
* dow (day of week with index zero as saturday)
* isodow (ISO day of week: Monday is 1, Sunday is 7, same as PostgreSQL)
* quarter
* decade (the first year of the decade, e.g. 1400 for 1405; PostgreSQL's `extract(decade ...)` is a count)
* century
* millennium
* week (Saturday-based week of the Jalali year; week 1 contains 1 Farvardin)
* isoweek, isoyear (Saturday..Friday weeks assigned to the year containing their 4th day)

The unit is case-insensitive and surrounding whitespace is ignored.
Any other value raises an error.

## Volatility
`date` and `timestamp` overloads are `IMMUTABLE PARALLEL SAFE` (usable in expression indexes);
`timestamptz` overloads are `STABLE PARALLEL SAFE` because they depend on the session time zone.

## Calendar rule
Leap years follow the 33-year cycle (`year % 33` in 1, 5, 9, 13, 17, 22, 26, 30). I compared it with the Jalaali-js
arithmetic (33-year cycles with breaks): they agree for every year from 1178 to 1633. From 1634 the two differ in some
cycles (1634 is a leap year here, 1635 in Jalaali-js), so dates after Esfand 1633 can be off by a day depending on
which rule you treat as correct. Neither is an astronomical calendar.

# Upgrading
> ALTER EXTENSION jalali_utils UPDATE;

# Installation

Requires PostgreSQL 14 or newer (`jalali_create_statistics` uses extended statistics on expressions). CI runs 14 to 18.

Dependencies:
> sudo apt install postgresql-server-dev-all   # or postgresql-server-dev-<major> for your server

Run make to build and install
> make

> sudo make install

Run this command in your psql shell:
> \d your_db_name

> CREATE EXTENSION jalali_utils;

# Running tests

> make installcheck

# License
MIT, see [LICENSE](LICENSE). Changes are listed in [CHANGELOG.md](CHANGELOG.md).
