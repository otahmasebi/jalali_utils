# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow the
extension version in `jalali_utils.control`. Upgrade with `ALTER EXTENSION jalali_utils UPDATE;`.

## [Unreleased]

Nothing yet.

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


### Performance
- Planner support (`in_jalali_period`): the planner rewrites it to `col >= jalali_trunc(...) AND col < jalali_period_end(...)`,
  so a plain btree index on `col` is used. Verified with `EXPLAIN` for `date`, `timestamp` and `timestamptz`.
- `jalali_create_statistics` fixes the row estimate of `jalali_part(...)` filters: in a test the estimate went from about 1 row to 18320
  against 18291 actual.
- Unit names are parsed once per call site and cached in `fn_extra` (units longer than 32 bytes are re-parsed).
- See [docs/PERFORMANCE.md](docs/PERFORMANCE.md) for the call graph and per-row costs (PG 18, 2M rows).

### PostgreSQL versions
Tested in CI (build, `make installcheck`, and the 7000+ case suite) on PostgreSQL 14, 15, 16, 17 and 18.
Not supported: 13 and older. `jalali_create_statistics` needs extended statistics on expressions (PG 14), and the
upgrade script fails on PG 13 at that point.

PostgreSQL features and whether the extension uses them:

| Feature | Since | Used? |
|---|---|---|
| `PARALLEL SAFE` / `IMMUTABLE` / `STABLE` markings | 9.6 / always | Yes. `date` and `timestamp` overloads are `IMMUTABLE`, `timestamptz` overloads `STABLE`. |
| Range types (`jalali_period` returns `daterange`, `tsrange`, `tstzrange`) | 9.2 | Yes |
| Planner support functions (`CREATE FUNCTION ... SUPPORT`, `SupportRequestIndexCondition`) | 12 | Yes, for `in_jalali_period` |
| Generated columns | 12 | Works (`IMMUTABLE` overloads), not required |
| `trusted` extensions | 13 | No. A C extension cannot be trusted. |
| Extended statistics on expressions (`CREATE STATISTICS ... ON (expr)`) | 14 | Yes. This is why 14 is the minimum. |
| `date_bin` | 14 | Not used. `jalali_bin` is the Jalali equivalent. |
| Multirange types | 14 | No |
| SQL-standard function bodies (`BEGIN ATOMIC`) | 14 | No. SQL wrappers use `AS $$ ... $$` so the scripts load on every supported version. |
| `PG_MODULE_MAGIC_EXT` | 18 | No. Plain `PG_MODULE_MAGIC` works on 14 to 18. |
| JIT | 11 | Not used. The functions are short, so `jit = off` is better for them. LLVM bitcode is built by PGXS but unused. |
| Virtual generated columns, `uuidv7()`, `OLD`/`NEW` in `RETURNING`, other PG 17 and 18 additions | 17, 18 | No |

### Known limitation
- Leap years follow the 33-year cycle. It matches the Jalaali-js arithmetic from 1178 to 1633 and differs in some cycles
  from 1634 on (see the README, "Calendar rule").

## [0.0.3]

### Changed
- Every function is marked `IMMUTABLE PARALLEL SAFE` for `date` and `timestamp`, and `STABLE PARALLEL SAFE` for
  `timestamptz`. They can be used in expression indexes and parallel plans.

### Performance
Measured on PG 18, 2,000,000 rows, `jit = off`, no parallelism, best of 2 runs:

| Query | 0.0.2 | 0.0.3 |
|---|---|---|
| `jalali_part('year', date)` | 212 ms | 168 ms |
| `jalali_part('dow', date)` | 242 ms | 167 ms |
| `jalali_part('day', timestamptz)` | 283 ms | 220 ms |
| `format_jalali(date)` | 343 ms | 223 ms |
| `format_jalali(timestamp)` | 494 ms | 282 ms |
| `format_jalali(timestamptz)` | 559 ms | 337 ms |

With the new markings, `jalali_part('day', d)` over 2M rows runs with parallel workers: 215 ms to 89 ms with 2 workers.

- Parsed unit cached in `fn_extra`.
- Weekday computed from the day count, with a cumulative month table instead of a month loop.
- `format_jalali` writes digits straight into the result (no `psprintf` or `cstring_to_text`).
- UTC offset window of the session time zone cached via `pg_next_dst_boundary`.
- Outputs are identical to the previous build for 200k random dates, 200k timestamps, 200k timestamptz in 11 zones, and
  every DST transition from 2020 to 2023.

## [0.0.2]

### Added
- `format_jalali` and `jalali_part` overloads for `timestamp` without time zone, read as it is with no conversion.

### Fixed
- `timestamptz` values were always rendered in `Asia/Tehran`. They now follow the session `TimeZone`, so grouping by
  day or month is correct for data stored in UTC when the connection sets its time zone.

## [0.0.1]

### Added
- `format_jalali(timestamptz | date)` and `jalali_part(text, timestamptz | date)`.
