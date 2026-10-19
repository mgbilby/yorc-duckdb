-- task:        affiliation-timeline
-- description: Affiliations as a timeline: every affiliation of the seven
--              types in start order with its span and an open-ended flag, and
--              the current ones (no end year; distinctions are point events and
--              never current).
-- requires:    orcid
-- params:      task_affiliation_timeline(path) -> TABLE(orcid, affiliation_type, org_name, role_title, department, start_year, end_year, years, is_current)
--              task_affiliation_current(path)  -> TABLE(the current rows of the timeline)
-- usage:       .read examples/tasks/affiliation-timeline.sql
--              SELECT * FROM task_affiliation_timeline('records/0000-0002-1825-0097.xml');
--              SELECT affiliation_type, count(*) FROM task_affiliation_current('records/*.xml') GROUP BY ALL;

CREATE OR REPLACE MACRO task_affiliation_timeline(path) AS TABLE
    SELECT orcid, affiliation_type, org_name, role_title, department,
           start_year, end_year,
           end_year - start_year AS years,
           end_year IS NULL AND affiliation_type <> 'distinction' AS is_current
    FROM orcid_affiliations(path)
    ORDER BY orcid, start_year NULLS LAST, end_year NULLS LAST, affiliation_type, put_code;

CREATE OR REPLACE MACRO task_affiliation_current(path) AS TABLE
    SELECT * FROM task_affiliation_timeline(path)
    WHERE is_current;
