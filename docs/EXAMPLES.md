# Jalali date/time SQL cookbook

Every query below was run on PostgreSQL 18 with `jalali_utils` 0.0.4 and the
output is copied from the real run. Session settings used:

```sql
SET TIME ZONE 'Asia/Tehran';
SET DateStyle = 'ISO, YMD';
```

## Sample data

Timestamps are stored in UTC instants (`timestamptz`). Several rows sit near a
Tehran midnight, so the UTC calendar day and the Jalali day differ. That is
exactly the case that makes naive `GROUP BY created_at::date` wrong.

```sql
CREATE TABLE orders (
    id         int PRIMARY KEY,
    created_at timestamptz NOT NULL,
    amount     numeric(10,2) NOT NULL
);
INSERT INTO orders VALUES
 (1, '2025-03-20 20:29:59+00', 100),   -- 23:59:59 Tehran, 1403/12/30
 (2, '2025-03-20 20:30:00+00', 200),   -- 00:00:00 Tehran, 1404/01/01
 (3, '2025-03-25 08:00:00+00', 150),
 (4, '2025-04-20 21:00:00+00', 300),   -- 00:30 Tehran, 1404/02/01
 (5, '2025-04-25 10:00:00+00',  50),
 (6, '2025-05-21 19:00:00+00', 400),   -- 22:30 Tehran, 1404/02/31
 (7, '2025-05-21 20:30:00+00',  80),   -- 1404/03/01
 (8, '2025-06-30 12:00:00+00', 120),
 (9, '2026-03-20 12:00:00+00', 500),   -- 1404/12/29
 (10,'2026-03-20 20:30:00+00',  60);   -- 1405/01/01
CREATE INDEX ON orders (created_at);
```

## 1. GROUP BY year

```sql
SELECT jalali_part('year', created_at) AS jyear,
       count(*) AS orders, sum(amount) AS total
FROM orders GROUP BY 1 ORDER BY 1;
```
```
 jyear | orders |  total
-------+--------+---------
  1403 |      1 |  100.00
  1404 |      8 | 1800.00
  1405 |      1 |   60.00
```

## 2. GROUP BY month

Year and month as numbers:

```sql
SELECT jalali_part('year', created_at)  AS jy,
       jalali_part('month', created_at) AS jm,
       count(*) AS orders, sum(amount) AS total
FROM orders GROUP BY 1, 2 ORDER BY 1, 2;
```
```
  jy  | jm | orders | total
------+----+--------+--------
 1403 | 12 |      1 | 100.00
 1404 |  1 |      2 | 350.00
 1404 |  2 |      3 | 750.00
 1404 |  3 |      1 |  80.00
 1404 |  4 |      1 | 120.00
 1404 | 12 |      1 | 500.00
 1405 |  1 |      1 |  60.00
```

Month start as a real date (sorts correctly, can be joined or charted) plus a
label:

```sql
SELECT jalali_trunc('month', created_at)::date AS month_start,
       to_char_jalali(created_at, 'YYYY Month') AS month_name,
       count(*), sum(amount)
FROM orders GROUP BY 1, 2 ORDER BY 1;
```
```
 month_start |    month_name    | count |  sum
-------------+------------------+-------+--------
 2025-02-19  | 1403 Esfand      |     1 | 100.00
 2025-03-21  | 1404 Farvardin   |     2 | 350.00
 2025-04-21  | 1404 Ordibehesht |     3 | 750.00
 2025-05-22  | 1404 Khordad     |     1 |  80.00
 2025-06-22  | 1404 Tir         |     1 | 120.00
 2026-02-20  | 1404 Esfand      |     1 | 500.00
 2026-03-21  | 1405 Farvardin   |     1 |  60.00
```

## 3. GROUP BY day

```sql
SELECT to_char_jalali(created_at, 'YYYY/MM/DD') AS jday,
       count(*) AS orders, sum(amount) AS total
FROM orders GROUP BY 1 ORDER BY 1;
```
```
    jday    | orders | total
------------+--------+--------
 1403/12/30 |      1 | 100.00
 1404/01/01 |      1 | 200.00
 1404/01/05 |      1 | 150.00
 1404/02/01 |      1 | 300.00
 1404/02/05 |      1 |  50.00
 1404/02/31 |      1 | 400.00
 1404/03/01 |      1 |  80.00
 1404/04/09 |      1 | 120.00
 1404/12/29 |      1 | 500.00
 1405/01/01 |      1 |  60.00
```

Why this beats grouping on the UTC date: orders 1/2 and 6/7 land on different
Jalali days than their UTC dates suggest.

```sql
SELECT created_at::date AS utc_day,
       to_char_jalali(created_at, 'YYYY/MM/DD') AS jalali_day
FROM orders WHERE id IN (1,2,6,7) ORDER BY id;
```
```
  utc_day   | jalali_day
------------+------------
 2025-03-20 | 1403/12/30
 2025-03-21 | 1404/01/01
 2025-05-21 | 1404/02/31
 2025-05-22 | 1404/03/01
```

## 4. Time zone behaviour

For `timestamptz` the day boundary follows the **session** `TimeZone`
(functions are `STABLE`). The same instant (order 2) in a UTC session:

```sql
SET TIME ZONE 'UTC';
SELECT to_char_jalali(created_at, 'YYYY/MM/DD') FROM orders WHERE id = 2;   -- 1403/12/30
```

Pass the zone explicitly to make a report independent of the session:

```sql
SELECT jalali_part('day', created_at, 'Asia/Tehran') AS day_in_tehran,
       jalali_part('day', created_at, 'UTC')         AS day_in_utc
FROM orders WHERE id = 2;
```
```
 day_in_tehran | day_in_utc
---------------+------------
             1 |         30
```

For a naive `timestamp` column nothing is converted; the value is read as
written. To go from `timestamptz` to local wall time yourself:
`jalali_part('year', created_at AT TIME ZONE 'Asia/Tehran')`.

## 5. Quarter, week, weekday

```sql
SELECT to_char_jalali(created_at, 'YYYY "Q"Q') AS quarter, sum(amount)
FROM orders GROUP BY 1 ORDER BY 1;
```
```
 quarter |   sum
---------+---------
 1403 Q4 |  100.00
 1404 Q1 | 1180.00
 1404 Q2 |  120.00
 1404 Q4 |  500.00
 1405 Q1 |   60.00
```

Weeks start on Saturday. `jalali_part('week')` counts week 1 as the week
holding 1 Farvardin, so the last days of an Esfand can belong to week 53 of the
old year while sharing a week start with week 1 of the new year. For
year-stable numbering use the ISO-style tokens `IYYY` and `IW`:

```sql
SELECT to_char_jalali(created_at, 'IYYY-"W"IW') AS iso_week, count(*)
FROM orders GROUP BY 1 ORDER BY 1 LIMIT 4;
```
```
 iso_week | count
----------+-------
 1403-W52 |     2
 1404-W01 |     1
 1404-W05 |     2
 1404-W09 |     2
```

Weekday (`dow` 0 = Saturday):

```sql
SELECT jalali_part('dow', created_at) AS dow,
       to_char_jalali(created_at, 'Day') AS weekday, count(*)
FROM orders GROUP BY 1, 2 ORDER BY 1;
```
```
 dow |   weekday    | count
-----+--------------+-------
   0 | Shanbe       |     1
   2 | Doshanbe     |     2
   3 | Seshanbe     |     1
   4 | Chaharshanbe |     1
   5 | Panjshanbe   |     2
   6 | Jomeh        |     3
```

Arbitrary bins (here three-month bins):

```sql
SELECT jalali_bin(3, 'month', created_at)::date AS quarter_bin, count(*)
FROM orders GROUP BY 1 ORDER BY 1;
```
```
 quarter_bin | count
-------------+-------
 2024-12-21  |     1
 2025-03-21  |     6
 2025-06-22  |     1
 2025-12-22  |     1
 2026-03-21  |     1
```

## 6. Fill gaps (months with no rows)

`GROUP BY` only returns months that have data. Generate the months and
`LEFT JOIN`:

```sql
SELECT g::date AS month_start,
       to_char_jalali(g, 'YYYY Month') AS month,
       coalesce(sum(o.amount), 0) AS total
FROM jalali_series(make_jalali_date(1404,1,1)::timestamptz,
                   make_jalali_date(1404,4,1)::timestamptz, 'month') g
LEFT JOIN orders o ON in_jalali_period(o.created_at, 'month', g)
GROUP BY g ORDER BY g;
```
```
 month_start |      month       | total
-------------+------------------+--------
 2025-03-21  | 1404 Farvardin   | 350.00
 2025-04-21  | 1404 Ordibehesht | 750.00
 2025-05-22  | 1404 Khordad     |  80.00
 2025-06-22  | 1404 Tir         | 120.00
```

## 7. "This month" filters that use the index

`in_jalali_period(col, unit, ref)` is rewritten by the planner into a plain
range on `col`, so an ordinary btree index is used:

```sql
SELECT * FROM orders
WHERE in_jalali_period(created_at, 'month', timestamptz '2025-05-10 12:00+00');
```
```
 id |        created_at         | amount
----+---------------------------+--------
  4 | 2025-04-21 00:30:00+03:30 | 300.00
  5 | 2025-04-25 13:30:00+03:30 |  50.00
  6 | 2025-05-21 22:30:00+03:30 | 400.00
```

The equivalent range-type form is
`created_at <@ jalali_period('month', ref)`. The plan:

```
Index Scan using orders_created_at_idx on orders
  Index Cond: ((created_at >= jalali_trunc('month', ...)) AND
               (created_at <  jalali_period_end('month', ...)))
```

Use `'year'`, `'quarter'`, `'week'`, `'day'` the same way; "last month" is
`jalali_add_months(...)` on the reference date.

## 8. Index the grouping expression

For date columns an expression index makes `GROUP BY` / filters on year or
month cheap (the date overloads are `IMMUTABLE`):

```sql
CREATE INDEX ON d_orders (jalali_part('year', d));
CREATE INDEX ON d_orders ((jalali_part('year', d) * 100 + jalali_part('month', d)));
```

`timestamptz` functions are `STABLE` and cannot be indexed directly; index the
column and use `in_jalali_period`, or store a date computed in your zone.
For better estimates on expression groupings (PostgreSQL 14+):

```sql
SELECT jalali_create_statistics('orders', 'created_at', 'Asia/Tehran');
```

## 9. Parse, build, arithmetic

```sql
SELECT jalali_to_date('1404/02/31') AS d,
       jalali_to_date('31 Ordibehesht 1404', 'DD Month YYYY') AS d2,
       make_jalali_date(1404,1,1) AS nowruz;
```
```
     d      |     d2     |   nowruz
------------+------------+------------
 2025-05-21 | 2025-05-21 | 2025-03-21
```

```sql
SELECT jalali_to_timestamptz('1404/02/31 22:30', 'YYYY/MM/DD HH24:MI');
-- 2025-05-21 22:30:00+03:30
```

Month arithmetic clamps to the month length; leap years are known:

```sql
SELECT jalali_add_months(make_jalali_date(1404,1,31), 1)  AS next_month,
       jalali_add_months(make_jalali_date(1403,12,30), 12) AS next_year,
       jalali_last_day_of_month(make_jalali_date(1403,12,1)) AS esfand_1403_end;
```
```
 next_month | next_year  | esfand_1403_end
------------+------------+-----------------
 2025-05-21 | 2026-03-20 | 2025-03-20
```
```sql
SELECT is_jalali_leap(1403), is_jalali_leap(1404),
       jalali_days_in_month(1403,12), jalali_days_in_month(1404,12);
--  t | f | 30 | 29
```

Age, in Jalali calendar terms:

```sql
SELECT jalali_age(make_jalali_date(1404,3,5), make_jalali_date(1402,1,20));
-- 2 years 1 mon 16 days
```

## 10. Formatted Persian output

```sql
SELECT to_char_jalali(created_at, 'FMDD Month YYYY', 'fa')
FROM orders WHERE id IN (2, 7) ORDER BY id;
```
```
     fa_date
----------------
 ۱ فروردین ۱۴۰۴
 ۱ خرداد ۱۴۰۴
```

## 11. The `jalali_date` type

A `date`-compatible type that prints and sorts as a Jalali date:

```sql
CREATE TABLE inv (due jalali_date);
INSERT INTO inv VALUES ('1404/02/01'), ('1403/12/29'), ('1404/01/05');
SELECT due, due::date AS gregorian FROM inv ORDER BY due;
```
```
    due     
------------
 1403/12/29
 1404/01/05
 1404/02/01
```

---

# Other datetime features users ask for

Status against 0.0.4. Suggestions marked *idea* are not implemented; they are
candidates for the roadmap.

| Need in SQL reports | Status | How / notes |
|---|---|---|
| Group by day / month / year / quarter | covered | `jalali_part`, `jalali_trunc`, `to_char_jalali` (sections 1-5) |
| Group by week, weekday (Saturday first) | covered | `week`, `dow`, `IW`, `IYYY` |
| Arbitrary N-unit buckets | covered | `jalali_bin(n, unit, ts)` |
| This / last / next month, year, week filters | covered | `in_jalali_period`, `jalali_period`, `jalali_add_months` |
| Index-friendly period filters | covered | planner support rewrites to a range |
| Fill empty periods | covered | `jalali_series` + `LEFT JOIN` |
| Month length, leap year, last day of month | covered | `jalali_days_in_month`, `is_jalali_leap`, `jalali_last_day_of_month` |
| Parse user input (`1404/02/31`, patterns) | covered | `jalali_to_date`, `jalali_to_timestamptz` |
| Formatting (Latin or Persian digits and names) | covered | `to_char_jalali` |
| Age / elapsed in Jalali calendar | covered | `jalali_age` |
| Explicit time zone per query | covered | zone overloads and `make_jalali_timestamp(..., tz)` |
| Native Jalali type | covered | `jalali_date` |
| Year-to-date, month-to-date | partial | `col >= jalali_trunc('year', now())` works today; a helper would be shorter. *idea:* `jalali_ytd(col)`, `jalali_mtd(col)` |
| Same period last year comparison | partial | `jalali_add_months(d, -12)`; *idea:* `jalali_same_period_last_year` |
| Fiscal year starting at a custom month (e.g. Mehr) | not covered | *idea:* `jalali_fiscal_year(ts, start_month)` and `jalali_trunc` with an offset |
| Half-year (semester) | not covered | *idea:* `H` token and `half` unit |
| Business days / Persian holidays / Friday weekend | not covered | needs a holiday table; *idea:* `jalali_is_holiday(date)` with a user-supplied calendar table, `jalali_add_business_days` |
| Day of year, days left in year / month | partial | `jalali_part('doy')`-style via `jalali_part`; *idea:* `jalali_days_left` |
| Birthday / anniversary next occurrence | not covered | *idea:* `jalali_next_anniversary(date)` handling 30 Esfand in non-leap years |
| Jalali `tstzrange` / `daterange` aggregation | partial | `jalali_period` returns a range; *idea:* range `jalali_period(unit, from, to)` over spans |
| Rolling windows (last N Jalali months) | partial | window functions over `jalali_trunc('month', ts)`; *idea:* `jalali_months_back(n)` |
| Parse with locale month names in Persian | not covered | *idea:* accept Persian digits and names in `jalali_to_date` |
| Cast operators `date -> jalali text` | partial | `jalali_date` type; *idea:* implicit text casts and `+`/`-` int operators |
| Index on `timestamptz` expression | not possible | functions are `STABLE` by nature; use `in_jalali_period` or a stored local date |
| Leap rule beyond year 1633 | known gap | the 33-year arithmetic diverges from some references after 1633, see README |

Contributions and requests for any *idea* above are welcome as GitHub issues.
