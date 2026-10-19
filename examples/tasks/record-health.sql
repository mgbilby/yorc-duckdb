-- task:        record-health
-- description: Per-record health checks over saved records or a dump: works
--              without a DOI or a publication year, DOIs repeated across works
--              (and across work groups, which ORCID would normally merge),
--              affiliations and fundings without a disambiguated organisation,
--              and current employments. Reads only; reports, never edits.
-- requires:    orcid
-- params:      task_health_summary(path) -> TABLE(one row per iD: counts per check)
--              task_health_works(path)   -> TABLE(one row per work with at least one issue)
-- usage:       .read examples/tasks/record-health.sql
--              SELECT * FROM task_health_summary('records/*.xml');
--              SELECT * FROM task_health_works('records/0000-0002-1825-0097.xml');
--
-- Each macro reads `path` once per section it uses. For a whole dump, load the
-- sections into tables first (docs/RECIPES.md, recipe 13) and adapt the
-- FROM clauses.

-- One row per iD. A record with no works still gets a row when it has
-- affiliations or fundings (and the other way round).
CREATE OR REPLACE MACRO task_health_summary(path) AS TABLE
    WITH w AS (
        SELECT orcid, put_code, doi, pub_year, group_index FROM orcid_works(path)),
    d AS (
        SELECT orcid, doi, count(*) AS works, count(DISTINCT group_index) AS groups
        FROM w WHERE doi IS NOT NULL GROUP BY orcid, doi),
    ws AS (
        SELECT orcid,
               count(*)                            AS works,
               count(*) FILTER (WHERE doi IS NULL)  AS works_without_doi,
               count(*) FILTER (WHERE pub_year IS NULL) AS works_without_year
        FROM w GROUP BY orcid),
    ds AS (
        SELECT orcid,
               count(*) FILTER (WHERE works > 1)  AS duplicate_dois,
               count(*) FILTER (WHERE groups > 1) AS dois_in_several_groups
        FROM d GROUP BY orcid),
    a AS (
        SELECT orcid,
               count(*) AS affiliations,
               count(*) FILTER (WHERE org_disambiguated_id IS NULL) AS affiliations_without_org_id,
               count(*) FILTER (WHERE affiliation_type = 'employment' AND end_year IS NULL) AS current_employments
        FROM orcid_affiliations(path) GROUP BY orcid),
    f AS (
        SELECT orcid,
               count(*) AS fundings,
               count(*) FILTER (WHERE org_disambiguated_id IS NULL) AS fundings_without_funder_id
        FROM orcid_fundings(path) GROUP BY orcid),
    ids AS (
        SELECT orcid FROM ws UNION SELECT orcid FROM a UNION SELECT orcid FROM f)
    SELECT ids.orcid,
           coalesce(ws.works, 0)                       AS works,
           coalesce(ws.works_without_doi, 0)           AS works_without_doi,
           coalesce(ws.works_without_year, 0)          AS works_without_year,
           coalesce(ds.duplicate_dois, 0)              AS duplicate_dois,
           coalesce(ds.dois_in_several_groups, 0)      AS dois_in_several_groups,
           coalesce(a.affiliations, 0)                 AS affiliations,
           coalesce(a.affiliations_without_org_id, 0)  AS affiliations_without_org_id,
           coalesce(a.current_employments, 0)          AS current_employments,
           coalesce(f.fundings, 0)                     AS fundings,
           coalesce(f.fundings_without_funder_id, 0)   AS fundings_without_funder_id
    FROM ids
    LEFT JOIN ws USING (orcid)
    LEFT JOIN ds USING (orcid)
    LEFT JOIN a USING (orcid)
    LEFT JOIN f USING (orcid)
    ORDER BY ids.orcid;

-- One row per work with at least one issue, the issues as a list:
-- 'no DOI', 'no publication year', 'DOI shared with another work'.
CREATE OR REPLACE MACRO task_health_works(path) AS TABLE
    WITH w AS (
        SELECT orcid, put_code, group_index, title, doi, pub_year FROM orcid_works(path)),
    shared AS (
        SELECT orcid, doi FROM w WHERE doi IS NOT NULL GROUP BY orcid, doi HAVING count(*) > 1),
    flagged AS (
        SELECT w.orcid, w.put_code, w.group_index, w.title, w.doi,
               list_filter([CASE WHEN w.doi IS NULL THEN 'no DOI' END,
                            CASE WHEN w.pub_year IS NULL THEN 'no publication year' END,
                            CASE WHEN s.doi IS NOT NULL THEN 'DOI shared with another work' END],
                           lambda x: x IS NOT NULL) AS issues
        FROM w LEFT JOIN shared s ON s.orcid = w.orcid AND s.doi = w.doi)
    SELECT * FROM flagged
    WHERE len(issues) > 0
    ORDER BY orcid, group_index, put_code;
