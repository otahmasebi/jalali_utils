# Regenerates tests/bug_probe.sql from the case list below.
CASES = [
# (id, description, expected, query, session TimeZone)
("A1","Naive timestamp path: UTC 19:44 -> Tehran 23:14 (naive) -> day of 1405/08/21","21",
 "select jalali_part('day', ('2026-11-12 19:44:17'::timestamp at time zone 'UTC' at time zone 'Asia/Tehran'))::text","UTC"),
("A2","Naive timestamp path: 00:15 Tehran on Nowruz 1405 (naive local) -> month 1","1",
 "select jalali_part('month', ('2026-03-20 20:45:00'::timestamp at time zone 'UTC' at time zone 'Asia/Tehran'))::text","UTC"),
("A3","Naive timestamp path: 23:30 Tehran on 29 Esfand 1404 -> month stays 12","12",
 "select jalali_part('month', ('2026-03-20 20:00:00'::timestamp at time zone 'UTC' at time zone 'Asia/Tehran'))::text","UTC"),
("A4","timestamptz follows the session TimeZone: New_York renders 14:44:17","1405/08/21 14:44:17",
 "select format_jalali('2026-11-12 19:44:17+00'::timestamptz)","America/New_York"),
("A5","timestamptz follows the session TimeZone: UTC renders 19:44:17","1405/08/21 19:44:17",
 "select format_jalali('2026-11-12 19:44:17+00'::timestamptz)","UTC"),
("A6","jalali_part(day, date) 2026-11-12 -> 21","21",
 "select jalali_part('day','2026-11-12'::date)::text","UTC"),
("A7","Tehran session: 19:44Z = 23:14 Tehran -> day 21","21",
 "select jalali_part('day','2026-11-12 19:44:17+00'::timestamptz)::text","Asia/Tehran"),
("A8","Tehran session: 20:30Z = 00:00 Tehran next day -> day 22","22",
 "select jalali_part('day','2026-11-12 20:30:00+00'::timestamptz)::text","Asia/Tehran"),
("A9","naive timestamp is never shifted by the session TimeZone (New_York session)","21",
 "select jalali_part('day','2026-11-12 23:14:17'::timestamp)::text","America/New_York"),
("B1","jalali_part(hour, date) is 0","0","select jalali_part('hour','2024-01-10'::date)::text","UTC"),
("B2","jalali_part(minute, date) is 0","0","select jalali_part('minute','2024-01-10'::date)::text","UTC"),
("B3","jalali_part(hour, date) in an old Iran DST period is 0","0","select jalali_part('hour','2019-07-07'::date)::text","UTC"),
("B4","jalali_part(second, date) is 0","0","select jalali_part('second','2024-01-10'::date)::text","UTC"),
("B5","jalali_part(hour, date) is 0 under a Tehran session too","0","select jalali_part('hour','2024-01-10'::date)::text","Asia/Tehran"),
("C1","format_jalali(date) Jalali year 979 (3 digits)","979/03/26","select format_jalali('1600-06-15'::date)","UTC"),
("C2","format_jalali(timestamptz) year 979; Tehran used local mean time (+3:25:44) before 1946","979/03/26 03:55:44",
 "select format_jalali('1600-06-15 00:30:00+00'::timestamptz)","Asia/Tehran"),
("C3","format_jalali(date) before the Jalali epoch (622-03-21) has no Jalali date: NULL","NULL",
 "select format_jalali('0100-06-15'::date)","UTC"),
("C4","format_jalali(date) length for a normal date","10","select length(format_jalali('2026-11-12'::date))::text","UTC"),
("C5","format_jalali(timestamptz, false) year 979","979/03/26","select format_jalali('1600-06-15 12:00:00+00'::timestamptz,false)","Asia/Tehran"),
("C6","format_jalali(date) on the Jalali epoch 622-03-21","1/01/01","select format_jalali('0622-03-21'::date)","UTC"),
("C7","format_jalali(date) the day before the epoch is NULL","NULL","select format_jalali('0622-03-20'::date)","UTC"),
("C8","dates before 1600 are correct (1599-03-19 = 977/12/28; used to give day -36)","977/12/28","select format_jalali('1599-03-19'::date)","UTC"),
("D1","format_jalali(timestamptz infinity) is NULL","NULL","select format_jalali('infinity'::timestamptz)","UTC"),
("D2","jalali_part(year, timestamptz -infinity) is NULL","NULL","select jalali_part('year','-infinity'::timestamptz)::text","UTC"),
("D3","jalali_part(year, date infinity) is NULL","NULL","select jalali_part('year','infinity'::date)::text","UTC"),
("D4","format_jalali(date max 5874897-12-31) does not overflow (checked against an independent big-integer implementation)","5874278/01/01",
 "select format_jalali('5874897-12-31'::date)","UTC"),
("D5","format_jalali(timestamp infinity) is NULL","NULL","select format_jalali('infinity'::timestamp)","UTC"),
("E1","Tehran 23:59:59.5 on 1999-12-31 is 1378/10/10 23:59:59","1378/10/10 23:59:59",
 "select format_jalali('1999-12-31 20:29:59.5+00'::timestamptz)","Asia/Tehran"),
("E2","control after 2000: Tehran 23:59:59.5 on 2026-03-20","1404/12/29 23:59:59",
 "select format_jalali('2026-03-20 20:29:59.5+00'::timestamptz)","Asia/Tehran"),
("E3","jalali_part(day) at 23:59:59.999999 Tehran before 2000 is 10","10",
 "select jalali_part('day','1999-12-31 20:29:59.999999+00'::timestamptz)::text","Asia/Tehran"),
("E4","jalali_part(second) of 23:59:59.5 before 2000 is 59","59",
 "select jalali_part('second','1999-12-31 20:29:59.5+00'::timestamptz)::text","Asia/Tehran"),
("F2","jalali_part(century) 1405 is the 15th century","15","select jalali_part('century','2026-11-12'::date)::text","UTC"),
("F4","jalali_part(isodow): Thursday is 4 (PostgreSQL ISO weekday, Monday=1)","4","select jalali_part('isodow','2026-11-12'::date)::text","UTC"),
("F4b","jalali_part(isodow): Sunday is 7","7","select jalali_part('isodow','2026-11-15'::date)::text","UTC"),
("F4c","jalali_part(isodow): Monday is 1","1","select jalali_part('isodow','2026-11-16'::date)::text","UTC"),
("F5","jalali_part(dow): Saturday is 0","0","select jalali_part('dow','2026-11-14'::date)::text","UTC"),
("F5b","jalali_part(dow): Friday is 6","6","select jalali_part('dow','2026-11-13'::date)::text","UTC"),
("F6","jalali_part(unknown units) raises a clear error","ERROR: jalali_part units \"nonsense\" not recognized or not supported",
 "select jalali_part('nonsense','2026-11-12'::date)::text","UTC"),
("F7","jalali_part(epoch) raises, not silently wrong","ERROR: jalali_part units \"epoch\" not recognized or not supported",
 "select jalali_part('epoch','2026-11-12'::date)::text","UTC"),
("F8","jalali_part(NULL units) is NULL (STRICT)","NULL","select jalali_part(NULL,'2026-11-12'::date)::text","UTC"),
("F9","jalali_part(week) is not supported (no Jalali week-of-year defined) and says so","ERROR: jalali_part units \"week\" not recognized or not supported",
 "select jalali_part('week','2026-11-12'::date)::text","UTC"),
("F10","jalali_part(decade) is the decade start year (1405 -> 1400), not PostgreSQL's decade count","1400",
 "select jalali_part('decade','2026-11-12'::date)::text","UTC"),
("G1","Nowruz 1405","1405/01/01","select format_jalali('2026-03-21'::date)","UTC"),
("G2","leap year 1403 has 30 Esfand","1403/12/30","select format_jalali('2025-03-20'::date)","UTC"),
("G3","1404 is not leap","1404/12/29","select format_jalali('2026-03-20'::date)","UTC"),
("G4","day of year, last day of leap year 1403","366","select jalali_part('doy','2025-03-20'::date)::text","UTC"),
("G5","day of year, 1 Mehr","187","select jalali_part('doy','2026-09-23'::date)::text","UTC"),
("G6","quarter of 1 Dey","4","select jalali_part('quarter','2025-12-22'::date)::text","UTC"),
("H1","2021-07-01 12:00Z, Iran DST (+4:30)","1400/04/10 16:30:00","select format_jalali('2021-07-01 12:00:00+00'::timestamptz)","Asia/Tehran"),
("H2","2023-07-01 12:00Z, no DST (+3:30)","1402/04/10 15:30:00","select format_jalali('2023-07-01 12:00:00+00'::timestamptz)","Asia/Tehran"),
]
def lit(x): return "'" + x.replace("'", "''") + "'"
rows = ",\n".join("(%s, %s, %s, %s, %s)" % tuple(lit(f) for f in c) for c in CASES)
open("bug_probe.sql", "w").write("""-- Regression probe for jalali_utils. Read-only (ROLLBACK). Generated; edit the case list in
-- tests/gen_probe.py. Output: id | description | expected | actual. Run with tests/run_probe.sh.
-- Each case runs under its own session TimeZone (only the timestamptz functions depend on it);
-- errors are captured as 'ERROR: <message>'.
BEGIN;
CREATE FUNCTION pg_temp.try(q text, tz text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
  PERFORM set_config('TimeZone', tz, true);
  EXECUTE q INTO r;
  RETURN coalesce(r, 'NULL');
EXCEPTION WHEN others THEN
  RETURN 'ERROR: ' || SQLERRM;
END $$;

CREATE TEMP TABLE t(id text, descr text, expected text, q text, tz text) ON COMMIT DROP;
INSERT INTO t VALUES
%s;

SELECT id, descr, expected, pg_temp.try(q, tz) AS actual FROM t ORDER BY id;
ROLLBACK;
""" % rows)
print(len(CASES), "cases")
