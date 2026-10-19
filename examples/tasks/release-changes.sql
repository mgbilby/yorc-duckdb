-- task:        release-changes
-- description: What changed between two Public Data File releases (or any
--              two sets of documents whose paths name the iD): the records
--              are paired by iD with orcid_documents and compared with
--              orcid_changes_xml. A record in only one release shows all its
--              items as added or removed.
-- requires:    orcid
-- params:      task_release_changes(old_path, new_path, members)        -> TABLE(orcid_changes columns)
--              task_release_change_summary(old_path, new_path, members) -> TABLE(section, change, items, records)
-- usage:       .read examples/tasks/release-changes.sql
--              SELECT * FROM task_release_change_summary(
--                  'ORCID_2024_10_summaries.tar.gz', 'ORCID_2025_10_summaries.tar.gz', '*/097/*');
--
-- members is the member glob applied to both archives: '*.xml' for all of them,
-- '*/097/*' for one iD prefix (about 1/1000 of a full release per pass).

CREATE OR REPLACE MACRO task_release_changes(old_path, new_path, members) AS TABLE
    WITH pairs AS (
        SELECT orcid, a.xml AS old_xml, b.xml AS new_xml
        FROM orcid_documents(old_path, members := members) a
        FULL JOIN orcid_documents(new_path, members := members) b USING (orcid))
    SELECT c.*
    FROM pairs p, orcid_changes_xml(p.old_xml, p.new_xml) c;

CREATE OR REPLACE MACRO task_release_change_summary(old_path, new_path, members) AS TABLE
    SELECT section, change, count(*) AS items, count(DISTINCT orcid) AS records
    FROM task_release_changes(old_path, new_path, members)
    GROUP BY ALL
    ORDER BY section, change;
