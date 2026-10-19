#include "orcid/sections.hpp"

#include "orcid/id.hpp"
#include "orcid/json.hpp"
#include "orcid/json_map.hpp"
#include "orcid/sha256.hpp"
#include "orcid/xml.hpp"

#include <algorithm>
#include <cstring>
#include <unordered_map>

namespace orcid {

// ---------------------------------------------------------------------------
// Section definitions
//
// Derived from the ORCID message schema v3.0 (reference/orcid-model/record_3.0,
// common_3.0, summary_3.0, notification_3.0 and group-id-3.0).
// docs/REFERENCE.md documents every column.
//
// Item patterns: local-name suffix of the element stack, '/'-separated; a
// leading '^' anchors the pattern at the document element, i.e. the whole
// stack must equal it ('^person' is a person document, '^record/person' the
// person of a record document, and nothing else). Items that the XSDs only
// allow at a fixed place (the record's person and history, the API messages)
// are anchored, so an element of the same local name elsewhere (another
// namespace, a full activity document) never becomes an item.
// Column paths: element path relative to the item, optional final '@attr',
// alternatives separated by '|' (the first one present in the document wins);
// '$name' selects a computed value. Text is taken from leaf elements, trimmed;
// empty text and repeated elements after the first are ignored.
// A leading '*' joins every non-empty match instead (JOIN_SEPARATOR, document
// order); a leading '#' counts the matching elements. An alternative starting
// with '^' is relative to the document element (e.g. '^@num-found'), first
// value wins, and fills the column of every row of that document.
// ---------------------------------------------------------------------------

namespace {

struct ItemDef {
	const char *section;
	const char *pattern;
};

struct ColumnDef {
	const char *section;
	const char *column;
	ColType type;
	const char *path;
};

constexpr ColType V = ColType::Varchar;
constexpr ColType I = ColType::Integer;
constexpr ColType B = ColType::Bigint;
constexpr ColType L = ColType::Boolean;

const ItemDef kItems[] = {
    {"works", "work-summary"},
    {"works", "work"},
    {"fundings", "funding-summary"},
    {"fundings", "funding"},
    {"affiliations", "distinction-summary"},
    {"affiliations", "distinction"},
    {"affiliations", "education-summary"},
    {"affiliations", "education"},
    {"affiliations", "employment-summary"},
    {"affiliations", "employment"},
    {"affiliations", "invited-position-summary"},
    {"affiliations", "invited-position"},
    {"affiliations", "membership-summary"},
    {"affiliations", "membership"},
    {"affiliations", "qualification-summary"},
    {"affiliations", "qualification"},
    {"affiliations", "service-summary"},
    {"affiliations", "service"},
    {"peer_reviews", "peer-review-summary"},
    {"peer_reviews", "peer-review"},
    {"research_resources", "research-resource-summary"},
    {"research_resources", "research-resource"},
    {"research_resource_items", "resource-items/resource-item"},
    {"contributors", "contributors/contributor"},
    {"external_ids", "external-ids/external-id"},
    {"external_ids", "review-identifiers/external-id"},
    {"external_ids", "subject-external-identifier"},
    {"person", "^person"},        // person-3.0 document (/person)
    {"person", "^record/person"}, // record-3.0: record/person:person only
    {"person", "^personal-details"},
    {"person_items", "other-names/other-name"},
    {"person_items", "^other-name"},
    {"person_items", "researcher-urls/researcher-url"},
    {"person_items", "^researcher-url"},
    {"person_items", "emails/email"},
    {"person_items", "^email"},
    {"person_items", "addresses/address"},
    {"person_items", "^address"},
    {"person_items", "keywords/keyword"},
    {"person_items", "^keyword"},
    {"person_items", "external-identifiers/external-identifier"},
    {"person_items", "^external-identifier"},
    {"history", "^history"},
    {"history", "^record/history"},
    {"timeline", "work-summary"},
    {"timeline", "work"},
    {"timeline", "funding-summary"},
    {"timeline", "funding"},
    {"timeline", "distinction-summary"},
    {"timeline", "distinction"},
    {"timeline", "education-summary"},
    {"timeline", "education"},
    {"timeline", "employment-summary"},
    {"timeline", "employment"},
    {"timeline", "invited-position-summary"},
    {"timeline", "invited-position"},
    {"timeline", "membership-summary"},
    {"timeline", "membership"},
    {"timeline", "qualification-summary"},
    {"timeline", "qualification"},
    {"timeline", "service-summary"},
    {"timeline", "service"},
    {"timeline", "peer-review-summary"},
    {"timeline", "peer-review"},
    {"timeline", "research-resource-summary"},
    {"timeline", "research-resource"},
    {"timeline", "other-names/other-name"},
    {"timeline", "researcher-urls/researcher-url"},
    {"timeline", "emails/email"},
    {"timeline", "addresses/address"},
    {"timeline", "keywords/keyword"},
    {"timeline", "external-identifiers/external-identifier"},
    {"timeline", "person/name"},
    {"timeline", "personal-details/name"},
    {"timeline", "person/biography"},
    {"timeline", "personal-details/biography"},
    // API messages outside the record (summary_3.0, search, error, notification_3.0, group-id-3.0).
    {"summary", "^record-summary"},
    {"summary_items", "^record-summary/external-identifiers/external-identifier"},
    {"summary_items", "^record-summary/employments/employment"},
    {"summary_items", "^record-summary/professional-activities/professional-activity"},
    {"summary_items", "^record-summary/education-qualifications/education-qualification"},
    {"summary_items", "^record-summary/email-domains/email-domain"},
    {"search_results", "^search/result"},
    {"search_results", "^expanded-search/expanded-result"},
    {"errors", "^error"},
    {"errors", "^bulk/error"},
    {"notifications", "^notification"},
    {"notifications", "^notifications/notification"},
    {"group_ids", "^group-id-record"},
    {"group_ids", "^group-id-records/group-id-record"},
    // research-resource-3.0.xsd: proposal/hosts and resource-item/hosts (last, so
    // that the earlier sections keep their order)
    {"research_resource_hosts", "hosts/organization"},
};

const ColumnDef kColumns[] = {
    // works: the first 18 columns are the original contract; keep their order.
    {"works", "orcid", V, "$orcid"},
    {"works", "put_code", B, "@put-code"},
    {"works", "group_index", I, "$group"},
    {"works", "display_index", I, "@display-index"},
    {"works", "title", V, "title/title"},
    {"works", "subtitle", V, "title/subtitle"},
    {"works", "journal", V, "journal-title"},
    {"works", "type", V, "type"},
    {"works", "pub_year", I, "publication-date/year"},
    {"works", "pub_month", I, "publication-date/month"},
    {"works", "pub_day", I, "publication-date/day"},
    {"works", "doi", V, "$doi"},
    {"works", "url", V, "url"},
    {"works", "source_name", V, "source/source-name"},
    {"works", "visibility", V, "@visibility"},
    {"works", "created", V, "created-date"},
    {"works", "last_modified", V, "last-modified-date"},
    {"works", "source_hash", V, "$hash"},
    {"works", "translated_title", V, "title/translated-title"},
    {"works", "translated_title_language", V, "title/translated-title@language-code"},
    {"works", "short_description", V, "short-description"},
    {"works", "citation_type", V, "citation/citation-type"},
    {"works", "citation_value", V, "citation/citation-value"},
    {"works", "language_code", V, "language-code"},
    {"works", "country", V, "country"},
    {"works", "path", V, "@path"},
    {"works", "source_client_id", V, "source/source-client-id/path"},
    {"works", "source_orcid", V, "source/source-orcid/path"},
    {"works", "assertion_origin_name", V, "source/assertion-origin-name"},
    {"works", "assertion_origin_orcid", V, "source/assertion-origin-orcid/path"},
    {"works", "assertion_origin_client_id", V, "source/assertion-origin-client-id/path"},
    {"works", "item", V, "$item"},

    {"fundings", "orcid", V, "$orcid"},
    {"fundings", "put_code", B, "@put-code"},
    {"fundings", "group_index", I, "$group"},
    {"fundings", "display_index", I, "@display-index"},
    {"fundings", "title", V, "title/title"},
    {"fundings", "translated_title", V, "title/translated-title"},
    {"fundings", "translated_title_language", V, "title/translated-title@language-code"},
    {"fundings", "type", V, "type"},
    {"fundings", "organization_defined_type", V, "organization-defined-type"},
    {"fundings", "short_description", V, "short-description"},
    {"fundings", "amount", V, "amount"},
    {"fundings", "currency", V, "amount@currency-code"},
    {"fundings", "start_year", I, "start-date/year"},
    {"fundings", "start_month", I, "start-date/month"},
    {"fundings", "start_day", I, "start-date/day"},
    {"fundings", "end_year", I, "end-date/year"},
    {"fundings", "end_month", I, "end-date/month"},
    {"fundings", "end_day", I, "end-date/day"},
    {"fundings", "org_name", V, "organization/name"},
    {"fundings", "org_city", V, "organization/address/city"},
    {"fundings", "org_region", V, "organization/address/region"},
    {"fundings", "org_country", V, "organization/address/country"},
    {"fundings", "org_disambiguated_id", V, "organization/disambiguated-organization/disambiguated-organization-identifier"},
    {"fundings", "org_disambiguation_source", V, "organization/disambiguated-organization/disambiguation-source"},
    {"fundings", "url", V, "url"},
    {"fundings", "visibility", V, "@visibility"},
    {"fundings", "path", V, "@path"},
    {"fundings", "source_name", V, "source/source-name"},
    {"fundings", "source_client_id", V, "source/source-client-id/path"},
    {"fundings", "source_orcid", V, "source/source-orcid/path"},
    {"fundings", "assertion_origin_name", V, "source/assertion-origin-name"},
    {"fundings", "created", V, "created-date"},
    {"fundings", "last_modified", V, "last-modified-date"},
    {"fundings", "item", V, "$item"},
    {"fundings", "source_hash", V, "$hash"},
    // appended (CROSSCHECK #10): the assertion origin, as works read it
    {"fundings", "assertion_origin_orcid", V, "source/assertion-origin-orcid/path"},
    {"fundings", "assertion_origin_client_id", V, "source/assertion-origin-client-id/path"},

    {"affiliations", "orcid", V, "$orcid"},
    {"affiliations", "affiliation_type", V, "$kind"},
    {"affiliations", "put_code", B, "@put-code"},
    {"affiliations", "group_index", I, "$group"},
    {"affiliations", "display_index", I, "@display-index"},
    {"affiliations", "department", V, "department-name"},
    {"affiliations", "role_title", V, "role-title"},
    {"affiliations", "start_year", I, "start-date/year"},
    {"affiliations", "start_month", I, "start-date/month"},
    {"affiliations", "start_day", I, "start-date/day"},
    {"affiliations", "end_year", I, "end-date/year"},
    {"affiliations", "end_month", I, "end-date/month"},
    {"affiliations", "end_day", I, "end-date/day"},
    {"affiliations", "org_name", V, "organization/name"},
    {"affiliations", "org_city", V, "organization/address/city"},
    {"affiliations", "org_region", V, "organization/address/region"},
    {"affiliations", "org_country", V, "organization/address/country"},
    {"affiliations", "org_disambiguated_id", V, "organization/disambiguated-organization/disambiguated-organization-identifier"},
    {"affiliations", "org_disambiguation_source", V, "organization/disambiguated-organization/disambiguation-source"},
    {"affiliations", "url", V, "url"},
    {"affiliations", "visibility", V, "@visibility"},
    {"affiliations", "path", V, "@path"},
    {"affiliations", "source_name", V, "source/source-name"},
    {"affiliations", "source_client_id", V, "source/source-client-id/path"},
    {"affiliations", "source_orcid", V, "source/source-orcid/path"},
    {"affiliations", "assertion_origin_name", V, "source/assertion-origin-name"},
    {"affiliations", "created", V, "created-date"},
    {"affiliations", "last_modified", V, "last-modified-date"},
    {"affiliations", "item", V, "$item"},
    {"affiliations", "source_hash", V, "$hash"},
    // appended (CROSSCHECK #10): the assertion origin, as works read it
    {"affiliations", "assertion_origin_orcid", V, "source/assertion-origin-orcid/path"},
    {"affiliations", "assertion_origin_client_id", V, "source/assertion-origin-client-id/path"},

    {"peer_reviews", "orcid", V, "$orcid"},
    {"peer_reviews", "put_code", B, "@put-code"},
    {"peer_reviews", "group_index", I, "$group"},
    {"peer_reviews", "display_index", I, "@display-index"},
    {"peer_reviews", "reviewer_role", V, "reviewer-role"},
    {"peer_reviews", "review_type", V, "review-type"},
    {"peer_reviews", "review_url", V, "review-url"},
    {"peer_reviews", "completion_year", I, "completion-date/year|review-completion-date/year"},
    {"peer_reviews", "completion_month", I, "completion-date/month|review-completion-date/month"},
    {"peer_reviews", "completion_day", I, "completion-date/day|review-completion-date/day"},
    {"peer_reviews", "review_group_id", V, "review-group-id"},
    {"peer_reviews", "subject_type", V, "subject-type"},
    {"peer_reviews", "subject_name", V, "subject-name/title"},
    {"peer_reviews", "subject_subtitle", V, "subject-name/subtitle"},
    {"peer_reviews", "subject_translated_name", V, "subject-name/translated-title"},
    {"peer_reviews", "subject_container_name", V, "subject-container-name"},
    {"peer_reviews", "subject_url", V, "subject-url"},
    {"peer_reviews", "subject_id_type", V, "subject-external-identifier/external-id-type"},
    {"peer_reviews", "subject_id_value", V, "subject-external-identifier/external-id-value"},
    {"peer_reviews", "convening_org_name", V, "convening-organization/name"},
    {"peer_reviews", "convening_org_city", V, "convening-organization/address/city"},
    {"peer_reviews", "convening_org_region", V, "convening-organization/address/region"},
    {"peer_reviews", "convening_org_country", V, "convening-organization/address/country"},
    {"peer_reviews", "convening_org_disambiguated_id", V, "convening-organization/disambiguated-organization/disambiguated-organization-identifier"},
    {"peer_reviews", "convening_org_disambiguation_source", V, "convening-organization/disambiguated-organization/disambiguation-source"},
    {"peer_reviews", "visibility", V, "@visibility"},
    {"peer_reviews", "path", V, "@path"},
    {"peer_reviews", "source_name", V, "source/source-name"},
    {"peer_reviews", "source_client_id", V, "source/source-client-id/path"},
    {"peer_reviews", "source_orcid", V, "source/source-orcid/path"},
    {"peer_reviews", "assertion_origin_name", V, "source/assertion-origin-name"},
    {"peer_reviews", "created", V, "created-date"},
    {"peer_reviews", "last_modified", V, "last-modified-date"},
    {"peer_reviews", "item", V, "$item"},
    {"peer_reviews", "source_hash", V, "$hash"},
    // after source_hash: added with the writers, which need the language a
    // subject translated-title must carry (common:string-with-lang-code).
    {"peer_reviews", "subject_translated_name_language", V, "subject-name/translated-title@language-code"},
    // appended (CROSSCHECK #10): the assertion origin, as works read it
    {"peer_reviews", "assertion_origin_orcid", V, "source/assertion-origin-orcid/path"},
    {"peer_reviews", "assertion_origin_client_id", V, "source/assertion-origin-client-id/path"},

    {"research_resources", "orcid", V, "$orcid"},
    {"research_resources", "put_code", B, "@put-code"},
    {"research_resources", "group_index", I, "$group"},
    {"research_resources", "display_index", I, "@display-index"},
    {"research_resources", "title", V, "proposal/title/title"},
    {"research_resources", "translated_title", V, "proposal/title/translated-title"},
    {"research_resources", "translated_title_language", V, "proposal/title/translated-title@language-code"},
    {"research_resources", "host_name", V, "proposal/hosts/organization/name"},
    {"research_resources", "host_city", V, "proposal/hosts/organization/address/city"},
    {"research_resources", "host_region", V, "proposal/hosts/organization/address/region"},
    {"research_resources", "host_country", V, "proposal/hosts/organization/address/country"},
    {"research_resources", "host_disambiguated_id", V, "proposal/hosts/organization/disambiguated-organization/disambiguated-organization-identifier"},
    {"research_resources", "host_disambiguation_source", V, "proposal/hosts/organization/disambiguated-organization/disambiguation-source"},
    {"research_resources", "start_year", I, "proposal/start-date/year"},
    {"research_resources", "start_month", I, "proposal/start-date/month"},
    {"research_resources", "start_day", I, "proposal/start-date/day"},
    {"research_resources", "end_year", I, "proposal/end-date/year"},
    {"research_resources", "end_month", I, "proposal/end-date/month"},
    {"research_resources", "end_day", I, "proposal/end-date/day"},
    {"research_resources", "url", V, "proposal/url"},
    {"research_resources", "visibility", V, "@visibility"},
    {"research_resources", "path", V, "@path"},
    {"research_resources", "source_name", V, "source/source-name"},
    {"research_resources", "source_client_id", V, "source/source-client-id/path"},
    {"research_resources", "source_orcid", V, "source/source-orcid/path"},
    {"research_resources", "assertion_origin_name", V, "source/assertion-origin-name"},
    {"research_resources", "created", V, "created-date"},
    {"research_resources", "last_modified", V, "last-modified-date"},
    {"research_resources", "item", V, "$item"},
    {"research_resources", "source_hash", V, "$hash"},
    // appended (CROSSCHECK #10): the assertion origin, as works read it
    {"research_resources", "assertion_origin_orcid", V, "source/assertion-origin-orcid/path"},
    {"research_resources", "assertion_origin_client_id", V, "source/assertion-origin-client-id/path"},

    {"research_resource_items", "orcid", V, "$orcid"},
    {"research_resource_items", "owner_put_code", B, "$owner_put_code"},
    {"research_resource_items", "ordinal", I, "$ordinal"},
    {"research_resource_items", "resource_name", V, "resource-name"},
    {"research_resource_items", "resource_type", V, "resource-type"},
    {"research_resource_items", "host_name", V, "hosts/organization/name"},
    {"research_resource_items", "host_city", V, "hosts/organization/address/city"},
    {"research_resource_items", "host_region", V, "hosts/organization/address/region"},
    {"research_resource_items", "host_country", V, "hosts/organization/address/country"},
    {"research_resource_items", "host_disambiguated_id", V, "hosts/organization/disambiguated-organization/disambiguated-organization-identifier"},
    {"research_resource_items", "host_disambiguation_source", V, "hosts/organization/disambiguated-organization/disambiguation-source"},
    {"research_resource_items", "url", V, "url"},
    {"research_resource_items", "source_hash", V, "$hash"},

    {"contributors", "orcid", V, "$orcid"},
    {"contributors", "owner", V, "$owner"},
    {"contributors", "owner_put_code", B, "$owner_put_code"},
    {"contributors", "ordinal", I, "$ordinal"},
    {"contributors", "contributor_orcid", V, "contributor-orcid/path"},
    {"contributors", "contributor_orcid_uri", V, "contributor-orcid/uri"},
    {"contributors", "credit_name", V, "credit-name"},
    {"contributors", "email", V, "contributor-email"},
    {"contributors", "sequence", V, "contributor-attributes/contributor-sequence"},
    {"contributors", "role", V, "contributor-attributes/contributor-role"},
    {"contributors", "source_hash", V, "$hash"},
    // appended (CROSSCHECK #10): common:orcid-id's host, as summary and search_results read it
    {"contributors", "contributor_orcid_host", V, "contributor-orcid/host"},

    {"external_ids", "orcid", V, "$orcid"},
    {"external_ids", "owner", V, "$owner"},
    {"external_ids", "owner_put_code", B, "$owner_put_code"},
    {"external_ids", "group_index", I, "$group"},
    {"external_ids", "container", V, "$container"},
    {"external_ids", "type", V, "external-id-type"},
    {"external_ids", "value", V, "external-id-value"},
    {"external_ids", "normalized", V, "external-id-normalized"},
    {"external_ids", "normalized_error", V, "external-id-normalized-error/error-message"},
    {"external_ids", "url", V, "external-id-url"},
    {"external_ids", "relationship", V, "external-id-relationship"},
    {"external_ids", "source_hash", V, "$hash"},
    // appended (CROSSCHECK #10): the code of ORCID's normalization error
    {"external_ids", "normalized_error_code", V, "external-id-normalized-error/error-code"},

    {"person", "orcid", V, "$orcid"},
    {"person", "given_names", V, "name/given-names"},
    {"person", "family_name", V, "name/family-name"},
    {"person", "credit_name", V, "name/credit-name"},
    {"person", "name_visibility", V, "name@visibility"},
    {"person", "biography", V, "biography/content"},
    {"person", "biography_visibility", V, "biography@visibility"},
    {"person", "last_modified", V, "last-modified-date"},
    {"person", "name_last_modified", V, "name/last-modified-date"},
    {"person", "biography_last_modified", V, "biography/last-modified-date"},
    {"person", "path", V, "@path"},
    {"person", "item", V, "$item"},
    {"person", "source_hash", V, "$hash"},
    // appended (CROSSCHECK #10): the record's orcid-identifier (record-3.0.xsd);
    // NULL for a standalone person or personal-details document
    {"person", "orcid_uri", V, "^orcid-identifier/uri"},
    {"person", "orcid_host", V, "^orcid-identifier/host"},

    {"person_items", "orcid", V, "$orcid"},
    {"person_items", "kind", V, "$item"},
    {"person_items", "put_code", B, "@put-code"},
    {"person_items", "display_index", I, "@display-index"},
    {"person_items", "value", V, "content|email|country|external-id-value|url-name"},
    {"person_items", "url", V, "url|external-id-url"},
    {"person_items", "type", V, "external-id-type"},
    {"person_items", "relationship", V, "external-id-relationship"},
    {"person_items", "verified", L, "@verified"},
    {"person_items", "primary", L, "@primary"},
    {"person_items", "visibility", V, "@visibility"},
    {"person_items", "path", V, "@path"},
    {"person_items", "source_name", V, "source/source-name"},
    {"person_items", "source_client_id", V, "source/source-client-id/path"},
    {"person_items", "source_orcid", V, "source/source-orcid/path"},
    {"person_items", "created", V, "created-date"},
    {"person_items", "last_modified", V, "last-modified-date"},
    {"person_items", "source_hash", V, "$hash"},
    // appended (CROSSCHECK #10): the assertion origin, and the normalized form
    // of a person external identifier (common:external-id), as external_ids reads it
    {"person_items", "assertion_origin_name", V, "source/assertion-origin-name"},
    {"person_items", "assertion_origin_orcid", V, "source/assertion-origin-orcid/path"},
    {"person_items", "assertion_origin_client_id", V, "source/assertion-origin-client-id/path"},
    {"person_items", "normalized", V, "external-id-normalized"},
    {"person_items", "normalized_error", V, "external-id-normalized-error/error-message"},
    {"person_items", "normalized_error_code", V, "external-id-normalized-error/error-code"},

    {"history", "orcid", V, "$orcid"},
    {"history", "creation_method", V, "creation-method"},
    {"history", "completion_date", V, "completion-date"},
    {"history", "submission_date", V, "submission-date"},
    {"history", "last_modified", V, "last-modified-date"},
    {"history", "claimed", L, "claimed"},
    {"history", "deactivation_date", V, "deactivation-date"},
    {"history", "verified_email", L, "verified-email"},
    {"history", "verified_primary_email", L, "verified-primary-email"},
    {"history", "visibility", V, "@visibility"},
    {"history", "source_name", V, "source/source-name"},
    {"history", "source_client_id", V, "source/source-client-id/path"},
    {"history", "source_orcid", V, "source/source-orcid/path"},
    {"history", "source_hash", V, "$hash"},
    // appended (CROSSCHECK #10): the assertion origin of the history's common:source
    {"history", "assertion_origin_name", V, "source/assertion-origin-name"},
    {"history", "assertion_origin_orcid", V, "source/assertion-origin-orcid/path"},
    {"history", "assertion_origin_client_id", V, "source/assertion-origin-client-id/path"},

    // timeline: one row per dated event (an item's creation and, if later, its
    // last modification), across every activity, person item, name and biography.
    {"timeline", "orcid", V, "$orcid"},
    {"timeline", "event_at", V, "$at"},
    {"timeline", "event", V, "$event"},
    {"timeline", "kind", V, "$kind"},
    {"timeline", "put_code", B, "@put-code"},
    {"timeline", "label", V, "title/title|proposal/title/title|role-title|subject-name/title|content|url-name|email|country|external-id-value|given-names"},
    {"timeline", "organization", V, "organization/name|convening-organization/name|proposal/hosts/organization/name"},
    {"timeline", "visibility", V, "@visibility"},
    {"timeline", "source_name", V, "source/source-name"},
    {"timeline", "created", V, "created-date"},
    {"timeline", "last_modified", V, "last-modified-date"},
    {"timeline", "path", V, "@path"},
    {"timeline", "source_hash", V, "$hash"},

    // summary: the record summary (summary-3.0.xsd), one row per document. The
    // per-type counts are ORCID's own (the listed items may be fewer); the listed
    // items themselves are in summary_items.
    {"summary", "orcid", V, "$orcid"},
    {"summary", "orcid_uri", V, "orcid-identifier/uri"},
    {"summary", "orcid_host", V, "orcid-identifier/host"},
    {"summary", "credit_name", V, "credit-name"},
    {"summary", "created", V, "created-date"},
    {"summary", "last_modified", V, "last-modified-date"},
    {"summary", "external_identifier_count", I, "#external-identifiers/external-identifier"},
    {"summary", "external_identifier_types", V, "*external-identifiers/external-identifier/external-id-type"},
    {"summary", "employment_count", I, "employments/count"},
    {"summary", "professional_activity_count", I, "professional-activities/count"},
    {"summary", "education_qualification_count", I, "education-qualifications/count"},
    {"summary", "funding_self_asserted_count", I, "fundings/self-asserted-count"},
    {"summary", "funding_validated_count", I, "fundings/validated-count"},
    {"summary", "work_self_asserted_count", I, "works/self-asserted-count"},
    {"summary", "work_validated_count", I, "works/validated-count"},
    {"summary", "peer_review_publication_grants", I, "peer-reviews/peer-review-publication-grants"},
    {"summary", "peer_review_self_asserted_count", I, "peer-reviews/self-asserted-count"},
    {"summary", "peer_review_total", I, "peer-reviews/total"},
    {"summary", "research_resource_self_asserted_count", I, "research-resources/self-asserted-count"},
    {"summary", "research_resource_validated_count", I, "research-resources/validated-count"},
    {"summary", "email_domain_count", I, "email-domains/count"},
    {"summary", "email_domains", V, "*email-domains/email-domain/value"},
    {"summary", "source_hash", V, "$hash"},

    // summary_items: every item listed in a record summary.
    {"summary_items", "orcid", V, "$orcid"},
    {"summary_items", "kind", V, "$item"},
    {"summary_items", "ordinal", I, "$ordinal"},
    {"summary_items", "put_code", B, "put-code"},
    {"summary_items", "type", V, "type|external-id-type"},
    {"summary_items", "value", V, "external-id-value|value"},
    {"summary_items", "url", V, "url|external-id-url"},
    {"summary_items", "organization_name", V, "organization-name"},
    {"summary_items", "role", V, "role"},
    {"summary_items", "start_year", I, "start-date/year"},
    {"summary_items", "start_month", I, "start-date/month"},
    {"summary_items", "start_day", I, "start-date/day"},
    {"summary_items", "end_year", I, "end-date/year"},
    {"summary_items", "end_month", I, "end-date/month"},
    {"summary_items", "end_day", I, "end-date/day"},
    {"summary_items", "validated", L, "validated"},
    {"summary_items", "verification_date", V, "verification-date"},
    {"summary_items", "created", V, "created-date"},
    {"summary_items", "last_modified", V, "last-modified-date"},
    {"summary_items", "source_hash", V, "$hash"},

    // search_results: search-3.0 (iD only) and expanded-search-3.0 results.
    {"search_results", "num_found", B, "^@num-found"},
    {"search_results", "ordinal", I, "$ordinal"},
    {"search_results", "orcid", V, "orcid-identifier/path|orcid-id"},
    {"search_results", "orcid_uri", V, "orcid-identifier/uri"},
    {"search_results", "orcid_host", V, "orcid-identifier/host"},
    {"search_results", "given_names", V, "given-names"},
    {"search_results", "family_names", V, "family-names"},
    {"search_results", "credit_name", V, "credit-name"},
    {"search_results", "other_names", V, "*other-name"},
    {"search_results", "emails", V, "*email"},
    {"search_results", "institution_names", V, "*institution-name"},
    {"search_results", "item", V, "$item"},
    {"search_results", "source_hash", V, "$hash"},

    // errors: error-3.0, standalone or per item of a bulk response.
    // orcid: the document's iD, else the iD the message names (locked and
    // deprecated records in the Public Data File are error documents:
    // "409 Conflict: The ORCID record is locked ... ORCID <iD>",
    // "301 Moved Permanently: ... Please refer to account: <primary>. ORCID <iD>").
    {"errors", "orcid", V, "$message_orcid"},
    {"errors", "response_code", I, "response-code"},
    {"errors", "developer_message", V, "developer-message"},
    {"errors", "user_message", V, "user-message"},
    {"errors", "error_code", I, "error-code"},
    {"errors", "more_info", V, "more-info"},
    {"errors", "container", V, "$container"},
    {"errors", "position", I, "$position"},
    {"errors", "source_hash", V, "$hash"},
    {"errors", "primary_orcid", V, "$message_primary_orcid"},

    // notifications: notification-permission-3.0 and notification-custom-3.0.
    {"notifications", "put_code", B, "@put-code"},
    {"notifications", "notification_type", V, "notification-type"},
    {"notifications", "subject", V, "notification-subject|subject"},
    {"notifications", "intro", V, "notification-intro"},
    {"notifications", "body_text", V, "body-text"},
    {"notifications", "body_html", V, "body-html"},
    {"notifications", "language", V, "@xml:lang"},
    {"notifications", "authorization_url", V, "authorization-url/uri"},
    {"notifications", "authorization_path", V, "authorization-url/path"},
    {"notifications", "authorization_host", V, "authorization-url/host"},
    {"notifications", "item_count", I, "#items/item"},
    {"notifications", "item_types", V, "*items/item/item-type"},
    {"notifications", "item_names", V, "*items/item/item-name"},
    {"notifications", "item_external_id_types", V, "*items/item/external-id/external-id-type"},
    {"notifications", "item_external_id_values", V, "*items/item/external-id/external-id-value"},
    {"notifications", "created", V, "created-date"},
    {"notifications", "sent_date", V, "sent-date"},
    {"notifications", "read_date", V, "read-date"},
    {"notifications", "actioned_date", V, "actioned-date"},
    {"notifications", "archived_date", V, "archived-date"},
    {"notifications", "source_name", V, "source/source-name"},
    {"notifications", "source_client_id", V, "source/source-client-id/path"},
    {"notifications", "source_orcid", V, "source/source-orcid/path"},
    {"notifications", "source_hash", V, "$hash"},
    // appended (CROSSCHECK #10): the assertion origin, and the rest of each
    // item's common:external-id (joined like the types and values)
    {"notifications", "assertion_origin_name", V, "source/assertion-origin-name"},
    {"notifications", "assertion_origin_orcid", V, "source/assertion-origin-orcid/path"},
    {"notifications", "assertion_origin_client_id", V, "source/assertion-origin-client-id/path"},
    {"notifications", "item_external_id_urls", V, "*items/item/external-id/external-id-url"},
    {"notifications", "item_external_id_relationships", V, "*items/item/external-id/external-id-relationship"},
    {"notifications", "item_external_id_normalized", V, "*items/item/external-id/external-id-normalized"},

    // group_ids: group-id-3.0 records (the source fields are inline, as the
    // record extends common:source-type); total/page/page-size from a
    // group-id-records page.
    {"group_ids", "put_code", B, "@put-code"},
    {"group_ids", "name", V, "name"},
    {"group_ids", "group_id", V, "group-id"},
    {"group_ids", "description", V, "description"},
    {"group_ids", "type", V, "type"},
    {"group_ids", "source_name", V, "source-name|source/source-name"},
    {"group_ids", "source_client_id", V, "source-client-id/path|source/source-client-id/path"},
    {"group_ids", "source_orcid", V, "source-orcid/path|source/source-orcid/path"},
    {"group_ids", "total", I, "^total"},
    {"group_ids", "page", I, "^page"},
    {"group_ids", "page_size", I, "^page-size"},
    {"group_ids", "source_hash", V, "$hash"},

    // research_resource_hosts (CROSSCHECK #9): every host organization of a
    // proposal or resource item (research-resource-3.0.xsd: hosts holds 1 to 10
    // common:organization). The host columns of research_resources and
    // research_resource_items keep the first non-empty value per column.
    // owner_put_code: the research resource (summary or full); container: the
    // element holding `hosts` (proposal, resource-item); item_ordinal: the
    // resource item's ordinal (as research_resource_items.ordinal), NULL for
    // the proposal; ordinal: the host's position in its `hosts`.
    {"research_resource_hosts", "orcid", V, "$orcid"},
    {"research_resource_hosts", "owner_put_code", B, "$owner_put_code"},
    {"research_resource_hosts", "ordinal", I, "$ordinal"},
    {"research_resource_hosts", "host_name", V, "name"},
    {"research_resource_hosts", "host_city", V, "address/city"},
    {"research_resource_hosts", "host_region", V, "address/region"},
    {"research_resource_hosts", "host_country", V, "address/country"},
    {"research_resource_hosts", "host_disambiguated_id", V, "disambiguated-organization/disambiguated-organization-identifier"},
    {"research_resource_hosts", "host_disambiguation_source", V, "disambiguated-organization/disambiguation-source"},
    {"research_resource_hosts", "container", V, "$hosts_parent"},
    {"research_resource_hosts", "item_ordinal", I, "$resource_item_ordinal"},
    {"research_resource_hosts", "source_hash", V, "$hash"},
};

std::vector<std::string> Split(const std::string &s, char sep) {
	std::vector<std::string> out;
	size_t start = 0;
	while (true) {
		size_t pos = s.find(sep, start);
		out.push_back(s.substr(start, pos == std::string::npos ? std::string::npos : pos - start));
		if (pos == std::string::npos) {
			return out;
		}
		start = pos + 1;
	}
}

PathAlt ParseAlt(std::string text) {
	PathAlt alt;
	if (!text.empty() && text[0] == '^') {
		alt.root = true;
		text = text.substr(1);
	}
	std::string elements = text;
	size_t at = text.find('@');
	if (at != std::string::npos) {
		elements = text.substr(0, at);
		alt.attr = text.substr(at + 1);
	}
	if (!elements.empty()) {
		alt.elements = Split(elements, '/');
	}
	return alt;
}

std::vector<SectionSpec> BuildSections() {
	std::vector<SectionSpec> sections;
	auto get = [&](const char *name) -> SectionSpec & {
		for (auto &s : sections) {
			if (s.name == name) {
				return s;
			}
		}
		sections.emplace_back();
		sections.back().name = name;
		return sections.back();
	};
	for (auto &item : kItems) {
		ItemPattern p;
		std::string pattern = item.pattern;
		if (!pattern.empty() && pattern[0] == '^') {
			p.root_only = true;
			pattern = pattern.substr(1);
		}
		p.suffix = Split(pattern, '/');
		get(item.section).items.push_back(std::move(p));
	}
	for (auto &col : kColumns) {
		ColumnSpec c;
		c.name = col.column;
		c.type = col.type;
		std::string path = col.path;
		if (!path.empty() && (path[0] == '*' || path[0] == '#')) {
			c.mode = path[0] == '*' ? ColMode::Join : ColMode::Count;
			path = path.substr(1);
		}
		if (!path.empty() && path[0] == '$') {
			c.special = path;
		} else {
			for (auto &alt : Split(path, '|')) {
				c.alts.push_back(ParseAlt(alt));
			}
		}
		get(col.section).columns.push_back(std::move(c));
	}
	return sections;
}

// ---------------------------------------------------------------------------
// Extraction
// ---------------------------------------------------------------------------

std::string Trim(const std::string &s) {
	size_t b = 0, e = s.size();
	while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) {
		b++;
	}
	while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) {
		e--;
	}
	return s.substr(b, e - b);
}

std::string AsciiLower(std::string s) {
	for (auto &c : s) {
		if (c >= 'A' && c <= 'Z') {
			c = static_cast<char>(c - 'A' + 'a');
		}
	}
	return s;
}

//! The first valid iD in `text` that directly follows `marker` (bare, or as an
//! orcid.org URL; trailing punctuation ignored), canonical; "" if none.
std::string IdAfter(const std::string &text, const std::string &marker) {
	for (size_t pos = text.find(marker); pos != std::string::npos; pos = text.find(marker, pos + 1)) {
		size_t b = pos + marker.size();
		while (b < text.size() && text[b] == ' ') {
			b++;
		}
		size_t e = b;
		while (e < text.size() && text[e] != ' ' && text[e] != '\t' && text[e] != '\n' && text[e] != '\r') {
			e++;
		}
		std::string token = text.substr(b, e - b);
		while (!token.empty() && (token.back() == '.' || token.back() == ',' || token.back() == ';' ||
		                          token.back() == ')' || token.back() == '/')) {
			token.pop_back();
		}
		std::string id;
		if (!token.empty() && NormalizeId(token, id)) {
			return id;
		}
	}
	return std::string();
}

//! First segment of an ORCID element path ("/0000-.../work/1" -> "0000-...").
std::string FirstPathSegment(const std::string &path) {
	size_t start = path.empty() || path[0] != '/' ? 0 : 1;
	size_t end = path.find('/', start);
	return path.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

struct Frame {
	std::string name;
	std::string put_code;
	bool has_put_code = false;
	int ordinal = 1;  // among same-name siblings
	int position = 1; // among all element siblings
	std::vector<std::pair<std::string, int>> child_counts;
};

// Element paths below an item used by the built-in $doi tracking.
const std::vector<std::string> EID = {"external-ids", "external-id"};
const std::vector<std::string> EID_TYPE = {"external-ids", "external-id", "external-id-type"};
const std::vector<std::string> EID_VALUE = {"external-ids", "external-id", "external-id-value"};
const std::vector<std::string> EID_REL = {"external-ids", "external-id", "external-id-relationship"};

bool IsGroup(const std::string &name) {
	return name == "group" || name == "affiliation-group";
}

//! (column, alternative) pairs indexed by the last element name of the path
//! ("" for paths on the item element itself). Count columns are indexed as
//! text (they count elements, whatever their content); root-relative
//! alternatives are kept apart in `root`.
struct PathIndex {
	std::unordered_map<std::string, std::vector<std::pair<size_t, size_t>>> text;
	std::unordered_map<std::string, std::vector<std::pair<size_t, size_t>>> attrs;
	std::vector<std::pair<size_t, size_t>> root;

	explicit PathIndex(const SectionSpec &spec) {
		for (size_t c = 0; c < spec.columns.size(); c++) {
			for (size_t a = 0; a < spec.columns[c].alts.size(); a++) {
				auto &alt = spec.columns[c].alts[a];
				if (alt.root) {
					root.emplace_back(c, a);
					continue;
				}
				std::string key = alt.elements.empty() ? std::string() : alt.elements.back();
				bool as_text = alt.attr.empty() || spec.columns[c].mode == ColMode::Count;
				(as_text ? text : attrs)[key].emplace_back(c, a);
			}
		}
	}
};

//! Reads one document from a pull reader: XmlReader for XML, ElementReader
//! for JSON mapped onto the same element tree (json_map.hpp). Both expose
//! Next / Name / LocalName / Attribute / Text, hence the templates.
class Extractor {
public:
	Extractor(const SectionSpec &spec, const PathIndex &index) : spec_(spec), index_(index) {
		for (size_t c = 0; c < spec.columns.size(); c++) {
			if (spec.columns[c].special == "$doi") {
				doi_col_ = static_cast<int>(c);
			}
			if (spec.columns[c].mode == ColMode::Count) {
				count_cols_.push_back(c);
			}
		}
		counts_.assign(spec.columns.size(), 0);
		root_values_.assign(spec.columns.size(), std::nullopt);
	}

	//! `doc` / `len` are the source bytes, hashed for $hash into `hash` (empty
	//! until first needed, so several documents of one input hash it once).
	template <class Reader>
	void Run(Reader &reader, const char *doc, size_t len, std::string &hash, std::vector<Row> &out) {
		const size_t first_new = out.size();
		std::vector<std::string> item_paths; // @path of each new row, for the $orcid fallback
		for (auto ev = reader.Next(); ev != XmlEvent::End; ev = reader.Next()) {
			if (ev == XmlEvent::StartElement) {
				Start(reader);
			} else if (ev == XmlEvent::Text) {
				text_ += reader.Text();
			} else {
				End(reader, out, item_paths);
			}
		}
		if (!stack_.empty()) {
			throw XmlError("unexpected end of document inside <" + stack_.back().name + ">");
		}
		std::string orcid = !root_orcid_.empty() ? root_orcid_ : identifier_orcid_;
		// $message_orcid / $message_primary_orcid read the message columns of the row.
		int dev_col = -1, user_col = -1;
		for (size_t c = 0; c < spec_.columns.size(); c++) {
			if (spec_.columns[c].name == "developer_message") {
				dev_col = static_cast<int>(c);
			} else if (spec_.columns[c].name == "user_message") {
				user_col = static_cast<int>(c);
			}
		}
		auto message_id = [&](const Row &row, const std::string &marker) {
			for (int c : {dev_col, user_col}) {
				if (c >= 0 && row[c]) {
					std::string id = IdAfter(*row[c], marker);
					if (!id.empty()) {
						return id;
					}
				}
			}
			return std::string();
		};
		for (size_t c = 0; c < spec_.columns.size(); c++) {
			auto &special = spec_.columns[c].special;
			if (special == "$hash" && first_new < out.size()) {
				if (hash.empty()) {
					hash = Sha256Hex(doc, len);
				}
				for (size_t r = first_new; r < out.size(); r++) {
					out[r][c] = hash;
				}
			} else if (special == "$orcid" || special == "$message_orcid") {
				for (size_t r = first_new; r < out.size(); r++) {
					std::string value = !orcid.empty() ? orcid : FirstPathSegment(item_paths[r - first_new]);
					if (value.empty() && special == "$message_orcid") {
						value = message_id(out[r], "ORCID ");
					}
					if (!value.empty()) {
						out[r][c] = value;
					}
				}
			} else if (special == "$message_primary_orcid") {
				for (size_t r = first_new; r < out.size(); r++) {
					std::string value = message_id(out[r], "refer to account:");
					if (!value.empty()) {
						out[r][c] = value;
					}
				}
			}
		}
		// Document-level ('^') values: known only once the whole document is read.
		for (auto &ca : index_.root) {
			const auto &value = root_values_[ca.first];
			if (!value) {
				continue;
			}
			for (size_t r = first_new; r < out.size(); r++) {
				if (!out[r][ca.first]) {
					out[r][ca.first] = value;
				}
			}
		}
		ExpandEvents(out, first_new);
	}

	//! For specs with $at/$event columns: replace each item row by its events, a
	//! 'created' row (at = created) and, when the last modification differs, a
	//! 'modified' row (at = last_modified). Undated items keep one row with NULLs.
	void ExpandEvents(std::vector<Row> &out, size_t first_new) {
		int at = -1, event = -1, created = -1, modified = -1;
		for (size_t c = 0; c < spec_.columns.size(); c++) {
			auto &col = spec_.columns[c];
			if (col.special == "$at") {
				at = static_cast<int>(c);
			} else if (col.special == "$event") {
				event = static_cast<int>(c);
			} else if (col.name == "created") {
				created = static_cast<int>(c);
			} else if (col.name == "last_modified") {
				modified = static_cast<int>(c);
			}
		}
		if (at < 0 || event < 0 || created < 0 || modified < 0) {
			return;
		}
		std::vector<Row> expanded;
		for (size_t r = first_new; r < out.size(); r++) {
			const Row &row = out[r];
			const auto &c = row[created];
			const auto &m = row[modified];
			if (c) {
				expanded.push_back(row);
				expanded.back()[at] = *c;
				expanded.back()[event] = std::string("created");
			}
			if (m && (!c || *m != *c)) {
				expanded.push_back(row);
				expanded.back()[at] = *m;
				expanded.back()[event] = std::string("modified");
			}
			if (!c && !m) {
				expanded.push_back(row);
			}
		}
		out.resize(first_new);
		for (auto &row : expanded) {
			out.push_back(std::move(row));
		}
	}

private:
	template <class Reader>
	void Start(const Reader &reader) {
		std::string name = reader.LocalName();
		int ordinal = 1;
		int position = 1;
		if (!stack_.empty()) {
			auto &counts = stack_.back().child_counts;
			for (auto &kv : counts) {
				position += kv.second;
			}
			bool found = false;
			for (auto &kv : counts) {
				if (kv.first == name) {
					ordinal = ++kv.second;
					found = true;
					break;
				}
			}
			if (!found) {
				counts.emplace_back(name, 1);
			}
		}
		Frame frame;
		frame.name = name;
		frame.ordinal = ordinal;
		frame.position = position;
		frame.has_put_code = reader.Attribute("put-code", frame.put_code);
		stack_.push_back(std::move(frame));
		text_.clear();

		if (stack_.size() == 1) {
			std::string path;
			if (reader.Attribute("path", path)) {
				root_orcid_ = FirstPathSegment(Trim(path));
			}
		}
		for (auto &ca : index_.root) {
			auto &alt = spec_.columns[ca.first].alts[ca.second];
			if (!alt.attr.empty() && !root_values_[ca.first] && RootRelEquals(alt.elements)) {
				std::string value;
				if (reader.Attribute(alt.attr.c_str(), value) && !(value = Trim(value)).empty()) {
					root_values_[ca.first] = value;
				}
			}
		}

		if (item_depth_ < 0) {
			// The pre-3.0 peer-review layout, `group/summary` (local names; read
			// sample peer-reviews-3.0.xml), is outside the v3.0 XSDs, which put
			// peer reviews in `peer-review-group/peer-review-summary`: like
			// yorc-rs, yorc-zig and ORCID's own classes, read nothing inside it
			// (it held 2 external ids no other implementation reads; FINDINGS #3).
			// One length check per element outside items; no v3.0 element is
			// named `summary`.
			if (opaque_depth_ < 0 && name.size() == 7 && name == "summary" && stack_.size() >= 2 &&
			    stack_[stack_.size() - 2].name == "group") {
				opaque_depth_ = static_cast<int>(stack_.size()) - 1;
			}
			if (opaque_depth_ < 0 && MatchesItem()) {
				BeginItem(reader);
			}
			return;
		}
		auto it = index_.attrs.find(name);
		if (it != index_.attrs.end()) {
			for (auto &ca : it->second) {
				auto &col = spec_.columns[ca.first];
				auto &alt = col.alts[ca.second];
				if ((col.mode == ColMode::Join || !row_[ca.first]) && RelEquals(alt.elements)) {
					std::string value;
					if (reader.Attribute(alt.attr.c_str(), value) && !(value = Trim(value)).empty()) {
						Put(ca.first, value);
					}
				}
			}
		}
		if (RelEquals(EID)) {
			eid_type_.clear();
			eid_value_.clear();
			eid_rel_.clear();
		}
	}

	template <class Reader>
	void End(const Reader &reader, std::vector<Row> &out, std::vector<std::string> &item_paths) {
		if (stack_.empty()) {
			throw XmlError("unbalanced end tag </" + reader.Name() + ">");
		}
		if (stack_.back().name != reader.LocalName()) {
			throw XmlError("mismatched end tag </" + reader.Name() + ">, expected </" + stack_.back().name + ">");
		}
		const std::string value = Trim(text_);
		if (identifier_orcid_.empty() && stack_.size() >= 2 && stack_.back().name == "path" &&
		    stack_[stack_.size() - 2].name == "orcid-identifier") {
			identifier_orcid_ = value;
		}
		if (!value.empty()) {
			for (auto &ca : index_.root) {
				auto &alt = spec_.columns[ca.first].alts[ca.second];
				if (alt.attr.empty() && !root_values_[ca.first] && RootRelEquals(alt.elements)) {
					root_values_[ca.first] = value;
				}
			}
		}
		if (item_depth_ >= 0) {
			if (stack_.size() - 1 == static_cast<size_t>(item_depth_)) {
				if (doi_col_ >= 0 && !doi_.empty()) {
					row_[doi_col_] = doi_;
				}
				for (size_t c : count_cols_) {
					row_[c] = std::to_string(counts_[c]);
				}
				out.push_back(std::move(row_));
				item_paths.push_back(item_path_);
				item_depth_ = -1;
			} else {
				auto it = index_.text.find(stack_.back().name);
				if (it != index_.text.end()) {
					for (auto &ca : it->second) {
						auto &col = spec_.columns[ca.first];
						if (col.mode == ColMode::First && (value.empty() || row_[ca.first])) {
							continue;
						}
						if (!RelEquals(col.alts[ca.second].elements)) {
							continue;
						}
						if (col.mode == ColMode::Count) {
							counts_[ca.first]++;
						} else if (!value.empty()) {
							Put(ca.first, value);
						}
					}
				}
				if (doi_col_ >= 0) {
					TrackDoi(value);
				}
			}
		}
		if (opaque_depth_ >= 0 && stack_.size() - 1 == static_cast<size_t>(opaque_depth_)) {
			opaque_depth_ = -1;
		}
		stack_.pop_back();
		text_.clear();
	}

	void TrackDoi(const std::string &value) {
		if (RelEquals(EID_TYPE)) {
			eid_type_ = value;
		} else if (RelEquals(EID_VALUE)) {
			eid_value_ = value;
		} else if (RelEquals(EID_REL)) {
			eid_rel_ = value;
		} else if (RelEquals(EID)) {
			if (doi_.empty() && eid_type_ == "doi" && eid_rel_ == "self" && !eid_value_.empty()) {
				doi_ = AsciiLower(eid_value_);
			}
		}
	}

	bool MatchesItem() const {
		for (auto &p : spec_.items) {
			if (p.root_only && stack_.size() != p.suffix.size()) {
				continue;
			}
			if (p.suffix.size() > stack_.size()) {
				continue;
			}
			bool ok = true;
			size_t base = stack_.size() - p.suffix.size();
			for (size_t i = 0; i < p.suffix.size(); i++) {
				if (stack_[base + i].name != p.suffix[i]) {
					ok = false;
					break;
				}
			}
			if (ok) {
				return true;
			}
		}
		return false;
	}

	//! Stores a non-empty value: first one wins, or appended for ColMode::Join.
	void Put(size_t c, const std::string &value) {
		auto &cell = row_[c];
		if (spec_.columns[c].mode == ColMode::Join && cell) {
			*cell += JOIN_SEPARATOR;
			*cell += value;
		} else if (!cell) {
			cell = value;
		}
	}

	//! True when the element path below the document element equals `rel`.
	bool RootRelEquals(const std::vector<std::string> &rel) const {
		if (stack_.size() != rel.size() + 1) {
			return false;
		}
		for (size_t i = 0; i < rel.size(); i++) {
			if (stack_[i + 1].name != rel[i]) {
				return false;
			}
		}
		return true;
	}

	//! True when the element path below the item equals `rel`.
	bool RelEquals(const std::vector<std::string> &rel) const {
		size_t start = static_cast<size_t>(item_depth_) + 1;
		if (stack_.size() - start != rel.size()) {
			return false;
		}
		for (size_t i = 0; i < rel.size(); i++) {
			if (stack_[start + i].name != rel[i]) {
				return false;
			}
		}
		return true;
	}

	template <class Reader>
	void BeginItem(const Reader &reader) {
		item_depth_ = static_cast<int>(stack_.size()) - 1;
		row_.assign(spec_.columns.size(), std::nullopt);
		std::fill(counts_.begin(), counts_.end(), 0);
		doi_.clear();
		item_path_.clear();
		reader.Attribute("path", item_path_);
		item_path_ = Trim(item_path_);

		const Frame &item = stack_.back();
		const Frame *group = nullptr;
		const Frame *owner = nullptr;
		for (size_t i = stack_.size() - 1; i-- > 0;) {
			if (!group && IsGroup(stack_[i].name)) {
				group = &stack_[i];
			}
			if (!owner && stack_[i].has_put_code) {
				owner = &stack_[i];
			}
		}
		const Frame *group_owner = nullptr;
		if (!owner) {
			for (size_t i = stack_.size() - 1; i-- > 0;) {
				if (IsGroup(stack_[i].name) || stack_[i].name == "peer-review-group") {
					group_owner = &stack_[i];
					break;
				}
			}
		}

		for (size_t c = 0; c < spec_.columns.size(); c++) {
			auto &col = spec_.columns[c];
			const std::string &s = col.special;
			if (s.empty()) {
				for (auto &alt : col.alts) {
					std::string value;
					if (!alt.root && col.mode != ColMode::Count && alt.elements.empty() && !alt.attr.empty() &&
					    reader.Attribute(alt.attr.c_str(), value) && !(value = Trim(value)).empty()) {
						row_[c] = value;
						break;
					}
				}
			} else if (s == "$item") {
				row_[c] = item.name;
			} else if (s == "$kind") {
				const std::string suffix = "-summary";
				std::string kind = item.name;
				if (kind.size() > suffix.size() && kind.compare(kind.size() - suffix.size(), suffix.size(), suffix) == 0) {
					kind.resize(kind.size() - suffix.size());
				}
				row_[c] = kind;
			} else if (s == "$ordinal") {
				row_[c] = std::to_string(item.ordinal);
			} else if (s == "$position") {
				row_[c] = std::to_string(item.position);
			} else if (s == "$container") {
				if (stack_.size() >= 2) {
					row_[c] = stack_[stack_.size() - 2].name;
				}
			} else if (s == "$hosts_parent") {
				// item `hosts/organization`: the element holding `hosts`
				if (stack_.size() >= 3) {
					row_[c] = stack_[stack_.size() - 3].name;
				}
			} else if (s == "$resource_item_ordinal") {
				for (size_t i = stack_.size() - 1; i-- > 0;) {
					if (stack_[i].name == "resource-item") {
						row_[c] = std::to_string(stack_[i].ordinal);
						break;
					}
				}
			} else if (s == "$group") {
				if (group) {
					row_[c] = std::to_string(group->ordinal);
				}
			} else if (s == "$owner") {
				if (owner) {
					row_[c] = owner->name;
				} else if (group_owner) {
					row_[c] = group_owner->name;
				}
			} else if (s == "$owner_put_code") {
				if (owner && !Trim(owner->put_code).empty()) {
					row_[c] = Trim(owner->put_code);
				}
			}
		}
	}

	const SectionSpec &spec_;
	const PathIndex &index_;
	std::vector<Frame> stack_;
	std::string text_;
	std::string root_orcid_;
	std::string identifier_orcid_;
	int item_depth_ = -1;
	int opaque_depth_ = -1; //!< depth of a pre-3.0 `group/summary` being skipped, or -1
	Row row_;
	std::string item_path_;
	int doi_col_ = -1;
	std::string doi_, eid_type_, eid_value_, eid_rel_;
	std::vector<size_t> count_cols_;
	std::vector<int64_t> counts_;   // per column, for ColMode::Count, reset per item
	std::vector<std::optional<std::string>> root_values_; // per column, for '^' alternatives
};

} // namespace

const std::vector<SectionSpec> &Sections() {
	static const std::vector<SectionSpec> sections = BuildSections();
	return sections;
}

const SectionSpec *FindSection(const std::string &name) {
	for (auto &s : Sections()) {
		if (s.name == name) {
			return &s;
		}
	}
	return nullptr;
}

void ExtractSection(const SectionSpec &spec, const char *xml, size_t len, std::vector<Row> &out) {
	// The bytes enter here (files, archive members, lateral strings, changes):
	// check UTF-8 once, as the JSON parser does, so that no malformed text can
	// reach a VARCHAR column. The XML reader itself only copies bytes.
	const size_t valid = Utf8ValidPrefix(xml, len);
	if (valid != len) {
		throw XmlError("invalid UTF-8 at byte " + std::to_string(valid));
	}
	const PathIndex index(spec);
	XmlReader reader(xml, len);
	std::string hash;
	Extractor(spec, index).Run(reader, xml, len, hash, out);
}

DocFormat DetectFormat(const char *data, size_t len) {
	size_t i = 0;
	if (len >= 3 && std::memcmp(data, "\xEF\xBB\xBF", 3) == 0) {
		i = 3;
	}
	while (i < len && (data[i] == ' ' || data[i] == '\t' || data[i] == '\r' || data[i] == '\n')) {
		i++;
	}
	return i < len && (data[i] == '{' || data[i] == '[') ? DocFormat::Json : DocFormat::Xml;
}

void ExtractDocument(const SectionSpec &spec, const char *data, size_t len, DocFormat format,
                     const std::string &json_root, std::vector<Row> &out) {
	if (format == DocFormat::Xml) {
		ExtractSection(spec, data, len, out);
		return;
	}
	const JsonValue doc = ParseJson(data, len);
	std::vector<Element> roots;
	JsonToElements(doc, json_root, roots);
	const PathIndex index(spec);
	std::string hash;
	for (auto &root : roots) {
		ElementReader reader(root);
		Extractor(spec, index).Run(reader, data, len, hash, out);
	}
}

std::optional<int64_t> ParseInteger(const std::string &text, size_t max_digits) {
	if (text.empty() || text.size() > max_digits) {
		return std::nullopt;
	}
	int64_t v = 0;
	for (char c : text) {
		if (c < '0' || c > '9') {
			return std::nullopt;
		}
		v = v * 10 + (c - '0');
	}
	return v;
}

std::optional<bool> ParseBoolean(const std::string &text) {
	if (text == "true" || text == "1") {
		return true;
	}
	if (text == "false" || text == "0") {
		return false;
	}
	return std::nullopt;
}

} // namespace orcid
