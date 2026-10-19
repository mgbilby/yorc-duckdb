-- task:        doi-dedupe
-- description: DOI duplicate report. Within a record: DOIs listed by more than
--              one work, with their work groups (repeats inside one group are
--              ORCID's own grouping of the same work from several sources;
--              repeats across groups are the ones to review). Across records:
--              DOIs claimed by more than one iD.
-- requires:    orcid
-- params:      task_doi_duplicates(path)  -> TABLE(orcid, doi, works, groups, put_codes)
--              task_doi_cross_group(path) -> TABLE(the rows of task_doi_duplicates with groups > 1)
--              task_doi_coclaims(path)    -> TABLE(doi, records, orcids)
-- usage:       .read examples/tasks/doi-dedupe.sql
--              SELECT * FROM task_doi_cross_group('records/*.xml');
--              SELECT * FROM task_doi_coclaims('ORCID_2025_10_summaries.tar.gz');
--
-- The DOI is orcid_works.doi: the first item-level external id of type doi
-- with relationship self, ASCII-lowercased (docs/REFERENCE.md).

CREATE OR REPLACE MACRO task_doi_duplicates(path) AS TABLE
    SELECT orcid, doi,
           count(*)                          AS works,
           count(DISTINCT group_index)       AS groups,
           list(put_code ORDER BY put_code)  AS put_codes
    FROM orcid_works(path)
    WHERE doi IS NOT NULL
    GROUP BY orcid, doi
    HAVING count(*) > 1
    ORDER BY works DESC, orcid, doi;

CREATE OR REPLACE MACRO task_doi_cross_group(path) AS TABLE
    SELECT * FROM task_doi_duplicates(path)
    WHERE groups > 1;

CREATE OR REPLACE MACRO task_doi_coclaims(path) AS TABLE
    SELECT doi,
           count(DISTINCT orcid)                       AS records,
           list_sort(list(DISTINCT orcid))             AS orcids
    FROM orcid_works(path)
    WHERE doi IS NOT NULL
    GROUP BY doi
    HAVING count(DISTINCT orcid) > 1
    ORDER BY records DESC, doi;
