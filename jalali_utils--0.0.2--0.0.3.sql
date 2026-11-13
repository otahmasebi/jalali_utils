-- Volatility and parallel safety. The functions are pure, so the default
-- VOLATILE / PARALLEL UNSAFE only hurt: no expression indexes, no parallel plans,
-- and the planner cannot fold or reorder calls.
--
-- date and timestamp (no time zone) overloads depend only on their arguments.
-- timestamptz overloads also read the session TimeZone, so they are STABLE.

ALTER FUNCTION format_jalali(date) IMMUTABLE PARALLEL SAFE;
ALTER FUNCTION format_jalali(timestamp WITHOUT TIME ZONE, boolean) IMMUTABLE PARALLEL SAFE;
ALTER FUNCTION jalali_part(text, date) IMMUTABLE PARALLEL SAFE;
ALTER FUNCTION jalali_part(text, timestamp WITHOUT TIME ZONE) IMMUTABLE PARALLEL SAFE;

ALTER FUNCTION format_jalali(timestamp WITH TIME ZONE, boolean) STABLE PARALLEL SAFE;
ALTER FUNCTION jalali_part(text, timestamp WITH TIME ZONE) STABLE PARALLEL SAFE;
