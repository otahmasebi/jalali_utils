#include "postgres.h"
#include <ctype.h>
#include "fmgr.h"
#include "parser/scansup.h"
#include "pgtime.h"
#include "nodes/makefuncs.h"
#include "utils/builtins.h"
#include "utils/timestamp.h"
#include "utils/datetime.h"
#include "utils/date.h"
#include "utils/fmgrprotos.h"
#include "lib/stringinfo.h"
#include "mb/pg_wchar.h"
#include "access/stratnum.h"
#include "catalog/pg_am.h"
#include "catalog/pg_type.h"
#include "nodes/supportnodes.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "optimizer/optimizer.h"
#include "parser/parse_func.h"
#include "utils/lsyscache.h"

/* days before the first of each Gregorian month in a non-leap year */
static const int g_cum_days[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
static const int j_days_in_month[12] = {31, 31, 31, 31, 31, 31, 30, 30, 30, 30, 30, 29};

#define IS_GREGORIAN_LEAP(y) (((y) % 4 == 0 && (y) % 100 != 0) || (y) % 400 == 0)

PG_MODULE_MAGIC;
PG_FUNCTION_INFO_V1(format_jalali_timestamp);
PG_FUNCTION_INFO_V1(format_jalali_timestamp_naive);
PG_FUNCTION_INFO_V1(format_jalali_date);
PG_FUNCTION_INFO_V1(jalali_part_timestamp);
PG_FUNCTION_INFO_V1(jalali_part_timestamp_naive);
PG_FUNCTION_INFO_V1(jalali_part_date);
PG_FUNCTION_INFO_V1(is_jalali_leap);
PG_FUNCTION_INFO_V1(jalali_days_in_month);
PG_FUNCTION_INFO_V1(make_jalali_date);
PG_FUNCTION_INFO_V1(jalali_to_date);
PG_FUNCTION_INFO_V1(jalali_trunc_timestamp);
PG_FUNCTION_INFO_V1(jalali_trunc_timestamp_naive);
PG_FUNCTION_INFO_V1(jalali_trunc_date);
PG_FUNCTION_INFO_V1(jalali_period_end_timestamp);
PG_FUNCTION_INFO_V1(jalali_period_end_timestamp_naive);
PG_FUNCTION_INFO_V1(jalali_period_end_date);
PG_FUNCTION_INFO_V1(to_char_jalali_timestamp);
PG_FUNCTION_INFO_V1(to_char_jalali_timestamp_naive);
PG_FUNCTION_INFO_V1(to_char_jalali_date);
PG_FUNCTION_INFO_V1(jalali_add_months_date);
PG_FUNCTION_INFO_V1(jalali_add_months_timestamp);
PG_FUNCTION_INFO_V1(jalali_add_months_timestamp_naive);
PG_FUNCTION_INFO_V1(jalali_age_date);
PG_FUNCTION_INFO_V1(jalali_age_timestamp);
PG_FUNCTION_INFO_V1(jalali_age_timestamp_naive);
PG_FUNCTION_INFO_V1(jalali_date_in);
PG_FUNCTION_INFO_V1(jalali_date_out);
PG_FUNCTION_INFO_V1(jalali_to_date_fmt);
PG_FUNCTION_INFO_V1(jalali_to_timestamp_fmt);
PG_FUNCTION_INFO_V1(jalali_bin_date);
PG_FUNCTION_INFO_V1(jalali_bin_timestamp);
PG_FUNCTION_INFO_V1(jalali_bin_timestamp_naive);
PG_FUNCTION_INFO_V1(in_jalali_period_date);
PG_FUNCTION_INFO_V1(in_jalali_period_timestamp);
PG_FUNCTION_INFO_V1(in_jalali_period_timestamp_naive);
PG_FUNCTION_INFO_V1(jalali_period_support);

typedef struct JalaliTm {
  int year, mon, day;
  int hour, min, sec;
  int doy;  /* day of the Jalali year, 1-based */
  int dow;  /* 0 = Saturday */
  int isodow;  /* 1 = Monday .. 7 = Sunday, as in PostgreSQL */
  int gy, gm, gd;  /* the same local day in the Gregorian calendar */
  int usec;  /* microseconds within the second */
  bool has_tz;  /* set only by to_char_jalali(timestamptz) */
  int gmtoff;  /* seconds east of UTC */
  char tzn[16];
} JalaliTm;

#define JU_ISOWEEK 1001  /* private unit id, not a DTK_* value */

static int64 jalali_to_julian(int64 jy, int jm, int jd);
static bool jalali_year_is_leap(int64 y);
static int jalali_month_len(int64 y, int m);
static void jalali_iso_week(const JalaliTm *j, int *wy, int *w);

static int64 floor_div(int64 a, int64 b) {
  int64 q = a / b;
  if (a % b != 0 && ((a < 0) != (b < 0)))
    q--;
  return q;
}

/* Days of the shifted (+400 years) Gregorian count used by the algorithm. */
static int64 gregorian_day_no(int gy, int gm, int gd) {
  int64 gy_s = (int64) gy + 400;
  int64 n = 365 * gy_s + (gy_s + 3) / 4 - (gy_s + 99) / 100 + (gy_s + 399) / 400;
  n += g_cum_days[gm - 1];
  if (gm > 2 && IS_GREGORIAN_LEAP(gy))
    n += 1;
  return n + gd - 1;
}

/*
 * Convert the Gregorian broken-down time in `g` (tm_year is the real year,
 * tm_mon is 1-based, as filled in by timestamp2tm / j2date) to Jalali.
 * Returns false for dates before 622-03-21 (1/1/1 Jalali), which have no
 * Jalali representation.
 */
static bool gregorian_to_jalali(const struct pg_tm *g, JalaliTm *j) {
  int gy = g->tm_year, gm = g->tm_mon, gd = g->tm_mday;
  int64 g_day_no, j_day_no, j_np;
  int jy, i, gdow;

  if (gy < 622 || (gy == 622 && (gm < 3 || (gm == 3 && gd < 21))))
    return false;

  /* The algorithm counts years from 1600. Shifting by 2000 years (5 Gregorian
   * 400-year cycles = 730485 days) keeps every year positive for the C
   * integer divisions, and is subtracted again below. */
  g_day_no = gregorian_day_no(gy, gm, gd);

  j_day_no = g_day_no - 79 - 730485;  /* negative before 1600-03-20 */
  j_np = floor_div(j_day_no, 12053);
  j_day_no -= j_np * 12053;
  jy = (int) (979 + 33 * j_np + 4 * (j_day_no / 1461));
  j_day_no %= 1461;
  if (j_day_no >= 366) {
    jy += (int) ((j_day_no - 1) / 365);
    j_day_no = (j_day_no - 1) % 365;
  }
  for (i = 0; i < 11 && j_day_no >= j_days_in_month[i]; i++)
    j_day_no -= j_days_in_month[i];

  j->year = jy;
  j->mon = i + 1;
  j->day = (int) j_day_no + 1;
  j->hour = g->tm_hour;
  j->min = g->tm_min;
  j->sec = g->tm_sec;
  j->usec = 0;
  j->has_tz = false;
  j->gmtoff = 0;
  j->tzn[0] = '\0';
  j->gy = gy;
  j->gm = gm;
  j->gd = gd;
  if (j->mon <= 6)
    j->doy = (j->mon - 1) * 31 + j->day;
  else
    j->doy = 186 + (j->mon - 7) * 30 + j->day;
  /* g_day_no counts days from 0000-03-01-style epoch shifted by 400 years;
   * (g_day_no + 6) % 7 is 0 for Sunday, same as j2day(date2j(...)). */
  gdow = (int) ((g_day_no + 6) % 7);
  j->dow = (gdow + 1) % 7;
  j->isodow = gdow == 0 ? 7 : gdow;
  return true;
}

/*
 * UTC offset cache for the session time zone. Converting through timestamp2tm
 * does a time zone database lookup for every row; for a column of timestamps
 * the offset is the same for long stretches, so remember the window
 * [tzc_lo, tzc_hi) (in Unix seconds) in which `tzc_off` applies for `tzc_zone`.
 */
static const pg_tz *tzc_zone = NULL;
static pg_time_t tzc_lo = 0, tzc_hi = 0;
static long tzc_off = 0;

#define PG_TO_UNIX_SECS \
  ((int64) (POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY)

/* An instant, rendered in the session time zone. NULL (false) for infinity. */
static bool timestamptz_to_jalali(TimestampTz ts, JalaliTm *j) {
  struct pg_tm tm;
  fsec_t fsec;
  int tz;
  pg_time_t t;
  int64 local, days, secs;

  if (TIMESTAMP_NOT_FINITE(ts))
    return false;

  t = floor_div(ts, USECS_PER_SEC) + PG_TO_UNIX_SECS;

  if (tzc_zone != session_timezone || t < tzc_lo || t >= tzc_hi) {
    long before_off, after_off;
    int before_dst, after_dst;
    pg_time_t boundary;
    int rc;

    rc = pg_next_dst_boundary(&t, &before_off, &before_dst, &boundary,
                              &after_off, &after_dst, session_timezone);
    if (rc < 0 || rc > 1) {
      /* not expected; fall back to the generic path */
      tzc_zone = NULL;
      if (timestamp2tm(ts, &tz, &tm, &fsec, NULL, session_timezone) != 0)
        ereport(ERROR,
                (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                 errmsg("timestamp out of range")));
      return gregorian_to_jalali(&tm, j);
    }
    tzc_zone = session_timezone;
    tzc_lo = t;
    if (rc == 0) {
      /* no further transition: the "before" offset is valid from here on */
      tzc_off = before_off;
      tzc_hi = PG_INT64_MAX;
    } else {
      tzc_off = before_off;
      tzc_hi = boundary;
    }
  }

  /* timestamp2tm's range check, kept so out-of-range input still errors */
  if (ts < MIN_TIMESTAMP || ts >= END_TIMESTAMP)
    ereport(ERROR,
            (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
             errmsg("timestamp out of range")));

  local = (int64) t + tzc_off;
  days = floor_div(local, SECS_PER_DAY);
  secs = local - days * SECS_PER_DAY;
  memset(&tm, 0, sizeof(tm));
  j2date((int) (days + UNIX_EPOCH_JDATE), &tm.tm_year, &tm.tm_mon, &tm.tm_mday);
  tm.tm_hour = (int) (secs / SECS_PER_HOUR);
  tm.tm_min = (int) ((secs % SECS_PER_HOUR) / SECS_PER_MINUTE);
  tm.tm_sec = (int) (secs % SECS_PER_MINUTE);
  if (!gregorian_to_jalali(&tm, j))
    return false;
  j->usec = (int) (ts - floor_div(ts, USECS_PER_SEC) * USECS_PER_SEC);
  return true;
}

/* A wall-clock time, already in the caller's time zone: no zone conversion. */
static bool timestamp_to_jalali(Timestamp ts, JalaliTm *j) {
  struct pg_tm tm;
  fsec_t fsec;

  if (TIMESTAMP_NOT_FINITE(ts))
    return false;
  if (timestamp2tm(ts, NULL, &tm, &fsec, NULL, NULL) != 0)
    ereport(ERROR,
            (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
             errmsg("timestamp out of range")));
  if (!gregorian_to_jalali(&tm, j))
    return false;
  j->usec = (int) fsec;
  return true;
}

/* A date has no time of day and no time zone: hour, minute and second are 0. */
static bool date_to_jalali(DateADT date, JalaliTm *j) {
  struct pg_tm tm;

  if (DATE_NOT_FINITE(date))
    return false;
  memset(&tm, 0, sizeof(tm));
  j2date(date + POSTGRES_EPOCH_JDATE, &tm.tm_year, &tm.tm_mon, &tm.tm_mday);
  return gregorian_to_jalali(&tm, j);
}

/* Writes `v` (>= 0) zero-padded to at least `width` digits; returns the new end. */
static char *put_int(char *p, int v, int width) {
  char tmp[12];
  int n = 0;
  do {
    tmp[n++] = (char) ('0' + v % 10);
    v /= 10;
  } while (v > 0);
  while (width-- > n)
    *p++ = '0';
  while (n > 0)
    *p++ = tmp[--n];
  return p;
}

/* "Y/MM/DD" or "Y/MM/DD hh:mm:ss"; the year is not padded (same as "%d"). */
static text *format_jalali_text(const JalaliTm *j, bool with_time) {
  text *result = (text *) palloc(VARHDRSZ + 32);
  char *start = VARDATA(result);
  char *p = start;

  p = put_int(p, j->year, 1);
  *p++ = '/';
  p = put_int(p, j->mon, 2);
  *p++ = '/';
  p = put_int(p, j->day, 2);
  if (with_time) {
    *p++ = ' ';
    p = put_int(p, j->hour, 2);
    *p++ = ':';
    p = put_int(p, j->min, 2);
    *p++ = ':';
    p = put_int(p, j->sec, 2);
  }
  SET_VARSIZE(result, VARHDRSZ + (p - start));
  return result;
}

/* Returns the DTK_* field for `units`; raises an error if it isn't supported. */
static int parse_units_uncached(text *units, bool trunc) {
  const char *src = VARDATA_ANY(units);
  int len = VARSIZE_ANY_EXHDR(units);
  char *lowunits;
  int type, val = 0;

  /* ignore surrounding whitespace */
  while (len > 0 && isspace((unsigned char) *src)) {
    src++;
    len--;
  }
  while (len > 0 && isspace((unsigned char) src[len - 1]))
    len--;

  lowunits = downcase_truncate_identifier(src, len, false);

  if (!trunc && strcmp(lowunits, "isoweek") == 0)
    return JU_ISOWEEK;

  type = DecodeUnits(0, lowunits, &val);
  if (type == UNKNOWN_FIELD)
    type = DecodeSpecial(0, lowunits, &val);

  if (type != UNKNOWN_FIELD) {
    switch (val) {
      case DTK_DOW:
      case DTK_ISODOW:
      case DTK_DOY:
      case DTK_ISOYEAR:
        if (trunc)
          break;
        return val;
      case DTK_SECOND:
      case DTK_MINUTE:
      case DTK_HOUR:
      case DTK_DAY:
      case DTK_WEEK:
      case DTK_MONTH:
      case DTK_QUARTER:
      case DTK_YEAR:
      case DTK_DECADE:
      case DTK_CENTURY:
      case DTK_MILLENNIUM:
        return val;
      default:
        break;
    }
  }
  ereport(ERROR,
          (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
           errmsg("%s units \"%s\" not recognized or not supported",
                  trunc ? "jalali_trunc" : "jalali_part", lowunits)));
  return 0;  /* not reached */
}

/*
 * The unit is nearly always a constant, so parse it once per call site and
 * remember the result (and the raw text it came from) in fn_extra.
 */
#define UNIT_CACHE_MAX 32
typedef struct UnitCache {
  int field;
  int len;
  char text[UNIT_CACHE_MAX];
} UnitCache;

static int parse_units(FunctionCallInfo fcinfo, text *units, bool trunc) {
  const char *raw = VARDATA_ANY(units);
  int len = VARSIZE_ANY_EXHDR(units);
  UnitCache *c = (UnitCache *) fcinfo->flinfo->fn_extra;

  if (c != NULL && c->len == len && memcmp(c->text, raw, len) == 0)
    return c->field;

  {
    int field = parse_units_uncached(units, trunc);  /* errors out on bad input */
    if (len <= UNIT_CACHE_MAX) {
      if (c == NULL)
        c = (UnitCache *) MemoryContextAlloc(fcinfo->flinfo->fn_mcxt,
                                             sizeof(UnitCache));
      c->field = field;
      c->len = len;
      memcpy(c->text, raw, len);
      fcinfo->flinfo->fn_extra = c;
    }
    return field;
  }
}


static int jalali_field(int field, const JalaliTm *j) {
  switch (field) {
    case DTK_SECOND:  return j->sec;
    case DTK_MINUTE:  return j->min;
    case DTK_HOUR:    return j->hour;
    case DTK_DAY:     return j->day;
    case DTK_MONTH:   return j->mon;
    case DTK_YEAR:    return j->year;
    case DTK_DOW:     return j->dow;
    case DTK_ISODOW:  return j->isodow;
    case DTK_DOY:     return j->doy;
    case DTK_QUARTER: return (j->mon - 1) / 3 + 1;
    case DTK_DECADE:  return (j->year / 10) * 10;
    case DTK_CENTURY: return (j->year + 99) / 100;
    case DTK_MILLENNIUM: return (j->year + 999) / 1000;
    case DTK_ISOYEAR:
    case JU_ISOWEEK: {
      int wy, w;
      jalali_iso_week(j, &wy, &w);
      return field == DTK_ISOYEAR ? wy : w;
    }
    case DTK_WEEK: {
      /* Saturday-based week; week 1 is the one containing 1 Farvardin */
      int jan1_dow = (((j->dow - (j->doy - 1)) % 7) + 7) % 7;
      return (j->doy - 1 + jan1_dow) / 7 + 1;
    }
  }
  return 0;  /* not reached: parse_units only returns the fields above */
}

Datum format_jalali_timestamp(PG_FUNCTION_ARGS) {
  JalaliTm j;
  if (!timestamptz_to_jalali(PG_GETARG_TIMESTAMPTZ(0), &j))
    PG_RETURN_NULL();
  PG_RETURN_TEXT_P(format_jalali_text(&j, PG_GETARG_BOOL(1)));
}

Datum format_jalali_timestamp_naive(PG_FUNCTION_ARGS) {
  JalaliTm j;
  if (!timestamp_to_jalali(PG_GETARG_TIMESTAMP(0), &j))
    PG_RETURN_NULL();
  PG_RETURN_TEXT_P(format_jalali_text(&j, PG_GETARG_BOOL(1)));
}

Datum format_jalali_date(PG_FUNCTION_ARGS) {
  JalaliTm j;
  if (!date_to_jalali(PG_GETARG_DATEADT(0), &j))
    PG_RETURN_NULL();
  PG_RETURN_TEXT_P(format_jalali_text(&j, false));
}

Datum jalali_part_timestamp(PG_FUNCTION_ARGS) {
  int field = parse_units(fcinfo, PG_GETARG_TEXT_PP(0), false);
  JalaliTm j;
  if (!timestamptz_to_jalali(PG_GETARG_TIMESTAMPTZ(1), &j))
    PG_RETURN_NULL();
  PG_RETURN_INT32(jalali_field(field, &j));
}

Datum jalali_part_timestamp_naive(PG_FUNCTION_ARGS) {
  int field = parse_units(fcinfo, PG_GETARG_TEXT_PP(0), false);
  JalaliTm j;
  if (!timestamp_to_jalali(PG_GETARG_TIMESTAMP(1), &j))
    PG_RETURN_NULL();
  PG_RETURN_INT32(jalali_field(field, &j));
}

Datum jalali_part_date(PG_FUNCTION_ARGS) {
  int field = parse_units(fcinfo, PG_GETARG_TEXT_PP(0), false);
  JalaliTm j;
  if (!date_to_jalali(PG_GETARG_DATEADT(1), &j))
    PG_RETURN_NULL();
  PG_RETURN_INT32(jalali_field(field, &j));
}

/* ---------------------------------------------------------------------
 * Calendar helpers and the reverse (Jalali -> Gregorian) conversion
 * ------------------------------------------------------------------- */

/* leap years of the 33-year cycle: year % 33 is one of these */
static const int j_leap_residue[8] = {1, 5, 9, 13, 17, 22, 26, 30};

static bool jalali_year_is_leap(int64 y) {
  int r = (int) (((y % 33) + 33) % 33), i;
  for (i = 0; i < 8; i++)
    if (j_leap_residue[i] == r)
      return true;
  return false;
}

static int jalali_month_len(int64 y, int m) {
  if (m == 12)
    return jalali_year_is_leap(y) ? 30 : 29;
  return j_days_in_month[m - 1];
}

/* Julian day number of the Jalali date; the caller validates the fields. */
static int64 jalali_to_julian(int64 jy, int jm, int jd) {
  int64 n = jy - 1;
  int64 leaps = (n / 33) * 8;
  int r = (int) (n % 33), i;
  int doy;

  for (i = 0; i < 8; i++)
    if (j_leap_residue[i] <= r)
      leaps++;
  doy = jm <= 6 ? (jm - 1) * 31 + jd : 186 + (jm - 7) * 30 + jd;
  return (int64) date2j(622, 3, 21) + n * 365 + leaps + doy - 1;
}

#define JALALI_MAX_YEAR 5874000

static DateADT julian_to_date(int64 jul) {
  if (jul < DATETIME_MIN_JULIAN || jul >= DATE_END_JULIAN)
    ereport(ERROR,
            (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
             errmsg("date out of range")));
  return (DateADT) (jul - POSTGRES_EPOCH_JDATE);
}

static DateADT jalali_ymd_to_date(int64 y, int m, int d) {
  if (y < 1 || y > JALALI_MAX_YEAR || m < 1 || m > 12 || d < 1 ||
      d > jalali_month_len(y, m))
    ereport(ERROR,
            (errcode(ERRCODE_DATETIME_FIELD_OVERFLOW),
             errmsg("Jalali date field value out of range: %lld-%d-%d",
                    (long long) y, m, d)));
  return julian_to_date(jalali_to_julian(y, m, d));
}

/*
 * ISO-style week for the Saturday-first week: a week (Saturday..Friday)
 * belongs to the Jalali year that contains its 4th day (Tuesday), exactly as
 * ISO 8601 assigns a Monday..Sunday week by its Thursday.
 */
static void jalali_iso_week(const JalaliTm *j, int *wy, int *w) {
  int64 jul = jalali_to_julian(j->year, j->mon, j->day);
  int64 mid = jul - j->dow + 3;
  int64 y = j->year;

  if (mid >= jalali_to_julian(y + 1, 1, 1))
    y++;
  else if (y > 1 && mid < jalali_to_julian(y, 1, 1))
    y--;
  *wy = (int) y;
  *w = (int) ((mid - jalali_to_julian(y, 1, 1)) / 7) + 1;
}

Datum is_jalali_leap(PG_FUNCTION_ARGS) {
  int32 y = PG_GETARG_INT32(0);
  if (y < 1)
    PG_RETURN_NULL();
  PG_RETURN_BOOL(jalali_year_is_leap(y));
}

Datum jalali_days_in_month(PG_FUNCTION_ARGS) {
  int32 y = PG_GETARG_INT32(0), m = PG_GETARG_INT32(1);
  if (m < 1 || m > 12)
    ereport(ERROR,
            (errcode(ERRCODE_DATETIME_FIELD_OVERFLOW),
             errmsg("Jalali month out of range: %d", m)));
  if (y < 1)
    PG_RETURN_NULL();
  PG_RETURN_INT32(jalali_month_len(y, m));
}

Datum make_jalali_date(PG_FUNCTION_ARGS) {
  PG_RETURN_DATEADT(jalali_ymd_to_date(PG_GETARG_INT32(0), PG_GETARG_INT32(1),
                                       PG_GETARG_INT32(2)));
}

/* "Y/M/D" or "Y-M-D", ASCII or Persian/Arabic-Indic digits. false if malformed. */
static bool parse_jalali_text(const unsigned char *s, int n, int64 v[3]) {
  int i = 0, k;

  for (k = 0; k < 3; k++) {
    int digits = 0;
    v[k] = 0;
    while (i < n && isspace(s[i]))
      i++;
    while (i < n) {
      int d = -1, step = 1;
      if (s[i] >= '0' && s[i] <= '9')
        d = s[i] - '0';
      else if (i + 1 < n && s[i] == 0xDB && s[i + 1] >= 0xB0 && s[i + 1] <= 0xB9) {
        d = s[i + 1] - 0xB0;
        step = 2;
      } else if (i + 1 < n && s[i] == 0xD9 && s[i + 1] >= 0xA0 && s[i + 1] <= 0xA9) {
        d = s[i + 1] - 0xA0;
        step = 2;
      }
      if (d < 0)
        break;
      v[k] = v[k] * 10 + d;
      if (++digits > 9)
        return false;
      i += step;
    }
    if (digits == 0)
      return false;
    while (i < n && isspace(s[i]))
      i++;
    if (k < 2) {
      if (i >= n || (s[i] != '/' && s[i] != '-'))
        return false;
      i++;
    }
  }
  return i == n;
}

Datum jalali_to_date(PG_FUNCTION_ARGS) {
  text *t = PG_GETARG_TEXT_PP(0);
  int64 v[3];

  if (!parse_jalali_text((const unsigned char *) VARDATA_ANY(t), VARSIZE_ANY_EXHDR(t), v))
    ereport(ERROR,
            (errcode(ERRCODE_INVALID_DATETIME_FORMAT),
             errmsg("invalid Jalali date \"%s\", expected Y/M/D",
                    text_to_cstring(t))));
  PG_RETURN_DATEADT(jalali_ymd_to_date(v[0], (int) v[1], (int) v[2]));
}

/* ---------------------------------------------------------------------
 * jalali_trunc / period ranges
 * ------------------------------------------------------------------- */

typedef struct JalaliPeriod {
  int64 start_jul, start_tod;  /* tod in microseconds */
  int64 end_jul, end_tod;
} JalaliPeriod;

static bool field_is_subday(int field) {
  return field == DTK_SECOND || field == DTK_MINUTE || field == DTK_HOUR;
}

static int64 month_start_julian(int64 y, int m0 /* 0-based, may overflow */) {
  y += m0 / 12;
  m0 %= 12;
  return jalali_to_julian(y, m0 + 1, 1);
}

static void compute_period(int field, const JalaliTm *j, JalaliPeriod *p) {
  int64 y = j->year, span_months = 0, raw_y = j->year;
  int m0 = j->mon - 1;
  int64 hms = (int64) j->hour * USECS_PER_HOUR + (int64) j->min * USECS_PER_MINUTE +
              (int64) j->sec * USECS_PER_SEC;
  int64 delta = 0;

  p->start_tod = p->end_tod = 0;
  switch (field) {
    case DTK_SECOND: delta = USECS_PER_SEC; break;
    case DTK_MINUTE: hms -= (int64) j->sec * USECS_PER_SEC; delta = USECS_PER_MINUTE; break;
    case DTK_HOUR:
      hms = (int64) j->hour * USECS_PER_HOUR;
      delta = USECS_PER_HOUR;
      break;
    default: break;
  }
  if (field_is_subday(field)) {
    p->start_jul = p->end_jul = jalali_to_julian(j->year, j->mon, j->day);
    p->start_tod = hms;
    p->end_tod = hms + delta;
    return;
  }
  switch (field) {
    case DTK_DAY:
      p->start_jul = jalali_to_julian(j->year, j->mon, j->day);
      p->end_jul = p->start_jul + 1;
      return;
    case DTK_WEEK:
      p->start_jul = jalali_to_julian(j->year, j->mon, j->day) - j->dow;
      p->end_jul = p->start_jul + 7;
      return;
    case DTK_MONTH: span_months = 1; break;
    case DTK_QUARTER: m0 = m0 / 3 * 3; span_months = 3; break;
    case DTK_YEAR: m0 = 0; span_months = 12; break;
    case DTK_DECADE: m0 = 0; raw_y = (y / 10) * 10; span_months = 120; break;
    case DTK_CENTURY: m0 = 0; raw_y = ((y - 1) / 100) * 100 + 1; span_months = 1200; break;
    case DTK_MILLENNIUM: m0 = 0; raw_y = ((y - 1) / 1000) * 1000 + 1; span_months = 12000; break;
  }
  y = raw_y < 1 ? 1 : raw_y;
  p->start_jul = month_start_julian(y, m0);
  p->end_jul = month_start_julian(raw_y, m0 + (int) span_months);
}

static Timestamp period_naive(int64 jul, int64 tod) {
  if (jul < DATETIME_MIN_JULIAN || jul >= TIMESTAMP_END_JULIAN)
    ereport(ERROR,
            (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
             errmsg("timestamp out of range")));
  return (Timestamp) ((jul - POSTGRES_EPOCH_JDATE) * USECS_PER_DAY + tod);
}

static Datum to_tz(Timestamp ts) {
  return DirectFunctionCall1(timestamp_timestamptz, TimestampGetDatum(ts));
}

Datum jalali_trunc_timestamp_naive(PG_FUNCTION_ARGS) {
  int field = parse_units(fcinfo, PG_GETARG_TEXT_PP(0), true);
  Timestamp ts = PG_GETARG_TIMESTAMP(1);
  JalaliTm j;
  JalaliPeriod p;

  if (TIMESTAMP_NOT_FINITE(ts))
    PG_RETURN_TIMESTAMP(ts);
  if (!timestamp_to_jalali(ts, &j))
    PG_RETURN_NULL();
  compute_period(field, &j, &p);
  PG_RETURN_TIMESTAMP(period_naive(p.start_jul, p.start_tod));
}

Datum jalali_trunc_timestamp(PG_FUNCTION_ARGS) {
  int field = parse_units(fcinfo, PG_GETARG_TEXT_PP(0), true);
  TimestampTz ts = PG_GETARG_TIMESTAMPTZ(1);
  JalaliTm j;
  JalaliPeriod p;

  if (TIMESTAMP_NOT_FINITE(ts))
    PG_RETURN_TIMESTAMPTZ(ts);
  if (!timestamptz_to_jalali(ts, &j))
    PG_RETURN_NULL();
  compute_period(field, &j, &p);
  return to_tz(period_naive(p.start_jul, p.start_tod));
}

Datum jalali_trunc_date(PG_FUNCTION_ARGS) {
  int field = parse_units(fcinfo, PG_GETARG_TEXT_PP(0), true);
  DateADT d = PG_GETARG_DATEADT(1);
  JalaliTm j;
  JalaliPeriod p;

  if (DATE_NOT_FINITE(d))
    PG_RETURN_DATEADT(d);
  if (!date_to_jalali(d, &j))
    PG_RETURN_NULL();
  if (field_is_subday(field))
    PG_RETURN_DATEADT(d);
  compute_period(field, &j, &p);
  PG_RETURN_DATEADT(julian_to_date(p.start_jul));
}

/* exclusive end of the period containing the value */
Datum jalali_period_end_timestamp_naive(PG_FUNCTION_ARGS) {
  int field = parse_units(fcinfo, PG_GETARG_TEXT_PP(0), true);
  Timestamp ts = PG_GETARG_TIMESTAMP(1);
  JalaliTm j;
  JalaliPeriod p;

  if (TIMESTAMP_NOT_FINITE(ts))
    PG_RETURN_TIMESTAMP(ts);
  if (!timestamp_to_jalali(ts, &j))
    PG_RETURN_NULL();
  compute_period(field, &j, &p);
  PG_RETURN_TIMESTAMP(period_naive(p.end_jul, p.end_tod));
}

Datum jalali_period_end_timestamp(PG_FUNCTION_ARGS) {
  int field = parse_units(fcinfo, PG_GETARG_TEXT_PP(0), true);
  TimestampTz ts = PG_GETARG_TIMESTAMPTZ(1);
  JalaliTm j;
  JalaliPeriod p;

  if (TIMESTAMP_NOT_FINITE(ts))
    PG_RETURN_TIMESTAMPTZ(ts);
  if (!timestamptz_to_jalali(ts, &j))
    PG_RETURN_NULL();
  compute_period(field, &j, &p);
  if (field_is_subday(field)) {
    /* absolute length, so a DST change inside the period is respected */
    TimestampTz start = DatumGetTimestampTz(to_tz(period_naive(p.start_jul, p.start_tod)));
    PG_RETURN_TIMESTAMPTZ(start + (p.end_tod - p.start_tod));
  }
  return to_tz(period_naive(p.end_jul, p.end_tod));
}

Datum jalali_period_end_date(PG_FUNCTION_ARGS) {
  int field = parse_units(fcinfo, PG_GETARG_TEXT_PP(0), true);
  DateADT d = PG_GETARG_DATEADT(1);
  JalaliTm j;
  JalaliPeriod p;

  if (DATE_NOT_FINITE(d))
    PG_RETURN_DATEADT(d);
  if (!date_to_jalali(d, &j))
    PG_RETURN_NULL();
  if (field_is_subday(field))
    PG_RETURN_DATEADT(julian_to_date((int64) d + POSTGRES_EPOCH_JDATE + 1));
  compute_period(field, &j, &p);
  PG_RETURN_DATEADT(julian_to_date(p.end_jul));
}

/* ---------------------------------------------------------------------
 * to_char_jalali
 * ------------------------------------------------------------------- */

static const char *const month_en[12] = {
  "Farvardin", "Ordibehesht", "Khordad", "Tir", "Mordad", "Shahrivar",
  "Mehr", "Aban", "Azar", "Dey", "Bahman", "Esfand"};
static const char *const month_fa[12] = {
  "\xd9\x81\xd8\xb1\xd9\x88\xd8\xb1\xd8\xaf\xdb\x8c\xd9\x86",
  "\xd8\xa7\xd8\xb1\xd8\xaf\xdb\x8c\xd8\xa8\xd9\x87\xd8\xb4\xd8\xaa",
  "\xd8\xae\xd8\xb1\xd8\xaf\xd8\xa7\xd8\xaf",
  "\xd8\xaa\xdb\x8c\xd8\xb1",
  "\xd9\x85\xd8\xb1\xd8\xaf\xd8\xa7\xd8\xaf",
  "\xd8\xb4\xd9\x87\xd8\xb1\xdb\x8c\xd9\x88\xd8\xb1",
  "\xd9\x85\xd9\x87\xd8\xb1",
  "\xd8\xa2\xd8\xa8\xd8\xa7\xd9\x86",
  "\xd8\xa2\xd8\xb0\xd8\xb1",
  "\xd8\xaf\xdb\x8c",
  "\xd8\xa8\xd9\x87\xd9\x85\xd9\x86",
  "\xd8\xa7\xd8\xb3\xd9\x81\xd9\x86\xd8\xaf"};
/* indexed by JalaliTm.dow: 0 = Saturday */
static const char *const day_en[7] = {
  "Shanbe", "Yekshanbe", "Doshanbe", "Seshanbe", "Chaharshanbe", "Panjshanbe", "Jomeh"};
static const char *const day_fa[7] = {
  "\xd8\xb4\xd9\x86\xd8\xa8\xd9\x87",
  "\xdb\x8c\xda\xa9\xd8\xb4\xd9\x86\xd8\xa8\xd9\x87",
  "\xd8\xaf\xd9\x88\xd8\xb4\xd9\x86\xd8\xa8\xd9\x87",
  "\xd8\xb3\xd9\x87\xe2\x80\x8c\xd8\xb4\xd9\x86\xd8\xa8\xd9\x87",
  "\xda\x86\xd9\x87\xd8\xa7\xd8\xb1\xd8\xb4\xd9\x86\xd8\xa8\xd9\x87",
  "\xd9\xbe\xd9\x86\xd8\xac\xd8\xb4\xd9\x86\xd8\xa8\xd9\x87",
  "\xd8\xac\xd9\x85\xd8\xb9\xd9\x87"};
static const char *const day_short_en[7] = {"Sha", "Yek", "Dos", "Ses", "Cha", "Pan", "Jom"};
static const char *const day_short_fa[7] = {
  "\xd8\xb4", "\xdb\x8c", "\xd8\xaf", "\xd8\xb3", "\xda\x86", "\xd9\xbe", "\xd8\xac"};

enum { T_YYYY, T_YYY, T_YY, T_Y, T_IYYY, T_IYY, T_IY, T_IW, T_ID, T_MONTH, T_MON, T_MM,
       T_DDD, T_DAY, T_DY, T_DD, T_D, T_HH24, T_HH12, T_HH, T_MI, T_MS, T_SSSS, T_SS, T_US,
       T_AM, T_PM, T_WW, T_W, T_Q, T_CC, T_RM, T_TZH, T_TZM, T_TZ, T_OF, T_J };

/* longest spelling first within a shared prefix */
static const struct { const char *s; int id; } tokens[] = {
  {"YYYY", T_YYYY}, {"YYY", T_YYY}, {"YY", T_YY}, {"Y", T_Y},
  {"IYYY", T_IYYY}, {"IYY", T_IYY}, {"IY", T_IY}, {"IW", T_IW}, {"ID", T_ID},
  {"MONTH", T_MONTH}, {"MON", T_MON}, {"MM", T_MM},
  {"DDD", T_DDD}, {"DAY", T_DAY}, {"DY", T_DY}, {"DD", T_DD}, {"D", T_D},
  {"HH24", T_HH24}, {"HH12", T_HH12}, {"HH", T_HH}, {"MI", T_MI}, {"MS", T_MS},
  {"SSSS", T_SSSS}, {"SS", T_SS}, {"US", T_US}, {"AM", T_AM}, {"PM", T_PM},
  {"WW", T_WW}, {"W", T_W}, {"Q", T_Q}, {"CC", T_CC}, {"RM", T_RM},
  {"TZH", T_TZH}, {"TZM", T_TZM}, {"TZ", T_TZ}, {"OF", T_OF}, {"J", T_J}};

static const char *const roman[12] = {"I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX", "X", "XI", "XII"};

static char up(char c) { return (c >= 'a' && c <= 'z') ? (char) (c - 32) : c; }

static void emit_num(StringInfo out, int64 v, int width, bool fm, bool fa) {
  char buf[24];
  int n = 0;
  if (v < 0) {
    appendStringInfoChar(out, '-');
    v = -v;
  }
  do {
    buf[n++] = (char) ('0' + v % 10);
    v /= 10;
  } while (v > 0);
  if (!fm)
    while (width-- > n)
      appendStringInfoString(out, fa ? "\xdb\xb0" : "0");
  while (n > 0) {
    int d = buf[--n] - '0';
    if (fa) {
      appendStringInfoChar(out, (char) 0xDB);
      appendStringInfoChar(out, (char) (0xB0 + d));
    } else
      appendStringInfoChar(out, (char) ('0' + d));
  }
}

/* name with the case implied by the token's spelling: MON / Mon / mon */
static void emit_name(StringInfo out, const char *name, const char *tok, bool latin) {
  bool u0 = tok[0] >= 'A' && tok[0] <= 'Z', u1 = tok[1] >= 'A' && tok[1] <= 'Z';
  for (; *name; name++) {
    char c = *name;
    if (latin) {
      if (u0 && u1)
        c = up(c);
      else if (!u0 && c >= 'A' && c <= 'Z')
        c = (char) (c + 32);
    }
    appendStringInfoChar(out, c);
  }
}

static text *to_char_core(const JalaliTm *j, text *pat, text *lang) {
  const char *p = VARDATA_ANY(pat);
  int n = VARSIZE_ANY_EXHDR(pat), i = 0;
  bool fm = false, fa;
  StringInfoData out;
  text *res;
  char l[8];
  int ll = lang ? VARSIZE_ANY_EXHDR(lang) : 2;

  if (lang) {
    memset(l, 0, sizeof(l));
    memcpy(l, VARDATA_ANY(lang), ll < 7 ? ll : 7);
    for (ll = 0; l[ll]; ll++)
      l[ll] = (char) ((l[ll] >= 'A' && l[ll] <= 'Z') ? l[ll] + 32 : l[ll]);
    if (strcmp(l, "en") == 0)
      fa = false;
    else if (strcmp(l, "fa") == 0)
      fa = true;
    else
      ereport(ERROR,
              (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
               errmsg("to_char_jalali language must be 'en' or 'fa'")));
  } else
    fa = false;
  if (fa && GetDatabaseEncoding() != PG_UTF8)
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("Persian output requires a UTF8 database")));

  initStringInfo(&out);
  while (i < n) {
    int t, tl = 0;
    char c = p[i];

    if (c == '"') {
      i++;
      while (i < n && p[i] != '"') {
        if (p[i] == '\\' && i + 1 < n)
          i++;
        appendStringInfoChar(&out, p[i++]);
      }
      i++;
      continue;
    }
    if (c == '\\' && i + 1 < n) {
      appendStringInfoChar(&out, p[i + 1]);
      i += 2;
      continue;
    }
    if (n - i >= 2 && up(p[i]) == 'F' && up(p[i + 1]) == 'M') {
      fm = true;
      i += 2;
      continue;
    }
    t = -1;
    for (int k = 0; k < (int) lengthof(tokens); k++) {
      int len = (int) strlen(tokens[k].s), m;
      if (len > n - i)
        continue;
      for (m = 0; m < len && up(p[i + m]) == tokens[k].s[m]; m++)
        ;
      if (m == len) {
        t = tokens[k].id;
        tl = len;
        break;
      }
    }
    if (t < 0) {
      appendStringInfoChar(&out, c);
      i++;
      continue;
    }
    switch (t) {
      case T_YYYY: emit_num(&out, j->year, 4, fm, fa); break;
      case T_YYY: emit_num(&out, j->year % 1000, 3, fm, fa); break;
      case T_YY: emit_num(&out, j->year % 100, 2, fm, fa); break;
      case T_Y: emit_num(&out, j->year % 10, 1, fm, fa); break;
      case T_CC: emit_num(&out, (j->year + 99) / 100, 2, fm, fa); break;
      case T_MM: emit_num(&out, j->mon, 2, fm, fa); break;
      case T_MONTH:
      case T_MON: {
        const char *nm = fa ? month_fa[j->mon - 1] : month_en[j->mon - 1];
        char shortn[8];
        if (t == T_MON && !fa) {
          memcpy(shortn, nm, 3);
          shortn[3] = '\0';
          nm = shortn;
        }
        emit_name(&out, nm, p + i, !fa);
        break;
      }
      case T_DD: emit_num(&out, j->day, 2, fm, fa); break;
      case T_DDD: emit_num(&out, j->doy, 3, fm, fa); break;
      case T_D: emit_num(&out, j->dow + 1, 1, fm, fa); break;  /* 1 = Saturday */
      case T_DAY: emit_name(&out, fa ? day_fa[j->dow] : day_en[j->dow], p + i, !fa); break;
      case T_DY: emit_name(&out, fa ? day_short_fa[j->dow] : day_short_en[j->dow], p + i, !fa); break;
      case T_HH24: emit_num(&out, j->hour, 2, fm, fa); break;
      case T_HH:
      case T_HH12: emit_num(&out, j->hour % 12 == 0 ? 12 : j->hour % 12, 2, fm, fa); break;
      case T_MI: emit_num(&out, j->min, 2, fm, fa); break;
      case T_SS: emit_num(&out, j->sec, 2, fm, fa); break;
      case T_MS: emit_num(&out, j->usec / 1000, 3, fm, fa); break;
      case T_US: emit_num(&out, j->usec, 6, fm, fa); break;
      case T_AM:
      case T_PM: {
        bool pm = j->hour >= 12;
        if (fa)
          appendStringInfoString(&out, pm ? "\xd8\xa8.\xd8\xb8." : "\xd9\x82.\xd8\xb8.");
        else if (p[i] >= 'a' && p[i] <= 'z')
          appendStringInfoString(&out, pm ? "pm" : "am");
        else
          appendStringInfoString(&out, pm ? "PM" : "AM");
        break;
      }
      case T_W: emit_num(&out, (j->day - 1) / 7 + 1, 1, fm, fa); break;
      case T_WW: emit_num(&out, (j->doy - 1) / 7 + 1, 2, fm, fa); break;
      case T_Q: emit_num(&out, (j->mon - 1) / 3 + 1, 1, fm, fa); break;
      case T_IYYY: case T_IYY: case T_IY: case T_IW: {
        int wy, w;
        jalali_iso_week(j, &wy, &w);
        if (t == T_IW)
          emit_num(&out, w, 2, fm, fa);
        else if (t == T_IYYY)
          emit_num(&out, wy, 4, fm, fa);
        else if (t == T_IYY)
          emit_num(&out, wy % 1000, 3, fm, fa);
        else
          emit_num(&out, wy % 100, 2, fm, fa);
        break;
      }
      case T_ID: emit_num(&out, j->isodow, 1, fm, fa); break;
      case T_SSSS: emit_num(&out, j->hour * 3600 + j->min * 60 + j->sec, 5, fm, fa); break;
      case T_J:
        emit_num(&out, date2j(j->gy, j->gm, j->gd), 7, fm, fa);
        break;
      case T_RM: {
        const char *r = roman[j->mon - 1];
        char buf[8];
        int k;
        for (k = 0; r[k]; k++)
          buf[k] = (p[i] >= 'a' && p[i] <= 'z') ? (char) (r[k] + 32) : r[k];
        buf[k] = '\0';
        if (!fm)
          for (int pad = 4 - k; pad > 0; pad--)
            appendStringInfoChar(&out, ' ');
        appendStringInfoString(&out, buf);
        break;
      }
      case T_TZ:
      case T_TZH:
      case T_TZM:
      case T_OF: {
        int off = j->gmtoff, a = off < 0 ? -off : off;
        if (!j->has_tz) {
          if (t == T_TZ || t == T_OF)
            break;  /* no zone on a date or a timestamp without time zone */
        }
        if (t == T_TZ) {
          const char *z = j->tzn;
          for (; *z; z++)
            appendStringInfoChar(&out, (p[i] >= 'a' && p[i] <= 'z' && *z >= 'A' && *z <= 'Z') ? (char) (*z + 32) : *z);
        } else if (t == T_TZH) {
          appendStringInfoChar(&out, off < 0 ? '-' : '+');
          emit_num(&out, a / 3600, 2, false, false);
        } else if (t == T_TZM)
          emit_num(&out, (a % 3600) / 60, 2, false, false);
        else {
          appendStringInfoChar(&out, off < 0 ? '-' : '+');
          emit_num(&out, a / 3600, 2, false, false);
          if (a % 3600 != 0) {
            appendStringInfoChar(&out, ':');
            emit_num(&out, (a % 3600) / 60, 2, false, false);
          }
        }
        break;
      }
    }
    fm = false;
    i += tl;
    /* ordinal suffix after a numeric field: TH / th */
    if (n - i >= 2 && (p[i] == 'T' || p[i] == 't') && (p[i + 1] == 'H' || p[i + 1] == 'h') &&
        t != T_MONTH && t != T_MON && t != T_DAY && t != T_DY && t != T_AM && t != T_PM &&
        t != T_RM && t != T_TZ && t != T_TZH && t != T_TZM && t != T_OF) {
      if (fa)
        appendStringInfoString(&out, "\xd8\xa7\xd9\x85");
      else {
        int lastd = (t == T_YYYY ? j->year : t == T_DD ? j->day : t == T_MM ? j->mon : t == T_HH24 ? j->hour :
                     t == T_DDD ? j->doy : t == T_MI ? j->min : t == T_SS ? j->sec : t == T_D ? j->dow + 1 :
                     t == T_Q ? (j->mon - 1) / 3 + 1 : t == T_ID ? j->isodow : 0);
        int last2 = lastd % 100, l1 = lastd % 10;
        const char *sfx = (last2 >= 11 && last2 <= 13) ? "th" : l1 == 1 ? "st" : l1 == 2 ? "nd" : l1 == 3 ? "rd" : "th";
        if (p[i] == 'T')
          appendStringInfoChar(&out, up(sfx[0])), appendStringInfoChar(&out, up(sfx[1]));
        else
          appendStringInfoString(&out, sfx);
      }
      i += 2;
    }
  }
  res = cstring_to_text_with_len(out.data, out.len);
  pfree(out.data);
  return res;
}

Datum to_char_jalali_timestamp(PG_FUNCTION_ARGS) {
  JalaliTm j;
  TimestampTz ts = PG_GETARG_TIMESTAMPTZ(0);
  if (!timestamptz_to_jalali(ts, &j))
    PG_RETURN_NULL();
  {
    struct pg_tm tm;
    fsec_t fsec;
    int tz;
    const char *tzn = NULL;
    if (timestamp2tm(ts, &tz, &tm, &fsec, &tzn, session_timezone) == 0) {
      j.has_tz = true;
      j.gmtoff = -tz;
      if (tzn)
        strlcpy(j.tzn, tzn, sizeof(j.tzn));
    }
  }
  PG_RETURN_TEXT_P(to_char_core(&j, PG_GETARG_TEXT_PP(1), PG_GETARG_TEXT_PP(2)));
}

Datum to_char_jalali_timestamp_naive(PG_FUNCTION_ARGS) {
  JalaliTm j;
  if (!timestamp_to_jalali(PG_GETARG_TIMESTAMP(0), &j))
    PG_RETURN_NULL();
  PG_RETURN_TEXT_P(to_char_core(&j, PG_GETARG_TEXT_PP(1), PG_GETARG_TEXT_PP(2)));
}

Datum to_char_jalali_date(PG_FUNCTION_ARGS) {
  JalaliTm j;
  if (!date_to_jalali(PG_GETARG_DATEADT(0), &j))
    PG_RETURN_NULL();
  PG_RETURN_TEXT_P(to_char_core(&j, PG_GETARG_TEXT_PP(1), PG_GETARG_TEXT_PP(2)));
}


/* ---------------------------------------------------------------------
 * jalali_add_months / jalali_age
 * ------------------------------------------------------------------- */

static int64 tod_us(const JalaliTm *j) {
  return (int64) j->hour * USECS_PER_HOUR + (int64) j->min * USECS_PER_MINUTE +
         (int64) j->sec * USECS_PER_SEC + j->usec;
}

/* Julian day of `j` moved by n Jalali months; the day is clamped to the month length. */
static int64 add_months_julian(const JalaliTm *j, int32 n) {
  int64 total = (int64) j->year * 12 + (j->mon - 1) + n;
  int64 ny = floor_div(total, 12);
  int nm = (int) (total - ny * 12) + 1, nd;

  if (ny < 1 || ny > JALALI_MAX_YEAR)
    ereport(ERROR,
            (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
             errmsg("Jalali date out of range")));
  nd = j->day;
  if (nd > jalali_month_len(ny, nm))
    nd = jalali_month_len(ny, nm);
  return jalali_to_julian(ny, nm, nd);
}

Datum jalali_add_months_date(PG_FUNCTION_ARGS) {
  DateADT d = PG_GETARG_DATEADT(0);
  JalaliTm j;

  if (DATE_NOT_FINITE(d))
    PG_RETURN_DATEADT(d);
  if (!date_to_jalali(d, &j))
    PG_RETURN_NULL();
  PG_RETURN_DATEADT(julian_to_date(add_months_julian(&j, PG_GETARG_INT32(1))));
}

Datum jalali_add_months_timestamp_naive(PG_FUNCTION_ARGS) {
  Timestamp ts = PG_GETARG_TIMESTAMP(0);
  JalaliTm j;

  if (TIMESTAMP_NOT_FINITE(ts))
    PG_RETURN_TIMESTAMP(ts);
  if (!timestamp_to_jalali(ts, &j))
    PG_RETURN_NULL();
  PG_RETURN_TIMESTAMP(period_naive(add_months_julian(&j, PG_GETARG_INT32(1)), tod_us(&j)));
}

Datum jalali_add_months_timestamp(PG_FUNCTION_ARGS) {
  TimestampTz ts = PG_GETARG_TIMESTAMPTZ(0);
  JalaliTm j;

  if (TIMESTAMP_NOT_FINITE(ts))
    PG_RETURN_TIMESTAMPTZ(ts);
  if (!timestamptz_to_jalali(ts, &j))
    PG_RETURN_NULL();
  return to_tz(period_naive(add_months_julian(&j, PG_GETARG_INT32(1)), tod_us(&j)));
}

/*
 * Difference a - b as years/months/days/time in the Jalali calendar, with the same
 * borrowing rule as PostgreSQL's age(): days borrow the length of the earlier date's month.
 */
static Interval *jalali_age_core(const JalaliTm *a, const JalaliTm *b) {
  const JalaliTm *hi = a, *lo = b;
  int sign = 1;
  int64 dt;
  int dd, dm, dy;
  Interval *r;

  if (a->year < b->year || (a->year == b->year &&
      (a->mon < b->mon || (a->mon == b->mon &&
      (a->day < b->day || (a->day == b->day && tod_us(a) < tod_us(b))))))) {
    hi = b;
    lo = a;
    sign = -1;
  }
  dt = tod_us(hi) - tod_us(lo);
  dd = hi->day - lo->day;
  dm = hi->mon - lo->mon;
  dy = hi->year - lo->year;
  if (dt < 0) {
    dt += USECS_PER_DAY;
    dd--;
  }
  if (dd < 0) {
    dd += jalali_month_len(lo->year, lo->mon);
    dm--;
  }
  if (dm < 0) {
    dm += 12;
    dy--;
  }
  r = (Interval *) palloc(sizeof(Interval));
  r->month = sign * (dy * 12 + dm);
  r->day = sign * dd;
  r->time = sign * dt;
  return r;
}

Datum jalali_age_date(PG_FUNCTION_ARGS) {
  DateADT x = PG_GETARG_DATEADT(0), y = PG_GETARG_DATEADT(1);
  JalaliTm a, b;

  if (DATE_NOT_FINITE(x) || DATE_NOT_FINITE(y) || !date_to_jalali(x, &a) || !date_to_jalali(y, &b))
    PG_RETURN_NULL();
  PG_RETURN_INTERVAL_P(jalali_age_core(&a, &b));
}

Datum jalali_age_timestamp_naive(PG_FUNCTION_ARGS) {
  JalaliTm a, b;

  if (!timestamp_to_jalali(PG_GETARG_TIMESTAMP(0), &a) || !timestamp_to_jalali(PG_GETARG_TIMESTAMP(1), &b))
    PG_RETURN_NULL();
  PG_RETURN_INTERVAL_P(jalali_age_core(&a, &b));
}

Datum jalali_age_timestamp(PG_FUNCTION_ARGS) {
  JalaliTm a, b;

  if (!timestamptz_to_jalali(PG_GETARG_TIMESTAMPTZ(0), &a) ||
      !timestamptz_to_jalali(PG_GETARG_TIMESTAMPTZ(1), &b))
    PG_RETURN_NULL();
  PG_RETURN_INTERVAL_P(jalali_age_core(&a, &b));
}

/* ---------------------------------------------------------------------
 * jalali_to_date(text, pattern) / jalali_to_timestamp(text, pattern)
 * ------------------------------------------------------------------- */

typedef struct ParsedDate {
  int64 year;
  int mon, day, doy, hour, min, sec, us;
  bool have_year, have_doy, have_h12, pm, have_ampm;
} ParsedDate;

static int digit_at(const unsigned char *s, int n, int i, int *step) {
  *step = 1;
  if (i >= n)
    return -1;
  if (s[i] >= '0' && s[i] <= '9')
    return s[i] - '0';
  if (i + 1 < n && s[i] == 0xDB && s[i + 1] >= 0xB0 && s[i + 1] <= 0xB9) {
    *step = 2;
    return s[i + 1] - 0xB0;
  }
  if (i + 1 < n && s[i] == 0xD9 && s[i + 1] >= 0xA0 && s[i + 1] <= 0xA9) {
    *step = 2;
    return s[i + 1] - 0xA0;
  }
  return -1;
}

static void bad_input(const char *what, text *in) {
  ereport(ERROR,
          (errcode(ERRCODE_INVALID_DATETIME_FORMAT),
           errmsg("invalid value for %s in Jalali date/time \"%s\"", what, text_to_cstring(in))));
}

static void parse_by_pattern(text *input, text *pat, ParsedDate *pd) {
  const unsigned char *s = (const unsigned char *) VARDATA_ANY(input);
  int sn = VARSIZE_ANY_EXHDR(input), si = 0;
  const char *p = VARDATA_ANY(pat);
  int pn = VARSIZE_ANY_EXHDR(pat), pi = 0;

  memset(pd, 0, sizeof(*pd));
  pd->year = 1;
  pd->mon = 1;
  pd->day = 1;

  while (pi < pn) {
    int t = -1, tl = 0, k, maxd = 0;
    char c = p[pi];

    if (c == '"') {
      pi++;
      while (pi < pn && p[pi] != '"') {
        if (p[pi] == '\\' && pi + 1 < pn)
          pi++;
        if (si < sn)
          si++;
        pi++;
      }
      pi++;
      continue;
    }
    if (c == '\\' && pi + 1 < pn) {
      if (si < sn)
        si++;
      pi += 2;
      continue;
    }
    if (pn - pi >= 2 && up(p[pi]) == 'F' && up(p[pi + 1]) == 'M') {
      pi += 2;
      continue;
    }
    for (k = 0; k < (int) lengthof(tokens); k++) {
      int len = (int) strlen(tokens[k].s), m;
      if (len > pn - pi)
        continue;
      for (m = 0; m < len && up(p[pi + m]) == tokens[k].s[m]; m++)
        ;
      if (m == len) {
        t = tokens[k].id;
        tl = len;
        break;
      }
    }
    if (t < 0) {
      /* a separator in the pattern consumes one non-alphanumeric input character */
      if (si < sn && !isalnum(s[si]) && digit_at(s, sn, si, &k) < 0)
        si++;
      pi++;
      continue;
    }
    pi += tl;

    switch (t) {
      case T_YYYY: maxd = 4; break;
      case T_YY: maxd = 2; break;
      case T_MM: case T_DD: case T_HH: case T_HH12: case T_HH24: case T_MI: case T_SS: maxd = 2; break;
      case T_DDD: maxd = 3; break;
      case T_MS: maxd = 3; break;
      case T_US: maxd = 6; break;
      default: break;
    }
    if (maxd > 0) {
      int64 v = 0;
      int nd = 0, step;
      while (si < sn && isspace(s[si]))
        si++;
      while (nd < maxd) {
        int d = digit_at(s, sn, si, &step);
        if (d < 0)
          break;
        v = v * 10 + d;
        si += step;
        nd++;
      }
      if (nd == 0)
        bad_input("a numeric field", input);
      switch (t) {
        case T_YYYY: pd->year = v; pd->have_year = true; break;
        case T_YY: pd->year = (v < 70 ? 1400 : 1300) + v; pd->have_year = true; break;
        case T_MM: pd->mon = (int) v; break;
        case T_DD: pd->day = (int) v; break;
        case T_DDD: pd->doy = (int) v; pd->have_doy = true; break;
        case T_HH24: pd->hour = (int) v; break;
        case T_HH: case T_HH12:
          if (v < 1 || v > 12)
            bad_input("HH12", input);
          pd->hour = (int) v % 12;
          pd->have_h12 = true;
          break;
        case T_MI: pd->min = (int) v; break;
        case T_SS: pd->sec = (int) v; break;
        case T_MS: while (nd++ < 3) v *= 10; pd->us += (int) v * 1000; break;
        case T_US: while (nd++ < 6) v *= 10; pd->us += (int) v; break;
      }
      continue;
    }
    if (t == T_AM || t == T_PM) {
      while (si < sn && isspace(s[si]))
        si++;
      if (si + 1 < sn && (s[si] == 'a' || s[si] == 'A' || s[si] == 'p' || s[si] == 'P') &&
          (s[si + 1] == 'm' || s[si + 1] == 'M')) {
        pd->pm = (s[si] == 'p' || s[si] == 'P');
        si += 2;
      } else if (sn - si >= 7 && memcmp(s + si, "\xd9\x82.\xd8\xb8.", 6) == 0) {
        pd->pm = false;
        si += 6;
      } else if (sn - si >= 7 && memcmp(s + si, "\xd8\xa8.\xd8\xb8.", 6) == 0) {
        pd->pm = true;
        si += 6;
      } else
        bad_input("AM/PM", input);
      pd->have_ampm = true;
      continue;
    }
    if (t == T_MONTH || t == T_MON) {
      int found = 0, flen = 0, m;
      for (m = 0; m < 12 && !found; m++) {
        const char *cand[2] = {month_en[m], month_fa[m]};
        int c2;
        for (c2 = 0; c2 < 2; c2++) {
          int len = (int) strlen(cand[c2]), q, minlen = (t == T_MON && c2 == 0) ? 3 : len;
          if (sn - si < minlen)
            continue;
          /* compare case-insensitively up to the full name; MON accepts the first 3 Latin letters */
          for (q = 0; q < len && si + q < sn; q++)
            if (up((char) s[si + q]) != up(cand[c2][q]))
              break;
          if (q >= minlen && (q == len || (t == T_MON && c2 == 0 && q == 3))) {
            /* prefer the longest match: Mehr vs. Mehr..., Tir is not a prefix of another */
            found = m + 1;
            flen = (t == T_MON && c2 == 0) ? (q == len ? len : 3) : len;
            break;
          }
        }
      }
      if (!found)
        bad_input("a month name", input);
      pd->mon = found;
      si += flen;
      continue;
    }
    if (t == T_DAY || t == T_DY) {
      /* names are accepted and ignored, as the date determines the weekday */
      while (si < sn && (isalpha(s[si]) || s[si] >= 0x80))
        si++;
      continue;
    }
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("pattern field not supported when parsing Jalali dates")));
  }
  while (si < sn && isspace(s[si]))
    si++;
  if (si < sn)
    ereport(ERROR,
            (errcode(ERRCODE_INVALID_DATETIME_FORMAT),
             errmsg("trailing characters remain in Jalali input string \"%s\"", text_to_cstring(input))));
  if (pd->have_h12 && pd->have_ampm && pd->pm)
    pd->hour += 12;
  else if (pd->have_h12 && !pd->have_ampm)
    pd->hour = pd->hour == 0 ? 12 : pd->hour;
}

static int64 parsed_to_julian(const ParsedDate *pd) {
  if (pd->have_doy) {
    int len = jalali_year_is_leap(pd->year) ? 366 : 365;
    if (pd->year < 1 || pd->year > JALALI_MAX_YEAR || pd->doy < 1 || pd->doy > len)
      ereport(ERROR,
              (errcode(ERRCODE_DATETIME_FIELD_OVERFLOW),
               errmsg("Jalali day of year out of range: %d", pd->doy)));
    return jalali_to_julian(pd->year, 1, 1) + pd->doy - 1;
  }
  return jalali_to_julian(pd->year, pd->mon, pd->day) ;
}

static void check_parsed(const ParsedDate *pd) {
  if (pd->year < 1 || pd->year > JALALI_MAX_YEAR || pd->mon < 1 || pd->mon > 12 || pd->day < 1 ||
      (!pd->have_doy && pd->day > jalali_month_len(pd->year, pd->mon)) ||
      pd->hour > 24 || pd->min > 59 || pd->sec > 59 || (pd->hour == 24 && (pd->min || pd->sec || pd->us)))
    ereport(ERROR,
            (errcode(ERRCODE_DATETIME_FIELD_OVERFLOW),
             errmsg("Jalali date/time field value out of range")));
}

Datum jalali_to_date_fmt(PG_FUNCTION_ARGS) {
  ParsedDate pd;
  parse_by_pattern(PG_GETARG_TEXT_PP(0), PG_GETARG_TEXT_PP(1), &pd);
  check_parsed(&pd);
  PG_RETURN_DATEADT(julian_to_date(parsed_to_julian(&pd)));
}

Datum jalali_to_timestamp_fmt(PG_FUNCTION_ARGS) {
  ParsedDate pd;
  int64 tod;
  parse_by_pattern(PG_GETARG_TEXT_PP(0), PG_GETARG_TEXT_PP(1), &pd);
  check_parsed(&pd);
  tod = (int64) pd.hour * USECS_PER_HOUR + (int64) pd.min * USECS_PER_MINUTE +
        (int64) pd.sec * USECS_PER_SEC + pd.us;
  PG_RETURN_TIMESTAMP(period_naive(parsed_to_julian(&pd), tod));
}

/* ---------------------------------------------------------------------
 * jalali_bin(n, unit, x): n-unit bins aligned to the Jalali calendar
 * ------------------------------------------------------------------- */

static void compute_bin(FunctionCallInfo fcinfo, int n, int field, const JalaliTm *j,
                        int64 *start_jul, int64 *start_tod) {
  int64 e = jalali_to_julian(1, 1, 1), jul = jalali_to_julian(j->year, j->mon, j->day);

  if (n < 1)
    ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("jalali_bin stride must be positive")));
  *start_tod = 0;
  switch (field) {
    case DTK_SECOND: case DTK_MINUTE: case DTK_HOUR: {
      int64 unit = field == DTK_SECOND ? USECS_PER_SEC : field == DTK_MINUTE ? USECS_PER_MINUTE : USECS_PER_HOUR;
      int64 t = tod_us(j), w = unit * n;
      *start_jul = jul;
      *start_tod = t - t % w;
      return;
    }
    case DTK_DAY:
      *start_jul = e + ((jul - e) / n) * n;
      return;
    case DTK_WEEK: {
      int64 ref = e - ((e + 2) % 7), ws = jul - j->dow;
      *start_jul = ref + ((ws - ref) / (7 * (int64) n)) * 7 * n;
      return;
    }
    case DTK_MONTH: case DTK_QUARTER: {
      int64 w = (field == DTK_QUARTER ? 3 : 1) * (int64) n;
      int64 mi = ((int64) j->year - 1) * 12 + (j->mon - 1);
      mi -= mi % w;
      *start_jul = jalali_to_julian(mi / 12 + 1, (int) (mi % 12) + 1, 1);
      return;
    }
    case DTK_YEAR: {
      int64 y = (int64) j->year - 1;
      y -= y % n;
      *start_jul = jalali_to_julian(y + 1, 1, 1);
      return;
    }
    default:
      ereport(ERROR,
              (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
               errmsg("jalali_bin units must be second, minute, hour, day, week, month, quarter or year")));
  }
}

Datum jalali_bin_date(PG_FUNCTION_ARGS) {
  int n = PG_GETARG_INT32(0);
  int field = parse_units(fcinfo, PG_GETARG_TEXT_PP(1), true);
  DateADT d = PG_GETARG_DATEADT(2);
  JalaliTm j;
  int64 sj, st;

  if (DATE_NOT_FINITE(d))
    PG_RETURN_DATEADT(d);
  if (!date_to_jalali(d, &j))
    PG_RETURN_NULL();
  if (field == DTK_SECOND || field == DTK_MINUTE || field == DTK_HOUR) {
    compute_bin(fcinfo, n, field, &j, &sj, &st);
    PG_RETURN_DATEADT(d);
  }
  compute_bin(fcinfo, n, field, &j, &sj, &st);
  PG_RETURN_DATEADT(julian_to_date(sj));
}

Datum jalali_bin_timestamp_naive(PG_FUNCTION_ARGS) {
  int n = PG_GETARG_INT32(0);
  int field = parse_units(fcinfo, PG_GETARG_TEXT_PP(1), true);
  Timestamp ts = PG_GETARG_TIMESTAMP(2);
  JalaliTm j;
  int64 sj, st;

  if (TIMESTAMP_NOT_FINITE(ts))
    PG_RETURN_TIMESTAMP(ts);
  if (!timestamp_to_jalali(ts, &j))
    PG_RETURN_NULL();
  compute_bin(fcinfo, n, field, &j, &sj, &st);
  PG_RETURN_TIMESTAMP(period_naive(sj, st));
}

Datum jalali_bin_timestamp(PG_FUNCTION_ARGS) {
  int n = PG_GETARG_INT32(0);
  int field = parse_units(fcinfo, PG_GETARG_TEXT_PP(1), true);
  TimestampTz ts = PG_GETARG_TIMESTAMPTZ(2);
  JalaliTm j;
  int64 sj, st;

  if (TIMESTAMP_NOT_FINITE(ts))
    PG_RETURN_TIMESTAMPTZ(ts);
  if (!timestamptz_to_jalali(ts, &j))
    PG_RETURN_NULL();
  compute_bin(fcinfo, n, field, &j, &sj, &st);
  return to_tz(period_naive(sj, st));
}

/* ---------------------------------------------------------------------
 * in_jalali_period(col, unit, ref) and its index support function
 * ------------------------------------------------------------------- */

Datum in_jalali_period_date(PG_FUNCTION_ARGS) {
  DateADT c = PG_GETARG_DATEADT(0), r = PG_GETARG_DATEADT(2);
  int field = parse_units(fcinfo, PG_GETARG_TEXT_PP(1), true);
  JalaliTm j;
  JalaliPeriod p;
  int64 cj;

  if (DATE_NOT_FINITE(c) || DATE_NOT_FINITE(r) || !date_to_jalali(r, &j))
    PG_RETURN_BOOL(false);
  if (field_is_subday(field))
    PG_RETURN_BOOL(c == r);
  compute_period(field, &j, &p);
  cj = (int64) c + POSTGRES_EPOCH_JDATE;
  PG_RETURN_BOOL(cj >= p.start_jul && cj < p.end_jul);
}

Datum in_jalali_period_timestamp_naive(PG_FUNCTION_ARGS) {
  Timestamp c = PG_GETARG_TIMESTAMP(0), r = PG_GETARG_TIMESTAMP(2);
  int field = parse_units(fcinfo, PG_GETARG_TEXT_PP(1), true);
  JalaliTm j;
  JalaliPeriod p;

  if (TIMESTAMP_NOT_FINITE(c) || TIMESTAMP_NOT_FINITE(r) || !timestamp_to_jalali(r, &j))
    PG_RETURN_BOOL(false);
  compute_period(field, &j, &p);
  PG_RETURN_BOOL(c >= period_naive(p.start_jul, p.start_tod) && c < period_naive(p.end_jul, p.end_tod));
}

Datum in_jalali_period_timestamp(PG_FUNCTION_ARGS) {
  TimestampTz c = PG_GETARG_TIMESTAMPTZ(0), r = PG_GETARG_TIMESTAMPTZ(2);
  int field = parse_units(fcinfo, PG_GETARG_TEXT_PP(1), true);
  JalaliTm j;
  JalaliPeriod p;
  TimestampTz lo, hi;

  if (TIMESTAMP_NOT_FINITE(c) || TIMESTAMP_NOT_FINITE(r) || !timestamptz_to_jalali(r, &j))
    PG_RETURN_BOOL(false);
  compute_period(field, &j, &p);
  lo = DatumGetTimestampTz(to_tz(period_naive(p.start_jul, p.start_tod)));
  if (field_is_subday(field))
    hi = lo + (p.end_tod - p.start_tod);
  else
    hi = DatumGetTimestampTz(to_tz(period_naive(p.end_jul, p.end_tod)));
  PG_RETURN_BOOL(c >= lo && c < hi);
}

static Oid find_fn(Oid self, const char *name, Oid coltype) {
  Oid args[2] = {TEXTOID, coltype};
  List *qual = list_make2(makeString(get_namespace_name(get_func_namespace(self))), makeString(pstrdup(name)));
  return LookupFuncName(qual, 2, args, true);
}

/*
 * col IN period  ->  col >= jalali_trunc(unit, ref) AND col < jalali_period_end(unit, ref)
 * so a plain btree index on col serves the filter.
 */
Datum jalali_period_support(PG_FUNCTION_ARGS) {
  Node *rawreq = (Node *) PG_GETARG_POINTER(0);
  Node *ret = NULL;

  if (IsA(rawreq, SupportRequestIndexCondition)) {
    SupportRequestIndexCondition *req = (SupportRequestIndexCondition *) rawreq;
    FuncExpr *fe = (FuncExpr *) req->node;
    Node *col, *unit, *ref;
    Oid ctype, ge, lt, f_trunc, f_end;

    if (req->indexarg != 0 || !IsA(fe, FuncExpr) || list_length(fe->args) != 3 ||
        req->index->relam != BTREE_AM_OID)
      PG_RETURN_POINTER(NULL);
    col = linitial(fe->args);
    unit = lsecond(fe->args);
    ref = lthird(fe->args);
    if (contain_volatile_functions(ref) || contain_volatile_functions(unit) ||
        (IsA(ref, Const) && ((Const *) ref)->constisnull))
      PG_RETURN_POINTER(NULL);
    ctype = exprType(col);
    if (exprType(ref) != ctype)
      PG_RETURN_POINTER(NULL);
    ge = get_opfamily_member(req->opfamily, ctype, ctype, BTGreaterEqualStrategyNumber);
    lt = get_opfamily_member(req->opfamily, ctype, ctype, BTLessStrategyNumber);
    f_trunc = find_fn(fe->funcid, "jalali_trunc", ctype);
    f_end = find_fn(fe->funcid, "jalali_period_end", ctype);
    if (!OidIsValid(ge) || !OidIsValid(lt) || !OidIsValid(f_trunc) || !OidIsValid(f_end))
      PG_RETURN_POINTER(NULL);
    {
      Expr *lo = make_opclause(ge, BOOLOID, false, (Expr *) col,
                               (Expr *) makeFuncExpr(f_trunc, ctype, list_make2(unit, ref),
                                                     InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL),
                               InvalidOid, req->indexcollation);
      Expr *hi = make_opclause(lt, BOOLOID, false, (Expr *) col,
                               (Expr *) makeFuncExpr(f_end, ctype, list_make2(unit, ref),
                                                     InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL),
                               InvalidOid, req->indexcollation);
      req->lossy = false;
      ret = (Node *) list_make2(lo, hi);
    }
  }
  PG_RETURN_POINTER(ret);
}

/* ---------------------------------------------------------------------
 * jalali_date: a date that reads and prints as Y/MM/DD in the Jalali calendar
 * ------------------------------------------------------------------- */

Datum jalali_date_in(PG_FUNCTION_ARGS) {
  const char *str = PG_GETARG_CSTRING(0);
  int n = (int) strlen(str);
  int64 v[3];
  char *lc = downcase_truncate_identifier(str, n, false);

  if (strcmp(lc, "infinity") == 0)
    PG_RETURN_DATEADT(DATEVAL_NOEND);
  if (strcmp(lc, "-infinity") == 0)
    PG_RETURN_DATEADT(DATEVAL_NOBEGIN);
  if (!parse_jalali_text((const unsigned char *) str, n, v))
    ereport(ERROR,
            (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
             errmsg("invalid input syntax for type jalali_date: \"%s\"", str)));
  PG_RETURN_DATEADT(jalali_ymd_to_date(v[0], (int) v[1], (int) v[2]));
}

Datum jalali_date_out(PG_FUNCTION_ARGS) {
  DateADT d = PG_GETARG_DATEADT(0);
  JalaliTm j;
  char *buf;

  if (DATE_IS_NOBEGIN(d))
    PG_RETURN_CSTRING(pstrdup("-infinity"));
  if (DATE_IS_NOEND(d))
    PG_RETURN_CSTRING(pstrdup("infinity"));
  buf = palloc(32);
  if (!date_to_jalali(d, &j)) {
    int y, m, dd;
    j2date(d + POSTGRES_EPOCH_JDATE, &y, &m, &dd);
    snprintf(buf, 32, "G:%04d-%02d-%02d", y, m, dd);  /* before 1/1/1: no Jalali form */
  } else
    snprintf(buf, 32, "%d/%02d/%02d", j.year, j.mon, j.day);
  PG_RETURN_CSTRING(buf);
}
