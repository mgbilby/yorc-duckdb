# Migrating from ad-hoc ORCID pipelines

A map from the usual ways of processing ORCID XML to orcid SQL. Pipelines that
work with the ORCID Public Data File, or with saved API responses, tend to
follow the same few steps: unpack the archive, convert or parse each document,
pick fields with XPath, flatten into tables, and diff releases by hand. Each
step has a direct SQL equivalent here.

This page describes other tools only as far as their public documentation
does. Where we are not sure how a tool behaves, it says so; check the tool's
own documentation before relying on a row. orcid is an independent
implementation written from ORCID's published message schema (see
[`audit/PROVENANCE.md`](../audit/PROVENANCE.md)) and shares no code with any
tool named here.

## The mental shift

A typical pipeline turns the archive into files, the files into objects, and
the objects into rows, with a script holding state in between. orcid makes
the archive itself a table: each **section reader** streams the documents and
emits rows, and SQL does the rest. A whole extraction becomes one statement
per output table:

```sql
COPY (SELECT orcid, put_code, title, type, pub_year, doi
      FROM orcid_works('ORCID_2025_10_summaries.tar.gz'))
TO 'works.parquet' (FORMAT parquet);
```

Two things change:
- **Nothing is unpacked.** Archives are read member by member, and
  `max_records` / `members` select what is parsed.
- **Columns are fixed and documented.** Every column has a defined XML source
  ([REFERENCE.md](REFERENCE.md)) and its schema coverage is listed
  ([COVERAGE.md](COVERAGE.md)). A field the pipeline used to pull with its own
  XPath either has a column, or is listed there as not mapped.

## Step map

| pipeline step | typical tools | orcid equivalent | notes |
|---|---|---|---|
| Download a release | a browser, `curl` / `wget` from figshare | the same, or no download: `orcid_<section>('https://…')` with `httpfs` | URLs, sizes and MD5s: `reference/orcid-dumps/index.csv` |
| Unpack `ORCID_<year>_summaries.tar.gz` | `tar -xzf` (tens of millions of files on disk) | read the archive in place | `members := '*/097/*'` reads one iD prefix |
| Parse each document | `lxml`, `xml.etree`, `xmltodict` (Python); JAXB with the ORCID model classes (Java) | the section readers | one pass per section; or `orcid_documents` once into a table, then the `_xml` functions |
| Convert XML to JSON first | ORCID's `orcid-conversion-lib`, as its README describes (we have not run it) | not needed | read either format directly. To convert a document, read it and write it back with the other writer (RECIPES.md 19); for generic JSON exports of the tables, `COPY ... (FORMAT json)` |
| Pick fields with XPath | `//work:work-summary/work:title/common:title`, ... | `orcid_works.title` and friends | `put_code`, `group_index`, `display_index`, `path`, the source and assertion origin are columns too |
| Find the record's iD | parse the file name, or `/record/@path` | `orcid` on every row | root `path`, else `orcid-identifier/path`, else the item's `path` |
| Skip non-records | check for `error:error` documents | `orcid_errors` | locked (9018) and deprecated (9007) records, with `primary_orcid` ([LOCKED_DEPRECATED.md](LOCKED_DEPRECATED.md)) |
| Flatten contributors and external ids | nested loops over lists | `orcid_contributors`, `orcid_external_ids` | joined back on `(source_hash, owner_put_code)` ([RECIPES.md](RECIPES.md) 5.1) |
| Detect changed records between releases | hash files, or compare on the API's last-modified dates | `source_hash` per row; `orcid_changes_xml` for the fields | [RECIPES.md](RECIPES.md) 10.2, `examples/tasks/release-changes.sql` |
| Validate iDs | a regex plus a hand-written check digit | `orcid_valid`, `orcid_normalize` | ISO 7064 MOD 11-2 |
| Load into a database | per-row inserts from the script | `CREATE TABLE ... AS SELECT`, `COPY ... TO` Parquet / CSV | parallel on DuckDB's threads |
| Stored XML in a database | re-parse in the application | `ATTACH` it and use the `_xml` functions | for a one-row table, use the constant-argument form (RECIPES.md 14.3) |

## Worked examples

**"Every employment with its organisation and dates."** XPath over every
`employment:employment-summary`, then four or five fields each:

```sql
SELECT orcid, org_name, org_country, role_title, start_year, end_year
FROM orcid_affiliations('ORCID_2025_10_summaries.tar.gz', max_records := 10000)
WHERE affiliation_type = 'employment';
```

**"Works with a DOI, one row per DOI."** A loop over `common:external-id`
with a type and relationship test:

```sql
SELECT orcid, put_code, value AS doi
FROM orcid_external_ids('ORCID_2025_10_activities_0.tar.gz', max_records := 10000)
WHERE type = 'doi' AND relationship = 'self' AND owner = 'work';
```

**"What changed since last year."** Unpack both releases, match files by
name, diff the parsed objects:

```sql
.read examples/tasks/release-changes.sql
SELECT * FROM task_release_change_summary(
    'ORCID_2024_10_summaries.tar.gz', 'ORCID_2025_10_summaries.tar.gz', '*/097/*');
```

## What does not carry over

- **API calls.** orcid reads documents; it does not fetch them. Code that
  calls the ORCID API (rate limits included) stays
  where it is, and hands orcid the responses, saved to files or held in a
  table.
- **JSON input.** The API also serves JSON, and orcid reads it into the same
  tables as the XML (the format is told per file; `_json` twins for columns).
  The Public Data File itself is XML only.
- **Writes.** orcid writes ORCID documents (`orcid_work_to_json`,
  `orcid_bulk_works_to_xml`, ...) and never sends anything. It does not
  provide access to the ORCID Sandbox or the ORCID Member API.
- **Elements without a column.** A handful of schema names carry no column
  (`preferences/locale`, the `transient` flag, ...). [COVERAGE.md](COVERAGE.md)
  lists them, together with the places where a mapped name is not read in
  every context. If a pipeline depends on one of these, keep that step, or ask
  for a column.
