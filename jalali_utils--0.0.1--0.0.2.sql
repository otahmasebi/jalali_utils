-- Overloads for a wall-clock `timestamp` (no time zone): the value is read as it is,
-- with no time zone conversion. Use them when the caller has already converted the
-- value to the user's time zone (e.g. `col AT TIME ZONE 'UTC' AT TIME ZONE tz`);
-- the `timestamptz` versions always render the instant in Asia/Tehran.

CREATE OR REPLACE FUNCTION format_jalali(
    timestamp WITHOUT TIME ZONE,
    with_time BOOLEAN DEFAULT TRUE
)
RETURNS TEXT AS 'MODULE_PATHNAME','format_jalali_timestamp_naive'
LANGUAGE C STRICT;

CREATE OR REPLACE FUNCTION jalali_part(
    text,
    timestamp WITHOUT TIME ZONE
)
RETURNS INTEGER AS 'MODULE_PATHNAME','jalali_part_timestamp_naive'
LANGUAGE C STRICT;
