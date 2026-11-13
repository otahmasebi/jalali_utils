#!/usr/bin/env python3
"""
Big test suite for jalali_utils (500+ cases). Expected values are computed here, independently of
the extension: a year-by-year Jalali calendar (leap years: y % 33 in {1,5,9,13,17,22,26,30}) and a
separate Gregorian day counter. Time zones come from Python's zoneinfo.

Usage:
  python3 suite.py docker exec -i -u postgres <container> psql -X -A -t postgres
The target database must already have the extension (CREATE EXTENSION jalali_utils).
Writes suite.sql (what was sent) and suite_results.txt (full result of every case).
"""
import collections
import datetime
import os
import random
import subprocess
import sys
import zoneinfo

random.seed(20260512)
HERE = os.path.dirname(os.path.abspath(__file__))

# ---------------------------------------------------------------- independent calendar
MD = [31] * 6 + [30] * 5 + [29]


def leap(y):
    return y % 33 in (1, 5, 9, 13, 17, 22, 26, 30)


def ylen(y):
    return 366 if leap(y) else 365


def greg_leap(y):
    return y % 4 == 0 and (y % 100 != 0 or y % 400 == 0)


CUM = [0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334]


def greg_ord(y, m, d):
    """Same numbering as datetime.date.toordinal(), but for any year >= 1."""
    yy = y - 1
    n = yy * 365 + yy // 4 - yy // 100 + yy // 400 + CUM[m - 1] + d
    if m > 2 and greg_leap(y):
        n += 1
    return n


EPOCH = greg_ord(622, 3, 21)  # 1/1/1 Jalali


def jal_from_ord(n):
    """n = days since 622-03-21. Returns (year, month, day, doy) or None before the epoch."""
    if n < 0:
        return None
    c, rem = divmod(n, 12053)
    y = 1 + 33 * c
    while rem >= ylen(y):
        rem -= ylen(y)
        y += 1
    doy = rem + 1
    for i in range(12):
        dm = MD[i] + (1 if i == 11 and leap(y) else 0)
        if rem >= dm:
            rem -= dm
        else:
            return y, i + 1, rem + 1, doy
    raise AssertionError


def days_before_year(y):
    c = (y - 1) // 33
    n = 12053 * c
    for k in range(33 * c + 1, y):
        n += ylen(k)
    return n


def fields(gy, gm, gd, H=0, M=0, S=0):
    o = greg_ord(gy, gm, gd)
    j = jal_from_ord(o - EPOCH)
    if j is None:
        return None
    y, m, d, doy = j
    iso = 7 if o % 7 == 0 else o % 7
    return {
        "year": y, "month": m, "day": d, "doy": doy,
        "dow": (o + 1) % 7, "isodow": iso,
        "quarter": (m - 1) // 3 + 1, "decade": (y // 10) * 10, "century": (y + 99) // 100,
        "hour": H, "minute": M, "second": S,
    }


def fmt(f, with_time):
    s = "%d/%02d/%02d" % (f["year"], f["month"], f["day"])
    if with_time:
        s += " %02d:%02d:%02d" % (f["hour"], f["minute"], f["second"])
    return s


def dlit(y, m, d):
    return "%04d-%02d-%02d" % (y, m, d)


def tlit(y, m, d, H, M, S, us=0):
    return "%04d-%02d-%02d %02d:%02d:%02d.%06d" % (y, m, d, H, M, S, us)


# self-check of the reference implementation against well-known dates
KNOWN = [
    ((2021, 3, 20), "1399/12/30"), ((2021, 3, 21), "1400/01/01"), ((2022, 3, 21), "1401/01/01"),
    ((2023, 3, 21), "1402/01/01"), ((2024, 3, 20), "1403/01/01"), ((2025, 3, 20), "1403/12/30"),
    ((2025, 3, 21), "1404/01/01"), ((2026, 3, 21), "1405/01/01"), ((2000, 1, 1), "1378/10/11"),
    ((1979, 2, 11), "1357/11/22"), ((1970, 1, 1), "1348/10/11"), ((2026, 11, 12), "1405/08/21"),
    ((1999, 12, 31), "1378/10/10"), ((2019, 7, 7), "1398/04/16"), ((2020, 2, 29), "1398/12/10"),
    ((622, 3, 21), "1/01/01"),
]
for (g, want) in KNOWN:
    assert fmt(fields(*g), False) == want, (g, fmt(fields(*g), False), want)
assert greg_ord(2021, 3, 21) == datetime.date(2021, 3, 21).toordinal()

# ---------------------------------------------------------------- case registry
CASES = []  # (id, category, description, expected, query, tz)
_counter = collections.Counter()


def add(cat, desc, query, expected, tz="UTC"):
    _counter[cat] += 1
    CASES.append(("%s%03d" % (cat, _counter[cat]), cat, desc, str(expected), query, tz))


def nul(x):
    return "NULL" if x is None else x


# ---- A: format_jalali(date) around the Jalali new year, and known dates -------------
for (g, want) in KNOWN:
    add("A", "known date %s" % dlit(*g), "select format_jalali(date '%s')" % dlit(*g), want)

YEARS = [1, 2, 3, 4, 5, 6, 33, 34, 35, 66, 100, 128, 200, 300, 400, 500, 600, 700, 800, 900, 977, 978,
         979, 980, 1000, 1100, 1200, 1300, 1342, 1343, 1370, 1375, 1378, 1379, 1395, 1398, 1399, 1400,
         1401, 1402, 1403, 1404, 1405, 1406, 1407, 1408, 1409, 1410, 1450, 1500, 1600, 2000, 3000,
         5000, 7000, 9000]
MAXORD = greg_ord(9999, 12, 31)
for jy in YEARS:
    n0 = days_before_year(jy)
    for off, label in ((-1, "last day of previous year"), (0, "Nowruz"), (1, "day after Nowruz")):
        n = n0 + off
        if n < 0 or EPOCH + n > MAXORD:
            continue
        g = datetime.date.fromordinal(EPOCH + n)
        f = fields(g.year, g.month, g.day)
        add("A", "%s of Jalali %d (%s)" % (label, jy, g), "select format_jalali(date '%s')" % g.isoformat(),
            fmt(f, False))

# ---- B: every jalali_part field on random dates -------------------------------------
dates = []
for _ in range(25):
    dates.append(datetime.date.fromordinal(random.randint(EPOCH, greg_ord(2300, 12, 31))))
for _ in range(5):
    dates.append(datetime.date.fromordinal(random.randint(EPOCH, greg_ord(1599, 12, 31))))
for _ in range(5):
    dates.append(datetime.date.fromordinal(random.randint(greg_ord(2300, 1, 1), MAXORD)))
for g in dates:
    f = fields(g.year, g.month, g.day)
    for part in ("year", "month", "day", "doy", "dow", "isodow", "quarter", "decade", "century"):
        add("B", "%s of %s" % (part, g), "select jalali_part('%s', date '%s')::text" % (part, g.isoformat()),
            f[part])

# ---- C: unit names ------------------------------------------------------------------
D0 = (2026, 11, 12)
F0 = fields(*D0)
ALIASES = {
    "year": ["year", "years", "yr", "yrs", "y", "YEAR", "Year"],
    "month": ["month", "months", "mon", "mons", "MONTH"],
    "day": ["day", "days", "d", "DAY"],
    "hour": ["hour", "hours", "hr", "hrs", "h"],
    "minute": ["minute", "minutes", "min", "mins", "m"],
    "second": ["second", "seconds", "sec", "secs", "s"],
    "doy": ["doy", "DOY"], "dow": ["dow", "Dow"], "isodow": ["isodow", "IsoDow"],
    "quarter": ["quarter", "qtr", "QUARTER"],
    "decade": ["decade", "decades", "dec", "decs"],
    "century": ["century", "centuries", "cent", "c"],
}
for field, names in ALIASES.items():
    for nm in names:
        add("C", "unit '%s' -> %s (date)" % (nm, field),
            "select jalali_part('%s', date '%s')::text" % (nm, dlit(*D0)), F0[field])
WS = [("E'\\tmonth\\n'", "month"), ("'  year'", "year"), ("'day   '", "day"), ("E' \\n doy \\t '", "doy"),
      ("repeat(' ',40)||'year'", "year"), ("'year'||repeat(' ',40)", "year"),
      ("repeat(' ',28)||'year'", "year"), ("repeat(' ',29)||'year'", "year"),
      ("repeat(' ',31)||'y'", "year"), ("repeat(' ',32)||'y'", "year")]
for expr, field in WS:
    add("C", "whitespace/long unit %s" % expr, "select jalali_part(%s, date '%s')::text" % (expr, dlit(*D0)),
        F0[field])
BAD = ["era", "julian", "jan", "monday", "am", "now", "today", "epoch",
       "infinity", "z", "est", "utc", "t", "ago", "timezone", "microseconds", "us", "ms",
       "msec", "milliseconds", "yearr", "ye", "x", "year;", "yea r"]
for nm in BAD:
    add("C", "unsupported unit '%s' errors" % nm, "select jalali_part('%s', date '%s')::text" % (nm, dlit(*D0)),
        'ERROR: jalali_part units "%s" not recognized or not supported' % nm)
add("C", "empty unit errors", "select jalali_part('', date '2026-11-12')::text",
    'ERROR: jalali_part units "" not recognized or not supported')
add("C", "100-char unit is truncated to 63 chars in the message",
    "select jalali_part(repeat('x',100), date '2026-11-12')::text",
    'ERROR: jalali_part units "%s" not recognized or not supported' % ("x" * 63))
add("C", "unit with only spaces errors", "select jalali_part('   ', date '2026-11-12')::text",
    'ERROR: jalali_part units "" not recognized or not supported')
# the same aliases must work for timestamp and timestamptz
TS0 = (2026, 11, 12, 19, 44, 17)
FT0 = fields(*TS0)
for field, nm in (("year", "yr"), ("month", "MON"), ("day", "d"), ("hour", "hrs"), ("minute", "mins"),
                  ("second", "secs"), ("doy", "doy"), ("dow", "dow"), ("isodow", "isodow"),
                  ("quarter", "qtr"), ("decade", "decs"), ("century", "cent")):
    add("C", "unit '%s' -> %s (timestamp)" % (nm, field),
        "select jalali_part('%s', timestamp '%s')::text" % (nm, tlit(*TS0)), FT0[field])
    add("C", "unit '%s' -> %s (timestamptz, UTC session)" % (nm, field),
        "select jalali_part('%s', timestamptz '%s+00')::text" % (nm, tlit(*TS0)), FT0[field])

# ---- D: timestamptz follows the session time zone ----------------------------------
ZONES = ["UTC", "Asia/Tehran", "America/New_York", "Europe/London", "Australia/Lord_Howe", "Asia/Kathmandu",
         "Asia/Kolkata", "Pacific/Apia", "Pacific/Kiritimati", "America/Sao_Paulo", "Europe/Moscow",
         "Africa/Casablanca"]
UTC = datetime.timezone.utc


def local_fields(dt_utc, zone):
    loc = dt_utc.astimezone(zoneinfo.ZoneInfo(zone))
    return fields(loc.year, loc.month, loc.day, loc.hour, loc.minute, loc.second)


def rand_instant(zone):
    hi = datetime.datetime(2021, 12, 31) if zone == "Asia/Tehran" else datetime.datetime(2035, 12, 31)
    lo = datetime.datetime(1980, 1, 1)
    span = int((hi - lo).total_seconds())
    return lo + datetime.timedelta(seconds=random.randint(0, span)), random.choice([0, 0, 1, 999999, 500000])


for i in range(110):
    zone = ZONES[i % len(ZONES)]
    t, us = rand_instant(zone)
    t = t.replace(tzinfo=UTC)
    f = local_fields(t, zone)
    lit = tlit(t.year, t.month, t.day, t.hour, t.minute, t.second, us)
    add("D", "format_jalali(timestamptz %s UTC) in %s" % (lit, zone),
        "select format_jalali(timestamptz '%s+00')" % lit, fmt(f, True), zone)
for i in range(40):
    zone = ZONES[(i * 5) % len(ZONES)]
    t, us = rand_instant(zone)
    t = t.replace(tzinfo=UTC)
    f = local_fields(t, zone)
    part = ("hour", "minute", "second", "day", "month", "year", "dow", "doy")[i % 8]
    lit = tlit(t.year, t.month, t.day, t.hour, t.minute, t.second, us)
    add("D", "%s of timestamptz %s UTC in %s" % (part, lit, zone),
        "select jalali_part('%s', timestamptz '%s+00')::text" % (part, lit), f[part], zone)
    add("D", "format_jalali(..., false) of %s UTC in %s" % (lit, zone),
        "select format_jalali(timestamptz '%s+00', false)" % lit, fmt(f, False), zone)

DST = [
    ("America/New_York", (2024, 3, 10, 6, 59, 59)), ("America/New_York", (2024, 3, 10, 7, 0, 0)),
    ("America/New_York", (2024, 11, 3, 5, 59, 59)), ("America/New_York", (2024, 11, 3, 6, 0, 0)),
    ("Europe/London", (2024, 3, 31, 0, 59, 59)), ("Europe/London", (2024, 3, 31, 1, 0, 0)),
    ("Europe/London", (2024, 10, 27, 0, 59, 59)), ("Europe/London", (2024, 10, 27, 1, 0, 0)),
    ("Australia/Lord_Howe", (2024, 4, 6, 14, 59, 59)), ("Australia/Lord_Howe", (2024, 4, 6, 15, 0, 0)),
    ("Australia/Lord_Howe", (2024, 10, 5, 15, 29, 59)), ("Australia/Lord_Howe", (2024, 10, 5, 15, 30, 0)),
    ("America/Sao_Paulo", (2018, 11, 4, 2, 59, 59)), ("America/Sao_Paulo", (2018, 11, 4, 3, 0, 0)),
    ("Asia/Tehran", (2019, 3, 21, 20, 29, 59)), ("Asia/Tehran", (2019, 3, 21, 20, 30, 0)),
    ("Asia/Tehran", (2019, 9, 21, 19, 29, 59)), ("Asia/Tehran", (2019, 9, 21, 19, 30, 0)),
    ("Pacific/Apia", (2011, 12, 30, 9, 59, 59)), ("Pacific/Apia", (2011, 12, 30, 10, 0, 0)),
    ("Europe/Moscow", (2014, 10, 25, 21, 59, 59)), ("Europe/Moscow", (2014, 10, 25, 22, 0, 0)),
]
for zone, t in DST:
    dt = datetime.datetime(*t, tzinfo=UTC)
    f = local_fields(dt, zone)
    add("D", "DST/offset change %s around %s UTC" % (zone, dt), "select format_jalali(timestamptz '%s+00')" % dt.strftime("%Y-%m-%d %H:%M:%S"),
        fmt(f, True), zone)

# Tehran after the 2022 abolition of DST is a fixed +03:30: compare around each Nowruz
for jy in range(1402, 1410):
    n0 = days_before_year(jy)
    g = datetime.date.fromordinal(EPOCH + n0)
    local_mid = datetime.datetime(g.year, g.month, g.day) - datetime.timedelta(hours=3, minutes=30)
    for delta in (-1, 0):
        u = local_mid + datetime.timedelta(seconds=delta)
        if delta == -1:
            f = fields(*(datetime.date.fromordinal(EPOCH + n0 - 1).timetuple()[:3] + (23, 59, 59)))
        else:
            f = fields(g.year, g.month, g.day, 0, 0, 0)
        add("D", "Tehran fixed +03:30, %s UTC around Nowruz %d" % (u, jy),
            "select format_jalali(timestamptz '%s+00')" % u.strftime("%Y-%m-%d %H:%M:%S"), fmt(f, True), "Asia/Tehran")

# ---- E: naive timestamp is never shifted ------------------------------------------
for i in range(40):
    o = random.randint(EPOCH, greg_ord(2600, 12, 31))
    g = datetime.date.fromordinal(o)
    H, M, S, us = random.randint(0, 23), random.randint(0, 59), random.randint(0, 59), random.choice([0, 1, 999999])
    f = fields(g.year, g.month, g.day, H, M, S)
    zone = ZONES[i % len(ZONES)]
    lit = tlit(g.year, g.month, g.day, H, M, S, us)
    add("E", "format_jalali(timestamp %s) with session %s" % (lit, zone),
        "select format_jalali(timestamp '%s')" % lit, fmt(f, True), zone)
    if i % 3 == 0:
        add("E", "format_jalali(timestamp %s, false)" % lit, "select format_jalali(timestamp '%s', false)" % lit,
            fmt(f, False), zone)
        add("E", "hour of timestamp %s" % lit, "select jalali_part('hour', timestamp '%s')::text" % lit, H, zone)
add("E", "Naive timestamp path: UTC 19:44 -> Tehran 23:14 (naive) -> day 21",
    "select jalali_part('day', timestamp '2026-11-12 19:44:17' at time zone 'UTC' at time zone 'Asia/Tehran')::text", 21)
add("E", "Naive timestamp path: 20:30 UTC -> 00:00 Tehran next day -> day 22",
    "select jalali_part('day', timestamp '2026-11-12 20:30:00' at time zone 'UTC' at time zone 'Asia/Tehran')::text", 22)
add("E", "Naive timestamp path: 20:29:59 UTC stays on day 21",
    "select jalali_part('day', timestamp '2026-11-12 20:29:59' at time zone 'UTC' at time zone 'Asia/Tehran')::text", 21)
add("E", "date has hour/minute/second 0 under any zone",
    "select jalali_part('hour', date '2026-11-12')::text||jalali_part('minute', date '2026-11-12')::text||jalali_part('second', date '2026-11-12')::text",
    "000", "Pacific/Kiritimati")

# ---- F: NULL, infinity, limits -----------------------------------------------------
for t in ("date", "timestamp", "timestamptz"):
    for v in ("infinity", "-infinity"):
        add("F", "format_jalali(%s %s) is NULL" % (t, v), "select format_jalali(%s '%s')" % (t, v), "NULL")
        add("F", "jalali_part(year, %s %s) is NULL" % (t, v), "select jalali_part('year', %s '%s')::text" % (t, v), "NULL")
        add("F", "jalali_part(hour, %s %s) is NULL" % (t, v), "select jalali_part('hour', %s '%s')::text" % (t, v), "NULL")
    add("F", "format_jalali(NULL::%s) is NULL" % t, "select format_jalali(NULL::%s)" % t, "NULL")
    add("F", "jalali_part('year', NULL::%s) is NULL" % t, "select jalali_part('year', NULL::%s)::text" % t, "NULL")
    add("F", "jalali_part(NULL, %s) is NULL" % t, "select jalali_part(NULL, %s '2026-01-01')::text" % t, "NULL")
add("F", "format_jalali(NULL timestamp, NULL bool)", "select format_jalali(timestamp '2026-01-01', NULL)", "NULL")
add("F", "date one day before the epoch is NULL", "select format_jalali(date '0622-03-20')", "NULL")
add("F", "date on the epoch is 1/01/01", "select format_jalali(date '0622-03-21')", "1/01/01")
add("F", "date 0001-01-01 is NULL", "select format_jalali(date '0001-01-01')", "NULL")
add("F", "date 4713-01-01 BC is NULL", "select format_jalali(date '4713-01-01 BC')", "NULL")
add("F", "timestamp one microsecond before the epoch is NULL",
    "select format_jalali(timestamp '0622-03-20 23:59:59.999999')", "NULL")
add("F", "timestamp at the epoch", "select format_jalali(timestamp '0622-03-21 00:00:00')", "1/01/01 00:00:00")
add("F", "jalali_part(year, date before epoch) is NULL", "select jalali_part('year', date '0100-01-01')::text", "NULL")
add("F", "timestamptz 4713 BC is NULL", "select format_jalali(timestamptz '4713-01-01 00:00:00+00 BC')", "NULL", "America/New_York")
mx = fields(5874897, 12, 31)
add("F", "max date 5874897-12-31", "select format_jalali(date '5874897-12-31')", fmt(mx, False))
add("F", "max date year via jalali_part", "select jalali_part('year', date '5874897-12-31')::text", mx["year"])
mt = fields(294276, 12, 31, 23, 59, 59)
add("F", "max timestamp", "select format_jalali(timestamp '294276-12-31 23:59:59.999999')", fmt(mt, True))
add("F", "max timestamptz in UTC", "select format_jalali(timestamptz '294276-12-31 23:59:59.999999+00')", fmt(mt, True))
mk = fields(294277, 1, 1, 13, 59, 59)
add("F", "max timestamptz in Kiritimati (+14) rolls into year 294277",
    "select format_jalali(timestamptz '294276-12-31 23:59:59.999999+00')", fmt(mk, True), "Pacific/Kiritimati")
for g in ((2020, 2, 29), (2000, 2, 29), (1900, 3, 1), (2100, 3, 1), (2024, 2, 29), (1600, 2, 29), (2023, 12, 31), (2024, 1, 1)):
    add("F", "Gregorian leap logic %s" % dlit(*g), "select format_jalali(date '%s')" % dlit(*g), fmt(fields(*g), False))
add("F", "week is supported (Saturday-based week of year)", "select jalali_part('week', date '2026-11-12')::text", "34")
add("F", "text is not a valid second argument", "select jalali_part('year', 'abc')::text",
    'ERROR: invalid input syntax for type timestamp with time zone: "abc"')

# ---- G: catalog and indexing -------------------------------------------------------
SIGS = [
    ("format_jalali(date)", "i"), ("format_jalali(timestamp without time zone,boolean)", "i"),
    ("jalali_part(text,date)", "i"), ("jalali_part(text,timestamp without time zone)", "i"),
    ("format_jalali(timestamp with time zone,boolean)", "s"), ("jalali_part(text,timestamp with time zone)", "s"),
]
for sig, vol in SIGS:
    add("G", "%s volatility" % sig, "select provolatile::text from pg_proc where oid = '%s'::regprocedure" % sig, vol)
    add("G", "%s parallel safe" % sig, "select proparallel::text from pg_proc where oid = '%s'::regprocedure" % sig, "s")
    add("G", "%s strict" % sig, "select proisstrict::text from pg_proc where oid = '%s'::regprocedure" % sig, "true")
    add("G", "%s language" % sig, "select l.lanname from pg_proc p join pg_language l on l.oid = p.prolang where p.oid = '%s'::regprocedure" % sig, "c")
add("G", "7 functions installed", "select count(*)::text from pg_proc where proname in ('format_jalali','jalali_part')", 7)
add("G", "extension version", "select extversion from pg_extension where extname = 'jalali_utils'", "0.0.4")
add("G", "all functions belong to the extension",
    "select count(*)::text from pg_depend d join pg_proc p on p.oid = d.objid where d.deptype='e' and d.refobjid = (select oid from pg_extension where extname='jalali_utils') and p.proname in ('format_jalali','jalali_part')", 7)
add("G", "format_jalali(timestamp) has default with_time", "select format_jalali(timestamp '2026-11-12 19:44:17')", "1405/08/21 19:44:17")
add("G", "format_jalali(timestamptz) has default with_time", "select format_jalali(timestamptz '2026-11-12 19:44:17+00')", "1405/08/21 19:44:17")
add("G", "index on date expression is allowed",
    "select pg_temp.ddl($d$create temp table g1(d date); create index on g1 (jalali_part('year', d))$d$)", "ok")
add("G", "index on naive timestamp expression is allowed",
    "select pg_temp.ddl($d$create temp table g2(t timestamp); create index on g2 (jalali_part('month', t))$d$)", "ok")
add("G", "index on timestamptz expression is rejected (STABLE)",
    "select pg_temp.ddl($d$create temp table g3(t timestamptz); create index on g3 (jalali_part('month', t))$d$)",
    "ERROR: functions in index expression must be marked IMMUTABLE")
add("G", "planner uses an expression index on a date column",
    "select pg_temp.plan($d$create temp table g4 as select date '2000-01-01' + i d from generate_series(1,20000) i; create index on g4 (jalali_part('year', d)); analyze g4$d$, $e$select * from g4 where jalali_part('year', d) = 1400$e$)",
    "index")
add("G", "generated column on date is allowed (needs IMMUTABLE)",
    "select pg_temp.ddl($d$create temp table g5(d date, jy int generated always as (jalali_part('year', d)) stored); insert into g5(d) values ('2026-11-12')$d$)", "ok")
add("G", "generated column value", "select jy::text from g5", 1405)
add("G", "generated column on timestamptz is rejected",
    "select pg_temp.ddl($d$create temp table g6(t timestamptz, jy int generated always as (jalali_part('year', t)) stored)$d$)",
    "ERROR: generation expression is not immutable")

# ---- H: per-call-site caches -------------------------------------------------------
D0L = dlit(*D0)


def agg(units, f=F0):
    return ",".join(str(f[u]) for u in units)


for units in (["year", "month", "day", "year", "Year", "day"], ["d", "day", "days", "doy", "dow", "isodow"],
              ["year", "YEAR", "Year", "year"], ["month", "months", "mon", "mons", "month"],
              ["day", "dec", "day", "dec"], ["hour", "year", "minute", "year", "second"]):
    canon = {"d": "day", "days": "day", "Year": "year", "YEAR": "year", "months": "month", "mon": "month",
             "mons": "month", "dec": "decade"}
    exp = agg([canon.get(u, u) for u in units])
    arr = "array[%s]" % ",".join("'%s'" % u for u in units)
    add("H", "one call site, changing unit %s" % units,
        "select string_agg(jalali_part(u, date '%s')::text, ',' order by o) from unnest(%s) with ordinality t(u, o)" % (D0L, arr), exp)
add("H", "changing unit in a SQL-language function",
    "select string_agg(pg_temp.sqlpart(u)::text, ',' order by o) from unnest(array['year','day','month','year']) with ordinality t(u,o)",
    agg(["year", "day", "month", "year"]))
add("H", "changing unit in a plpgsql loop", "select pg_temp.loopu()", agg(["year", "month", "day", "year", "day", "doy"]))
add("H", "invalid unit in the middle of a query errors with that unit",
    "select string_agg(jalali_part(u, date '%s')::text, ',' order by o) from unnest(array['year','nonsense','day']) with ordinality t(u,o)" % D0L,
    'ERROR: jalali_part units "nonsense" not recognized or not supported')
add("H", "a valid query works right after the error above", "select jalali_part('year', date '%s')::text" % D0L, F0["year"])
add("H", "unit length 32 and 33 bytes (cache boundary) in one call site",
    "select string_agg(jalali_part(u, date '%s')::text, ',' order by o) from unnest(array[repeat(' ',28)||'year', repeat(' ',29)||'year', 'year', repeat(' ',29)||'year']) with ordinality t(u,o)" % D0L,
    agg(["year"] * 4))
add("H", "alternating cached and uncached lengths with different units",
    "select string_agg(jalali_part(u, date '%s')::text, ',' order by o) from unnest(array['day', repeat(' ',40)||'month', 'day', repeat(' ',40)||'year']) with ordinality t(u,o)" % D0L,
    agg(["day", "month", "day", "year"]))


def flip_expected(count):
    out = []
    zs = ["Asia/Tehran", "America/New_York", "UTC"]
    for i in range(1, count + 1):
        t = datetime.datetime(2021, 3, 21, 12, 0, 0, tzinfo=UTC) + datetime.timedelta(hours=7 * i)
        out.append(fmt(local_fields(t, zs[i % 3]), True))
    return ";".join(out)


add("H", "session TimeZone changes row by row inside one statement (300 rows)",
    "select string_agg(format_jalali(pg_temp.flip(i)), ';' order by i) from generate_series(1,300) i",
    flip_expected(300))
add("H", "same, 60 rows", "select string_agg(format_jalali(pg_temp.flip(i)), ';' order by i) from generate_series(1,60) i",
    flip_expected(60))
# the same query text under many zones, back to back (offset cache must be invalidated each time)
for zone in ZONES:
    t = datetime.datetime(2022, 7, 15, 20, 45, 30, tzinfo=UTC)
    add("H", "same query, zone switched between statements: %s" % zone,
        "select format_jalali(timestamptz '2022-07-15 20:45:30+00')", fmt(local_fields(t, zone), True), zone)
add("H", "column scan crossing a DST change keeps the right offset per row",
    "select string_agg(format_jalali(t), ';' order by t) from generate_series(timestamptz '2024-03-10 05:00:00+00', timestamptz '2024-03-10 09:00:00+00', interval '30 minutes') t",
    ";".join(fmt(local_fields(datetime.datetime(2024, 3, 10, 5, 0, 0, tzinfo=UTC) + datetime.timedelta(minutes=30 * k),
                               "America/New_York"), True) for k in range(9)), "America/New_York")
add("H", "scan going backwards in time (offset window must be re-fetched)",
    "select string_agg(format_jalali(t), ';' order by t desc) from generate_series(timestamptz '2024-03-10 05:00:00+00', timestamptz '2024-03-10 09:00:00+00', interval '30 minutes') t",
    ";".join(fmt(local_fields(datetime.datetime(2024, 3, 10, 5, 0, 0, tzinfo=UTC) + datetime.timedelta(minutes=30 * k),
                               "America/New_York"), True) for k in reversed(range(9))), "America/New_York")
add("H", "scan jumping across years and DST seasons",
    "select string_agg(format_jalali(t), ';' order by o) from unnest(array[timestamptz '2020-01-15 12:00+00', timestamptz '2020-07-15 12:00+00', timestamptz '1990-07-15 12:00+00', timestamptz '2030-01-15 12:00+00', timestamptz '2020-01-15 12:00+00']) with ordinality u(t, o)",
    ";".join(fmt(local_fields(datetime.datetime(*x, tzinfo=UTC), "Europe/London"), True)
             for x in ((2020, 1, 15, 12, 0, 0), (2020, 7, 15, 12, 0, 0), (1990, 7, 15, 12, 0, 0), (2030, 1, 15, 12, 0, 0), (2020, 1, 15, 12, 0, 0))), "Europe/London")

# ---- I: 0.0.4 features: calendar, reverse conversion, week, trunc, period, to_char ----
def j2g(y, m, d):
    """Jalali -> datetime.date, computed from the independent calendar."""
    n = days_before_year(y) + sum(MD[i] + (1 if i == 11 and leap(y) else 0) for i in range(m - 1)) + d - 1
    return datetime.date.fromordinal(EPOCH + n)


def mlen(y, m):
    return MD[m - 1] + (1 if m == 12 and leap(y) else 0)


for y in list(range(1, 40)) + list(range(1395, 1415)) + [1599, 1600, 1633, 1634, 1700, 3000]:
    add("I", "is_jalali_leap(%d)" % y, "select is_jalali_leap(%d)" % y, "true" if leap(y) else "false")
    add("I", "jalali_days_in_month(%d,12)" % y, "select jalali_days_in_month(%d,12)" % y, mlen(y, 12))
for m in range(1, 13):
    add("I", "days in month %d" % m, "select jalali_days_in_month(1404,%d)" % m, mlen(1404, m))
add("I", "month 13", "select jalali_days_in_month(1404,13)", "ERROR: Jalali month out of range: 13")
add("I", "month 0", "select jalali_days_in_month(1404,0)", "ERROR: Jalali month out of range: 0")
add("I", "leap year 0 is NULL", "select is_jalali_leap(0)", "NULL")

for y, m, d in [(1, 1, 1), (1403, 12, 30), (1404, 1, 1), (1404, 12, 29), (1399, 12, 30), (1405, 1, 1), (3000, 6, 31)]:
    add("I", "make_jalali_date %d/%d/%d" % (y, m, d), "select make_jalali_date(%d,%d,%d)" % (y, m, d), j2g(y, m, d))
for _ in range(150):
    y = random.randint(1, 3500)
    m = random.randint(1, 12)
    d = random.randint(1, mlen(y, m))
    g = j2g(y, m, d)
    add("I", "make_jalali_date %d/%d/%d" % (y, m, d), "select make_jalali_date(%d,%d,%d)" % (y, m, d), g)
    add("I", "round trip %d/%d/%d" % (y, m, d), "select format_jalali(make_jalali_date(%d,%d,%d))" % (y, m, d), "%d/%02d/%02d" % (y, m, d))
    add("I", "jalali_to_date text %d/%d/%d" % (y, m, d), "select jalali_to_date('%d/%d/%d')" % (y, m, d), g)
for bad in [(1404, 12, 30), (1404, 13, 1), (1404, 0, 1), (1404, 1, 32), (1404, 7, 31), (0, 1, 1), (-5, 1, 1)]:
    add("I", "invalid %s" % (bad,), "select make_jalali_date(%d,%d,%d)" % bad, "ERROR: Jalali date field value out of range: %d-%d-%d" % bad)
add("I", "persian digits", "select jalali_to_date('۱۴۰۴/۰۱/۰۱')", "2025-03-21")
add("I", "arabic digits", "select jalali_to_date('\u0661\u0664\u0660\u0664-1-1')", "2025-03-21") if False else None
add("I", "dash and spaces", "select jalali_to_date(' 1404 - 1 - 1 ')", "2025-03-21")
add("I", "garbage", "select jalali_to_date('abc')", 'ERROR: invalid Jalali date "abc", expected Y/M/D')
add("I", "trailing junk", "select jalali_to_date('1404/1/1x')", 'ERROR: invalid Jalali date "1404/1/1x", expected Y/M/D')
add("I", "two parts", "select jalali_to_date('1404/1')", 'ERROR: invalid Jalali date "1404/1", expected Y/M/D')
add("I", "NULL text", "select jalali_to_date(NULL)", "NULL")

# week of year (Saturday based, week 1 holds 1 Farvardin), trunc and period on random dates
def week_of(g):
    f = fields(g.year, g.month, g.day)
    j1 = j2g(f["year"], 1, 1)
    j1dow = fields(j1.year, j1.month, j1.day)["dow"]
    return (f["doy"] - 1 + j1dow) // 7 + 1


def start_of(unit, g):
    f = fields(g.year, g.month, g.day)
    y, m = f["year"], f["month"]
    if unit == "day":
        return g, g + datetime.timedelta(days=1)
    if unit == "week":
        s = g - datetime.timedelta(days=f["dow"])
        return s, s + datetime.timedelta(days=7)
    if unit == "month":
        a, b = (y, m), (y + (m == 12), m % 12 + 1)
    elif unit == "quarter":
        q = (m - 1) // 3
        a, b = (y, q * 3 + 1), (y + (q == 3), (q + 1) * 3 % 12 + 1)
    elif unit == "year":
        a, b = (y, 1), (y + 1, 1)
    elif unit == "decade":
        a, b = (y // 10 * 10, 1), (y // 10 * 10 + 10, 1)
    elif unit == "century":
        a, b = ((y - 1) // 100 * 100 + 1, 1), ((y - 1) // 100 * 100 + 101, 1)
    else:
        a, b = ((y - 1) // 1000 * 1000 + 1, 1), ((y - 1) // 1000 * 1000 + 1001, 1)
    return j2g(a[0], a[1], 1), j2g(b[0], b[1], 1)


for _ in range(120):
    g = datetime.date(2000, 1, 1) + datetime.timedelta(days=random.randint(0, 20000))
    lit = "date '%s'" % g
    add("I", "week of %s" % g, "select jalali_part('week', %s)" % lit, week_of(g))
    for unit in ("day", "week", "month", "quarter", "year", "decade", "century", "millennium"):
        a, b = start_of(unit, g)
        add("I", "trunc %s %s" % (unit, g), "select jalali_trunc('%s', %s)" % (unit, lit), a)
        add("I", "period_end %s %s" % (unit, g), "select jalali_period_end('%s', %s)" % (unit, lit), b)
    a, b = start_of("month", g)
    add("I", "period month %s" % g, "select jalali_period('month', %s)" % lit, "[%s,%s)" % (a, b))
    a, b = start_of("year", g)
    add("I", "naive trunc year %s" % g, "select jalali_trunc('year', timestamp '%s 13:14:15')" % g, "%s 00:00:00" % a)
    add("I", "naive period year %s" % g, "select jalali_period('year', timestamp '%s 13:14:15')" % g,
        '["%s 00:00:00","%s 00:00:00")' % (a, b))
add("I", "week 1", "select jalali_part('week', date '2025-03-21')", 1)
add("I", "trunc dow is rejected", "select jalali_trunc('dow', date '2025-03-21')", 'ERROR: jalali_trunc units "dow" not recognized or not supported')
add("I", "trunc bad unit", "select jalali_trunc('fortnight', date '2025-03-21')", 'ERROR: jalali_trunc units "fortnight" not recognized or not supported')
add("I", "trunc hour naive", "select jalali_trunc('hour', timestamp '2025-03-21 10:59:59')", "2025-03-21 10:00:00")
add("I", "trunc minute naive", "select jalali_trunc('minute', timestamp '2025-03-21 10:59:59')", "2025-03-21 10:59:00")
add("I", "trunc second naive", "select jalali_trunc('second', timestamp '2025-03-21 10:59:59.9')", "2025-03-21 10:59:59")
add("I", "period hour naive", "select jalali_period('hour', timestamp '2025-03-21 10:59:59')", '["2025-03-21 10:00:00","2025-03-21 11:00:00")')
add("I", "trunc infinity", "select jalali_trunc('month', date 'infinity')", "infinity")
add("I", "trunc -infinity ts", "select jalali_trunc('month', timestamptz '-infinity')", "-infinity")
add("I", "trunc NULL", "select jalali_trunc('month', NULL::date)", "NULL")
add("I", "trunc before epoch", "select jalali_trunc('month', date '0500-01-01')", "NULL")
add("I", "trunc case/space", "select jalali_trunc(' MONTH ', date '2025-04-25')", "2025-04-21")
add("I", "trunc tz month", "select jalali_trunc('month', timestamptz '2025-04-20 21:00:00+00')", "2025-04-21 00:00:00+03:30", tz="Asia/Tehran")
add("I", "trunc tz day (UTC evening is next Tehran day)", "select jalali_trunc('day', timestamptz '2025-04-20 21:00:00+00')", "2025-04-21 00:00:00+03:30", tz="Asia/Tehran")
add("I", "trunc tz UTC", "select jalali_trunc('day', timestamptz '2025-04-20 21:00:00+00')", "2025-04-20 00:00:00+00", tz="UTC")
add("I", "period tz month", "select jalali_period('month', timestamptz '2025-04-25 12:00:00+00')", '["2025-04-21 00:00:00+03:30","2025-05-22 00:00:00+03:30")', tz="Asia/Tehran")
add("I", "trunc across DST (NY, Mehr 1403 -> spring)", "select jalali_trunc('month', timestamptz '2025-03-10 12:00:00+00')", "2025-02-19 00:00:00-05", tz="America/New_York")
add("I", "period hour across DST", "select jalali_period('hour', timestamptz '2025-03-09 07:30:00+00')", '["2025-03-09 03:00:00-04","2025-03-09 04:00:00-04")', tz="America/New_York")
add("I", "period_end hour before DST jump", "select jalali_period_end('hour', timestamptz '2025-03-09 06:30:00+00')", "2025-03-09 03:00:00-04", tz="America/New_York")

# to_char_jalali (en)
TS = "timestamp '2025-03-25 15:04:05.123456'"
for pat, want in [
    ("YYYY/MM/DD", "1404/01/05"), ("YYYY-MM-DD HH24:MI:SS", "1404-01-05 15:04:05"), ("YY", "04"), ("YYY", "404"), ("Y", "4"),
    ("DD", "05"), ("FMDD", "5"), ("FMMM", "1"), ("DDD", "005"), ("FMDDD", "5"), ("Month", "Farvardin"), ("MONTH", "FARVARDIN"),
    ("month", "farvardin"), ("Mon", "Far"), ("MON", "FAR"), ("mon", "far"), ("Day", "Seshanbe"), ("DAY", "SESHANBE"),
    ("day", "seshanbe"), ("Dy", "Ses"), ("D", "4"), ("HH", "03"), ("HH12", "03"), ("HH24", "15"), ("MI", "04"), ("SS", "05"),
    ("MS", "123"), ("US", "123456"), ("AM", "PM"), ("pm", "pm"), ("Q", "1"), ("W", "1"), ("WW", "01"), ("CC", "15"),
    ('"Year:" YYYY', "Year: 1404"), ("\\Y YYYY", "Y 1404"), ("\\\\Y", "\\4"), ("FMHH12:FMMI", "3:4"), ("", ""), ("no tokens!", "no tokens!"),
]:
    add("I", "to_char_jalali %r" % pat, "select to_char_jalali(%s, $p$%s$p$)" % (TS, pat), want)
add("I", "to_char date", "select to_char_jalali(date '2025-03-25', 'YYYY/MM/DD')", "1404/01/05")
add("I", "to_char tz", "select to_char_jalali(timestamptz '2025-03-25 22:00:00+00', 'YYYY/MM/DD HH24:MI')", "1404/01/06 01:30", tz="Asia/Tehran")
add("I", "to_char fa", "select to_char_jalali(date '2025-03-25', 'FMDD Month YYYY', 'fa')", "۵ فروردین ۱۴۰۴")
add("I", "to_char fa padded", "select to_char_jalali(date '2025-03-25', 'YYYY/MM/DD', 'fa')", "۱۴۰۴/۰۱/۰۵")
add("I", "to_char fa day", "select to_char_jalali(date '2025-03-25', 'Day', 'fa')", "سه\u200cشنبه")
add("I", "to_char bad lang", "select to_char_jalali(date '2025-03-25', 'YYYY', 'de')", "ERROR: to_char_jalali language must be 'en' or 'fa'")
add("I", "to_char infinity", "select to_char_jalali(date 'infinity', 'YYYY')", "NULL")
add("I", "to_char NULL", "select to_char_jalali(NULL::date, 'YYYY')", "NULL")
for m in range(1, 13):
    add("I", "month name %d" % m, "select to_char_jalali(make_jalali_date(1404,%d,1), 'Month')" % m,
        ["Farvardin", "Ordibehesht", "Khordad", "Tir", "Mordad", "Shahrivar", "Mehr", "Aban", "Azar", "Dey", "Bahman", "Esfand"][m - 1])

# statistics helper
add("I", "stats helper bad column", "select pg_temp.ddl($d$select jalali_create_statistics('pg_class','nope')$d$)", "ERROR: column nope not found in pg_class")
add("I", "stats helper bad type", "select pg_temp.ddl($d$select jalali_create_statistics('pg_class','relname')$d$)", "ERROR: column relname has type name, expected date or timestamp")


# ---- J: ISO-style week, add_months, age, series, parse, bin, jalali_date, in_jalali_period ----
def jdate(g):
    f = fields(g.year, g.month, g.day)
    return f["year"], f["month"], f["day"]


def iso_week_ref(g):
    f = fields(g.year, g.month, g.day)
    mid = g - datetime.timedelta(days=f["dow"]) + datetime.timedelta(days=3)  # Tuesday of the Sat..Fri week
    fy = fields(mid.year, mid.month, mid.day)["year"]
    return fy, (mid - j2g(fy, 1, 1)).days // 7 + 1


def add_months_ref(y, m, d, n):
    t = y * 12 + (m - 1) + n
    ny, nm = t // 12, t % 12 + 1
    return j2g(ny, nm, min(d, mlen(ny, nm)))


def age_ref(a, b):
    """a - b in Jalali y/m/d, borrowing the length of the earlier date's month."""
    sign = 1
    if jdate(a) < jdate(b):
        a, b, sign = b, a, -1
    (y1, m1, d1), (y0, m0, d0) = jdate(a), jdate(b)
    dd, dm, dy = d1 - d0, m1 - m0, y1 - y0
    if dd < 0:
        dd += mlen(y0, m0)
        dm -= 1
    if dm < 0:
        dm += 12
        dy -= 1
    months, days = sign * (dy * 12 + dm), sign * dd
    parts = []
    if months:
        yy, mm = abs(months) // 12, abs(months) % 12
        sg = "-" if months < 0 else ""
        if yy:
            parts.append("%s%d year%s" % (sg, yy, "" if (yy == 1 and not sg) else "s"))
        if mm:
            parts.append("%s%d mon%s" % (sg, mm, "" if (mm == 1 and not sg) else "s"))
    if days:
        parts.append("%d day%s" % (days, "" if days == 1 else "s"))
    return " ".join(parts) if parts else "00:00:00"


for _ in range(150):
    g = datetime.date(2000, 1, 1) + datetime.timedelta(days=random.randint(0, 30000))
    f = fields(g.year, g.month, g.day)
    w = iso_week_ref(g)
    add("J", "isoweek %s" % g, "select jalali_part('isoweek', date '%s')" % g, w[1])
    add("J", "isoyear %s" % g, "select jalali_part('isoyear', date '%s')" % g, w[0])
    add("J", "to_char IYYY-IW-ID %s" % g, "select to_char_jalali(date '%s', 'IYYY-IW-ID')" % g,
        "%d-%02d-%d" % (w[0], w[1], f["isodow"]))
    add("J", "to_char J %s" % g, "select to_char_jalali(date '%s', 'J')" % g, g.toordinal() + 1721425)
    add("J", "to_char RM %s" % g, "select to_char_jalali(date '%s', 'FMRM')" % g,
        ["I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX", "X", "XI", "XII"][f["month"] - 1])
    n = random.randint(-30, 40)
    y, m, d = jdate(g)
    try:
        want = add_months_ref(y, m, d, n)
    except Exception:
        continue
    add("J", "add_months %s %+d" % (g, n), "select jalali_add_months(date '%s', %d)" % (g, n), want)
    add("J", "add_months naive keeps time %s %+d" % (g, n),
        "select jalali_add_months(timestamp '%s 10:11:12.5', %d)" % (g, n), "%s 10:11:12.5" % want)
    h = datetime.date(2000, 1, 1) + datetime.timedelta(days=random.randint(0, 30000))
    add("J", "age %s %s" % (g, h), "select jalali_age(date '%s', date '%s')" % (g, h), age_ref(g, h))
    # parse round trip with patterns
    add("J", "to_date pattern %s" % g, "select jalali_to_date('%d/%02d/%02d', 'YYYY/MM/DD')" % (y, m, d), g)
    add("J", "to_date month name %s" % g, "select jalali_to_date(to_char_jalali(date '%s', 'FMDD Month YYYY'), 'DD Month YYYY')" % g, g)
    add("J", "to_date persian name %s" % g, "select jalali_to_date(to_char_jalali(date '%s', 'FMDD Month YYYY', 'fa'), 'DD Month YYYY')" % g, g)
    add("J", "to_date DDD %s" % g, "select jalali_to_date('%d-%03d', 'YYYY-DDD')" % (y, f["doy"]), g)
    add("J", "jalali_date round trip %s" % g, "select (date '%s'::jalali_date)::text" % g, "%d/%02d/%02d" % (y, m, d))
    add("J", "jalali_date parse %s" % g, "select ('%d/%02d/%02d'::jalali_date)::date" % (y, m, d), g)
    # bins
    k = random.choice([1, 2, 3, 4, 6])
    mi = (y - 1) * 12 + (m - 1)
    mi -= mi % k
    add("J", "bin %d month %s" % (k, g), "select jalali_bin(%d,'month',date '%s')" % (k, g), j2g(mi // 12 + 1, mi % 12 + 1, 1))
    q = (y - 1) * 4 + (m - 1) // 3
    q -= q % k
    add("J", "bin %d quarter %s" % (k, g), "select jalali_bin(%d,'quarter',date '%s')" % (k, g), j2g(q // 4 + 1, (q % 4) * 3 + 1, 1))
    yy = y - 1
    yy -= yy % k
    add("J", "bin %d year %s" % (k, g), "select jalali_bin(%d,'year',date '%s')" % (k, g), j2g(yy + 1, 1, 1))
    e0 = j2g(1, 1, 1)
    ref = e0 - datetime.timedelta(days=fields(e0.year, e0.month, e0.day)["dow"])
    ws = g - datetime.timedelta(days=f["dow"])
    add("J", "bin %d week %s" % (k, g), "select jalali_bin(%d,'week',date '%s')" % (k, g),
        ref + datetime.timedelta(days=((ws - ref).days // (7 * k)) * 7 * k))
    add("J", "bin %d day %s" % (k, g), "select jalali_bin(%d,'day',date '%s')" % (k, g),
        e0 + datetime.timedelta(days=((g - e0).days // k) * k))

for y in (1399, 1400, 1403, 1404, 1405):
    s0, e0 = j2g(y, 1, 1), j2g(y + 1, 1, 1)
    add("J", "series month count %d" % y, "select count(*) from jalali_series(make_jalali_date(%d,1,1), make_jalali_date(%d,1,1) - 1, 'month')" % (y, y + 1), 12)
    add("J", "series day count %d" % y, "select count(*) from jalali_series(make_jalali_date(%d,1,1), make_jalali_date(%d,1,1) - 1, 'day')" % (y, y + 1), (e0 - s0).days)
    add("J", "series week first/last %d" % y, "select min(x)||' '||count(*) from jalali_series(make_jalali_date(%d,1,1), make_jalali_date(%d,1,1) - 1, 'week') x" % (y, y + 1), "%s %d" % (s0, ((e0 - s0).days + 6) // 7))
    add("J", "series quarter %d" % y, "select array_to_string(array_agg(x), ',') from jalali_series(make_jalali_date(%d,1,1), make_jalali_date(%d,1,1) - 1, 'quarter') x" % (y, y + 1),
        ",".join(str(j2g(y, mo, 1)) for mo in (1, 4, 7, 10)))
add("J", "series clamps without drift", "select array_to_string(array_agg(x), ',') from jalali_series(make_jalali_date(1404,1,31), make_jalali_date(1404,8,1), 'month') x",
    ",".join(str(add_months_ref(1404, 1, 31, i)) for i in range(7)))
add("J", "series negative step", "select array_to_string(array_agg(x), ',') from jalali_series(make_jalali_date(1404,3,1), make_jalali_date(1404,1,1), 'month', -1) x",
    ",".join(str(j2g(1404, mo, 1)) for mo in (3, 2, 1)))
add("J", "series zero step", "select count(*) from jalali_series(date '2025-01-01', date '2025-02-01', 'day', 0)", "ERROR: step size cannot equal zero")
add("J", "series bad unit", "select count(*) from jalali_series(date '2025-01-01', date '2025-02-01', 'decade')", 'ERROR: jalali_series units "decade" not recognized or not supported')

add("J", "age same date", "select jalali_age(date '2025-03-21', date '2025-03-21')", "00:00:00")
add("J", "age 1 year exactly", "select jalali_age(make_jalali_date(1404,1,1), make_jalali_date(1403,1,1))", "1 year")
add("J", "age timestamps time part", "select jalali_age(timestamp '2025-03-21 10:00', timestamp '2025-03-20 12:00')", "22:00:00")
add("J", "age NULL", "select jalali_age(NULL::date, date '2025-03-21')", "NULL")
add("J", "add_months infinity", "select jalali_add_months(date 'infinity', 3)", "infinity")
add("J", "add_months esfand 30 + 12", "select jalali_add_months(make_jalali_date(1403,12,30), 12)", j2g(1404, 12, 29))
add("J", "add_months before epoch is NULL", "select jalali_add_months(date '0500-01-01', 1)", "NULL")
add("J", "last day of month 1404/12", "select jalali_last_day_of_month(make_jalali_date(1404,12,3))", j2g(1404, 12, 29))
add("J", "last day of month 1403/12", "select jalali_last_day_of_month(make_jalali_date(1403,12,3))", j2g(1403, 12, 30))
add("J", "last day of month 1404/07", "select jalali_last_day_of_month(make_jalali_date(1404,7,3))", j2g(1404, 7, 30))

# parsing details
for text, pat, want in [
    ("1404/01/05", "YYYY/MM/DD", "2025-03-25"), ("1404-1-5", "YYYY-MM-DD", "2025-03-25"), ("۱۴۰۴/۰۱/۰۵", "YYYY/MM/DD", "2025-03-25"),
    ("5 Farvardin 1404", "DD Month YYYY", "2025-03-25"), ("5 FARVARDIN 1404", "DD MONTH YYYY", "2025-03-25"), ("5 far 1404", "DD Mon YYYY", "2025-03-25"),
    ("5 فروردین 1404", "DD Month YYYY", "2025-03-25"), ("1404/12/29", "YYYY/MM/DD", "2026-03-20"), ("99/01/01", "YY/MM/DD", str(j2g(1399, 1, 1))),
    ("04/01/01", "YY/MM/DD", str(j2g(1404, 1, 1))), ("Seshanbe 5 Farvardin 1404", "Day DD Month YYYY", "2025-03-25"),
    ("1404/12/30", "YYYY/MM/DD", "ERROR: Jalali date/time field value out of range"),
    ("1404/13/01", "YYYY/MM/DD", "ERROR: Jalali date/time field value out of range"),
    ("1404/01/05 junk", "YYYY/MM/DD", 'ERROR: trailing characters remain in Jalali input string "1404/01/05 junk"'),
    ("x", "YYYY", 'ERROR: invalid value for a numeric field in Jalali date/time "x"'),
    ("5 Foo 1404", "DD Month YYYY", 'ERROR: invalid value for a month name in Jalali date/time "5 Foo 1404"'),
    ("1404-366", "YYYY-DDD", "ERROR: Jalali day of year out of range: 366"),
]:
    add("J", "to_date %r %r" % (text, pat), "select jalali_to_date($t$%s$t$, $t$%s$t$)" % (text, pat), want)
for text, pat, want in [
    ("1404/01/05 03:04:05 PM", "YYYY/MM/DD HH12:MI:SS AM", "2025-03-25 15:04:05"), ("1404/01/05 12:00:00 AM", "YYYY/MM/DD HH12:MI:SS AM", "2025-03-25 00:00:00"),
    ("1404/01/05 12:30 PM", "YYYY/MM/DD HH12:MI AM", "2025-03-25 12:30:00"), ("1404/01/05 23:59:58", "YYYY/MM/DD HH24:MI:SS", "2025-03-25 23:59:58"),
    ("1404/01/05 10:11:12.123456", "YYYY/MM/DD HH24:MI:SS.US", "2025-03-25 10:11:12.123456"), ("1404/01/05 10:11:12.5", "YYYY/MM/DD HH24:MI:SS.MS", "2025-03-25 10:11:12.5"),
    ("1404/01/05 25:00", "YYYY/MM/DD HH24:MI", "ERROR: Jalali date/time field value out of range"),
]:
    add("J", "to_timestamp %r" % text, "select jalali_to_timestamp($t$%s$t$, $t$%s$t$)" % (text, pat), want)
add("J", "to_timestamptz zone", "select jalali_to_timestamptz('1404/01/05 12:00','YYYY/MM/DD HH24:MI','Asia/Tehran')", "2025-03-25 12:00:00+03:30", tz="Asia/Tehran")
add("J", "to_timestamptz UTC session", "select jalali_to_timestamptz('1404/01/05 12:00','YYYY/MM/DD HH24:MI')", "2025-03-25 12:00:00+00", tz="UTC")
add("J", "to_timestamptz explicit zone", "select jalali_to_timestamptz('1404/01/05 12:00','YYYY/MM/DD HH24:MI','America/New_York')", "2025-03-25 16:00:00+00", tz="UTC")
add("J", "make_jalali_timestamp", "select make_jalali_timestamp(1404,1,5,13,14,15.25)", "2025-03-25 13:14:15.25")
add("J", "make_jalali_timestamptz zone", "select make_jalali_timestamptz(1404,1,5,0,0,0,'Asia/Tehran')", "2025-03-25 00:00:00+03:30", tz="Asia/Tehran")
add("J", "make_jalali_timestamptz bad", "select make_jalali_timestamp(1404,12,30,0,0,0)", "ERROR: Jalali date field value out of range: 1404-12-30")

# explicit zone argument
add("J", "part with zone", "select jalali_part('day', timestamptz '2025-04-20 21:00+00', 'Asia/Tehran')", 1, tz="UTC")
add("J", "trunc with zone", "select jalali_trunc('month', timestamptz '2025-04-20 21:00+00', 'Asia/Tehran')", "2025-04-20 20:30:00+00", tz="UTC")
add("J", "period_end with zone", "select jalali_period_end('month', timestamptz '2025-04-25 12:00+00', 'Asia/Tehran')", "2025-05-21 20:30:00+00", tz="UTC")
add("J", "period with zone", "select jalali_period('day', timestamptz '2025-04-20 21:00+00', 'Asia/Tehran')", '["2025-04-20 20:30:00+00","2025-04-21 20:30:00+00")', tz="UTC")

# to_char additions
for pat, want in [("TH", "TH"), ("DDth", "05th"), ("FMDDth", "5th"), ("FMDDTH", "5TH"), ("FMMMth", "1st"), ("RM", "   I"), ("FMRM", "I"), ("rm", "   i"),
                  ("SSSS", "54245"), ("TZ", ""), ("OF", ""), ("ID", "2"), ("IW", "01"), ("IYYY", "1404")]:
    add("J", "to_char_jalali %r on a timestamp" % pat, "select to_char_jalali(timestamp '2025-03-25 15:04:05', $p$%s$p$)" % pat, want)
add("J", "to_char TZ names", "select to_char_jalali(timestamptz '2025-03-25 12:00+00', 'TZ OF TZH TZM')", "+0330 +03:30 +03 30", tz="Asia/Tehran")
add("J", "to_char OF whole hours", "select to_char_jalali(timestamptz '2025-03-25 12:00+00', 'OF')", "+00", tz="UTC")
add("J", "to_char persian ordinal", "select to_char_jalali(date '2025-03-25', 'FMDDth', 'fa')", "۵ام")

# jalali_date type
add("J", "jalali_date infinity", "select ('infinity'::jalali_date)::text", "infinity")
add("J", "jalali_date -infinity", "select ('-infinity'::jalali_date)::text", "-infinity")
add("J", "jalali_date garbage", "select 'abc'::jalali_date", 'ERROR: invalid input syntax for type jalali_date: "abc"')
add("J", "jalali_date invalid", "select '1404/12/30'::jalali_date", "ERROR: Jalali date field value out of range: 1404-12-30")
add("J", "jalali_date order", "select array_to_string(array_agg(x::text order by x), ',') from (values ('1404/02/01'::jalali_date),('1403/12/29'::jalali_date),('1404/01/05'::jalali_date)) v(x)", "1403/12/29,1404/01/05,1404/02/01")
add("J", "jalali_date compare", "select '1404/01/05'::jalali_date < '1404/01/06'::jalali_date and '1404/01/05'::jalali_date = date '2025-03-25'", "true")
add("J", "jalali_date to_char", "select to_char_jalali('1404/01/05'::jalali_date, 'YYYY Month')", "1404 Farvardin")
add("J", "jalali_date part via cast", "select jalali_part('month', '1404/03/05'::jalali_date)", 3)
add("J", "jalali_date arithmetic via date", "select ('1404/01/05'::jalali_date + 1)::jalali_date::text", "1404/01/06")
add("J", "jalali_date before epoch", "select (date '0500-01-01')::jalali_date::text", "G:0500-01-01")
add("J", "jalali_date index and group", "select pg_temp.plan($d$create temp table jd(d jalali_date); insert into jd select date '2020-01-01'+i from generate_series(0,50000) i; create index on jd(d); analyze jd$d$, $e$select * from jd where d = '1404/01/05'$e$)", "index")

# in_jalali_period and index use
add("J", "in_jalali_period date true", "select in_jalali_period(date '2025-04-21', 'month', date '2025-05-21')", "true")
add("J", "in_jalali_period date false (end exclusive)", "select in_jalali_period(date '2025-05-22', 'month', date '2025-05-21')", "false")
add("J", "in_jalali_period week", "select in_jalali_period(date '2025-04-19', 'week', date '2025-04-25')", "true")
add("J", "in_jalali_period week edge", "select in_jalali_period(date '2025-04-26', 'week', date '2025-04-25')", "false")
add("J", "in_jalali_period tz", "select in_jalali_period(timestamptz '2025-04-20 21:00+00', 'day', timestamptz '2025-04-21 10:00+00')", "true", tz="Asia/Tehran")
add("J", "in_jalali_period tz UTC differs", "select in_jalali_period(timestamptz '2025-04-20 21:00+00', 'day', timestamptz '2025-04-21 10:00+00')", "false", tz="UTC")
add("J", "in_jalali_period NULL", "select in_jalali_period(NULL::date, 'month', date '2025-05-21')", "NULL")
add("J", "in_jalali_period uses index (date)", "select pg_temp.plan($d$create temp table ip(d date); insert into ip select date '2020-01-01'+i from generate_series(0,50000) i; create index on ip(d); analyze ip$d$, $e$select * from ip where in_jalali_period(d,'month',date '2025-04-25')$e$)", "index")
add("J", "in_jalali_period uses index (timestamptz)", "select pg_temp.plan($d$create temp table ipt(d timestamptz); insert into ipt select timestamptz '2020-01-01'+i*interval '1 hour' from generate_series(0,50000) i; create index on ipt(d); analyze ipt$d$, $e$select * from ipt where in_jalali_period(d,'year',now())$e$)", "index")
add("J", "in_jalali_period matches range", "select (select count(*) from generate_series(timestamp '2024-01-01', timestamp '2026-12-31', interval '1 day') g where in_jalali_period(g::date,'month',date '2025-04-25')) = (select count(*) from generate_series(timestamp '2024-01-01', timestamp '2026-12-31', interval '1 day') g where g::date >= jalali_trunc('month', date '2025-04-25') and g::date < jalali_period_end('month', date '2025-04-25'))", "true")


# ---------------------------------------------------------------- build and run
PRELUDE = r"""
\set ON_ERROR_STOP off
create function pg_temp.r(q text) returns text language plpgsql as $$
declare v text;
begin execute q into v; return coalesce(v,'NULL');
exception when others then return 'ERROR: '||sqlerrm; end $$;
create function pg_temp.ddl(q text) returns text language plpgsql as $$
begin execute q; return 'ok';
exception when others then return 'ERROR: '||sqlerrm; end $$;
create function pg_temp.plan(setup text, q text) returns text language plpgsql as $$
declare l text; res text := 'seq';
begin
  execute setup;
  set local enable_seqscan = off; set local enable_bitmapscan = off;
  for l in execute 'explain '||q loop if l ilike '%Index%' then res := 'index'; end if; end loop;
  return res;
exception when others then return 'ERROR: '||sqlerrm; end $$;
create function pg_temp.flip(i int) returns timestamptz language plpgsql as $$
begin
  perform set_config('TimeZone', case i%3 when 0 then 'Asia/Tehran' when 1 then 'America/New_York' else 'UTC' end, true);
  return timestamptz '2021-03-21 12:00:00+00' + i * interval '7 hours';
end $$;
create function pg_temp.sqlpart(u text) returns int language sql as $$ select jalali_part(u, date '2026-11-12') $$;
create function pg_temp.loopu() returns text language plpgsql as $$
declare u text; r text[] := '{}';
begin
  foreach u in array array['year','month','day','year','day','doy'] loop
    r := r || jalali_part(u, date '2026-11-12')::text;
  end loop;
  return array_to_string(r, ',');
end $$;
"""


def build_sql():
    out = [PRELUDE]
    cur = None
    for cid, cat, desc, exp, q, tz in CASES:
        if tz != cur:
            out.append("set time zone '%s';" % tz)
            cur = tz
        out.append("select '%s|'||pg_temp.r($q$%s$q$);" % (cid, q))
    return "\n".join(out) + "\n"


def main():
    cmd = sys.argv[1:]
    if not cmd:
        print(__doc__)
        sys.exit(2)
    sql = build_sql()
    open(os.path.join(HERE, "suite.sql"), "w").write(sql)
    res = subprocess.run(cmd, input=sql, capture_output=True, text=True)
    actual = {}
    for line in res.stdout.splitlines():
        head, sep, rest = line.partition("|")
        if sep and head[:1] in "ABCDEFGHIJ" and head[1:].isdigit():
            actual[head] = rest
    lines, per, bad = [], collections.defaultdict(lambda: [0, 0]), []
    for cid, cat, desc, exp, q, tz in CASES:
        act = actual.get(cid, "<no result>")
        ok = act == exp
        per[cat][0] += ok
        per[cat][1] += 1
        lines.append("%s | %s | tz=%s | %s | expected=%s | actual=%s" % ("PASS" if ok else "FAIL", cid, tz, desc, exp, act))
        if not ok:
            bad.append((cid, desc, tz, exp, act, q))
    open(os.path.join(HERE, "suite_results.txt"), "w").write("\n".join(lines) + "\n")
    names = {"A": "format_jalali(date) calendar", "B": "jalali_part fields on dates", "C": "unit names",
             "D": "timestamptz and time zones", "E": "naive timestamp path", "F": "NULL, infinity, limits",
             "G": "catalog, indexes", "H": "caches and zone switching",
             "I": "0.0.4 features",
             "J": "0.0.4 extras: iso week, age, parse, bin, type, index"}
    total = ok_total = 0
    for cat in sorted(per):
        print("%s  %-32s %4d/%-4d" % (cat, names[cat], per[cat][0], per[cat][1]))
        total += per[cat][1]
        ok_total += per[cat][0]
    print("TOTAL %d/%d passed" % (ok_total, total))
    for cid, desc, tz, exp, act, q in bad:
        print("FAIL %s [%s] %s\n   expected: %s\n   actual:   %s\n   query:    %s" % (cid, tz, desc, exp, act, q))
    sys.exit(0 if not bad else 1)


if __name__ == "__main__":
    main()
