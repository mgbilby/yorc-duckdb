-- task:        coauthors
-- description: Co-author pairs from the contributors of full works and
--              fundings: two contributor iDs on the same item make a pair.
--              Full works are in the Public Data File's activities archives
--              (summaries list no contributors) and in /work and bulk API
--              responses.
-- requires:    orcid
-- params:      task_coauthor_pairs(path)  -> TABLE(orcid_a, orcid_b, items)   with orcid_a < orcid_b
--              task_coauthor_counts(path) -> TABLE(orcid, coauthors, items)
-- usage:       .read examples/tasks/coauthors.sql
--              SELECT * FROM task_coauthor_pairs('ORCID_2025_10_activities_0.tar.gz') LIMIT 20;
--
-- Items are keyed by (source_hash, owner, owner_put_code): the document, the
-- owning element (work or funding) and its put-code, which also works for
-- documents that carry no iD of their own (a bulk response).

CREATE OR REPLACE MACRO task_coauthor_pairs(path) AS TABLE
    WITH c AS (
        SELECT DISTINCT source_hash, owner, owner_put_code, contributor_orcid
        FROM orcid_contributors(path)
        WHERE contributor_orcid IS NOT NULL)
    SELECT a.contributor_orcid AS orcid_a,
           b.contributor_orcid AS orcid_b,
           count(*)            AS items
    FROM c a
    JOIN c b
      ON a.source_hash = b.source_hash AND a.owner = b.owner
     AND a.owner_put_code = b.owner_put_code
     AND a.contributor_orcid < b.contributor_orcid
    GROUP BY ALL
    ORDER BY items DESC, orcid_a, orcid_b;

-- Per iD: how many distinct co-authors, over how many pair occurrences.
CREATE OR REPLACE MACRO task_coauthor_counts(path) AS TABLE
    WITH p AS (SELECT * FROM task_coauthor_pairs(path)),
    both_sides AS (
        SELECT orcid_a AS orcid, orcid_b AS other, items FROM p
        UNION ALL
        SELECT orcid_b, orcid_a, items FROM p)
    SELECT orcid, count(DISTINCT other) AS coauthors, sum(items) AS items
    FROM both_sides
    GROUP BY orcid
    ORDER BY coauthors DESC, orcid;
