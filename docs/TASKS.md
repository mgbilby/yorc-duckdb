# Task bundles

A **task bundle** is a plain `.sql` file of `CREATE OR REPLACE MACRO`
statements under a header comment. It records a reusable job (a record health
check, a DOI duplicate report, a release-to-release change report) as named
macros that you load once and apply with ordinary SQL. A bundle needs no
plugin system, no install step and no runtime of its own, because macros are
pure SQL over the extension's functions. It is a text file you check into your
repository, review in a pull request and share by copying.

## The convention

A bundle file has two parts.

**1. The header**: comment lines at the top, one `key: value` per line.

```sql
-- task:        record-health
-- description: What the bundle does, in a sentence or two.
-- requires:    orcid
-- params:      task_health_summary(path) -> TABLE(one row per iD: counts per check)
--              task_health_works(path)   -> TABLE(one row per work with at least one issue)
-- usage:       .read examples/tasks/record-health.sql
--              SELECT * FROM task_health_summary('records/*.xml');
```

Required keys:
- `task`: the bundle name, matching the file name;
- `description`;
- `requires`: the extensions the macros assume are loaded;
- `params`: every macro the bundle defines, with its signature and what it
  returns;
- `usage`: at least one invocation you can copy and paste.

**2. The macros**: nothing but `CREATE OR REPLACE MACRO` statements (scalar
or `AS TABLE`), so loading a bundle is idempotent and never touches data. By
convention:
- **Names.** Macro names carry a `task_` prefix plus the bundle's noun
  (`task_health_summary`, `task_doi_duplicates`), so bundles collide neither
  with the extension's `orcid_` namespace nor with each other.
- **`path` comes first.** Table macros over files take `path` first. It is
  anything the `orcid_<section>(path)` readers take: a file, a glob or an
  archive (an `https://` URL with `httpfs`).
- **Report, don't write.** A bundle only *defines* macros. The extension never
  writes ORCID data back, and nothing is exported without an explicit `COPY`
  in your session.
- **No blank lines inside a statement.** Separate statements with one blank
  line. `scripts/sync_tasks_test.py` splits a bundle on blank lines to copy it
  into the sqllogictest (below).

## Running a bundle

Interactively, or in a script, load the bundle, then apply it:

```sql
.read examples/tasks/doi-dedupe.sql
SELECT * FROM task_doi_duplicates('records/*.xml');
COPY (SELECT * FROM task_doi_cross_group('records/*.xml')) TO 'review.csv' (HEADER);
```

Non-interactively, `-init` loads the bundle before the commands run:

```sh
duckdb -init examples/tasks/release-changes.sql \
  -c "SELECT * FROM task_release_change_summary('ORCID_2024_10_summaries.tar.gz', 'ORCID_2025_10_summaries.tar.gz', '*/097/*');"
```

Have `INSTALL yorc FROM community; LOAD yorc;` in `~/.duckdbrc`, or put
those two lines at the top of a site-local copy of the bundle. Several bundles
can be `.read` into one session; the `task_` prefix keeps them apart.

## The example bundles

Five bundles ship in `examples/tasks/`. Each is built only from existing orcid
functions, and `test/sql/orcid_tasks.test` exercises each against the
`test/data/` fixtures.

| bundle | macros | job |
|---|---|---|
| `record-health.sql` | `task_health_summary`, `task_health_works` | per-record checks: works without a DOI or a year, repeated DOIs, affiliations and fundings without a disambiguated organisation, current employments; and the list of works with issues |
| `doi-dedupe.sql` | `task_doi_duplicates`, `task_doi_cross_group`, `task_doi_coclaims` | DOIs on more than one work of a record, those spread over several work groups, and DOIs claimed by more than one iD |
| `coauthors.sql` | `task_coauthor_pairs`, `task_coauthor_counts` | co-author pairs from the contributor iDs of full works and fundings, and co-author counts per iD |
| `affiliation-timeline.sql` | `task_affiliation_timeline`, `task_affiliation_current` | every affiliation in start order with its span and an open-ended flag; the current ones |
| `release-changes.sql` | `task_release_changes`, `task_release_change_summary` | records paired by iD across two releases and compared field by field; counts by section and change |

A worked session with the health bundle, on the public record
0000-0002-2771-9344 (a record ORCID staff use as an example; it has works
only, so the affiliation and funding checks are 0):

```sql
.read examples/tasks/record-health.sql
SELECT * FROM task_health_summary('test/data/demo/0000-0002-2771-9344.xml');
-- orcid | works | works_without_doi | works_without_year | duplicate_dois | dois_in_several_groups
--       | affiliations | affiliations_without_org_id | current_employments | fundings | fundings_without_funder_id
-- 0000-0002-2771-9344 | 12 | 0 | 0 | 1 | 0 | 0 | 0 | 0 | 0 | 0
SELECT put_code, title, issues FROM task_health_works('test/data/demo/0000-0002-2771-9344.xml') LIMIT 5;
-- 224052943 and 224052945, both ['DOI shared with another work']
```

The repeated DOI of that record, `10.25592/uhhfdm.19102`, is on two works of
one work group (`dois_in_several_groups = 0`). ORCID groups works by shared
identifiers, so a repeat inside a group is the same work arriving from several
sources, not a duplicate to clean up.

## Writing your own

Start from the closest example, and keep three habits:
- **Compose, don't copy.** Reach for the `orcid_*` functions and other macros.
  If a step needs something no function provides (an element the extension
  does not read, see [COVERAGE.md](COVERAGE.md)), that is a feature request
  for the extension, not a reason to parse XML in SQL.
- **Read once for big inputs.** Each `orcid_<section>(path)` call reads its
  input. Over a full dump, load the sections you need into tables once
  ([RECIPES.md](RECIPES.md), recipe 13) and point the macros' `FROM` clauses
  at those tables.
- **Test against fixtures.** Run each macro over `test/data/` (or your own
  small files) and check the output by hand before pointing it at a full
  release. A bundle that graduates into this repository gets a block in
  `test/sql/orcid_tasks.test`: add `# BEGIN examples/tasks/<file>.sql` /
  `# END examples/tasks/<file>.sql` markers, run
  `python3 scripts/sync_tasks_test.py`, and write the expected values from the
  fixtures, not from a run.
