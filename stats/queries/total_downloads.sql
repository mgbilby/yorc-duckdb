-- Total INSTALL-event downloads for yorc-duckdb, summed over all published weekly
-- stat files. yorc-duckdb first appeared in ISO week 43 of 2026 (week of 2026-10-19).
-- The + INTERVAL 1 DAY ensures the in-progress current week's file is included.
SELECT
    sum(yorc)::BIGINT AS total_downloads
FROM read_json([
    printf(
        'https://community-extensions.duckdb.org/download-stats-weekly/%s/%s.json',
        week.strftime('%Y'),
        week.strftime('%V').regexp_replace('^0', '')
    )
    FOR week
    IN range(
        TIMESTAMP '2026-10-19',
        now()::TIMESTAMP + INTERVAL 1 DAY,
        INTERVAL 1 WEEK
    )
    IF strftime(week, '%V') != '53'
], union_by_name := true, columns = { 'yorc': 'BIGINT' });