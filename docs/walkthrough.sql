-- docs/walkthrough.sql: every cell of docs/WALKTHROUGH.md, in order.
--
-- Run from the package root:
--     duckdb < docs/walkthrough.sql
-- or, inside duckdb started there:
--     .read docs/walkthrough.sql
--
-- Each cell prints "Cell N. <title>" before its output; compare it with the
-- "Expected output" blocks of docs/WALKTHROUGH.md. Cell 9 makes a network
-- request, so it is not run here: it only prints where to find it.
-- Cell 5 writes walkthrough_works.csv to the current directory.
--
-- Keep in step with docs/WALKTHROUGH.md: each cell below is the SQL block
-- marked <!-- cell N --> there, verbatim. test/sql/orcid_walkthrough.test
-- checks it.

-- ---------------------------------------------------------------- cell 0
.print
.print "Cell 0. Setup: load the extension, the version check and the paths"
.mode box
.pager off
LOAD yorc;
SELECT version() AS duckdb, extension_name, loaded
FROM duckdb_extensions()
WHERE extension_name = 'yorc';
SELECT filename, size
FROM read_blob(['test/data/demo/0000-0002-2771-9344.xml',
                'reference/orcid-model/record_3.0/samples/read_samples/record-3.0.xml',
                'reference/orcid-model/record_3.0/samples/read_samples/work-full-3.0.xml',
                'reference/orcid-model/record_3.0/samples/write_samples/work-full-3.0.xml',
                'reference/orcid-model/record_3.0/samples/write_samples/work-full-3.0.json',
                'test/data/dump/orcid_test_summaries.tar.gz'])
ORDER BY filename;

-- ---------------------------------------------------------------- cell 1
.print
.print "Cell 1. Check and normalise ORCID iDs"
SELECT input, orcid_valid(input) AS valid, orcid_normalize(input) AS normalized,
       'https://orcid.org/' || orcid_normalize(input) AS uri
FROM (VALUES (1, '0000-0002-1825-0097'),
             (2, '0000-0002-1825-0098'),
             (3, 'https://orcid.org/000000021694233x'),
             (4, ' 0000 0002 2771 9344 ')) AS t(n, input)
ORDER BY n;

-- ---------------------------------------------------------------- cell 2
.print
.print "Cell 2. Read a saved ORCID record and summarise it"
SELECT p.orcid, p.given_names, p.family_name, p.credit_name,
       (SELECT count(*) FROM orcid_works('test/data/demo/0000-0002-2771-9344.xml'))        AS works,
       (SELECT count(*) FROM orcid_fundings('test/data/demo/0000-0002-2771-9344.xml'))     AS fundings,
       (SELECT count(*) FROM orcid_affiliations('test/data/demo/0000-0002-2771-9344.xml')) AS affiliations,
       (SELECT count(*) FROM orcid_peer_reviews('test/data/demo/0000-0002-2771-9344.xml')) AS peer_reviews
FROM orcid_person('test/data/demo/0000-0002-2771-9344.xml') p;

-- ---------------------------------------------------------------- cell 3
.print
.print "Cell 3. List the record's works with year, type and DOI, newest first, top 10"
SELECT put_code, pub_year, type, doi, title
FROM orcid_works('test/data/demo/0000-0002-2771-9344.xml')
ORDER BY pub_year DESC NULLS LAST, pub_month DESC NULLS LAST, pub_day DESC NULLS LAST, put_code
LIMIT 10;

-- ---------------------------------------------------------------- cell 4
.print
.print "Cell 4. Read ORCID JSON, and convert a document XML→JSON→XML"
.print "4a. ORCID JSON: the write sample work-full-3.0.json"
SELECT title, type, make_date(pub_year, pub_month, pub_day) AS published,
       (SELECT count(*) FROM orcid_external_ids('reference/orcid-model/record_3.0/samples/write_samples/work-full-3.0.json')) AS external_ids
FROM orcid_works('reference/orcid-model/record_3.0/samples/write_samples/work-full-3.0.json');
SELECT credit_name, contributor_orcid
FROM orcid_contributors('reference/orcid-model/record_3.0/samples/write_samples/work-full-3.0.json')
ORDER BY ordinal;
.print "4b. work-full-3.0.xml to JSON and back to XML, in crosswalk mode"
CREATE OR REPLACE TEMP TABLE rec_xml AS
SELECT struct_insert(w,
         external_ids := (SELECT list(e) FROM orcid_external_ids('reference/orcid-model/record_3.0/samples/write_samples/work-full-3.0.xml') e),
         contributors := (SELECT list(c ORDER BY c.ordinal) FROM orcid_contributors('reference/orcid-model/record_3.0/samples/write_samples/work-full-3.0.xml') c)) AS rec
FROM orcid_works('reference/orcid-model/record_3.0/samples/write_samples/work-full-3.0.xml') w;
CREATE OR REPLACE TEMP TABLE as_json AS
SELECT orcid_work_to_json(rec, true) AS doc FROM rec_xml;
CREATE OR REPLACE TEMP TABLE rec_json AS
SELECT struct_insert(w,
         external_ids := (SELECT list(e) FROM orcid_external_ids_json((SELECT doc FROM as_json)) e),
         contributors := (SELECT list(c ORDER BY c.ordinal) FROM orcid_contributors_json((SELECT doc FROM as_json)) c)) AS rec
FROM orcid_works_json((SELECT doc FROM as_json)) w;
SELECT array_to_string(regexp_extract_all(doc, '(?m)^  "([a-z-]+)":', 1), ', ') AS json_keys
FROM as_json;
SELECT (SELECT count(*)
        FROM (SELECT * EXCLUDE (source_hash) FROM orcid_works('reference/orcid-model/record_3.0/samples/write_samples/work-full-3.0.xml')
              EXCEPT
              SELECT * EXCLUDE (source_hash) FROM orcid_works_json((SELECT doc FROM as_json)))) = 0 AS same_rows,
       (SELECT orcid_work_to_xml(rec, true) FROM rec_json) = (SELECT orcid_work_to_xml(rec, true) FROM rec_xml) AS same_xml;

-- ---------------------------------------------------------------- cell 5
.print
.print "Cell 5. Flatten the record into section tables and export one as CSV"
CREATE OR REPLACE TABLE works                   AS SELECT * FROM orcid_works('test/data/demo/0000-0002-2771-9344.xml');
CREATE OR REPLACE TABLE fundings                AS SELECT * FROM orcid_fundings('test/data/demo/0000-0002-2771-9344.xml');
CREATE OR REPLACE TABLE affiliations            AS SELECT * FROM orcid_affiliations('test/data/demo/0000-0002-2771-9344.xml');
CREATE OR REPLACE TABLE peer_reviews            AS SELECT * FROM orcid_peer_reviews('test/data/demo/0000-0002-2771-9344.xml');
CREATE OR REPLACE TABLE research_resources      AS SELECT * FROM orcid_research_resources('test/data/demo/0000-0002-2771-9344.xml');
CREATE OR REPLACE TABLE research_resource_items AS SELECT * FROM orcid_research_resource_items('test/data/demo/0000-0002-2771-9344.xml');
CREATE OR REPLACE TABLE contributors            AS SELECT * FROM orcid_contributors('test/data/demo/0000-0002-2771-9344.xml');
CREATE OR REPLACE TABLE external_ids            AS SELECT * FROM orcid_external_ids('test/data/demo/0000-0002-2771-9344.xml');
CREATE OR REPLACE TABLE person                  AS SELECT * FROM orcid_person('test/data/demo/0000-0002-2771-9344.xml');
CREATE OR REPLACE TABLE person_items            AS SELECT * FROM orcid_person_items('test/data/demo/0000-0002-2771-9344.xml');
CREATE OR REPLACE TABLE history                 AS SELECT * FROM orcid_history('test/data/demo/0000-0002-2771-9344.xml');
CREATE OR REPLACE TABLE search_results          AS SELECT * FROM orcid_search_results('test/data/demo/0000-0002-2771-9344.xml');
CREATE OR REPLACE TABLE errors                  AS SELECT * FROM orcid_errors('test/data/demo/0000-0002-2771-9344.xml');
CREATE OR REPLACE TABLE research_resource_hosts AS SELECT * FROM orcid_research_resource_hosts('test/data/demo/0000-0002-2771-9344.xml');
WITH cols AS (SELECT table_name, count(*) AS column_count
              FROM duckdb_columns()
              WHERE schema_name = 'main' AND database_name = current_database()
              GROUP BY table_name)
SELECT r.section_table, r.row_count, cols.column_count
FROM (SELECT 1 AS n, 'works' AS section_table, count(*) AS row_count FROM works
      UNION ALL SELECT 2, 'fundings', count(*) FROM fundings
      UNION ALL SELECT 3, 'affiliations', count(*) FROM affiliations
      UNION ALL SELECT 4, 'peer_reviews', count(*) FROM peer_reviews
      UNION ALL SELECT 5, 'research_resources', count(*) FROM research_resources
      UNION ALL SELECT 6, 'research_resource_items', count(*) FROM research_resource_items
      UNION ALL SELECT 7, 'contributors', count(*) FROM contributors
      UNION ALL SELECT 8, 'external_ids', count(*) FROM external_ids
      UNION ALL SELECT 9, 'person', count(*) FROM person
      UNION ALL SELECT 10, 'person_items', count(*) FROM person_items
      UNION ALL SELECT 11, 'history', count(*) FROM history
      UNION ALL SELECT 12, 'search_results', count(*) FROM search_results
      UNION ALL SELECT 13, 'errors', count(*) FROM errors
      UNION ALL SELECT 14, 'research_resource_hosts', count(*) FROM research_resource_hosts) r
JOIN cols ON cols.table_name = r.section_table
ORDER BY r.n;
COPY works TO 'walkthrough_works.csv' (HEADER, DELIMITER ',');
.mode list
.headers off
SELECT array_to_string(string_split(content, chr(10))[1:3], chr(10)) FROM read_text('walkthrough_works.csv');
.headers on
.mode box

-- ---------------------------------------------------------------- cell 6
.print
.print "Cell 6. Contributors and co-authors of the works"
SELECT credit_name, contributor_orcid, sequence, role
FROM (SELECT 'read' AS sample, *
      FROM orcid_contributors('reference/orcid-model/record_3.0/samples/read_samples/work-full-3.0.xml')
      UNION ALL
      SELECT 'write', *
      FROM orcid_contributors('reference/orcid-model/record_3.0/samples/write_samples/work-full-3.0.xml'))
ORDER BY sample, ordinal;
SELECT count(DISTINCT contributor_orcid) AS coauthors
FROM (SELECT * FROM orcid_contributors('reference/orcid-model/record_3.0/samples/read_samples/work-full-3.0.xml')
      UNION ALL
      SELECT * FROM orcid_contributors('reference/orcid-model/record_3.0/samples/write_samples/work-full-3.0.xml'));

-- ---------------------------------------------------------------- cell 7
.print
.print "Cell 7. An affiliation timeline"
SELECT start_year, end_year, affiliation_type AS type, org_name,
       org_disambiguation_source AS id_source, org_disambiguated_id AS org_id
FROM orcid_affiliations('reference/orcid-model/record_3.0/samples/read_samples/record-3.0.xml')
WHERE affiliation_type IN ('employment', 'education')
ORDER BY start_year NULLS LAST, start_month NULLS LAST, start_day NULLS LAST, affiliation_type, put_code;

-- ---------------------------------------------------------------- cell 8
.print
.print "Cell 8. Stream the Public Data File dump fixture"
.print "8a. The documents of the dump, in archive order"
SELECT name, orcid, regexp_extract(xml, '<([A-Za-z-]+:[A-Za-z-]+)', 1) AS root, strlen(xml) AS bytes
FROM orcid_documents('test/data/dump/orcid_test_summaries.tar.gz');
.print "8b. The counts, and the iDs of the records"
SELECT count(*) AS xml_members,
       count(*) FILTER (WHERE regexp_extract(xml, '<([A-Za-z-]+:[A-Za-z-]+)', 1) = 'record:record') AS records,
       count(orcid) AS named_by_valid_id
FROM orcid_documents('test/data/dump/orcid_test_summaries.tar.gz');
SELECT string_agg(orcid, ', ' ORDER BY orcid) AS record_ids
FROM orcid_person('test/data/dump/orcid_test_summaries.tar.gz');
.print "8c. A seeded sample of the records: the same picks as yorc-rs and yorc-zig"
SELECT 42 AS seed, orcid, eligible FROM orcid_dump_sample('test/data/dump/orcid_test_summaries.tar.gz', 1, 42)
UNION ALL
SELECT 7, orcid, eligible FROM orcid_dump_sample('test/data/dump/orcid_test_summaries.tar.gz', 1, 7)
ORDER BY seed DESC;

-- ---------------------------------------------------------------- cell 9
.print
.print "Cell 9. Getting one public record from ORCID"
.print "Manual, makes one request to orcid.org: not run by this file."
.print "See docs/WALKTHROUGH.md, cell 9, for the curl command and the query."
-- After saving the record with curl (one request):
-- SELECT orcid, given_names, family_name FROM orcid_person('downloads/0000-0002-2771-9344.xml');
-- SELECT count(*) AS works FROM orcid_works('downloads/0000-0002-2771-9344.xml');
