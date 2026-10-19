# Niche and composition in the DuckDB ecosystem

orcid occupies one niche: **the ORCID v3.0 record model as SQL**. It turns the
XML and JSON that ORCID publishes (API responses, saved records, the yearly Public Data
File) into typed tables, and typed rows back into ORCID documents, and it knows the schema's semantics: which element is
an item, which iD a document belongs to, how work groups and owners nest, what
a locked or deprecated record looks like, and how two snapshots of a record
differ. Everything that is not that is left to DuckDB itself or to sibling
extensions: transport, other file formats, search, fuzzy matching, graph
analysis, other registries. The extension is designed to compose with them in
the same session. Before adding a feature here, check this page: if a core or
community extension already covers the generic half, the right change is a
recipe or a task bundle that composes, not a reimplementation.

## What orcid owns

- **A streaming reader for ORCID v3.0 XML and JSON**, written from the published
  message schema (`reference/orcid-model/`). It covers records, section
  collections, single items, bulk responses, and the summary, search,
  expanded-search, error, notification and group-id messages. JSON is mapped
  onto the same model, so both formats give the same rows.
- **Writers for ORCID v3.0 XML and JSON**: ORCID documents from rows (works,
  bulk works, fundings, affiliations, peer reviews, research resources,
  person items, group ids), as scalars. Nothing is sent to ORCID: the
  extension does not provide access to the ORCID Member API.
- **The section model**: 19 sections with stable column orders (new columns are appended). Each comes as
  a file reader and `_xml` / `_json` twins. Every row carries its record iD and the
  SHA-256 of its source document ([REFERENCE.md](REFERENCE.md)).
- **ORCID iD rules**: the ISO 7064 MOD 11-2 check and the canonical form
  (`orcid_valid`, `orcid_normalize`).
- **The Public Data File as it is published**: `.tar` / `.tar.gz` archives
  streamed member by member, the iD-prefix layout (`members`), early stopping
  (`max_records`), a bound on member size (`max_member_bytes`), and locked and
  deprecated records served as error documents.
- **Change semantics**: per-item timelines (`orcid_timeline`), field-level
  change logs between two snapshots (`orcid_changes`), and raw documents to
  pair releases with (`orcid_documents`).

## What orcid deliberately does not rebuild

| generic capability | use jointly | how it composes with orcid |
|---|---|---|
| HTTP(S) and S3 reading | `httpfs` (core) | every file reader takes an `https://` or `s3://` path, so a dump on figshare is read in place, downloading only the bytes up to the last member read (RECIPES.md 3.5) |
| Caching remote reads | `cache_httpfs` (community) | repeated exploratory queries over the same remote dump |
| Parquet and CSV output | `parquet` (core, in release builds), CSV (core) | `COPY (SELECT ... FROM orcid_works(...)) TO 'works.parquet'` (RECIPES.md 13) |
| JSON | `json` (core) | ORCID's JSON is read and written by orcid itself (its own parser, no dependency on `json`), into the same sections as the XML. Use the `json` extension for JSON that is not an ORCID document, or `to_json` / `COPY ... (FORMAT json)` for generic exports of orcid's tables |
| Full-text name search | `fts` (core) | index `orcid_person` names, `orcid_person_items` other names and keywords, or work titles; orcid supplies the columns |
| Collation and locale-aware sorting | `icu` (core) | name and title lists `ORDER BY ... COLLATE` |
| Fuzzy matching and record linkage | `jaro_winkler_similarity`, `levenshtein` (core); `rapidfuzz` (community) | match organisation names or credit names that carry no identifier; orcid supplies the names and the disambiguated ids where they exist |
| Graph queries | `duckpgq` (community: SQL/PGQ property graphs) | co-author pairs (RECIPES.md 7, `examples/tasks/coauthors.sql`) as edge tables for path and centrality queries |
| Other registries (ROR, Crossref, DataCite) | `read_json`, `read_csv`, `read_parquet` (core) over their own dumps | join on the identifiers orcid extracts (DOIs, disambiguated organisation ids, ISNI and Wikidata ids); [LINKING.md](LINKING.md) |
| Generic XML | `webbed` (community) | XML that is not an ORCID message. ORCID documents stay on orcid's reader, which knows the items and the iD rules |
| Archives other than tar | `zipfs` (community) | glob `.xml` members of a zip into the readers. Check that the extension is published for your platform and DuckDB version |
| Spreadsheets | `excel` (core: `read_xlsx`, `COPY ... (FORMAT xlsx)`) | lists of iDs in, reports out |
| Other databases | `postgres`, `sqlite`, `mysql` scanners (core) | `ATTACH` a database that holds stored ORCID XML, then read its column with the `_xml` functions |
| Geography | `spatial` (core) | little to do here: ORCID addresses carry a country code and organisation addresses a city, region and country, no coordinates. Coordinates come from a registry such as ROR (LINKING.md) |
| Calling the ORCID API | `http_client` (community), or any client outside DuckDB | orcid parses responses you already have and writes request bodies for you to send. It contains no network code and never calls ORCID; how and when to call it is the application's business |

Names above are DuckDB core or community extension names as of DuckDB v1.5.
Availability varies by platform and release, and community extensions change
hands; check `duckdb_extensions()` or the community extension list before
relying on one. orcid *requires* none of them. It stays dependency-free so that
it loads anywhere DuckDB does; composition is opt-in per session.

## The test for new features

Ask: is the hard part ORCID knowledge, or generic plumbing?

- **ORCID knowledge** belongs here: an element of the schema that no column
  carries ([COVERAGE.md](COVERAGE.md)), an item type, an iD rule, a quirk of
  the Public Data File, a change semantic.
- **Generic plumbing** composes: a transport, a non-ORCID format, an index, a
  matching algorithm, another registry. Add at most a recipe in
  [RECIPES.md](RECIPES.md) or a task bundle ([TASKS.md](TASKS.md)) that
  assumes the sibling extension is loaded.

## What stays out

- **API clients.** OAuth flows, rate limiting and token storage are
  deployment concerns with their own security model; they live in the
  applications that call ORCID, not in a parser.
- **Other registries' data.** ROR, Crossref, DataCite, GRID and Ringgold have
  their own dumps, licences and release cycles. Recipes show the joins, and
  the extension stays ignorant of every registry but ORCID's.
- **Language wrappers.** Python- or Rust-style entry points that connect,
  load the extension and hand back readers are sugar over the SQL surface.
  They follow a different release cadence and toolchain, and are not part of
  this repository, which stays C++, SQL, schemas, tests and specifications.
