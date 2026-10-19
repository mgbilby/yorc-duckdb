# Linking ORCID data to other registries

An ORCID record points outwards through identifiers: DOIs and other work ids,
disambiguated organisation ids (ROR, GRID, Ringgold, Crossref Funder Registry,
LEI), and person identifiers such as ISNI, Scopus or ResearcherID. orcid
extracts them. Joining them to the registries that issued them is ordinary SQL
over each registry's own data dump, read with DuckDB's core readers
(`read_json`, `read_csv`, `read_parquet`).

The extension itself stays registry-free. It ships no copy of any other
registry, calls no API, and knows no identifier scheme beyond ORCID's own iD.
Each registry has its own licence, release cycle and schema, so its data
belongs in your pipeline, not in a parser. The recipes below are templates:
the dump layouts named here are the registries' own and change between
releases, so check each against the release you download.

## Where the identifiers are

| identifier | column | notes |
|---|---|---|
| work DOI | `orcid_works.doi` | first item-level `doi` / `self` external id, ASCII-lowercased |
| every work, funding, peer-review and research-resource id | `orcid_external_ids.type`, `.value`, `.normalized`, `.url`, `.relationship` | `owner` / `owner_put_code` name the item; group-level ids have `owner = 'group'` |
| person ids (Scopus, ResearcherID, ISNI, ...) | `orcid_person_items` where `kind = 'external-identifier'`: `type`, `value`, `url` | as the researcher (or a member) added them |
| organisation ids | `org_disambiguated_id` + `org_disambiguation_source` in `orcid_affiliations` and `orcid_fundings`; `convening_org_*` in `orcid_peer_reviews`; `host_*` in the research-resource tables | sources `ROR`, `GRID`, `RINGGOLD`, `FUNDREF`, `LEI` |

The spellings differ by source, so normalise before joining:

| source | as ORCID writes it | key |
|---|---|---|
| `ROR` | `https://ror.org/0abcdef12` | the 9 characters after `ror.org/` |
| `GRID` | `grid.5335.0` | lower case as is |
| `FUNDREF` | `http://dx.doi.org/10.13039/501100000780` | the suffix after `10.13039/`; Crossref also uses the full DOI |
| `RINGGOLD` | `6396` | the integer (Ringgold ids are proprietary; registries that are open do not carry them) |
| `LEI` | `54930027COZ5DVT86J14` | upper case as is |

```sql
CREATE OR REPLACE MACRO org_key(source, id) AS
  CASE upper(source)
    WHEN 'ROR'     THEN regexp_extract(lower(id), '(0[0-9a-hjkmnp-tv-z]{6}[0-9]{2})/?$', 1)
    WHEN 'GRID'    THEN lower(trim(id))
    WHEN 'FUNDREF' THEN regexp_extract(lower(id), '10\.13039/([0-9]+)', 1)
    WHEN 'RINGGOLD' THEN CAST(TRY_CAST(trim(id) AS BIGINT) AS VARCHAR)
    ELSE upper(trim(id))
  END;
```

## DOIs: Crossref and DataCite metadata

Crossref and DataCite each publish their metadata in bulk: Crossref as an
annual public data file, DataCite as an annual public data file, plus both
REST APIs. The files are JSON. Load the part you need into a table once, with
`read_json` or `read_ndjson`, keeping only the DOI and the fields you need,
then join on the lowercased DOI:

```sql
-- Crossref: works in JSON, one {"items": [...]} object per file in recent
-- public data files (check the layout of your release)
CREATE TABLE crossref AS
SELECT lower(item.DOI) AS doi, item."container-title"[1] AS container, item.type AS cr_type,
       item.issued."date-parts"[1][1] AS cr_year
FROM (SELECT unnest(items) AS item FROM read_json('crossref/*.json.gz', maximum_object_size = 1000000000));

SELECT w.orcid, w.put_code, w.title, w.pub_year, c.cr_year, c.container
FROM orcid_works('records/*.xml') w
JOIN crossref c USING (doi)
WHERE w.pub_year IS DISTINCT FROM c.cr_year;    -- year disagreements to review
```

The same join answers "which DOIs in these records are not registered with
Crossref" (an anti-join), and, with a DataCite table built the same way,
"which are DataCite DOIs" (datasets, software, theses). DOIs are
case-insensitive. `orcid_works.doi` is already lowercased; lowercase the other
side.

A DOI's registration agency can also be read from its prefix: `10.5281` is
Zenodo (DataCite), for example. A prefix table built from the agencies' lists
is enough to route DOIs without either dump.

## Organisations: ROR

The [Research Organization Registry](https://ror.org/) publishes its whole
registry as a CC0 data dump on Zenodo, a zip of JSON files. Read the
**schema v2** JSON (`*-ror-data_schema_v2.json` in 2024 releases;
`*-ror-data.json` once v1 was retired) with `read_json`:

```sql
CREATE TABLE ror AS
SELECT regexp_extract(id, '(0[0-9a-hjkmnp-tv-z]{6}[0-9]{2})$', 1) AS ror_id,
       list_filter(names, lambda n: list_contains(n.types, 'ror_display'))[1].value AS name,
       status, types,
       locations[1].geonames_details.country_code AS country,
       external_ids
FROM read_json('ror/v1.67-2025-06-24-ror-data.json', format = 'array');

-- external ids as rows: grid, isni, fundref, wikidata
CREATE TABLE ror_ids AS
SELECT ror_id, lower(e.type) AS type, v AS value
FROM ror, unnest(external_ids) u(e), unnest(e."all") w(v);
```

Then map ORCID's organisations onto ROR ids, directly for `ROR`, and through
ROR's external ids for `GRID` and `FUNDREF`:

```sql
WITH a AS (
  SELECT orcid, put_code, affiliation_type, org_name, org_country,
         upper(org_disambiguation_source) AS source, org_key(org_disambiguation_source, org_disambiguated_id) AS k
  FROM orcid_affiliations('records/*.xml'))
SELECT a.*, coalesce(r.ror_id, g.ror_id, f.ror_id) AS ror_id
FROM a
LEFT JOIN ror r     ON a.source = 'ROR'     AND r.ror_id = a.k
LEFT JOIN ror_ids g ON a.source = 'GRID'    AND g.type = 'grid'    AND g.value = a.k
LEFT JOIN ror_ids f ON a.source = 'FUNDREF' AND f.type = 'fundref' AND f.value = a.k;
```

Notes:
- **Several records per id.** A GRID or FundRef id can appear on more than
  one ROR record, for example a record and its successor. Prefer the record
  where it is the `preferred` value, then an active one.
- **Withdrawn records.** Follow `relationships` of type `successor` to the
  current organisation.
- **Ringgold** ids have no open crosswalk. Organisations disambiguated only
  by Ringgold, or not at all, can be matched on exact name and country
  against ROR's names (labels and aliases, not acronyms), accepting only
  unique matches. Treat those as lower confidence.
- **Countries.** ROR's location gives a country (and coordinates) where an
  ORCID organisation address is incomplete. ORCID's own `org_country` is the
  researcher's statement, and wins when both exist.

## People: ISNI, Wikidata and other person ids

Person identifiers are in `orcid_person_items`, as the researcher or a member
added them:

```sql
SELECT orcid, type, value, url
FROM orcid_person_items('records/*.xml')
WHERE kind = 'external-identifier'
ORDER BY orcid, type;
```

`type` is free text chosen by the source (`Scopus Author ID`, `ResearcherID`,
`ISNI`, `Loop profile`, ...). Normalise it before grouping, for example
`lower(regexp_replace(type, '[^A-Za-z]', '', 'g'))`.
- **ISNI.** Compare without spaces, with an upper-case `X`.
- **Wikidata.** Wikidata items record ORCID iDs (property P496), so the link
  usually runs the other way: from a Wikidata dump or a SPARQL result to
  `orcid_normalize(value)`. ROR's external ids carry ISNI and Wikidata ids
  for organisations, which joins `ror_ids` (above) to Wikidata as well.

## Funders: Crossref Funder Registry

`FUNDREF` organisation ids are Crossref Funder Registry DOIs
(`10.13039/...`). The registry is published as an RDF/XML dump, and ROR now
carries the same ids in its `fundref` external ids. The ROR route above is
usually the simpler one. Funding amounts, grant numbers and award DOIs, where
the funding has them, are in `orcid_fundings` and `orcid_external_ids` (types
such as `grant_number`).
