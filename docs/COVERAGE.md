# Schema coverage

Every element and attribute name declared in the vendored ORCID message
schema v3.0 (`reference/orcid-model/`, yorc-rs commit `9592e2d`: the 27
record_3.0 XSDs, including `search-3.0`, `expanded-search-3.0`, `error-3.0`
and `bulk-3.0`, plus `common-3.0`, `summary-3.0`, the two notification XSDs
and `group-id-3.0`), the section columns that carry it, how it appears in
ORCID's JSON, and whether a writer writes it.

The table is generated: `python3 audit/tools/xsd_coverage.py .` reads the XSDs,
the extraction tables in `src/core/sections.cpp` and the writer templates in
`src/core/writer.cpp`, and prints the totals and the rows below. Re-run it
after changing any of them and replace the table.

## Totals

| | names |
|---|---:|
| declared names (32 XSDs) | **214** (204 elements, 10 attributes) |
| data-bearing (text content, simple content, or an attribute) | **109** |
| — carried by at least one column | **106** |
| — not mapped | **3**: `approval-date`, `locale`, `@transient` |
| structural (elements whose content is only other elements) | **105** |
| — used: item elements, path steps, group/owner markers | **101** |
| — not mapped | **4**: `preferences`, `application-orcid`, `application-group-orcid`, `group-orcid-identifier` |
| written by a writer (`src/core/writer.cpp`) | **90** names, 52 of them data-bearing (2026-10-18: `host`, a contributor's `contributor-orcid/host`) |

The 2026-10-18 changes (CROSSCHECK #9 and #10) leave the name totals as they
were: every name they read was already carried by some column. They close
the context gaps below, where a mapped name was not read in every place the
XSDs put it, and add the section `research_resource_hosts` (19 sections).

Plus one attribute declared by reference rather than by name, `xml:lang`
(notification-custom-3.0): `notifications.language`; in JSON the member `lang` (assumed: ORCID publishes no JSON notification sample). Not written.

## How to read it

- **JSON** is the name's form in ORCID's JSON as the reader maps it
  (`src/core/json_map.cpp`, REFERENCE.md "JSON documents"): `same` is a member
  of the same name (an array when the element repeats; simple values ORCID
  wraps are `{"value": ...}` objects), `member` an attribute carried as a
  member of the element's object. The JSON-only wrapper `summaries` (around
  affiliation summaries) is no schema name and so has no row.
- **written** means a writer template emits the name. The read-only names
  (source, created / last-modified dates, path, display index, ORCID's
  normalized identifiers) are never written, by design; nor are the names of
  documents no writer covers (person, history, summary, search, error,
  notifications). REFERENCE.md, "Writing ORCID documents".

- **Granularity is the local name.** A name counts as *mapped* when at least
  one column path ends in it (`title/title` → `works.title`), or when a
  computed column reads it (`$orcid` reads the root `@path` and
  `orcid-identifier/path`; `$doi` reads `external-id-type`, `-value` and
  `-relationship`; `$group` counts `group` / `affiliation-group`; `$owner`
  and `$owner_put_code` read the nearest `@put-code`). A name used in several
  types is mapped as soon as one of them is read; the context gaps below list
  the places where a mapped name is still not read.
- **Structural** names are containers. They are read as item elements (the
  element a section emits one row for), as steps of column paths
  (`organization/address/city`), or as the markers above. Their content is
  covered by the names below them.
- **Classification** follows the declared type: an element whose type (named
  or inline) has simple content, mixed content or a simple base is
  data-bearing; one with only an element sequence or choice is structural.
  Six names are declared both ways in different types (`name`, `title`, ...);
  they count as data-bearing.

## Context gaps

Places where a name that is mapped elsewhere is not read. Found by reading
the XSDs against the column paths in `src/core/sections.cpp`. The table
lists what is left; the gaps closed on 2026-10-18 (CROSSCHECK #9 and #10)
follow it.

| where | not read | note |
|---|---|---|
| `source/assertion-origin-*` | `group_ids`, `timeline` | group-id-3.0.xsd extends the source type of common-2.1 (its `schemaLocation`), which has no assertion origin; `timeline` is a derived event list and keeps `source_name` only |
| `external-id-normalized-error/error-message`, `/error-code` | `notifications` | a notification item's normalized form is joined (`item_external_id_normalized`); its errors are not, since a joined list of errors could not be matched to its items |
| collection and group `last-modified-date` (`activities-summary`, `works`, `group`, `affiliation-group`, `group-id-records`, ...) | everywhere | not added (CROSSCHECK §1: no change). Item-level dates are read; the per-collection maximum is `max(last_modified)` |
| `source-orcid`, `source-client-id`, `assertion-origin-*` as `uri` / `host` | everywhere | the `path` is read; the URI and host follow from it (`https://orcid.org/<path>`, `orcid.org`) |
| `@transient` | everywhere | always `true` (see the table) |

### Closed on 2026-10-18

| gap | now read in | columns |
|---|---|---|
| `source/assertion-origin-orcid`, `-client-id` outside `works` | `fundings`, `affiliations`, `peer_reviews`, `research_resources` | `assertion_origin_orcid`, `assertion_origin_client_id` |
| any assertion origin in `person_items`, `history`, `notifications` | those three | `assertion_origin_name`, `assertion_origin_orcid`, `assertion_origin_client_id` |
| `external-id-normalized`, `-normalized-error` outside `external_ids` | `person_items` (a person external identifier is a `common:external-id`), `notifications` (joined) | `normalized`, `normalized_error`, `normalized_error_code`; `item_external_id_normalized` |
| `external-id-normalized-error/error-code` | `external_ids`, `person_items` | `normalized_error_code` |
| `items/item/external-id/external-id-url`, `-relationship` | `notifications` (joined, as the types and values) | `item_external_id_urls`, `item_external_id_relationships` |
| `contributor-orcid/host` | `contributors` (also written now) | `contributor_orcid_host` |
| record `orcid-identifier/uri`, `/host` | `person` (the record's person row; NULL for a standalone person document) | `orcid_uri`, `orcid_host` |
| a second (to tenth) `hosts/organization` | the new section `research_resource_hosts`, for proposals and resource items (one table, told apart by `container`) | every column of the section |

`summary_items` was listed with the normalized forms before; its
`external-identifier` (summary-3.0.xsd) has only type, value and URL, so
there is nothing to read there.

## Columns added on 2026-10-18

Every new column is appended after the existing ones, so the positions of
the earlier columns do not move (as with `peer_reviews`' 36th column,
`subject_translated_name_language`). yorc-rs's `rows` has the same
columns in the same positions for the 14 sections it shares
(`tests/samples.rs`, `schema_matches_yorc_duckdb`), and so has yorc-zig's
`rows` (`src/rows.zig`, "schema matches yorc-duckdb"); the
cross-implementation check compares all three ([BENCHMARKS.md](../BENCHMARKS.md#parity)).

| section | # | column | type | path (relative to the item) | width before → after |
|---|---:|---|---|---|---|
| `fundings` | 36 | `assertion_origin_orcid` | VARCHAR | `source/assertion-origin-orcid/path` | 35 → 37 |
| `fundings` | 37 | `assertion_origin_client_id` | VARCHAR | `source/assertion-origin-client-id/path` | |
| `affiliations` | 31 | `assertion_origin_orcid` | VARCHAR | `source/assertion-origin-orcid/path` | 30 → 32 |
| `affiliations` | 32 | `assertion_origin_client_id` | VARCHAR | `source/assertion-origin-client-id/path` | |
| `peer_reviews` | 37 | `assertion_origin_orcid` | VARCHAR | `source/assertion-origin-orcid/path` | 36 → 38 |
| `peer_reviews` | 38 | `assertion_origin_client_id` | VARCHAR | `source/assertion-origin-client-id/path` | |
| `research_resources` | 31 | `assertion_origin_orcid` | VARCHAR | `source/assertion-origin-orcid/path` | 30 → 32 |
| `research_resources` | 32 | `assertion_origin_client_id` | VARCHAR | `source/assertion-origin-client-id/path` | |
| `contributors` | 12 | `contributor_orcid_host` | VARCHAR | `contributor-orcid/host` | 11 → 12 |
| `external_ids` | 13 | `normalized_error_code` | VARCHAR | `external-id-normalized-error/error-code` | 12 → 13 |
| `person` | 14 | `orcid_uri` | VARCHAR | `^orcid-identifier/uri` (below the document element) | 13 → 15 |
| `person` | 15 | `orcid_host` | VARCHAR | `^orcid-identifier/host` | |
| `person_items` | 19 | `assertion_origin_name` | VARCHAR | `source/assertion-origin-name` | 18 → 24 |
| `person_items` | 20 | `assertion_origin_orcid` | VARCHAR | `source/assertion-origin-orcid/path` | |
| `person_items` | 21 | `assertion_origin_client_id` | VARCHAR | `source/assertion-origin-client-id/path` | |
| `person_items` | 22 | `normalized` | VARCHAR | `external-id-normalized` | |
| `person_items` | 23 | `normalized_error` | VARCHAR | `external-id-normalized-error/error-message` | |
| `person_items` | 24 | `normalized_error_code` | VARCHAR | `external-id-normalized-error/error-code` | |
| `history` | 15 | `assertion_origin_name` | VARCHAR | `source/assertion-origin-name` | 14 → 17 |
| `history` | 16 | `assertion_origin_orcid` | VARCHAR | `source/assertion-origin-orcid/path` | |
| `history` | 17 | `assertion_origin_client_id` | VARCHAR | `source/assertion-origin-client-id/path` | |
| `notifications` | 25 | `assertion_origin_name` | VARCHAR | `source/assertion-origin-name` | 24 → 30 |
| `notifications` | 26 | `assertion_origin_orcid` | VARCHAR | `source/assertion-origin-orcid/path` | |
| `notifications` | 27 | `assertion_origin_client_id` | VARCHAR | `source/assertion-origin-client-id/path` | |
| `notifications` | 28 | `item_external_id_urls` | VARCHAR | every `items/item/external-id/external-id-url`, joined | |
| `notifications` | 29 | `item_external_id_relationships` | VARCHAR | every `items/item/external-id/external-id-relationship`, joined | |
| `notifications` | 30 | `item_external_id_normalized` | VARCHAR | every `items/item/external-id/external-id-normalized`, joined | |
| `research_resource_hosts` (new; item `hosts/organization`) | 1 | `orcid` | VARCHAR | `$orcid` | new: 12 |
| | 2 | `owner_put_code` | BIGINT | `$owner_put_code` (the research resource) | |
| | 3 | `ordinal` | INTEGER | `$ordinal` (position in `hosts`) | |
| | 4 | `host_name` | VARCHAR | `name` | |
| | 5 | `host_city` | VARCHAR | `address/city` | |
| | 6 | `host_region` | VARCHAR | `address/region` | |
| | 7 | `host_country` | VARCHAR | `address/country` | |
| | 8 | `host_disambiguated_id` | VARCHAR | `disambiguated-organization/disambiguated-organization-identifier` | |
| | 9 | `host_disambiguation_source` | VARCHAR | `disambiguated-organization/disambiguation-source` | |
| | 10 | `container` | VARCHAR | `$hosts_parent`: `proposal` or `resource-item` | |
| | 11 | `item_ordinal` | INTEGER | `$resource_item_ordinal`: the resource item's ordinal, NULL for the proposal | |
| | 12 | `source_hash` | VARCHAR | `$hash` | |

Unchanged: `works` 32, `research_resource_items` 13, `timeline` 13,
`summary` 23, `summary_items` 20, `search_results` 13, `errors` 10,
`group_ids` 12. The first-host columns of `research_resources` and
`research_resource_items` stay, for compatibility: they hold the first
non-empty value of each column among the hosts.

## Table

`declared in` names the XSDs (without `.xsd`) that declare the name; `@`
marks an attribute. Long column lists are cut after four (`+n` more); the
full lists come from `python3 audit/tools/xsd_coverage.py . --json`.

| name | declared in | status | carried by / note | JSON | written |
|---|---|---|---|---|---|
| `actioned-date` | notification-permission-3.0 | mapped | `notifications.actioned_date` | same; `{"value": epoch ms}` |  |
| `activities-summary` | activities-3.0 | structural | container (record wrapper of the activity collections); items are found below it by suffix | same |  |
| `address` | common-3.0, address-3.0 | structural | item element of `person_items`, `timeline`; path step in `affiliations`, `fundings`, `peer_reviews`, `research_resource_hosts`, `research_resource_items`, `research_resources` | same | yes |
| `addresses` | address-3.0 | structural | item element of `person_items`, `timeline` | same |  |
| `affiliation-group` | activities-3.0 | structural | affiliations.group_index ($group) | same; its items in `summaries` wrapper objects |  |
| `amount` | funding-3.0 | mapped | `fundings.amount` | same | yes |
| `application-group-orcid` | common-3.0 | not mapped | not mapped: top-level declaration in common-3.0.xsd that no vendored type references | same |  |
| `application-orcid` | common-3.0 | not mapped | not mapped: top-level declaration in common-3.0.xsd that no vendored type references | same |  |
| `approval-date` | common-3.0 | not mapped | not mapped: declared at the top level of common-3.0.xsd but referenced by no type in the vendored schemas, so no document carries it | same; `{"value": epoch ms}` |  |
| `archived-date` | common-3.0 | mapped | `notifications.archived_date` | same; `{"value": epoch ms}` |  |
| `assertion-origin-client-id` | common-3.0 | structural | path step in `affiliations`, `fundings`, `history`, `notifications`, `peer_reviews`, `person_items`, `research_resources`, `works` | same |  |
| `assertion-origin-name` | common-3.0 | mapped | `affiliations.assertion_origin_name`, `fundings.assertion_origin_name`, `history.assertion_origin_name`, `notifications.assertion_origin_name` (+4) | same |  |
| `assertion-origin-orcid` | common-3.0 | structural | path step in `affiliations`, `fundings`, `history`, `notifications`, `peer_reviews`, `person_items`, `research_resources`, `works` | same |  |
| `authorization-url` | notification-permission-3.0 | structural | path step in `notifications` | same |  |
| `biography` | person-3.0, personal-details-3.0 | structural | item element of `timeline`; path step in `person` | same |  |
| `body-html` | notification-custom-3.0 | mapped | `notifications.body_html` | same |  |
| `body-text` | notification-custom-3.0 | mapped | `notifications.body_text` | same |  |
| `bulk` | bulk-3.0 | structural | item element of `errors` | same; entries are `{"work": ...}` / `{"error": ...}` wrapper objects | yes |
| `citation` | work-3.0 | structural | path step in `works` | same | yes |
| `citation-type` | work-3.0 | mapped | `works.citation_type` | same | yes |
| `citation-value` | work-3.0 | mapped | `works.citation_value` | same | yes |
| `city` | common-3.0 | mapped | `affiliations.org_city`, `fundings.org_city`, `peer_reviews.convening_org_city`, `research_resource_hosts.host_city` (+2) | same | yes |
| `claimed` | history-3.0 | mapped | `history.claimed` | same |  |
| `completion-date` | history-3.0, peer-review-3.0 | mapped | `history.completion_date` (also declared as a container elsewhere) | same |  |
| `content` | keyword-3.0, other-name-3.0, personal-details-3.0 | mapped | `person.biography`, `person_items.value`, `timeline.label` | same | yes |
| `contributor` | funding-3.0, work-3.0 | structural | item element of `contributors` | same | yes |
| `contributor-attributes` | funding-3.0, work-3.0 | structural | path step in `contributors` | same | yes |
| `contributor-email` | funding-3.0, work-3.0 | mapped | `contributors.email` | same | yes |
| `contributor-orcid` | common-3.0 | structural | path step in `contributors` | same | yes |
| `contributor-role` | common-3.0, funding-3.0, work-3.0 | mapped | `contributors.role` | same | yes |
| `contributor-sequence` | common-3.0, work-3.0 | mapped | `contributors.sequence` | same | yes |
| `contributors` | funding-3.0, work-3.0 | structural | item element of `contributors` | same | yes |
| `convening-organization` | peer-review-3.0 | structural | path step in `peer_reviews`, `timeline` | same | yes |
| `count` | summary-3.0 | mapped | `summary.education_qualification_count`, `summary.email_domain_count`, `summary.employment_count`, `summary.professional_activity_count` | same |  |
| `country` | common-3.0, address-3.0 | mapped | `affiliations.org_country`, `fundings.org_country`, `peer_reviews.convening_org_country`, `person_items.value` (+5) | same | yes |
| `created-date` | common-3.0 | mapped | `affiliations.created`, `fundings.created`, `notifications.created`, `peer_reviews.created` (+6) | same; `{"value": epoch ms}` |  |
| `creation-method` | history-3.0 | mapped | `history.creation_method` | same |  |
| `credit-name` | expanded-search-3.0, funding-3.0, personal-details-3.0, work-3.0, summary-3.0 | mapped | `contributors.credit_name`, `person.credit_name`, `search_results.credit_name`, `summary.credit_name` | same | yes |
| `@currency-code` | common-3.0 | mapped | `fundings.currency` | member of the `{"value"}` object | yes |
| `day` | common-3.0 | mapped | `affiliations.end_day`, `affiliations.start_day`, `fundings.end_day`, `fundings.start_day` (+6) | same | yes |
| `deactivation-date` | history-3.0 | mapped | `history.deactivation_date` | same; `{"value": epoch ms}` |  |
| `department-name` | common-3.0 | mapped | `affiliations.department` | same | yes |
| `description` | group-id-3.0 | mapped | `group_ids.description` | same | yes |
| `developer-message` | error-3.0 | mapped | `errors.developer_message`; errors.orcid / errors.primary_orcid (iD in the message) | same |  |
| `disambiguated-organization` | common-3.0 | structural | path step in `affiliations`, `fundings`, `peer_reviews`, `research_resource_hosts`, `research_resource_items`, `research_resources` | same | yes |
| `disambiguated-organization-identifier` | common-3.0 | mapped | `affiliations.org_disambiguated_id`, `fundings.org_disambiguated_id`, `peer_reviews.convening_org_disambiguated_id`, `research_resource_hosts.host_disambiguated_id` (+2) | same | yes |
| `disambiguation-source` | common-3.0 | mapped | `affiliations.org_disambiguation_source`, `fundings.org_disambiguation_source`, `peer_reviews.convening_org_disambiguation_source`, `research_resource_hosts.host_disambiguation_source` (+2) | same | yes |
| `@display-index` | common-3.0 | mapped | `affiliations.display_index`, `fundings.display_index`, `peer_reviews.display_index`, `person_items.display_index` (+2) | member |  |
| `distinction` | distinction-3.0 | structural | item element of `affiliations`, `timeline` | same | yes |
| `distinction-summary` | distinction-3.0 | structural | item element of `affiliations`, `timeline` | same |  |
| `distinctions` | activities-3.0 | structural | container (affiliation collection); its `affiliation-group`s and items are read | same |  |
| `education` | education-3.0 | structural | item element of `affiliations`, `timeline` | same | yes |
| `education-qualification` | summary-3.0 | structural | item element of `summary_items` | same |  |
| `education-qualifications` | summary-3.0 | structural | item element of `summary_items`; path step in `summary` | same |  |
| `education-summary` | education-3.0 | structural | item element of `affiliations`, `timeline` | same |  |
| `educations` | activities-3.0 | structural | container (affiliation collection); its `affiliation-group`s and items are read | same |  |
| `email` | email-3.0, expanded-search-3.0 | mapped | `person_items.value`, `search_results.emails`, `timeline.label` (also declared as a container elsewhere) | same |  |
| `email-domain` | summary-3.0 | structural | item element of `summary_items`; path step in `summary` | same |  |
| `email-domains` | summary-3.0 | structural | item element of `summary_items`; path step in `summary` | same |  |
| `emails` | email-3.0 | structural | item element of `person_items`, `timeline` | same |  |
| `employment` | employment-3.0, summary-3.0 | structural | item element of `affiliations`, `summary_items`, `timeline` | same | yes |
| `employment-summary` | employment-3.0 | structural | item element of `affiliations`, `timeline` | same |  |
| `employments` | activities-3.0, summary-3.0 | structural | item element of `summary_items`; path step in `summary` | same |  |
| `end-date` | common-3.0 | structural | path step in `affiliations`, `fundings`, `research_resources`, `summary_items` | same | yes |
| `error` | error-3.0 | structural | item element of `errors` | same |  |
| `error-code` | common-3.0, error-3.0 | mapped | `errors.error_code`, `external_ids.normalized_error_code`, `person_items.normalized_error_code` | same |  |
| `error-message` | common-3.0 | mapped | `external_ids.normalized_error`, `person_items.normalized_error` | same |  |
| `expanded-result` | expanded-search-3.0 | structural | item element of `search_results` | same |  |
| `expanded-search` | expanded-search-3.0 | structural | item element of `search_results` | same |  |
| `external-id` | common-3.0 | structural | item element of `external_ids`; path step in `notifications` | same | yes |
| `external-id-normalized` | common-3.0 | mapped | `external_ids.normalized`, `notifications.item_external_id_normalized`, `person_items.normalized` | same |  |
| `external-id-normalized-error` | common-3.0 | structural | path step in `external_ids`, `person_items` | same |  |
| `external-id-relationship` | common-3.0 | mapped | `external_ids.relationship`, `notifications.item_external_id_relationships`, `person_items.relationship`; works.doi | same | yes |
| `external-id-type` | common-3.0, summary-3.0 | mapped | `external_ids.type`, `notifications.item_external_id_types`, `peer_reviews.subject_id_type`, `person_items.type` (+2); works.doi ($doi: type doi + relationship self) | same | yes |
| `external-id-url` | common-3.0, summary-3.0 | mapped | `external_ids.url`, `notifications.item_external_id_urls`, `person_items.url`, `summary_items.url` | same | yes |
| `external-id-value` | common-3.0, summary-3.0 | mapped | `external_ids.value`, `notifications.item_external_id_values`, `peer_reviews.subject_id_value`, `person_items.value` (+2); works.doi | same | yes |
| `external-identifier` | person-external-identifier-3.0, summary-3.0 | structural | item element of `person_items`, `summary_items`, `timeline`; path step in `summary` | same | yes |
| `external-identifiers` | person-external-identifier-3.0, summary-3.0 | structural | item element of `person_items`, `summary_items`, `timeline`; path step in `summary` | same |  |
| `external-ids` | common-3.0 | structural | item element of `external_ids` | same | yes |
| `family-name` | personal-details-3.0 | mapped | `person.family_name` | same |  |
| `family-names` | expanded-search-3.0 | mapped | `search_results.family_names` | same |  |
| `funding` | funding-3.0 | structural | item element of `fundings`, `timeline` | same | yes |
| `funding-summary` | funding-3.0 | structural | item element of `fundings`, `timeline` | same |  |
| `fundings` | activities-3.0, summary-3.0 | structural | path step in `summary` | same |  |
| `fuzzy-date` | common-3.0 | structural | type-carrier element (common-3.0.xsd); dates are read as `*/year|month|day` | same |  |
| `given-names` | expanded-search-3.0, personal-details-3.0 | mapped | `person.given_names`, `search_results.given_names`, `timeline.label` | same |  |
| `group` | activities-3.0 | structural | *.group_index ($group) | same |  |
| `group-id` | group-id-3.0 | mapped | `group_ids.group_id` | same | yes |
| `group-id-record` | group-id-3.0 | structural | item element of `group_ids` | same | yes |
| `group-id-records` | group-id-3.0 | structural | item element of `group_ids` | same |  |
| `group-orcid-identifier` | common-3.0 | not mapped | not mapped: top-level declaration in common-3.0.xsd that no vendored type references | same |  |
| `history` | history-3.0 | structural | item element of `history` | same |  |
| `host` | common-3.0, notification-permission-3.0 | mapped | `contributors.contributor_orcid_host`, `notifications.authorization_host`, `person.orcid_host`, `search_results.orcid_host` (+1) | same | yes |
| `hosts` | research-resource-3.0 | structural | item element of `research_resource_hosts`; path step in `research_resource_items`, `research_resources`, `timeline` | same | yes |
| `institution-name` | expanded-search-3.0 | mapped | `search_results.institution_names` | same |  |
| `invited-position` | invited-position-3.0 | structural | item element of `affiliations`, `timeline` | same | yes |
| `invited-position-summary` | invited-position-3.0 | structural | item element of `affiliations`, `timeline` | same |  |
| `invited-positions` | activities-3.0 | structural | container (affiliation collection); its `affiliation-group`s and items are read | same |  |
| `item` | notification-permission-3.0 | structural | path step in `notifications` | same |  |
| `item-name` | notification-permission-3.0 | mapped | `notifications.item_names` | same |  |
| `item-type` | notification-permission-3.0 | mapped | `notifications.item_types` | same |  |
| `items` | notification-permission-3.0 | structural | path step in `notifications` | same |  |
| `journal-title` | work-3.0 | mapped | `works.journal` | same | yes |
| `keyword` | keyword-3.0 | structural | item element of `person_items`, `timeline` | same | yes |
| `keywords` | keyword-3.0 | structural | item element of `person_items`, `timeline` | same |  |
| `@language-code` | common-3.0 | mapped | `fundings.translated_title_language`, `peer_reviews.subject_translated_name_language`, `research_resources.translated_title_language`, `works.translated_title_language` | member of the `{"value"}` object | yes |
| `language-code` | common-3.0 | mapped | `works.language_code` | same | yes |
| `last-modified-date` | common-3.0 | mapped | `affiliations.last_modified`, `fundings.last_modified`, `history.last_modified`, `peer_reviews.last_modified` (+9) | same; `{"value": epoch ms}` |  |
| `locale` | preferences-3.0 | not mapped | not mapped: the only child of `preferences` (see there) | same |  |
| `membership` | membership-3.0 | structural | item element of `affiliations`, `timeline` | same | yes |
| `membership-summary` | membership-3.0 | structural | item element of `affiliations`, `timeline` | same |  |
| `memberships` | activities-3.0 | structural | container (affiliation collection); its `affiliation-group`s and items are read | same |  |
| `month` | common-3.0 | mapped | `affiliations.end_month`, `affiliations.start_month`, `fundings.end_month`, `fundings.start_month` (+6) | same | yes |
| `more-info` | error-3.0 | mapped | `errors.more_info` | same |  |
| `name` | common-3.0, group-id-3.0, person-3.0, personal-details-3.0 | mapped | `affiliations.org_name`, `fundings.org_name`, `group_ids.name`, `peer_reviews.convening_org_name` (+4) (also declared as a container elsewhere) | same | yes |
| `notification` | notification-custom-3.0, notification-permission-3.0 | structural | item element of `notifications` | same |  |
| `notification-intro` | notification-permission-3.0 | mapped | `notifications.intro` | same |  |
| `notification-subject` | notification-permission-3.0 | mapped | `notifications.subject` | same |  |
| `notification-type` | notification-custom-3.0, notification-permission-3.0 | mapped | `notifications.notification_type` | same |  |
| `notifications` | notification-permission-3.0 | structural | item element of `notifications` | same |  |
| `@num-found` | expanded-search-3.0, search-3.0 | mapped | `search_results.num_found` | member |  |
| `orcid-id` | common-3.0, expanded-search-3.0 | mapped | `search_results.orcid` (also declared as a container elsewhere) | same |  |
| `orcid-identifier` | common-3.0 | structural | path step in `person`, `search_results`, `summary`; *.orcid fallback (orcid-identifier/path) | same |  |
| `organization` | common-3.0 | structural | item element of `research_resource_hosts`; path step in `affiliations`, `fundings`, `research_resource_items`, `research_resources`, `timeline` | same | yes |
| `organization-defined-type` | funding-3.0 | mapped | `fundings.organization_defined_type` | same | yes |
| `organization-name` | summary-3.0 | mapped | `summary_items.organization_name` | same |  |
| `other-name` | expanded-search-3.0, other-name-3.0 | mapped | `search_results.other_names` (also declared as a container elsewhere) | same | yes |
| `other-names` | other-name-3.0 | structural | item element of `person_items`, `timeline` | same |  |
| `page` | group-id-3.0 | mapped | `group_ids.page` | same |  |
| `page-size` | group-id-3.0 | mapped | `group_ids.page_size` | same |  |
| `@path` | common-3.0, activities-3.0, address-3.0, email-3.0, keyword-3.0, other-name-3.0, person-3.0, person-external-identifier-3.0, personal-details-3.0, record-3.0, researcher-url-3.0 | mapped | `affiliations.path`, `fundings.path`, `peer_reviews.path`, `person.path` (+4); every record section .orcid (root @path); contributors/external_ids .orcid fallback (item @path) | member `path` (an element in `{uri, path, host}` objects) |  |
| `path` | common-3.0, notification-permission-3.0 | mapped | `affiliations.assertion_origin_client_id`, `affiliations.assertion_origin_orcid`, `affiliations.source_client_id`, `affiliations.source_orcid` (+33) | same | yes |
| `peer-review` | peer-review-3.0 | structural | item element of `peer_reviews`, `timeline` | same | yes |
| `peer-review-group` | activities-3.0 | structural | container (peer-review subgroup); `owner` of external ids below it | same |  |
| `peer-review-publication-grants` | summary-3.0 | mapped | `summary.peer_review_publication_grants` | same |  |
| `peer-review-summary` | peer-review-3.0 | structural | item element of `peer_reviews`, `timeline` | same |  |
| `peer-reviews` | activities-3.0, summary-3.0 | structural | path step in `summary` | same |  |
| `person` | person-3.0 | structural | item element of `person`, `timeline` | same |  |
| `personal-details` | personal-details-3.0 | structural | item element of `person`, `timeline` | same |  |
| `preferences` | preferences-3.0 | not mapped | not mapped: researcher preferences (only `locale`); record-3.0.xsd imports the namespace but references no element of it, and there is no section for a standalone document | same |  |
| `@primary` | email-3.0 | mapped | `person_items.primary` | member |  |
| `professional-activities` | summary-3.0 | structural | item element of `summary_items`; path step in `summary` | same |  |
| `professional-activity` | summary-3.0 | structural | item element of `summary_items` | same |  |
| `proposal` | research-resource-3.0 | structural | path step in `research_resources`, `timeline` | same | yes |
| `publication-date` | common-3.0 | structural | path step in `works` | same | yes |
| `@put-code` | common-3.0, group-id-3.0, notification-custom-3.0, notification-permission-3.0 | mapped | `affiliations.put_code`, `fundings.put_code`, `group_ids.put_code`, `notifications.put_code` (+5); *.owner_put_code, *.owner (nearest ancestor with @put-code) | member | yes |
| `put-code` | summary-3.0 | mapped | `summary_items.put_code` | same |  |
| `qualification` | qualification-3.0 | structural | item element of `affiliations`, `timeline` | same | yes |
| `qualification-summary` | qualification-3.0 | structural | item element of `affiliations`, `timeline` | same |  |
| `qualifications` | activities-3.0 | structural | container (affiliation collection); its `affiliation-group`s and items are read | same |  |
| `read-date` | common-3.0 | mapped | `notifications.read_date` | same; `{"value": epoch ms}` |  |
| `record` | record-3.0 | structural | item element of `history`, `person` | same |  |
| `record-summary` | summary-3.0 | structural | item element of `summary`, `summary_items` | same |  |
| `region` | common-3.0 | mapped | `affiliations.org_region`, `fundings.org_region`, `peer_reviews.convening_org_region`, `research_resource_hosts.host_region` (+2) | same | yes |
| `research-resource` | research-resource-3.0 | structural | item element of `research_resources`, `timeline` | same | yes |
| `research-resource-summary` | research-resource-3.0 | structural | item element of `research_resources`, `timeline` | same |  |
| `research-resources` | activities-3.0, summary-3.0 | structural | path step in `summary` | same |  |
| `researcher-url` | researcher-url-3.0 | structural | item element of `person_items`, `timeline` | same | yes |
| `researcher-urls` | researcher-url-3.0 | structural | item element of `person_items`, `timeline` | same |  |
| `resource-item` | research-resource-3.0 | structural | item element of `research_resource_items` | same | yes |
| `resource-items` | research-resource-3.0 | structural | item element of `research_resource_items` | no member: `resource-item` sits in the research resource | yes |
| `resource-name` | research-resource-3.0 | mapped | `research_resource_items.resource_name` | same | yes |
| `resource-type` | research-resource-3.0 | mapped | `research_resource_items.resource_type` | same | yes |
| `response-code` | error-3.0 | mapped | `errors.response_code` | same |  |
| `result` | search-3.0 | structural | item element of `search_results` | same |  |
| `review-completion-date` | peer-review-3.0 | structural | path step in `peer_reviews` | same | yes |
| `review-group-id` | peer-review-3.0 | mapped | `peer_reviews.review_group_id` | same | yes |
| `review-identifiers` | peer-review-3.0 | structural | item element of `external_ids` | same | yes |
| `review-type` | peer-review-3.0 | mapped | `peer_reviews.review_type` | same | yes |
| `review-url` | peer-review-3.0 | mapped | `peer_reviews.review_url` | same | yes |
| `reviewer-role` | peer-review-3.0 | mapped | `peer_reviews.reviewer_role` | same | yes |
| `role` | summary-3.0 | mapped | `summary_items.role` | same |  |
| `role-title` | common-3.0 | mapped | `affiliations.role_title`, `timeline.label` | same | yes |
| `search` | search-3.0 | structural | item element of `search_results` | same |  |
| `self-asserted-count` | summary-3.0 | mapped | `summary.funding_self_asserted_count`, `summary.peer_review_self_asserted_count`, `summary.research_resource_self_asserted_count`, `summary.work_self_asserted_count` | same |  |
| `sent-date` | common-3.0 | mapped | `notifications.sent_date` | same; `{"value": epoch ms}` |  |
| `service` | service-3.0 | structural | item element of `affiliations`, `timeline` | same | yes |
| `service-summary` | service-3.0 | structural | item element of `affiliations`, `timeline` | same |  |
| `services` | activities-3.0 | structural | container (affiliation collection); its `affiliation-group`s and items are read | same |  |
| `short-description` | funding-3.0, work-3.0 | mapped | `fundings.short_description`, `works.short_description` | same | yes |
| `source` | common-3.0 | structural | path step in `affiliations`, `fundings`, `group_ids`, `history`, `notifications`, `peer_reviews`, `person_items`, `research_resources`, `timeline`, `works` | same |  |
| `source-client-id` | common-3.0 | structural | path step in `affiliations`, `fundings`, `group_ids`, `history`, `notifications`, `peer_reviews`, `person_items`, `research_resources`, `works` | same |  |
| `source-name` | common-3.0 | mapped | `affiliations.source_name`, `fundings.source_name`, `group_ids.source_name`, `history.source_name` (+6) | same |  |
| `source-orcid` | common-3.0 | structural | path step in `affiliations`, `fundings`, `group_ids`, `history`, `notifications`, `peer_reviews`, `person_items`, `research_resources`, `works` | same |  |
| `start-date` | common-3.0 | structural | path step in `affiliations`, `fundings`, `research_resources`, `summary_items` | same | yes |
| `subject` | notification-custom-3.0 | mapped | `notifications.subject` | same |  |
| `subject-container-name` | peer-review-3.0 | mapped | `peer_reviews.subject_container_name` | same | yes |
| `subject-external-identifier` | peer-review-3.0 | structural | item element of `external_ids`; path step in `peer_reviews` | same | yes |
| `subject-name` | peer-review-3.0 | structural | path step in `peer_reviews`, `timeline` | same | yes |
| `subject-type` | peer-review-3.0 | mapped | `peer_reviews.subject_type` | same | yes |
| `subject-url` | peer-review-3.0 | mapped | `peer_reviews.subject_url` | same | yes |
| `submission-date` | history-3.0 | mapped | `history.submission_date` | same; `{"value": epoch ms}` |  |
| `subtitle` | common-3.0 | mapped | `peer_reviews.subject_subtitle`, `works.subtitle` | same | yes |
| `title` | common-3.0, funding-3.0, research-resource-3.0, work-3.0 | mapped | `fundings.title`, `peer_reviews.subject_name`, `research_resources.title`, `timeline.label` (+1) (also declared as a container elsewhere) | same | yes |
| `total` | group-id-3.0, summary-3.0 | mapped | `group_ids.total`, `summary.peer_review_total` | same |  |
| `@transient` | common-3.0 | not mapped | not mapped: always `true` (pattern restriction); it marks `external-id-normalized` and its error as populated by ORCID, not the source. The values themselves are mapped | member (of the `{"value"}` object for `external-id-normalized`) |  |
| `translated-title` | common-3.0 | mapped | `fundings.translated_title`, `peer_reviews.subject_translated_name`, `research_resources.translated_title`, `works.translated_title` | same | yes |
| `type` | group-id-3.0, funding-3.0, work-3.0, summary-3.0 | mapped | `fundings.type`, `group_ids.type`, `summary_items.type`, `works.type` | same | yes |
| `uri` | common-3.0, notification-permission-3.0 | mapped | `contributors.contributor_orcid_uri`, `notifications.authorization_url`, `person.orcid_uri`, `search_results.orcid_uri` (+1) | same | yes |
| `url` | common-3.0, researcher-url-3.0, summary-3.0 | mapped | `affiliations.url`, `fundings.url`, `person_items.url`, `research_resource_items.url` (+3) | same | yes |
| `url-name` | researcher-url-3.0 | mapped | `person_items.value`, `timeline.label` | same | yes |
| `user-message` | error-3.0 | mapped | `errors.user_message`; errors.orcid / errors.primary_orcid (iD in the message) | same |  |
| `validated` | summary-3.0 | mapped | `summary_items.validated` | same |  |
| `validated-count` | summary-3.0 | mapped | `summary.funding_validated_count`, `summary.research_resource_validated_count`, `summary.work_validated_count` | same |  |
| `value` | summary-3.0 | mapped | `summary.email_domains`, `summary_items.value` | same |  |
| `verification-date` | summary-3.0 | mapped | `summary_items.verification_date` | same; `{"value": epoch ms}` |  |
| `@verified` | email-3.0 | mapped | `person_items.verified` | member |  |
| `verified-email` | history-3.0 | mapped | `history.verified_email` | same |  |
| `verified-primary-email` | history-3.0 | mapped | `history.verified_primary_email` | same |  |
| `@visibility` | common-3.0, history-3.0, personal-details-3.0 | mapped | `affiliations.visibility`, `fundings.visibility`, `history.visibility`, `peer_reviews.visibility` (+6) | member | yes |
| `work` | work-3.0 | structural | item element of `timeline`, `works` | same | yes |
| `work-summary` | work-3.0 | structural | item element of `timeline`, `works` | same |  |
| `works` | activities-3.0, summary-3.0 | structural | path step in `summary` | same |  |
| `year` | common-3.0 | mapped | `affiliations.end_year`, `affiliations.start_year`, `fundings.end_year`, `fundings.start_year` (+6) | same | yes |
