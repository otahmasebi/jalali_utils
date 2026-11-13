# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow the
extension version in `jalali_utils.control`. Upgrade with `ALTER EXTENSION jalali_utils UPDATE;`.

## [0.0.4]

### Added
- Calendar helpers: `is_jalali_leap`, `jalali_days_in_month`, `jalali_last_day_of_month`.
- Reverse conversion: `make_jalali_date`, `jalali_to_date(text)` (Persian digits accepted),
  `jalali_to_date(text, pattern)`, `jalali_to_timestamp`, `jalali_to_timestamptz`,
  `make_jalali_timestamp`, `make_jalali_timestamptz`.
- `jalali_trunc`, `jalali_period_end` and `jalali_period` (a `[start, end)` range) for second to millennium.
- `jalali_part` units `week`, `isoweek`, `isoyear`, `millennium`. Weeks start on Saturday; `week` 1 contains 1 Farvardin;
  ISO-style weeks belong to the year containing their 4th day.
- Overloads of `jalali_part`, `jalali_trunc`, `jalali_period_end` and `jalali_period` that take an explicit time zone.
- `to_char_jalali(value, pattern, lang)` with PostgreSQL-like patterns (YYYY, MM, DD, HH24, Day, Month, IW, IYYY, RM,
  TZ, OF, SSSS, J, the TH suffix, FM and more) and Persian names and digits (`'fa'`).
- `jalali_add_months`, `jalali_age`, `jalali_series` and `jalali_bin` (Jalali `date_bin`).
- `in_jalali_period(col, unit, ref)` with planner support: it is rewritten to a range condition on `col`, so a plain btree
  index is used.
- `jalali_create_statistics(table, column, tz)` creates extended statistics so the planner estimates
  `jalali_part(...)` filters correctly.
- The `jalali_date` type, binary compatible with `date`, which prints as `YYYY/MM/DD`, with casts, operators and btree and hash
  operator classes.
- Tests: a 7000+ case suite (`tests/suite.py`) and more `pg_regress` files.

### Changed
- `jalali_part('week', ...)` is supported (it was rejected before).

### Known limitation
- Leap years follow the 33-year cycle. It matches the Jalaali-js arithmetic from 1178 to 1633 and differs in some cycles
  from 1634 on (see the README, "Calendar rule").

## [0.0.3]

### Changed
- Every function is marked `IMMUTABLE PARALLEL SAFE` for `date` and `timestamp`, and `STABLE PARALLEL SAFE` for
  `timestamptz`. They can be used in expression indexes and parallel plans.

### Performance
- Faster Jalali conversion and unit parsing, with the time zone offset window cached between calls.

## [0.0.2]

### Added
- `format_jalali` and `jalali_part` overloads for `timestamp` without time zone, read as it is with no conversion.

### Fixed
- `timestamptz` values were always rendered in `Asia/Tehran`. They now follow the session `TimeZone`, so grouping by
  day or month is correct for data stored in UTC when the connection sets its time zone.

## [0.0.1]

### Added
- `format_jalali(timestamptz | date)` and `jalali_part(text, timestamptz | date)`.
