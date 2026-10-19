# Locked and deprecated records

Proposal for how a client that persists ORCID documents should store locked and
deprecated records. What the extension does is fixed (see
[REFERENCE.md](REFERENCE.md#orcid_errors)); the storage rules are a proposal.

## What the data looks like

ORCID does not publish a locked or deprecated record. In its place the API
serves an `error:error` document, and the Public Data File's summaries archive
holds the same document under the record's path, `<iD>.xml`. The 2025 file has
136 of them in the first 5,000 summaries of the committed sample. All of them
use one of these two shapes:

| state | `response-code` | `error-code` | `developer-message` |
|---|---|---|---|
| locked (127) | 409 | 9018 | `409 Conflict: The ORCID record is locked and cannot be edited. ORCID <iD>` (the iD bare or as `https://orcid.org/<iD>`, sometimes followed by `Full validation error: <iD> is locked`) |
| deprecated (9) | 301 | 9007 | `301 Moved Permanently: This account is deprecated. Please refer to account: https://orcid.org/<primary>. ORCID https://orcid.org/<iD>` |

The document has no `path` attribute and no `orcid-identifier`, and it is
the only place where the iD appears. It contains no person, history or
activities, so every record section yields zero rows for it.

`orcid_errors` / `orcid_errors_xml` give one row per such document:

- `orcid` is the record's iD, taken from the message.
- `primary_orcid` (the last column) is the primary account of a deprecated record. It is NULL otherwise.
- `response_code` and `error_code` tell the two states apart.

Fixtures: `test/data/errors/0000-0001-5272-5001_locked.xml` and
`test/data/errors/0000-0001-5433-3001_deprecated.xml`.

## Proposed storage rules

1. **They are record documents.** Their root (`error`) is not an item kind, so
   they go through the record path unchanged. The record's iD is the one
   `orcid_errors_xml(xml)` returns. If it is NULL, use the file stem's first
   19 characters, as for other records. If neither is a valid iD, the file
   fails.
2. **Store the document as the record.** It gets a `records` row with its own
   `source_hash`, and the usual upsert rule applies.
   - When a record that was loaded earlier becomes locked or deprecated, the
     error document replaces it. The section rows derived from the old version
     are deleted, and nothing is re-derived.
   - This is correct: ORCID no longer publishes that data.
   - When the record is unlocked again, the next load replaces the error
     document the same way.
3. **Persist `errors` as a section.** Append `errors` to `SECTIONS`, after
   `history`, and derive it with the same pinned INSERT. A record's state is then
   a query, not a parse:

   ```sql
   -- locked records
   SELECT orcid FROM errors WHERE error_code = 9018;
   -- deprecated records and where they point
   SELECT orcid, primary_orcid FROM errors WHERE error_code = 9007;
   -- records the database holds only as an error document
   SELECT r.orcid FROM records r JOIN errors e USING (orcid, source_hash);
   ```

   `errors.orcid` / `errors.source_hash` are overwritten from the parent row as
   for every section, so they always equal the `records` row.
4. **Follow deprecated iDs only on request.** `fetch` or `load` should not
   replace a deprecated iD with its primary. A client may offer to fetch
   `primary_orcid` as an extra record. The deprecated iD keeps its error
   document, so the mapping old → primary stays queryable.
5. **Other error documents are not records.** An error with any other code
   (for example a 404 from the API, or a 5xx) is a failed fetch. It is reported,
   not stored. Only the 9018 and 9007 shapes above stand in for a record.
6. **Export.** `export --format xml` writes the stored error document as is,
   so a round trip keeps the record's state.
