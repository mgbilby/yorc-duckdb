// Standalone checks for src/core/ (no DuckDB). Build and run with `make core_test`.
// Run from the repository root; the fixture is read by relative path.

#include "orcid/changes.hpp"
#include "orcid/id.hpp"
#include "orcid/json.hpp"
#include "orcid/json_map.hpp"
#include "orcid/reservoir.hpp"
#include "orcid/sha256.hpp"
#include "orcid/sections.hpp"
#include "orcid/tar.hpp"
#include "orcid/write_tree.hpp"
#include "orcid/writer.hpp"
#include "orcid/xml.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <optional>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

static int failures = 0;

#define CHECK(cond)                                                                                                    \
	do {                                                                                                               \
		if (!(cond)) {                                                                                                 \
			std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                             \
			failures++;                                                                                                \
		}                                                                                                              \
	} while (0)

#define CHECK_EQ(a, b)                                                                                                 \
	do {                                                                                                               \
		auto _a = (a);                                                                                                 \
		auto _b = (b);                                                                                                 \
		if (!(_a == _b)) {                                                                                             \
			std::ostringstream _os;                                                                                    \
			_os << _a << " != " << _b;                                                                                 \
			std::fprintf(stderr, "%s:%d: CHECK_EQ failed: %s == %s (%s)\n", __FILE__, __LINE__, #a, #b,                \
			             _os.str().c_str());                                                                           \
			failures++;                                                                                                \
		}                                                                                                              \
	} while (0)

static std::string Normalized(const char *s) {
	std::string out;
	return orcid::NormalizeId(s, out) ? out : std::string("<invalid>");
}

static void TestId() {
	CHECK_EQ(Normalized("0000-0003-0100-6634"), std::string("0000-0003-0100-6634"));
	CHECK_EQ(Normalized("https://orcid.org/0000-0002-1825-0097"), std::string("0000-0002-1825-0097"));
	CHECK_EQ(Normalized(" 000000021694233x "), std::string("0000-0002-1694-233X"));
	CHECK(!orcid::IsValidId("0000-0003-0100-6635"));
	CHECK(!orcid::IsValidId("0000-0003-0100"));
	CHECK(!orcid::IsValidId("0000-0003-0100-66341"));
	CHECK(!orcid::IsValidId("abcd-0003-0100-6634"));
	CHECK(!orcid::IsValidId(""));
}

static std::string Sha(const std::string &s) {
	return orcid::Sha256Hex(s.data(), s.size());
}

static void TestSha256() {
	// FIPS 180-4 examples, plus lengths around the 55/56/64-byte padding edges.
	CHECK_EQ(Sha(""), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
	CHECK_EQ(Sha("abc"), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
	CHECK_EQ(Sha("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
	         std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
	CHECK_EQ(Sha(std::string(1000000, 'a')),
	         std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

static std::vector<std::string> Events(const std::string &xml) {
	std::vector<std::string> out;
	orcid::XmlReader r(xml.data(), xml.size());
	for (auto ev = r.Next(); ev != orcid::XmlEvent::End; ev = r.Next()) {
		if (ev == orcid::XmlEvent::StartElement) {
			out.push_back("<" + std::string(r.LocalName()));
		} else if (ev == orcid::XmlEvent::EndElement) {
			out.push_back("/" + std::string(r.LocalName()));
		} else {
			out.push_back("T:" + r.Text());
		}
	}
	return out;
}

static void TestXml() {
	auto ev = Events("<?xml version=\"1.0\"?><!DOCTYPE x [<!ENTITY a \"b\">]><!-- c --><a:x k='v'><y/>"
	                 "t&amp;&#65;&#x42;<![CDATA[<raw>]]></a:x>");
	std::vector<std::string> want = {"<x", "<y", "/y", "T:t&AB", "T:<raw>", "/x"};
	CHECK(ev == want);

	const std::string attr_xml = "<a b=\"1 &lt; 2\"/>";
	orcid::XmlReader r(attr_xml.data(), attr_xml.size());
	CHECK(r.Next() == orcid::XmlEvent::StartElement);
	std::string v;
	CHECK(r.Attribute("b", v));
	CHECK_EQ(v, std::string("1 < 2"));

	bool threw = false;
	try {
		Events("<a b=c/>");
	} catch (const orcid::XmlError &) {
		threw = true;
	}
	CHECK(threw);
}

static std::string ReadFile(const std::string &path) {
	std::ifstream in(path, std::ios::binary);
	if (!in.good()) {
		std::fprintf(stderr, "cannot open %s\n", path.c_str());
		failures++;
		return std::string();
	}
	return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

//! Rows of one section for one document, plus column lookup by name.
struct Extracted {
	const orcid::SectionSpec *spec = nullptr;
	std::vector<orcid::Row> rows;

	size_t Col(const char *name) const {
		for (size_t c = 0; c < spec->columns.size(); c++) {
			if (spec->columns[c].name == name) {
				return c;
			}
		}
		std::fprintf(stderr, "no column %s in %s\n", name, spec->name.c_str());
		failures++;
		return 0;
	}
	//! Value of `name` in row `r`, or "<null>".
	std::string Get(size_t r, const char *name) const {
		if (r >= rows.size()) {
			return "<no row>";
		}
		auto &v = rows[r][Col(name)];
		return v ? *v : "<null>";
	}
};

static Extracted Extract(const char *section, const std::string &xml) {
	Extracted e;
	e.spec = orcid::FindSection(section);
	if (!e.spec) {
		std::fprintf(stderr, "no section %s\n", section);
		failures++;
		return e;
	}
	orcid::ExtractSection(*e.spec, xml.data(), xml.size(), e.rows);
	return e;
}

static const std::string SAMPLES = "reference/orcid-model/record_3.0/samples/read_samples/";

static void TestWorksFixture() {
	const std::string xml = ReadFile("test/data/0000-0003-0100-6634.xml");
	auto w = Extract("works", xml);

	// Expected values match test/sql/orcid_works.test.
	CHECK_EQ(w.rows.size(), size_t(382));
	std::set<std::string> groups, dois;
	long long put_code_sum = 0;
	size_t with_doi = 0, with_url = 0, with_journal = 0;
	for (size_t r = 0; r < w.rows.size(); r++) {
		groups.insert(w.Get(r, "group_index"));
		put_code_sum += std::stoll(w.Get(r, "put_code"));
		if (w.Get(r, "doi") != "<null>") {
			with_doi++;
			dois.insert(w.Get(r, "doi"));
		}
		with_url += w.Get(r, "url") != "<null>";
		with_journal += w.Get(r, "journal") != "<null>";
		CHECK_EQ(w.Get(r, "orcid"), std::string("0000-0003-0100-6634"));
		CHECK_EQ(w.Get(r, "item"), std::string("work-summary"));
	}
	CHECK_EQ(groups.size(), size_t(155));
	CHECK_EQ(put_code_sum, 36003218443LL);
	CHECK_EQ(with_doi, size_t(311));
	CHECK_EQ(dois.size(), size_t(226));
	CHECK_EQ(with_url, size_t(84));
	CHECK_EQ(with_journal, size_t(371));
	CHECK_EQ(w.Get(0, "title"), std::string("mgbilby/oga_src: initial oga_src release"));
	CHECK_EQ(w.Get(0, "source_client_id"), std::string("0000-0001-8099-6984"));
	CHECK_EQ(w.Get(0, "source_hash"), std::string("494662244addfd21664a3a56f222a88614b83f61084888ac6162eeb970324801"));
}

//! The literal from test/sql/orcid_works_xml.test that hung the SQL runner,
//! split into one construct per case. Each case prints before it runs, so if
//! one loops, the alarm() in main kills the process with that case on screen.
static void TestXmlEdgeCases() {
	const std::string head = "<r path=\"/x\"><activities-summary><works><group><work-summary put-code=\"7\">";
	const std::string tail = "</work-summary></group></works></activities-summary></r>";
	const std::vector<std::pair<const char *, std::string>> cases = {
	    {"minimal", head + tail},
	    {"amp", head + "<title><title>a &amp; b</title></title>" + tail},
	    {"numeric ref", head + "<title><title>a &#x2014; &#233; b</title></title>" + tail},
	    {"lt", head + "<title><title>a &lt;3</title></title>" + tail},
	    {"cdata", head + "<journal-title><![CDATA[a <b> & c]]></journal-title>" + tail},
	    {"self-closing", head + "<url/>" + tail},
	    {"comment", "<!-- c -->" + head + tail},
	    {"xml decl", "<?xml version=\"1.0\"?>" + head + tail},
	    {"orcid-identifier", "<r><orcid-identifier><path>0000-0002-1825-0097</path></orcid-identifier>"
	                         "<activities-summary><works><group><work-summary put-code=\"7\">" + tail},
	};
	for (auto &c : cases) {
		std::fprintf(stderr, "  xml case: %s\n", c.first);
		auto w = Extract("works", c.second);
		CHECK_EQ(w.rows.size(), size_t(1));
	}
	auto w = Extract("works", head + "<title><title>Fish &amp; Chips &#x2014; &#233;t&#xE9; &lt;3</title></title>"
	                                 "<journal-title><![CDATA[A <raw> & journal]]></journal-title>" + tail);
	CHECK_EQ(w.Get(0, "title"), std::string("Fish & Chips \xE2\x80\x94 \xC3\xA9t\xC3\xA9 <3"));
	CHECK_EQ(w.Get(0, "journal"), std::string("A <raw> & journal"));
	CHECK_EQ(w.Get(0, "orcid"), std::string("x"));

	// A run of '&' with its only ';' at the end: each '&' looks at most 12
	// bytes ahead, so this is linear (it was quadratic: the alarm in main
	// fails a regression). The text is kept as it is.
	std::fprintf(stderr, "  xml case: a run of '&'\n");
	const std::string amps(size_t(1) << 22, '&');
	auto run = Extract("works", head + "<title><title>" + amps + ";</title></title>" + tail);
	CHECK(run.Get(0, "title") == amps + ";");
}

//! The exact literal of the query that hangs under the SQL runner, read from the
//! test file itself, so core and SQL parse identical bytes.
static void TestSqlTestLiteral() {
	const std::string test = ReadFile("test/sql/orcid_works_xml.test");
	const std::string open = "orcid_works_xml('<?xml";
	size_t start = test.find(open);
	CHECK(start != std::string::npos);
	if (start == std::string::npos) {
		return;
	}
	start += std::string("orcid_works_xml('").size();
	size_t end = test.find("');", start);
	CHECK(end != std::string::npos);
	std::string xml = test.substr(start, end - start);
	std::fprintf(stderr, "  xml case: sql test literal (%zu bytes)\n", xml.size());
	auto w = Extract("works", xml);
	CHECK_EQ(w.rows.size(), size_t(1));
	CHECK_EQ(w.Get(0, "orcid"), std::string("0000-0002-1825-0097"));
	CHECK_EQ(w.Get(0, "doi"), std::string("10.1000/xyz"));
	CHECK_EQ(w.Get(0, "pub_month"), std::string("xx"));
}

//! Tar reading over the plain (pax) fixture; the .tar.gz variant needs gzip,
//! which DuckDB provides, so it is covered by the SQL tests.
static void TestTar() {
	const std::string data = ReadFile("test/data/dump/orcid_test_summaries_pax.tar");
	size_t pos = 0;
	orcid::TarReader tar([&](char *buf, size_t n) -> size_t {
		size_t k = std::min(n, data.size() - pos);
		std::memcpy(buf, data.data() + pos, k);
		pos += k;
		return k;
	});
	std::vector<std::string> names;
	size_t works = 0;
	orcid::TarMember m;
	while (tar.Next(m)) {
		names.push_back(m.name);
		if (orcid::GlobMatch("*.xml", m.name)) {
			auto xml = tar.ReadContent();
			works += Extract("works", xml).rows.size();
		}
	}
	CHECK_EQ(names.size(), size_t(4)); // regular files only; directories skipped
	CHECK_EQ(works, size_t(386));      // 382 + 1 + 3
	CHECK(names.back().size() > 100);  // pax long path
	CHECK(orcid::GlobMatch("*/634/*", "ORCID_test_summaries/634/0000-0003-0100-6634.xml"));
	CHECK(!orcid::GlobMatch("*.xml", "ORCID_test_summaries/README.txt"));
	CHECK(orcid::GlobMatch("a?c*", "abcdef"));

	bool threw = false;
	try {
		size_t p2 = 0;
		const std::string junk(1024, 'x');
		orcid::TarReader bad([&](char *buf, size_t n) -> size_t {
			size_t k = std::min(n, junk.size() - p2);
			std::memcpy(buf, junk.data() + p2, k);
			p2 += k;
			return k;
		});
		bad.Next(m);
	} catch (const orcid::TarError &) {
		threw = true;
	}
	CHECK(threw);
}

static void TestTimeline() {
	auto t = Extract("timeline", ReadFile(SAMPLES + "record-3.0.xml"));
	// 19 dated items: 18 'created' (17 on 2001-12-31, the research resource on
	// 2015-06-25) + 'modified' for the name (no created-date) and the research
	// resource (2017-09-08).
	CHECK_EQ(t.rows.size(), size_t(20));
	size_t created = 0, modified = 0;
	std::set<std::string> kinds;
	for (size_t r = 0; r < t.rows.size(); r++) {
		created += t.Get(r, "event") == "created";
		modified += t.Get(r, "event") == "modified";
		kinds.insert(t.Get(r, "kind"));
		if (t.Get(r, "kind") == "research-resource" && t.Get(r, "event") == "modified") {
			CHECK_EQ(t.Get(r, "event_at"), std::string("2017-09-08T13:31:19.987Z"));
			CHECK_EQ(t.Get(r, "label"), std::string("Giant Laser Award"));
		}
		if (t.Get(r, "kind") == "employment") {
			CHECK_EQ(t.Get(r, "label"), std::string("employment:role-title"));
		}
	}
	CHECK_EQ(created, size_t(18));
	CHECK_EQ(modified, size_t(2));
	CHECK_EQ(kinds.size(), size_t(19));
}

static void TestChanges() {
	const std::string old_xml = ReadFile("test/data/changes/old.xml");
	const std::string new_xml = ReadFile("test/data/changes/new.xml");
	std::vector<orcid::ChangeRow> c;
	orcid::DiffDocuments(old_xml.data(), old_xml.size(), new_xml.data(), new_xml.size(), c);
	CHECK_EQ(c.size(), size_t(5));
	if (c.size() == 5) {
		CHECK_EQ(c[0].section + "/" + c[0].change + "/" + c[0].field.value_or("-"), std::string("person/modified/given_names"));
		CHECK_EQ(c[0].old_value.value_or("-"), std::string("give-names"));
		CHECK_EQ(c[1].section + "/" + c[1].kind + "/" + c[1].change, std::string("person_items/keyword/added"));
		CHECK_EQ(c[1].put_code.value_or("-"), std::string("2"));
		CHECK_EQ(c[2].section + "/" + c[2].field.value_or("-"), std::string("works/title"));
		CHECK_EQ(c[2].new_value.value_or("-"), std::string("Revised title"));
		CHECK_EQ(c[3].section + "/" + c[3].change, std::string("fundings/removed"));
		CHECK_EQ(c[4].kind + "/" + c[4].field.value_or("-"), std::string("employment/role_title"));
		CHECK_EQ(c[4].label.value_or("-"), std::string("Senior employment:role-title"));
		CHECK_EQ(c[4].orcid, std::string("8888-8888-8888-8880"));
	}
	std::vector<orcid::ChangeRow> same;
	orcid::DiffDocuments(old_xml.data(), old_xml.size(), old_xml.data(), old_xml.size(), same);
	CHECK_EQ(same.size(), size_t(0));
}

static void TestSampleRecord() {
	const std::string xml = ReadFile(SAMPLES + "record-3.0.xml");
	CHECK_EQ(Extract("works", xml).rows.size(), size_t(1));
	CHECK_EQ(Extract("fundings", xml).rows.size(), size_t(1));
	CHECK_EQ(Extract("peer_reviews", xml).rows.size(), size_t(1));
	CHECK_EQ(Extract("research_resources", xml).rows.size(), size_t(1));
	CHECK_EQ(Extract("research_resource_items", xml).rows.size(), size_t(0));
	CHECK_EQ(Extract("contributors", xml).rows.size(), size_t(0));
	CHECK_EQ(Extract("external_ids", xml).rows.size(), size_t(16));
	CHECK_EQ(Extract("history", xml).rows.size(), size_t(1));

	auto a = Extract("affiliations", xml);
	CHECK_EQ(a.rows.size(), size_t(7));
	std::set<std::string> kinds;
	for (size_t r = 0; r < a.rows.size(); r++) {
		kinds.insert(a.Get(r, "affiliation_type"));
		CHECK_EQ(a.Get(r, "orcid"), std::string("8888-8888-8888-8880"));
	}
	CHECK(kinds == std::set<std::string>({"distinction", "education", "employment", "invited-position", "membership",
	                                      "qualification", "service"}));

	auto p = Extract("person", xml);
	CHECK_EQ(p.rows.size(), size_t(1));
	CHECK_EQ(p.Get(0, "given_names"), std::string("give-names"));
	CHECK_EQ(p.Get(0, "family_name"), std::string("family-name"));
	CHECK_EQ(p.Get(0, "credit_name"), std::string("credit-name"));
	CHECK_EQ(p.Get(0, "biography"), std::string("biography"));
	CHECK_EQ(p.Get(0, "name_visibility"), std::string("public"));

	auto items = Extract("person_items", xml);
	CHECK_EQ(items.rows.size(), size_t(6));
	std::set<std::string> values;
	for (size_t r = 0; r < items.rows.size(); r++) {
		values.insert(items.Get(r, "kind") + "=" + items.Get(r, "value"));
	}
	CHECK(values == std::set<std::string>({"other-name=other-name-1", "researcher-url=url-name-1",
	                                       "email=user1@email.com", "address=US", "keyword=keyword1",
	                                       "external-identifier=value-1"}));
}

static void TestSampleWorkFull() {
	const std::string xml = ReadFile(SAMPLES + "work-full-3.0.xml");
	auto w = Extract("works", xml);
	CHECK_EQ(w.rows.size(), size_t(1));
	CHECK_EQ(w.Get(0, "item"), std::string("work"));
	CHECK_EQ(w.Get(0, "put_code"), std::string("123"));
	CHECK_EQ(w.Get(0, "orcid"), std::string("<null>"));
	CHECK_EQ(w.Get(0, "group_index"), std::string("<null>"));
	CHECK_EQ(w.Get(0, "translated_title"), std::string("common:translated-title"));
	CHECK_EQ(w.Get(0, "translated_title_language"), std::string("en"));
	CHECK_EQ(w.Get(0, "citation_type"), std::string("formatted-unspecified"));
	CHECK_EQ(w.Get(0, "citation_value"), std::string("work:citation"));
	CHECK_EQ(w.Get(0, "short_description"), std::string("work:short-description"));
	CHECK_EQ(w.Get(0, "language_code"), std::string("en"));
	CHECK_EQ(w.Get(0, "country"), std::string("AF"));
	CHECK_EQ(w.Get(0, "doi"), std::string("work:doi"));
	CHECK_EQ(w.Get(0, "pub_year"), std::string("1948"));
	CHECK_EQ(w.Get(0, "pub_month"), std::string("02"));
	CHECK_EQ(w.Get(0, "source_name"), std::string("<null>"));

	auto c = Extract("contributors", xml);
	CHECK_EQ(c.rows.size(), size_t(1));
	CHECK_EQ(c.Get(0, "owner"), std::string("work"));
	CHECK_EQ(c.Get(0, "owner_put_code"), std::string("123"));
	CHECK_EQ(c.Get(0, "ordinal"), std::string("1"));
	CHECK_EQ(c.Get(0, "contributor_orcid"), std::string("8888-8888-8888-8880"));
	CHECK_EQ(c.Get(0, "credit_name"), std::string("work:credit-name"));
	CHECK_EQ(c.Get(0, "sequence"), std::string("first"));
	CHECK_EQ(c.Get(0, "role"), std::string("author"));

	auto e = Extract("external_ids", xml);
	CHECK_EQ(e.rows.size(), size_t(2));
	CHECK_EQ(e.Get(1, "type"), std::string("doi"));
	CHECK_EQ(e.Get(1, "value"), std::string("work:doi"));
	CHECK_EQ(e.Get(1, "owner"), std::string("work"));
	CHECK_EQ(e.Get(1, "container"), std::string("external-ids"));
}

static void TestSampleResearchResource() {
	const std::string xml = ReadFile(SAMPLES + "research-resource-3.0.xml");
	auto r = Extract("research_resources", xml);
	CHECK_EQ(r.rows.size(), size_t(1));
	CHECK_EQ(r.Get(0, "orcid"), std::string("0000-0003-0902-4386"));
	CHECK_EQ(r.Get(0, "title"), std::string("Giant Laser Award"));
	CHECK_EQ(r.Get(0, "translated_title_language"), std::string("de"));
	CHECK_EQ(r.Get(0, "host_name"), std::string("XSEDE")); // first host wins
	CHECK_EQ(r.Get(0, "start_year"), std::string("1999"));
	CHECK_EQ(r.Get(0, "end_year"), std::string("2012"));
	CHECK_EQ(r.Get(0, "url"), std::string("http://xsede.org/GiantLaserAward"));

	auto items = Extract("research_resource_items", xml);
	CHECK_EQ(items.rows.size(), size_t(2));
	CHECK_EQ(items.Get(0, "owner_put_code"), std::string("1234"));
	CHECK_EQ(items.Get(1, "ordinal"), std::string("2"));
	CHECK_EQ(items.Get(1, "host_name"), std::string("Moon Holdings Ltd"));
	CHECK_EQ(items.Get(0, "resource_name"), std::string("Giant Laser 1"));
	CHECK_EQ(items.Get(1, "resource_name"), std::string("Moon Targets"));
	CHECK_EQ(items.Get(1, "url"), std::string("http://blah2.com"));

	CHECK_EQ(Extract("external_ids", xml).rows.size(), size_t(5));
}

static void TestSamplePeerReviewFull() {
	const std::string xml = ReadFile(SAMPLES + "peer-review-full-3.0.xml");
	auto p = Extract("peer_reviews", xml);
	CHECK_EQ(p.rows.size(), size_t(1));
	CHECK_EQ(p.Get(0, "put_code"), std::string("12345"));
	CHECK_EQ(p.Get(0, "completion_year"), std::string("1948"));
	CHECK_EQ(p.Get(0, "review_group_id"), std::string("orcid-generated:12345"));
	CHECK_EQ(p.Get(0, "subject_translated_name"), std::string("peer-review:subject-translated-name"));
	CHECK_EQ(p.Get(0, "subject_id_type"), std::string("doi"));
	CHECK_EQ(p.Get(0, "convening_org_disambiguation_source"), std::string("FUNDREF"));

	auto e = Extract("external_ids", xml);
	CHECK_EQ(e.rows.size(), size_t(2));
	CHECK_EQ(e.Get(0, "container"), std::string("review-identifiers"));
	CHECK_EQ(e.Get(1, "container"), std::string("peer-review"));

	// The pre-3.0 layout of read sample peer-reviews-3.0.xml (activities:group/
	// peer-review:summary) is skipped, as yorc-rs, yorc-zig and ORCID's classes
	// skip it (FINDINGS #3): the group's own external id stays, the two inside
	// the summary go.
	const std::string old_layout = ReadFile(SAMPLES + "peer-reviews-3.0.xml");
	auto g = Extract("external_ids", old_layout);
	CHECK_EQ(g.rows.size(), size_t(1));
	CHECK_EQ(g.Get(0, "value"), std::string("orcid-generated:011484675699116"));
	CHECK_EQ(Extract("peer_reviews", old_layout).rows.size(), size_t(0));
	CHECK_EQ(Extract("timeline", old_layout).rows.size(), size_t(0));
}

static void TestSampleCollections() {
	CHECK_EQ(Extract("works", ReadFile(SAMPLES + "works-3.0.xml")).rows.size(), size_t(3));
	auto a = Extract("affiliations", ReadFile(SAMPLES + "employments-3.0.xml"));
	CHECK_EQ(a.rows.size(), size_t(2));
	CHECK_EQ(a.Get(1, "group_index"), std::string("2"));
	CHECK_EQ(Extract("person_items", ReadFile(SAMPLES + "person-3.0.xml")).rows.size(), size_t(6));
	CHECK_EQ(Extract("works", ReadFile(SAMPLES + "bulk-work-err.xml")).rows.size(), size_t(1));
	auto h = Extract("history", ReadFile(SAMPLES + "history-3.0.xml"));
	CHECK_EQ(h.rows.size(), size_t(1));
	CHECK_EQ(h.Get(0, "claimed"), std::string("true"));
	CHECK_EQ(h.Get(0, "creation_method"), std::string("API"));
}

//! API messages outside the record. Expected values are read off the samples
//! (reference/orcid-model) and match test/sql/orcid_api_messages.test.
static void TestSummary() {
	const std::string xml = ReadFile("reference/orcid-model/summary_3.0/samples/summary-3.0.xml");
	auto s = Extract("summary", xml);
	CHECK_EQ(s.rows.size(), size_t(1));
	CHECK_EQ(s.Get(0, "orcid"), std::string("8888-8888-8888-8880"));
	CHECK_EQ(s.Get(0, "orcid_uri"), std::string("https://orcid.org/8888-8888-8888-8880"));
	CHECK_EQ(s.Get(0, "orcid_host"), std::string("orcid.org"));
	CHECK_EQ(s.Get(0, "credit_name"), std::string("User credited name"));
	CHECK_EQ(s.Get(0, "created"), std::string("2024-01-01T12:00:00"));
	CHECK_EQ(s.Get(0, "last_modified"), std::string("2024-01-01T12:00:00"));
	CHECK_EQ(s.Get(0, "external_identifier_count"), std::string("3"));
	CHECK_EQ(s.Get(0, "external_identifier_types"), std::string("Scopus; ResearcherID; Other"));
	CHECK_EQ(s.Get(0, "employment_count"), std::string("5"));
	CHECK_EQ(s.Get(0, "professional_activity_count"), std::string("5"));
	CHECK_EQ(s.Get(0, "education_qualification_count"), std::string("<null>"));
	CHECK_EQ(s.Get(0, "funding_self_asserted_count"), std::string("0"));
	CHECK_EQ(s.Get(0, "funding_validated_count"), std::string("1"));
	CHECK_EQ(s.Get(0, "work_self_asserted_count"), std::string("0"));
	CHECK_EQ(s.Get(0, "work_validated_count"), std::string("1"));
	CHECK_EQ(s.Get(0, "peer_review_publication_grants"), std::string("6"));
	CHECK_EQ(s.Get(0, "peer_review_self_asserted_count"), std::string("0"));
	CHECK_EQ(s.Get(0, "peer_review_total"), std::string("6"));
	CHECK_EQ(s.Get(0, "research_resource_self_asserted_count"), std::string("<null>"));
	CHECK_EQ(s.Get(0, "email_domain_count"), std::string("2"));
	CHECK_EQ(s.Get(0, "email_domains"), std::string("orcid.org; domain.org"));

	auto items = Extract("summary_items", xml);
	CHECK_EQ(items.rows.size(), size_t(9)); // 3 external ids, 3 employments, 1 activity, 2 domains
	size_t validated_true = 0, validated_false = 0;
	for (size_t r = 0; r < items.rows.size(); r++) {
		CHECK_EQ(items.Get(r, "orcid"), std::string("8888-8888-8888-8880"));
		validated_true += items.Get(r, "validated") == "true";
		validated_false += items.Get(r, "validated") == "false";
	}
	CHECK_EQ(validated_true, size_t(4));
	CHECK_EQ(validated_false, size_t(3));
	CHECK_EQ(items.Get(0, "kind"), std::string("external-identifier"));
	CHECK_EQ(items.Get(0, "put_code"), std::string("1"));
	CHECK_EQ(items.Get(0, "type"), std::string("Scopus"));
	CHECK_EQ(items.Get(0, "url"), std::string("https://test.orcid.org/1"));
	CHECK_EQ(items.Get(5, "kind"), std::string("employment"));
	CHECK_EQ(items.Get(5, "ordinal"), std::string("3"));
	CHECK_EQ(items.Get(5, "organization_name"), std::string("Fake role title 3"));
	CHECK_EQ(items.Get(5, "role"), std::string("Org # 3"));
	CHECK_EQ(items.Get(5, "end_day"), std::string("31"));
	CHECK_EQ(items.Get(5, "validated"), std::string("true"));
	CHECK_EQ(items.Get(6, "kind"), std::string("professional-activity"));
	CHECK_EQ(items.Get(6, "type"), std::string("distinction"));
	CHECK_EQ(items.Get(7, "value"), std::string("orcid.org"));
	CHECK_EQ(items.Get(7, "verification_date"), std::string("2024-11-04T17:31:37.355+02:00"));
	CHECK_EQ(items.Get(8, "value"), std::string("domain.org"));
	CHECK_EQ(items.Get(8, "verification_date"), std::string("<null>"));
}

static void TestSearchAndErrors() {
	auto s = Extract("search_results", ReadFile(SAMPLES + "search-3.0.xml"));
	CHECK_EQ(s.rows.size(), size_t(1));
	CHECK_EQ(s.Get(0, "num_found"), std::string("1"));
	CHECK_EQ(s.Get(0, "ordinal"), std::string("1"));
	CHECK_EQ(s.Get(0, "orcid"), std::string("8888-8888-8888-8880"));
	CHECK_EQ(s.Get(0, "orcid_uri"), std::string("https://orcid.org/8888-8888-8888-8880"));
	CHECK_EQ(s.Get(0, "item"), std::string("result"));

	// No expanded-search sample is vendored; this follows expanded-search-3.0.xsd.
	const std::string expanded =
	    "<es:expanded-search num-found=\"7\" xmlns:es=\"http://www.orcid.org/ns/expanded-search\">"
	    "<es:expanded-result><es:orcid-id>0000-0002-1825-0097</es:orcid-id><es:given-names>Given</es:given-names>"
	    "<es:family-names>Family</es:family-names><es:credit-name>G. Family</es:credit-name>"
	    "<es:other-name>Other A</es:other-name><es:other-name>Other B</es:other-name>"
	    "<es:email>a@example.org</es:email><es:institution-name>Inst A</es:institution-name>"
	    "<es:institution-name>Inst B</es:institution-name></es:expanded-result>"
	    "<es:expanded-result><es:orcid-id>0000-0003-0100-6634</es:orcid-id></es:expanded-result>"
	    "</es:expanded-search>";
	auto e = Extract("search_results", expanded);
	CHECK_EQ(e.rows.size(), size_t(2));
	CHECK_EQ(e.Get(0, "num_found"), std::string("7"));
	CHECK_EQ(e.Get(1, "num_found"), std::string("7"));
	CHECK_EQ(e.Get(1, "ordinal"), std::string("2"));
	CHECK_EQ(e.Get(0, "orcid"), std::string("0000-0002-1825-0097"));
	CHECK_EQ(e.Get(0, "given_names"), std::string("Given"));
	CHECK_EQ(e.Get(0, "family_names"), std::string("Family"));
	CHECK_EQ(e.Get(0, "credit_name"), std::string("G. Family"));
	CHECK_EQ(e.Get(0, "other_names"), std::string("Other A; Other B"));
	CHECK_EQ(e.Get(0, "emails"), std::string("a@example.org"));
	CHECK_EQ(e.Get(0, "institution_names"), std::string("Inst A; Inst B"));
	CHECK_EQ(e.Get(1, "orcid"), std::string("0000-0003-0100-6634"));
	CHECK_EQ(e.Get(1, "other_names"), std::string("<null>"));
	CHECK_EQ(e.Get(1, "item"), std::string("expanded-result"));

	auto err = Extract("errors", ReadFile(SAMPLES + "error-3.0.xml"));
	CHECK_EQ(err.rows.size(), size_t(1));
	CHECK_EQ(err.Get(0, "response_code"), std::string("0"));
	CHECK_EQ(err.Get(0, "developer_message"), std::string("error:developer-message"));
	CHECK_EQ(err.Get(0, "user_message"), std::string("error:user-message"));
	CHECK_EQ(err.Get(0, "error_code"), std::string("0"));
	CHECK_EQ(err.Get(0, "more_info"), std::string("http://tempuri.org"));
	CHECK_EQ(err.Get(0, "container"), std::string("<null>"));
	CHECK_EQ(err.Get(0, "position"), std::string("1"));

	auto bulk = Extract("errors", ReadFile(SAMPLES + "bulk-work-err.xml"));
	CHECK_EQ(bulk.rows.size(), size_t(1));
	CHECK_EQ(bulk.Get(0, "response_code"), std::string("400"));
	CHECK_EQ(bulk.Get(0, "user_message"), std::string("Please specify a work type."));
	CHECK_EQ(bulk.Get(0, "error_code"), std::string("9037"));
	CHECK_EQ(bulk.Get(0, "more_info"), std::string("http://members.orcid.org/api/api-error-codes"));
	CHECK_EQ(bulk.Get(0, "container"), std::string("bulk"));
	CHECK_EQ(bulk.Get(0, "position"), std::string("2")); // second bulk entry, after the work
	CHECK(bulk.Get(0, "developer_message").rfind("Invalid work type, valid values are:", 0) == 0);
	CHECK_EQ(Extract("errors", ReadFile(SAMPLES + "record-3.0.xml")).rows.size(), size_t(0));
}

static void TestNotificationsAndGroupIds() {
	const std::string dir = "reference/orcid-model/notification_3.0/samples/";
	auto p = Extract("notifications", ReadFile(dir + "notification-permission-3.0.xml"));
	CHECK_EQ(p.rows.size(), size_t(1));
	CHECK_EQ(p.Get(0, "put_code"), std::string("1"));
	CHECK_EQ(p.Get(0, "notification_type"), std::string("permission"));
	CHECK_EQ(p.Get(0, "subject"), std::string("Subject"));
	CHECK_EQ(p.Get(0, "intro"), std::string("Intro"));
	CHECK_EQ(p.Get(0, "body_text"), std::string("<null>"));
	CHECK_EQ(p.Get(0, "authorization_url"),
	         std::string("https://orcid.org/oauth/authorize?client_id=APP-U4UKCNSSIM1OCVQY&response_type=code"
	                     "&scope=/orcid-works/create&redirect_uri=http://somethirdparty.com"));
	CHECK_EQ(p.Get(0, "authorization_host"), std::string("orcid.org"));
	CHECK_EQ(p.Get(0, "item_count"), std::string("4"));
	CHECK_EQ(p.Get(0, "item_types"), std::string("work; employment; distinction; service"));
	CHECK_EQ(p.Get(0, "item_names"), std::string("A Really Interesting Research Article; Head of Research at "
	                                              "NanoBiologica; Honorary member; Volunteer society officer"));
	CHECK_EQ(p.Get(0, "item_external_id_types"), std::string("doi"));
	CHECK_EQ(p.Get(0, "item_external_id_values"), std::string("http://10.5555/12345ABCDE"));
	CHECK_EQ(p.Get(0, "created"), std::string("2014-01-01T09:17:56"));
	CHECK_EQ(p.Get(0, "sent_date"), std::string("2014-01-01T14:45:32"));
	CHECK_EQ(p.Get(0, "read_date"), std::string("<null>"));
	CHECK_EQ(p.Get(0, "source_name"), std::string("Member 1"));
	CHECK_EQ(p.Get(0, "source_client_id"), std::string("APP-U4UKCNSSIM1OCVQY"));

	auto c = Extract("notifications", ReadFile(dir + "notification-custom-3.0.xml"));
	CHECK_EQ(c.rows.size(), size_t(1));
	CHECK_EQ(c.Get(0, "notification_type"), std::string("custom"));
	CHECK_EQ(c.Get(0, "subject"), std::string("Important Notification from ORCID"));
	CHECK_EQ(c.Get(0, "body_text"), std::string("This is an email with important info."));
	const std::string html = c.Get(0, "body_html");
	CHECK(html.rfind("<p>", 0) == 0);
	CHECK(html.find("<em>important</em>") != std::string::npos);
	CHECK(html.size() >= 4 && html.compare(html.size() - 4, 4, "</p>") == 0);
	CHECK_EQ(c.Get(0, "language"), std::string("en-gb"));
	CHECK_EQ(c.Get(0, "item_count"), std::string("0"));
	CHECK_EQ(c.Get(0, "item_types"), std::string("<null>"));
	CHECK_EQ(c.Get(0, "source_orcid"), std::string("2111-1111-1111-1114"));

	auto g = Extract("group_ids", ReadFile("reference/orcid-model/group-id-3.0/samples/group-id-3.0.xml"));
	CHECK_EQ(g.rows.size(), size_t(1));
	CHECK_EQ(g.Get(0, "put_code"), std::string("<null>"));
	CHECK_EQ(g.Get(0, "name"), std::string("group-id:name"));
	CHECK_EQ(g.Get(0, "group_id"), std::string("issn:xxxxxxxx"));
	CHECK_EQ(g.Get(0, "description"), std::string("group-id:description"));
	CHECK_EQ(g.Get(0, "type"), std::string("publisher"));
	CHECK_EQ(g.Get(0, "total"), std::string("<null>"));

	// A group-id-records page (group-id-3.0.xsd): document-level total/page/page-size.
	const std::string page =
	    "<g:group-id-records xmlns:g=\"http://www.orcid.org/ns/group-id\" xmlns:common=\"http://www.orcid.org/ns/common\">"
	    "<g:total>2</g:total><g:page>0</g:page><g:page-size>100</g:page-size>"
	    "<g:group-id-record put-code=\"11\"><common:source-client-id><common:path>APP-1</common:path>"
	    "</common:source-client-id><common:source-name>Client</common:source-name><g:name>A</g:name>"
	    "<g:group-id>issn:0000-0001</g:group-id><g:description>First</g:description><g:type>journal</g:type>"
	    "</g:group-id-record><g:group-id-record put-code=\"12\"><g:name>B</g:name><g:group-id>issn:0000-0002</g:group-id>"
	    "<g:description>Second</g:description><g:type>conference</g:type></g:group-id-record></g:group-id-records>";
	auto gp = Extract("group_ids", page);
	CHECK_EQ(gp.rows.size(), size_t(2));
	CHECK_EQ(gp.Get(0, "put_code"), std::string("11"));
	CHECK_EQ(gp.Get(0, "source_client_id"), std::string("APP-1"));
	CHECK_EQ(gp.Get(0, "source_name"), std::string("Client"));
	CHECK_EQ(gp.Get(1, "group_id"), std::string("issn:0000-0002"));
	CHECK_EQ(gp.Get(1, "source_name"), std::string("<null>"));
	CHECK_EQ(gp.Get(1, "total"), std::string("2"));
	CHECK_EQ(gp.Get(1, "page"), std::string("0"));
	CHECK_EQ(gp.Get(1, "page_size"), std::string("100"));
}

//! A full activity document (one work from the 2025 Public Data File activities
//! archive, CC0) yields rows only in the activity sections. Record-level and API
//! message sections (person, history, summary, errors, ...) must yield none:
//! their item patterns are anchored at the document element. Expected values
//! from grep on the fixture.
static void TestFullActivityItem() {
	const std::string xml = ReadFile("test/data/activities/0000-0002-8635-8390_works_30089981.xml");
	const std::set<std::string> activity = {"works", "contributors", "external_ids", "timeline"};
	for (auto &spec : orcid::Sections()) {
		auto e = Extract(spec.name.c_str(), xml);
		if (!activity.count(spec.name)) {
			if (!e.rows.empty()) {
				std::fprintf(stderr, "section %s: %zu row(s) from a full work document\n", spec.name.c_str(),
				             e.rows.size());
			}
			CHECK_EQ(e.rows.size(), size_t(0));
		}
	}
	auto w = Extract("works", xml);
	CHECK_EQ(w.rows.size(), size_t(1));
	CHECK_EQ(w.Get(0, "orcid"), std::string("0000-0002-8635-8390"));
	CHECK_EQ(w.Get(0, "put_code"), std::string("30089981"));
	CHECK_EQ(w.Get(0, "item"), std::string("work"));
	CHECK_EQ(w.Get(0, "title"), std::string("Gaussian Job Archive for C21H23N3O3"));
	CHECK_EQ(w.Get(0, "doi"), std::string("10.6084/m9.figshare.1119230"));
	CHECK_EQ(w.Get(0, "source_name"), std::string("DataCite"));
	auto c = Extract("contributors", xml);
	CHECK_EQ(c.rows.size(), size_t(1));
	CHECK_EQ(c.Get(0, "credit_name"), std::string("Henry Rzepa"));
	CHECK_EQ(c.Get(0, "contributor_orcid"), std::string("0000-0002-8635-8390"));
	CHECK_EQ(c.Get(0, "owner_put_code"), std::string("30089981"));
	auto x = Extract("external_ids", xml);
	CHECK_EQ(x.rows.size(), size_t(1));
	CHECK_EQ(x.Get(0, "value"), std::string("10.6084/M9.FIGSHARE.1119230"));
	// created 2017-02-04, modified 2022-05-25: two events
	CHECK_EQ(Extract("timeline", xml).rows.size(), size_t(2));
}

//! Anchored item patterns: an element with the local name of a record-only or
//! API-message item, anywhere but its XSD position, is not an item.
static void TestAnchoredItems() {
	const std::string nested =
	    "<work:work xmlns:work=\"w\" xmlns:x=\"x\" put-code=\"1\" path=\"/0000-0002-1825-0097/work/1\">"
	    "<x:person><x:name><x:given-names>No</x:given-names></x:name></x:person>"
	    "<x:history><x:claimed>true</x:claimed></x:history>"
	    "<x:record><x:person><x:name><x:given-names>No</x:given-names></x:name></x:person></x:record>"
	    "<x:error><x:response-code>400</x:response-code></x:error>"
	    "<x:notification><x:notification-type>custom</x:notification-type></x:notification>"
	    "<x:record-summary><x:credit-name>No</x:credit-name></x:record-summary>"
	    "<x:search><x:result><x:orcid-identifier><x:path>0000-0002-1825-0097</x:path></x:orcid-identifier></x:result></x:search>"
	    "<x:group-id-record put-code=\"2\"><x:name>No</x:name></x:group-id-record>"
	    "</work:work>";
	for (const char *s : {"person", "history", "errors", "notifications", "summary", "summary_items", "search_results",
	                      "group_ids"}) {
		auto e = Extract(s, nested);
		if (!e.rows.empty()) {
			std::fprintf(stderr, "section %s: %zu row(s) from nested elements\n", s, e.rows.size());
		}
		CHECK_EQ(e.rows.size(), size_t(0));
	}
	CHECK_EQ(Extract("works", nested).rows.size(), size_t(1));

	// The XSD positions still match: record/person, record/history, a person document.
	const std::string record = "<record:record xmlns:record=\"r\" xmlns:person=\"p\" xmlns:history=\"h\" "
	                           "path=\"/0000-0002-1825-0097\"><person:person><person:name><personal-details:given-names "
	                           "xmlns:personal-details=\"d\">Yes</personal-details:given-names></person:name>"
	                           "</person:person><history:history><history:claimed>true</history:claimed>"
	                           "</history:history></record:record>";
	auto p = Extract("person", record);
	CHECK_EQ(p.rows.size(), size_t(1));
	CHECK_EQ(p.Get(0, "given_names"), std::string("Yes"));
	CHECK_EQ(p.Get(0, "orcid"), std::string("0000-0002-1825-0097"));
	CHECK_EQ(Extract("history", record).rows.size(), size_t(1));
	CHECK_EQ(Extract("person", "<person:person xmlns:person=\"p\" path=\"/0000-0002-1825-0097/person\"/>").rows.size(),
	         size_t(1));
	CHECK_EQ(Extract("history", "<history:history xmlns:history=\"h\"/>").rows.size(), size_t(1));
}

//! Locked and deprecated records: the Public Data File holds them as error
//! documents without a path; the iD comes from the message. Fixtures from the
//! 2025 summaries (CC0); expected values from grep.
static void TestLockedDeprecated() {
	auto locked = Extract("errors", ReadFile("test/data/errors/0000-0001-5272-5001_locked.xml"));
	CHECK_EQ(locked.rows.size(), size_t(1));
	CHECK_EQ(locked.Get(0, "orcid"), std::string("0000-0001-5272-5001"));
	CHECK_EQ(locked.Get(0, "response_code"), std::string("409"));
	CHECK_EQ(locked.Get(0, "error_code"), std::string("9018"));
	CHECK_EQ(locked.Get(0, "user_message"), std::string("The ORCID record is locked."));
	CHECK_EQ(locked.Get(0, "primary_orcid"), std::string("<null>"));
	CHECK_EQ(locked.spec->columns.back().name, std::string("primary_orcid"));

	auto dep = Extract("errors", ReadFile("test/data/errors/0000-0001-5433-3001_deprecated.xml"));
	CHECK_EQ(dep.rows.size(), size_t(1));
	CHECK_EQ(dep.Get(0, "orcid"), std::string("0000-0001-5433-3001"));
	CHECK_EQ(dep.Get(0, "primary_orcid"), std::string("0009-0008-8963-1314"));
	CHECK_EQ(dep.Get(0, "response_code"), std::string("301"));
	CHECK_EQ(dep.Get(0, "error_code"), std::string("9007"));

	// the other two message forms of the corpus: a bare iD, and a bare iD
	// followed by "Full validation error: <iD> is locked"
	auto msg = [](const std::string &m) {
		return "<error:error xmlns:error=\"http://www.orcid.org/ns/error\"><error:response-code>409</error:response-code>"
		       "<error:developer-message>" + m + "</error:developer-message></error:error>";
	};
	CHECK_EQ(Extract("errors", msg("409 Conflict: The ORCID record is locked and cannot be edited. ORCID 0000-0001-5553-5000"))
	             .Get(0, "orcid"),
	         std::string("0000-0001-5553-5000"));
	CHECK_EQ(Extract("errors", msg("409 Conflict: The ORCID record is locked and cannot be edited. ORCID "
	                               "0000-0001-5553-5000 Full validation error: 0000-0001-5553-5000 is locked"))
	             .Get(0, "orcid"),
	         std::string("0000-0001-5553-5000"));
	// no iD (or an invalid check digit) in the message: NULL, as before
	CHECK_EQ(Extract("errors", msg("Please specify a work type.")).Get(0, "orcid"), std::string("<null>"));
	CHECK_EQ(Extract("errors", msg("ORCID 0000-0001-5553-5001")).Get(0, "orcid"), std::string("<null>"));
	// a document with its own iD (path) keeps it
	CHECK_EQ(Extract("errors", "<error:error xmlns:error=\"e\" path=\"/0000-0002-1825-0097\"><error:developer-message>ORCID "
	                           "0000-0001-5553-5000</error:developer-message></error:error>")
	             .Get(0, "orcid"),
	         std::string("0000-0002-1825-0097"));
}

//! One ustar header block with a valid checksum; `size` is a raw 12-byte field.
static std::string TarHeader(const std::string &name, const std::string &size, char type) {
	std::string b(512, '\0');
	b.replace(0, name.size(), name);
	b.replace(100, 8, std::string("0000644\0", 8));
	b.replace(124, size.size(), size);
	b[156] = type;
	b.replace(257, 6, std::string("ustar\0", 6));
	b.replace(148, 8, std::string(8, ' '));
	unsigned sum = 0;
	for (unsigned char c : b) {
		sum += c;
	}
	char chk[8];
	std::snprintf(chk, sizeof(chk), "%06o", sum);
	b.replace(148, 7, std::string(chk, 6) + '\0');
	return b;
}

//! Corrupt headers found while fuzzing (test/fuzz/fuzz_tar): a size field
//! claiming terabytes must fail as a truncated archive after reading what is
//! there, not allocate the claimed size first; an oversized long-name / pax
//! header is rejected.
static void TestTarCorruptSizes() {
	auto threw = [](const std::string &data) {
		size_t pos = 0;
		orcid::TarReader tar([&](char *buf, size_t n) -> size_t {
			size_t k = std::min(n, data.size() - pos);
			std::memcpy(buf, data.data() + pos, k);
			pos += k;
			return k;
		});
		try {
			orcid::TarMember m;
			while (tar.Next(m)) {
				tar.ReadContent();
			}
		} catch (const orcid::TarError &) {
			return true;
		}
		return false;
	};
	// GNU base-256 sizes: 2^46 bytes of content, and a 2^38-byte long name
	const std::string huge("\x80\0\0\0\0\0\x40\0\0\0\0\0", 12);
	CHECK(threw(TarHeader("a.xml", huge, '0') + std::string(512, 'x')));
	CHECK(threw(TarHeader("././@LongLink", huge, 'L') + std::string(512, 'x')));
	CHECK(threw(TarHeader("pax", huge, 'x') + std::string(512, 'x')));
	// a pax record whose length overflows is ignored, and the member still reads
	std::string pax = "99999999999999999999 path=a\n";
	pax.resize(512, '\0');
	char size[12];
	std::snprintf(size, sizeof(size), "%011o", 28u);
	const std::string ok = TarHeader("pax", std::string(size, 11) + '\0', 'x') + pax +
	                       TarHeader("b.xml", std::string("00000000000\0", 12), '0') + std::string(1024, '\0');
	size_t pos = 0;
	orcid::TarReader tar([&](char *buf, size_t n) -> size_t {
		size_t k = std::min(n, ok.size() - pos);
		std::memcpy(buf, ok.data() + pos, k);
		pos += k;
		return k;
	});
	orcid::TarMember m;
	CHECK(tar.Next(m));
	CHECK_EQ(m.name, std::string("b.xml"));
	CHECK(!tar.Next(m));
}

static void TestParsers() {
	CHECK(orcid::ParseInteger("0042", 9) == std::optional<int64_t>(42));
	CHECK(!orcid::ParseInteger("4x", 9));
	CHECK(!orcid::ParseInteger("", 9));
	CHECK(!orcid::ParseInteger("1234567890", 9));
	CHECK(orcid::ParseBoolean("true") == std::optional<bool>(true));
	CHECK(orcid::ParseBoolean("0") == std::optional<bool>(false));
	CHECK(!orcid::ParseBoolean("yes"));
}

// ---------------------------------------------------------------------------
// JSON, the JSON crosswalk and the writers. The expected values come from the reference/orcid-model samples (read with
// python3 -I, json module) and from the rules in json_map.hpp / writer.cpp.
// ---------------------------------------------------------------------------

static const std::string WRITE_SAMPLES = "reference/orcid-model/record_3.0/samples/write_samples/";

//! Rows of a section for a document of either format (told by its first byte).
static Extracted ExtractAny(const char *section, const std::string &doc, const std::string &json_root = "") {
	Extracted e;
	e.spec = orcid::FindSection(section);
	if (!e.spec) {
		std::fprintf(stderr, "no section %s\n", section);
		failures++;
		return e;
	}
	orcid::ExtractDocument(*e.spec, doc.data(), doc.size(), orcid::DetectFormat(doc.data(), doc.size()), json_root,
	                       e.rows);
	return e;
}

template <class F>
static bool Throws(F f) {
	try {
		f();
	} catch (const std::exception &) {
		return true;
	}
	return false;
}

//! The error message f() throws, or "" when it does not throw.
template <class F>
static std::string Message(F f) {
	try {
		f();
	} catch (const std::exception &e) {
		return e.what();
	}
	return std::string();
}

static bool Contains(const std::string &s, const std::string &part) {
	return s.find(part) != std::string::npos;
}

static void TestJsonParser() {
	const std::string text = "\xEF\xBB\xBF {\"a\": [1, -2.5e3, true, null, \"x\\u00e9\\ud83d\\ude00\\/\"], \"a\": {}} ";
	auto doc = orcid::ParseJson(text.data(), text.size());
	CHECK(doc.type == orcid::JsonValue::Type::Object);
	CHECK_EQ(doc.members.size(), size_t(2)); // duplicate keys are kept
	const orcid::JsonValue *a = doc.Find("a");
	CHECK(a && a->type == orcid::JsonValue::Type::Array && a->items.size() == 5);
	if (a && a->items.size() == 5) {
		CHECK_EQ(a->items[0].text, std::string("1"));
		CHECK_EQ(a->items[1].text, std::string("-2.5e3")); // numbers keep their literal
		CHECK(a->items[2].type == orcid::JsonValue::Type::Bool && a->items[2].text == "true");
		CHECK(a->items[3].type == orcid::JsonValue::Type::Null);
		CHECK_EQ(a->items[4].text, std::string("x\xC3\xA9\xF0\x9F\x98\x80/")); // surrogate pair -> U+1F600
	}
	CHECK(!doc.Has("b"));

	auto parse = [](const std::string &s) { return [s] { orcid::ParseJson(s.data(), s.size()); }; };
	for (const char *bad : {"", "{", "[1,]", "{\"a\":1,}", "{'a':1}", "01", "1.", "-", "1e", "NaN", "1 2", "tru",
	                        "\"\\x\"", "\"\\u12\"", "\"\\ud800\"", "\"\\udc00\"", "\"\\ud800\\u0041\"",
	                        "{\"a\" 1}", "[1 2]", "\"abc"}) {
		CHECK(Throws(parse(bad)));
	}
	CHECK(Throws(parse(std::string("\"a\x01" "b\""))));      // raw control character
	CHECK(Throws(parse(std::string("\"\xC3\x28\""))));      // invalid UTF-8
	CHECK(Throws(parse(std::string("\"\xC0\xAF\""))));      // overlong form
	CHECK(Throws(parse(std::string("\"\xED\xA0\x80\""))));  // UTF-8-encoded surrogate
	CHECK(Contains(Message(parse("[1,]")), "at byte 3"));
	// nesting: JSON_MAX_DEPTH levels parse, one more does not
	CHECK(!Throws(parse(std::string(orcid::JSON_MAX_DEPTH, '[') + std::string(orcid::JSON_MAX_DEPTH, ']'))));
	CHECK(Throws(parse(std::string(orcid::JSON_MAX_DEPTH + 1, '[') + std::string(orcid::JSON_MAX_DEPTH + 1, ']'))));
	CHECK(Throws(parse(std::string(100000, '['))));
}

static void TestUtf8AndEscaping() {
	CHECK(orcid::IsValidUtf8("a\xC3\xA9\xE2\x80\x94\xF0\x9F\x98\x80", 10));
	for (const std::string &bad : {std::string("\xC3"), std::string("\xE2\x82"), std::string("\xED\xA0\x80"),
	                              std::string("\xF4\x90\x80\x80"), std::string("\xC1\xBF"), std::string("\x80"),
	                              std::string("\xFF")}) {
		CHECK(!orcid::IsValidUtf8(bad.data(), bad.size()));
	}

	std::string json;
	orcid::AppendJsonString(std::string("a\"b\\c\n\t\r\b\f\x01\x1F\x7F") + "\xC3\xA9\xF0\x9F\x98\x80\xE2\x80\xA8", json);
	CHECK_EQ(json, std::string("\"a\\\"b\\\\c\\n\\t\\r\\b\\f\\u0001\\u001f\\u007f\xC3\xA9\xF0\x9F\x98\x80\xE2\x80\xA8\""));
	// what AppendJsonString writes parses back to the same text
	const std::string back = orcid::ParseJson(json.data(), json.size()).text;
	CHECK_EQ(back, std::string("a\"b\\c\n\t\r\b\f\x01\x1F\x7F") + "\xC3\xA9\xF0\x9F\x98\x80\xE2\x80\xA8");
	CHECK(Throws([] {
		std::string out;
		orcid::AppendJsonString(std::string("ok\xC3\x28"), out);
	}));

	std::string text, attr;
	orcid::AppendXmlEscaped("a<b&c>\"d'\r\n\t\xC3\xA9", false, text);
	orcid::AppendXmlEscaped("a<b&c>\"d'\r\n\t\xC3\xA9", true, attr);
	CHECK_EQ(text, std::string("a&lt;b&amp;c&gt;\"d'&#13;\n\t\xC3\xA9"));
	CHECK_EQ(attr, std::string("a&lt;b&amp;c&gt;&quot;d'&#13;&#10;&#9;\xC3\xA9"));
	for (const std::string &bad : {std::string("a\x01"), std::string(1, '\0'), std::string("\xEF\xBF\xBE"),
	                              std::string("\xEF\xBF\xBF"), std::string("\xC3")}) {
		CHECK(Throws([&] {
			std::string out;
			orcid::AppendXmlEscaped(bad, false, out);
		}));
	}
	// escaped text reads back through the XML reader unchanged (CR included)
	const std::string doc = "<a>" + text + "</a>";
	auto ev = Events(doc);
	CHECK(ev.size() == 3 && ev[1] == "T:a<b&c>\"d'\r\n\t\xC3\xA9");
}

static void TestJsonCrosswalk() {
	CHECK(orcid::DetectFormat(" \n\t{", 4) == orcid::DocFormat::Json);
	CHECK(orcid::DetectFormat("\xEF\xBB\xBF[", 4) == orcid::DocFormat::Json);
	CHECK(orcid::DetectFormat("<a/>", 4) == orcid::DocFormat::Xml);
	CHECK(orcid::DetectFormat("", 0) == orcid::DocFormat::Xml);

	CHECK_EQ(orcid::EpochMillisToIso(0), std::string("1970-01-01T00:00:00.000Z"));
	CHECK_EQ(orcid::EpochMillisToIso(1540896558250LL), std::string("2018-10-30T10:49:18.250Z"));
	CHECK_EQ(orcid::EpochMillisToIso(951782400000LL), std::string("2000-02-29T00:00:00.000Z"));
	CHECK_EQ(orcid::EpochMillisToIso(-1), std::string("1969-12-31T23:59:59.999Z"));
	// a bare epoch number in a *-date member converts as the {"value": ...} form does
	{
		auto bare = ExtractAny("works", "{\"created-date\": 1540896558250, \"last-modified-date\": {\"value\": "
		                                "1540896558250}, \"title\": {\"title\": {\"value\": \"T\"}}, \"type\": "
		                                "\"other\", \"path\": \"/0000-0002-1825-0097/work/5\"}");
		CHECK_EQ(bare.rows.size(), size_t(1));
		if (bare.rows.size() == 1) {
			CHECK_EQ(bare.Get(0, "created"), std::string("2018-10-30T10:49:18.250Z"));
			CHECK_EQ(bare.Get(0, "last_modified"), std::string("2018-10-30T10:49:18.250Z"));
		}
	}

	// a work as the API returns it: root from the path, attributes, value wrappers, epoch dates
	const std::string work = "{\"created-date\": {\"value\": 0}, \"title\": {\"title\": {\"value\": \" T \"}, "
	                         "\"subtitle\": null, \"translated-title\": {\"value\": \"U\", \"language-code\": \"en\"}}, "
	                         "\"type\": \"other\", \"external-ids\": {\"external-id\": [{\"external-id-type\": \"doi\", "
	                         "\"external-id-value\": \"10.1/AB\", \"external-id-url\": {\"value\": \"https://doi.org/10.1/AB\"}, "
	                         "\"external-id-relationship\": \"self\"}]}, \"visibility\": \"public\", \"put-code\": 5, "
	                         "\"path\": \"/0000-0002-1825-0097/work/5\", \"display-index\": 0}";
	auto w = ExtractAny("works", work);
	CHECK_EQ(w.rows.size(), size_t(1));
	CHECK_EQ(w.Get(0, "item"), std::string("work"));
	CHECK_EQ(w.Get(0, "orcid"), std::string("0000-0002-1825-0097"));
	CHECK_EQ(w.Get(0, "put_code"), std::string("5"));
	CHECK_EQ(w.Get(0, "display_index"), std::string("0"));
	CHECK_EQ(w.Get(0, "title"), std::string("T"));
	CHECK_EQ(w.Get(0, "subtitle"), std::string("<null>"));
	CHECK_EQ(w.Get(0, "translated_title_language"), std::string("en"));
	CHECK_EQ(w.Get(0, "doi"), std::string("10.1/ab"));
	CHECK_EQ(w.Get(0, "created"), std::string("1970-01-01T00:00:00.000Z"));
	CHECK_EQ(w.Get(0, "visibility"), std::string("public"));
	CHECK_EQ(w.Get(0, "path"), std::string("/0000-0002-1825-0097/work/5"));
	auto ids = ExtractAny("external_ids", work);
	CHECK_EQ(ids.rows.size(), size_t(1));
	CHECK_EQ(ids.Get(0, "owner"), std::string("work"));
	CHECK_EQ(ids.Get(0, "owner_put_code"), std::string("5"));
	CHECK_EQ(ids.Get(0, "url"), std::string("https://doi.org/10.1/AB"));

	// a top-level array is a sequence of documents; identifier objects keep path as an element
	const std::string two = "[{\"orcid-identifier\": {\"uri\": \"https://orcid.org/0000-0002-1825-0097\", "
	                        "\"path\": \"0000-0002-1825-0097\", \"host\": \"orcid.org\"}, \"person\": {\"name\": "
	                        "{\"given-names\": {\"value\": \"G\"}, \"visibility\": \"public\"}}}, 7, "
	                        "{\"orcid-identifier\": {\"path\": \"0000-0003-0100-6634\", \"uri\": null}, \"history\": "
	                        "{\"claimed\": true}}]";
	auto p = ExtractAny("person", two);
	CHECK_EQ(p.rows.size(), size_t(1));
	CHECK_EQ(p.Get(0, "orcid"), std::string("0000-0002-1825-0097"));
	CHECK_EQ(p.Get(0, "given_names"), std::string("G"));
	CHECK_EQ(p.Get(0, "name_visibility"), std::string("public"));
	auto h = ExtractAny("history", two);
	CHECK_EQ(h.rows.size(), size_t(1));
	CHECK_EQ(h.Get(0, "orcid"), std::string("0000-0003-0100-6634"));
	CHECK_EQ(h.Get(0, "claimed"), std::string("true"));

	// shapes ORCID's JSON leaves ambiguous need json_root
	const std::string employment = "{\"department-name\": \"D\", \"organization\": {\"name\": \"O\"}}";
	CHECK(Contains(Message([&] { ExtractAny("affiliations", employment); }), "json_root"));
	auto e = ExtractAny("affiliations", employment, "education");
	CHECK_EQ(e.rows.size(), size_t(1));
	CHECK_EQ(e.Get(0, "affiliation_type"), std::string("education"));
	CHECK_EQ(e.Get(0, "org_name"), std::string("O"));
	CHECK(Throws([] { ExtractAny("person_items", "{\"content\": \"k\"}"); }));
	CHECK_EQ(ExtractAny("person_items", "{\"content\": \"k\"}", "keyword").Get(0, "kind"), std::string("keyword"));
	// not an ORCID document: no rows, no error
	CHECK_EQ(ExtractAny("works", "{\"name\": \"x\", \"version\": 1, \"dependencies\": {}}").rows.size(), size_t(0));
	CHECK(Throws([] { ExtractAny("works", "{\"title\": }"); }));

	// bulk: wrapper objects; errors keep their position among the entries
	const std::string bulk = "{\"bulk\": [{\"work\": {\"title\": {\"title\": {\"value\": \"W\"}}, \"type\": \"book\"}}, "
	                         "{\"error\": {\"response-code\": 400, \"developer-message\": \"m\", \"error-code\": 9037}}]}";
	CHECK_EQ(ExtractAny("works", bulk).rows.size(), size_t(1));
	auto err = ExtractAny("errors", bulk);
	CHECK_EQ(err.rows.size(), size_t(1));
	CHECK_EQ(err.Get(0, "position"), std::string("2"));
	CHECK_EQ(err.Get(0, "container"), std::string("bulk"));
	CHECK_EQ(err.Get(0, "response_code"), std::string("400"));
}

//! The record sample in JSON (read_samples/record-full-3.0.json). Expected values
//! from the file with python3 -I (json module): counts of work-summary,
//! funding-summary, the summaries of each affiliation-group, ...
static void TestJsonRecordSample() {
	const std::string json = ReadFile(SAMPLES + "record-full-3.0.json");
	auto w = ExtractAny("works", json);
	CHECK_EQ(w.rows.size(), size_t(1));
	CHECK_EQ(w.Get(0, "orcid"), std::string("0000-0002-7319-2192"));
	CHECK_EQ(w.Get(0, "put_code"), std::string("178602"));
	CHECK_EQ(w.Get(0, "group_index"), std::string("1"));
	CHECK_EQ(w.Get(0, "title"), std::string("Work Title"));
	CHECK_EQ(w.Get(0, "doi"), std::string("work:doi"));
	CHECK_EQ(w.Get(0, "pub_month"), std::string("10"));
	CHECK_EQ(w.Get(0, "created"), std::string("2019-06-07T14:16:57.189Z"));
	CHECK_EQ(w.Get(0, "source_name"), std::string("Demo Org Sandbox"));
	CHECK_EQ(w.Get(0, "item"), std::string("work-summary"));
	CHECK_EQ(w.Get(0, "source_hash"), Sha(json));
	CHECK_EQ(ExtractAny("fundings", json).rows.size(), size_t(2));
	auto a = ExtractAny("affiliations", json);
	CHECK_EQ(a.rows.size(), size_t(10)); // 1 + 3 + 1 + 1 + 1 + 1 + 2
	std::set<std::string> types;
	for (size_t r = 0; r < a.rows.size(); r++) {
		types.insert(a.Get(r, "affiliation_type"));
	}
	CHECK_EQ(types.size(), size_t(7));
	CHECK_EQ(ExtractAny("peer_reviews", json).rows.size(), size_t(2));
	auto rr = ExtractAny("research_resources", json);
	CHECK_EQ(rr.rows.size(), size_t(1));
	CHECK_EQ(rr.Get(0, "title"), std::string("Giant Rocket Award"));
	CHECK_EQ(rr.Get(0, "host_name"), std::string("NASA"));
	auto p = ExtractAny("person", json);
	CHECK_EQ(p.rows.size(), size_t(1));
	CHECK_EQ(p.Get(0, "given_names"), std::string("Three"));
	CHECK_EQ(p.Get(0, "family_name"), std::string("releasecandidate1"));
	CHECK_EQ(p.Get(0, "biography"), std::string("<null>"));
	CHECK_EQ(p.Get(0, "name_last_modified"), std::string("2018-10-30T10:49:18.538Z"));
	CHECK_EQ(ExtractAny("person_items", json).rows.size(), size_t(13)); // 5 other names, 2 urls, 3 addresses, 2 keywords, 1 id
	auto h = ExtractAny("history", json);
	CHECK_EQ(h.rows.size(), size_t(1));
	CHECK_EQ(h.Get(0, "creation_method"), std::string("Member-referred")); // JSON enum name -> XSD value (CROSSCHECK #14)
	CHECK_EQ(h.Get(0, "submission_date"), std::string("2018-10-30T10:49:18.250Z"));
	CHECK_EQ(h.Get(0, "completion_date"), std::string("<null>"));
	CHECK_EQ(h.Get(0, "claimed"), std::string("true"));
	CHECK_EQ(ExtractAny("external_ids", json).rows.size(), size_t(45));
	CHECK_EQ(ExtractAny("errors", json).rows.size(), size_t(0));

	// the one write-sample pair whose content is identical: same rows but the hash
	auto jx = ExtractAny("person_items", ReadFile(WRITE_SAMPLES + "external-identifier-3.0.json"));
	auto xx = ExtractAny("person_items", ReadFile(WRITE_SAMPLES + "external-identifier-3.0.xml"));
	CHECK_EQ(jx.rows.size(), size_t(1));
	CHECK_EQ(xx.rows.size(), size_t(1));
	if (jx.rows.size() == 1 && xx.rows.size() == 1) {
		for (size_t c = 0; c < jx.spec->columns.size(); c++) {
			if (jx.spec->columns[c].name != "source_hash") {
				CHECK(jx.rows[0][c] == xx.rows[0][c]);
			}
		}
	}

	// JSON and XML snapshots compare like two XML ones
	std::vector<orcid::ChangeRow> c;
	const std::string xml = ReadFile(WRITE_SAMPLES + "external-identifier-3.0.xml");
	const std::string js = ReadFile(WRITE_SAMPLES + "external-identifier-3.0.json");
	orcid::DiffDocuments(xml.data(), xml.size(), orcid::DocFormat::Xml, js.data(), js.size(), orcid::DocFormat::Json,
	                     std::string(), c);
	CHECK_EQ(c.size(), size_t(0));
}

static orcid::WriteRecord RecordOf(const Extracted &e, size_t r) {
	orcid::WriteRecord rec;
	for (size_t c = 0; c < e.spec->columns.size(); c++) {
		if (e.rows[r][c]) {
			rec.fields[e.spec->columns[c].name] = *e.rows[r][c];
		}
	}
	return rec;
}

static std::vector<orcid::WriteRecord> RecordsOf(const Extracted &e) {
	std::vector<orcid::WriteRecord> out;
	for (size_t r = 0; r < e.rows.size(); r++) {
		out.push_back(RecordOf(e, r));
	}
	return out;
}

//! Same rows, in order, ignoring source_hash (the hash of a different document).
static bool SameRows(const Extracted &a, const Extracted &b) {
	if (a.rows.size() != b.rows.size()) {
		std::fprintf(stderr, "  %s: %zu rows != %zu rows\n", a.spec->name.c_str(), a.rows.size(), b.rows.size());
		return false;
	}
	for (size_t r = 0; r < a.rows.size(); r++) {
		for (size_t c = 0; c < a.spec->columns.size(); c++) {
			if (a.spec->columns[c].name != "source_hash" && a.rows[r][c] != b.rows[r][c]) {
				std::fprintf(stderr, "  %s row %zu column %s: %s != %s\n", a.spec->name.c_str(), r,
				             a.spec->columns[c].name.c_str(), a.rows[r][c] ? a.rows[r][c]->c_str() : "NULL",
				             b.rows[r][c] ? b.rows[r][c]->c_str() : "NULL");
				return false;
			}
		}
	}
	return true;
}

static void TestWriterOutput() {
	orcid::WriteRecord w;
	w.fields["title"] = "T";
	w.fields["type"] = "other";
	CHECK_EQ(orcid::WriteDocument("work", w, orcid::WriteFormat::Xml),
	         std::string("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
	                     "<work:work xmlns:common=\"http://www.orcid.org/ns/common\" "
	                     "xmlns:work=\"http://www.orcid.org/ns/work\">\n"
	                     "\t<work:title>\n\t\t<common:title>T</common:title>\n\t</work:title>\n"
	                     "\t<work:type>other</work:type>\n</work:work>\n"));
	CHECK_EQ(orcid::WriteDocument("work", w, orcid::WriteFormat::Json),
	         std::string("{\n  \"title\": {\n    \"title\": {\n      \"value\": \"T\"\n    }\n  },\n"
	                     "  \"type\": \"other\"\n}\n"));

	// put-code: an attribute in XML, a number in JSON; month and day get two digits
	w.fields["put_code"] = "42";
	w.fields["pub_year"] = "2012";
	w.fields["pub_month"] = "2";
	w.fields["pub_day"] = "1";
	w.fields["source_name"] = "read-only, not written";
	const std::string xml = orcid::WriteDocument("work", w, orcid::WriteFormat::Xml);
	CHECK(Contains(xml, "<work:work xmlns:common=\"http://www.orcid.org/ns/common\" "
	                    "xmlns:work=\"http://www.orcid.org/ns/work\" put-code=\"42\">"));
	CHECK(Contains(xml, "<common:month>02</common:month>"));
	CHECK(Contains(xml, "<common:day>01</common:day>"));
	CHECK(!Contains(xml, "read-only"));
	const std::string json = orcid::WriteDocument("work", w, orcid::WriteFormat::Json);
	CHECK(Contains(json, "\"put-code\": 42"));
	CHECK(Contains(json, "\"month\": {\n      \"value\": \"02\"\n    }"));
	// a work's doi alone becomes its self DOI identifier
	w.fields["doi"] = "10.1/x";
	auto ids = ExtractAny("external_ids", orcid::WriteDocument("work", w, orcid::WriteFormat::Xml));
	CHECK_EQ(ids.rows.size(), size_t(1));
	CHECK_EQ(ids.Get(0, "type") + " " + ids.Get(0, "value") + " " + ids.Get(0, "relationship"),
	         std::string("doi 10.1/x self"));
	w.lists["external_ids"] = {}; // given, even empty: no doi fallback
	CHECK_EQ(ExtractAny("external_ids", orcid::WriteDocument("work", w, orcid::WriteFormat::Xml)).rows.size(), size_t(0));

	// escaping both ways, and text the formats cannot carry
	orcid::WriteRecord e;
	e.fields["title"] = "a < b & \"c\"\n\td \xF0\x9F\x98\x80";
	e.fields["type"] = "other";
	CHECK(Contains(orcid::WriteDocument("work", e, orcid::WriteFormat::Xml),
	               "<common:title>a &lt; b &amp; \"c\"\n\td \xF0\x9F\x98\x80</common:title>"));
	CHECK(Contains(orcid::WriteDocument("work", e, orcid::WriteFormat::Json),
	               "\"value\": \"a < b & \\\"c\\\"\\n\\td \xF0\x9F\x98\x80\""));
	for (auto format : {orcid::WriteFormat::Xml, orcid::WriteFormat::Json}) {
		auto back = ExtractAny("works", orcid::WriteDocument("work", e, format));
		CHECK_EQ(back.Get(0, "title"), std::string("a < b & \"c\"\n\td \xF0\x9F\x98\x80"));
	}
	e.fields["title"] = "bell \x07";
	CHECK(Contains(Message([&] { orcid::WriteDocument("work", e, orcid::WriteFormat::Xml); }), "XML 1.0"));
	CHECK(Contains(orcid::WriteDocument("work", e, orcid::WriteFormat::Json), "bell \\u0007"));
	e.fields["title"] = "bad \xC3\x28";
	CHECK(Throws([&] { orcid::WriteDocument("work", e, orcid::WriteFormat::Xml); }));
	CHECK(Throws([&] { orcid::WriteDocument("work", e, orcid::WriteFormat::Json); }));
}

static void TestWriterChecks() {
	auto message = [](const std::string &writer, orcid::WriteRecord r) {
		return Message([&] { orcid::WriteDocument(writer, r, orcid::WriteFormat::Xml); });
	};
	orcid::WriteRecord w;
	w.fields["type"] = "other";
	CHECK(Contains(message("work", w), "'title' is required"));
	w.fields["title"] = "T";
	w.fields["translated_title"] = "U";
	CHECK(Contains(message("work", w), "'translated_title_language' is required with 'translated_title'"));
	w.fields.erase("translated_title");
	w.fields["pub_year"] = "2001";
	w.fields["pub_day"] = "3";
	CHECK(Contains(message("work", w), "'pub_day' needs 'pub_month'"));
	w.fields["pub_month"] = "13";
	CHECK(Contains(message("work", w), "outside the schema's range 1..12"));
	w.fields["pub_month"] = "x";
	CHECK(Contains(message("work", w), "must be a number"));
	w.fields["pub_month"] = "3";
	w.fields["pub_year"] = "1899";
	CHECK(Contains(message("work", w), "1900..2100"));
	w.fields["pub_year"] = "2001";
	w.fields["put_code"] = "12a";
	CHECK(Contains(message("work", w), "must be an integer"));
	w.fields.erase("put_code");
	orcid::WriteRecord id;
	id.fields["value"] = "v";
	w.lists["external_ids"] = {id};
	CHECK(Contains(message("work", w), "'external_ids[1].type' is required"));
	w.lists.erase("external_ids");
	CHECK(message("work", w).empty());

	orcid::WriteRecord a;
	a.fields["org_name"] = "O";
	CHECK(Contains(message("affiliation", a), "'affiliation_type' is required"));
	a.fields["affiliation_type"] = "job";
	CHECK(Contains(message("affiliation", a), "must be one of"));
	a.fields["affiliation_type"] = "invited-position";
	const std::string inv = orcid::WriteDocument("affiliation", a, orcid::WriteFormat::Xml);
	CHECK(Contains(inv, "<invited-position:invited-position xmlns:common=\"http://www.orcid.org/ns/common\" "
	                    "xmlns:invited-position=\"http://www.orcid.org/ns/invited-position\">"));
	CHECK(Contains(inv, "<common:address/>")); // organization-address is required, its children are not
	CHECK_EQ(ExtractAny("affiliations", inv).Get(0, "affiliation_type"), std::string("invited-position"));
	a.fields["org_disambiguated_id"] = "https://ror.org/x";
	CHECK(Contains(message("affiliation", a), "'org_disambiguation_source' is required"));

	orcid::WriteRecord f;
	f.fields["type"] = "grant";
	CHECK(Contains(message("funding", f), "'org_name' is required"));
	f.fields["org_name"] = "O";
	f.fields["amount"] = "10";
	CHECK(Contains(message("funding", f), "'currency' is required with 'amount'"));
	f.fields["currency"] = "EUR";
	// funding-3.0.xsd has no contributor-sequence: an error, not a dropped value
	orcid::WriteRecord fc1, fc2;
	fc1.fields["credit_name"] = "A";
	fc1.fields["role"] = "lead";
	fc2.fields["credit_name"] = "B";
	fc2.fields["sequence"] = "first";
	f.lists["contributors"] = {fc1, fc2};
	const std::string seq = message("funding", f);
	CHECK(Contains(seq, "'contributors[2].sequence'"));
	CHECK(Contains(seq, "funding/contributors/contributor[2]/contributor-attributes/contributor-sequence"));
	CHECK(Contains(seq, "a funding contributor has no contributor-sequence (funding-3.0.xsd)"));
	CHECK(Throws([&] { orcid::WriteDocument("funding", f, orcid::WriteFormat::Json); }));
	f.lists["contributors"][1].fields["sequence"] = "  "; // blank is absent
	CHECK(message("funding", f).empty());
	f.lists.erase("contributors");

	orcid::WriteRecord rr;
	rr.fields["title"] = "T";
	rr.fields["host_name"] = "H";
	CHECK(Contains(message("research_resource", rr), "'resource_items' needs at least one item"));

	orcid::WriteRecord k;
	k.fields["kind"] = "email";
	k.fields["value"] = "a@b.c";
	CHECK(Contains(message("person_item", k), "kind must be one of"));
	k.fields["kind"] = "keyword";
	CHECK(Contains(orcid::WriteDocument("person_item", k, orcid::WriteFormat::Xml), "<keyword:content>a@b.c</keyword:content>"));

	// bulk: 1..MAX_BULK_WORKS works
	orcid::WriteRecord one;
	one.fields["title"] = "T";
	one.fields["type"] = "other";
	CHECK(Contains(Message([] { orcid::WriteBulkWorks({}, orcid::WriteFormat::Xml); }), "at least one work"));
	std::vector<orcid::WriteRecord> works(orcid::MAX_BULK_WORKS, one);
	auto bulk = ExtractAny("works", orcid::WriteBulkWorks(works, orcid::WriteFormat::Json));
	CHECK_EQ(bulk.rows.size(), orcid::MAX_BULK_WORKS);
	works.push_back(one);
	CHECK(Contains(Message([&] { orcid::WriteBulkWorks(works, orcid::WriteFormat::Xml); }), "at most 100"));
	works[50].fields.erase("type");
	works.resize(60);
	CHECK(Contains(Message([&] { orcid::WriteBulkWorks(works, orcid::WriteFormat::Xml); }), "'works[51].type'"));
}

//! Read -> write -> read over write samples: every section's rows come back
//! (source_hash aside, which hashes the new document).
static void TestWriterRoundTrips() {
	for (const char *name : {"work-full-3.0.xml", "work-full-3.0.json", "work-simple-3.0.xml"}) {
		const std::string doc = ReadFile(WRITE_SAMPLES + name);
		auto w = ExtractAny("works", doc);
		auto ids = ExtractAny("external_ids", doc);
		auto con = ExtractAny("contributors", doc);
		CHECK_EQ(w.rows.size(), size_t(1));
		if (w.rows.size() != 1) {
			continue;
		}
		orcid::WriteRecord rec = RecordOf(w, 0);
		rec.lists["external_ids"] = RecordsOf(ids);
		rec.lists["contributors"] = RecordsOf(con);
		for (auto format : {orcid::WriteFormat::Xml, orcid::WriteFormat::Json}) {
			std::fprintf(stderr, "  round trip: %s as %s\n", name, format == orcid::WriteFormat::Xml ? "XML" : "JSON");
			const std::string out = orcid::WriteDocument("work", rec, format);
			CHECK(SameRows(w, ExtractAny("works", out)));
			CHECK(SameRows(ids, ExtractAny("external_ids", out)));
			CHECK(SameRows(con, ExtractAny("contributors", out)));
		}
	}
	for (const char *name : {"peer-review-full-3.0.xml", "peer-review-full-3.0.json", "peer-review-simple-3.0.xml"}) {
		const std::string doc = ReadFile(WRITE_SAMPLES + name);
		auto p = ExtractAny("peer_reviews", doc);
		auto ids = ExtractAny("external_ids", doc);
		CHECK_EQ(p.rows.size(), size_t(1));
		if (p.rows.size() != 1) {
			continue;
		}
		orcid::WriteRecord rec = RecordOf(p, 0);
		rec.lists["external_ids"] = RecordsOf(ids); // the subject id is the item of container 'peer-review'
		for (auto format : {orcid::WriteFormat::Xml, orcid::WriteFormat::Json}) {
			std::fprintf(stderr, "  round trip: %s as %s\n", name, format == orcid::WriteFormat::Xml ? "XML" : "JSON");
			const std::string out = orcid::WriteDocument("peer_review", rec, format);
			CHECK(SameRows(p, ExtractAny("peer_reviews", out)));
			CHECK(SameRows(ids, ExtractAny("external_ids", out)));
		}
	}
	// research resource (XML sample): 2 proposal ids, then items of 2 and 1 ids
	// (grep -c '<common:external-id>' in the proposal and in each resource-item)
	{
		const std::string doc = ReadFile(WRITE_SAMPLES + "research-resource-3.0.xml");
		auto r = ExtractAny("research_resources", doc);
		auto items = ExtractAny("research_resource_items", doc);
		auto ids = ExtractAny("external_ids", doc);
		CHECK_EQ(items.rows.size(), size_t(2));
		CHECK_EQ(ids.rows.size(), size_t(5));
		if (r.rows.size() == 1 && items.rows.size() == 2 && ids.rows.size() == 5) {
			auto id_records = RecordsOf(ids);
			orcid::WriteRecord rec = RecordOf(r, 0);
			rec.lists["external_ids"] = {id_records[0], id_records[1]};
			orcid::WriteRecord i1 = RecordOf(items, 0), i2 = RecordOf(items, 1);
			i1.lists["external_ids"] = {id_records[2], id_records[3]};
			i2.lists["external_ids"] = {id_records[4]};
			rec.lists["resource_items"] = {i1, i2};
			for (auto format : {orcid::WriteFormat::Xml, orcid::WriteFormat::Json}) {
				const std::string out = orcid::WriteDocument("research_resource", rec, format);
				CHECK(SameRows(r, ExtractAny("research_resources", out)));
				CHECK(SameRows(items, ExtractAny("research_resource_items", out)));
				CHECK(SameRows(ids, ExtractAny("external_ids", out)));
			}
		}
	}
	// person items and group ids: the row is the whole record
	struct Single {
		const char *file;
		const char *section;
		const char *writer;
		const char *json_root;
	};
	for (auto &s : std::vector<Single> {{"address-3.0.xml", "person_items", "person_item", ""},
	                                    {"keyword-3.0.xml", "person_items", "person_item", "keyword"},
	                                    {"other-name-3.0.xml", "person_items", "person_item", "other-name"},
	                                    {"researcher-url-3.0.xml", "person_items", "person_item", ""},
	                                    {"external-identifier-3.0.json", "person_items", "person_item", ""},
	                                    {"peer-review-group-id.json", "group_ids", "group_id", ""}}) {
		auto e = ExtractAny(s.section, ReadFile(WRITE_SAMPLES + s.file));
		CHECK_EQ(e.rows.size(), size_t(1));
		if (e.rows.size() != 1) {
			continue;
		}
		for (auto format : {orcid::WriteFormat::Xml, orcid::WriteFormat::Json}) {
			std::fprintf(stderr, "  round trip: %s as %s\n", s.file, format == orcid::WriteFormat::Xml ? "XML" : "JSON");
			const std::string out = orcid::WriteDocument(s.writer, RecordOf(e, 0), format);
			CHECK(SameRows(e, ExtractAny(s.section, out, format == orcid::WriteFormat::Json ? s.json_root : "")));
		}
	}
	// the seven affiliation types (XML samples; JSON readback needs json_root)
	for (const char *type : {"distinction", "education", "employment", "invited-position", "membership",
	                         "qualification", "service"}) {
		const std::string doc = ReadFile(WRITE_SAMPLES + type + "-3.0.xml");
		auto a = ExtractAny("affiliations", doc);
		auto ids = ExtractAny("external_ids", doc);
		CHECK_EQ(a.rows.size(), size_t(1));
		if (a.rows.size() != 1) {
			continue;
		}
		orcid::WriteRecord rec = RecordOf(a, 0);
		rec.lists["external_ids"] = RecordsOf(ids);
		const std::string x = orcid::WriteDocument("affiliation", rec, orcid::WriteFormat::Xml);
		const std::string j = orcid::WriteDocument("affiliation", rec, orcid::WriteFormat::Json);
		CHECK(SameRows(a, ExtractAny("affiliations", x)));
		CHECK(SameRows(a, ExtractAny("affiliations", j, type)));
		CHECK(SameRows(ids, ExtractAny("external_ids", j, type)));
	}
}

// ---------------------------------------------------------------------------
// Crosswalk mode (src/core/crosswalk.cpp): yorc-rs's payload bytes.
// The exact strings follow yorc-rs's rules
// (src/xml/write.rs, src/json.rs); the keyword document is yorc-rs's own
// unit test (write.rs, `deterministic_layout_and_escaping`).
// ---------------------------------------------------------------------------

static std::string Crosswalk(const std::string &writer, const orcid::WriteRecord &r, orcid::WriteFormat format) {
	return orcid::WriteDocument(writer, r, format, orcid::WriteStyle::Crosswalk);
}

static void TestCrosswalk() {
	using orcid::WriteFormat;
	using orcid::WriteStyle;

	// serde_json's string escaping: DEL and U+2028 raw, other controls \u00xx in lower case
	std::string s;
	orcid::write_tree::AppendSerdeJsonString("\"q\" \\ /\b\f\n\r\t\x01\x1f\x7f \xE2\x80\xA8 \xF0\x9F\x98\x80", s);
	CHECK_EQ(s, std::string("\"\\\"q\\\" \\\\ /\\b\\f\\n\\r\\t\\u0001\\u001f\x7f \xE2\x80\xA8 \xF0\x9F\x98\x80\""));
	s.clear();
	CHECK(Throws([&] { orcid::write_tree::AppendSerdeJsonString("bad \xC3\x28", s); }));

	// put-code as yorc-rs prints its i64
	std::string n;
	CHECK(orcid::write_tree::CrosswalkInteger("007", n) && n == "7");
	CHECK(orcid::write_tree::CrosswalkInteger("0", n) && n == "0");
	CHECK(orcid::write_tree::CrosswalkInteger("-0", n) && n == "0");
	CHECK(orcid::write_tree::CrosswalkInteger("-012", n) && n == "-12");
	CHECK(orcid::write_tree::CrosswalkInteger("9223372036854775807", n) && n == "9223372036854775807");
	CHECK(orcid::write_tree::CrosswalkInteger("-9223372036854775808", n) && n == "-9223372036854775808");
	CHECK(!orcid::write_tree::CrosswalkInteger("9223372036854775808", n));
	CHECK(!orcid::write_tree::CrosswalkInteger("12345678901234567890", n));
	CHECK(!orcid::write_tree::CrosswalkInteger("", n));
	CHECK(!orcid::write_tree::CrosswalkInteger("-", n));
	CHECK(!orcid::write_tree::CrosswalkInteger("1a", n));

	// yorc-rs's own layout test (write.rs): declaration, the one namespace,
	// put-code then visibility, two-space indentation, text escaping, final LF
	orcid::WriteRecord k;
	k.fields["kind"] = "keyword";
	k.fields["value"] = "R&D <x> \"q\"";
	k.fields["put_code"] = "7";
	k.fields["visibility"] = "public";
	CHECK_EQ(Crosswalk("person_item", k, WriteFormat::Xml),
	         std::string("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
	                     "<keyword:keyword xmlns:keyword=\"http://www.orcid.org/ns/keyword\" put-code=\"7\" "
	                     "visibility=\"public\">\n"
	                     "  <keyword:content>R&amp;D &lt;x&gt; \"q\"</keyword:content>\n"
	                     "</keyword:keyword>\n"));
	CHECK_EQ(Crosswalk("person_item", k, WriteFormat::Json),
	         std::string("{\n  \"content\": \"R&D <x> \\\"q\\\"\",\n  \"put-code\": 7,\n  \"visibility\": \"public\"\n}\n"));

	// indentation at every depth, zero-padded dates, a normalized put-code; JSON keys sorted
	orcid::WriteRecord w;
	w.fields["title"] = "T";
	w.fields["type"] = "other";
	w.fields["pub_year"] = "2012";
	w.fields["pub_month"] = "1";
	w.fields["pub_day"] = "2";
	w.fields["put_code"] = "007";
	orcid::WriteRecord c;
	c.fields["credit_name"] = "A";
	c.fields["sequence"] = "first";
	w.lists["contributors"] = {c};
	CHECK_EQ(Crosswalk("work", w, WriteFormat::Xml),
	         std::string("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
	                     "<work:work xmlns:common=\"http://www.orcid.org/ns/common\" "
	                     "xmlns:work=\"http://www.orcid.org/ns/work\" put-code=\"7\">\n"
	                     "  <work:title>\n"
	                     "    <common:title>T</common:title>\n"
	                     "  </work:title>\n"
	                     "  <work:type>other</work:type>\n"
	                     "  <common:publication-date>\n"
	                     "    <common:year>2012</common:year>\n"
	                     "    <common:month>01</common:month>\n"
	                     "    <common:day>02</common:day>\n"
	                     "  </common:publication-date>\n"
	                     "  <work:contributors>\n"
	                     "    <work:contributor>\n"
	                     "      <work:credit-name>A</work:credit-name>\n"
	                     "      <work:contributor-attributes>\n"
	                     "        <work:contributor-sequence>first</work:contributor-sequence>\n"
	                     "      </work:contributor-attributes>\n"
	                     "    </work:contributor>\n"
	                     "  </work:contributors>\n"
	                     "</work:work>\n"));
	CHECK_EQ(Crosswalk("work", w, WriteFormat::Json),
	         std::string("{\n"
	                     "  \"contributors\": {\n"
	                     "    \"contributor\": [\n"
	                     "      {\n"
	                     "        \"contributor-attributes\": {\n"
	                     "          \"contributor-sequence\": \"first\"\n"
	                     "        },\n"
	                     "        \"credit-name\": {\n"
	                     "          \"value\": \"A\"\n"
	                     "        }\n"
	                     "      }\n"
	                     "    ]\n"
	                     "  },\n"
	                     "  \"publication-date\": {\n"
	                     "    \"day\": {\n"
	                     "      \"value\": \"02\"\n"
	                     "    },\n"
	                     "    \"month\": {\n"
	                     "      \"value\": \"01\"\n"
	                     "    },\n"
	                     "    \"year\": {\n"
	                     "      \"value\": \"2012\"\n"
	                     "    }\n"
	                     "  },\n"
	                     "  \"put-code\": 7,\n"
	                     "  \"title\": {\n"
	                     "    \"title\": {\n"
	                     "      \"value\": \"T\"\n"
	                     "    }\n"
	                     "  },\n"
	                     "  \"type\": \"other\"\n"
	                     "}\n"));
	// the default is unchanged: tabs, schema order, the put-code as given
	CHECK_EQ(orcid::WriteDocument("work", w, WriteFormat::Xml, WriteStyle::Native),
	         orcid::WriteDocument("work", w, WriteFormat::Xml));
	CHECK_EQ(orcid::WriteDocument("work", w, WriteFormat::Json, WriteStyle::Native),
	         orcid::WriteDocument("work", w, WriteFormat::Json));
	CHECK(Contains(orcid::WriteDocument("work", w, WriteFormat::Xml), "\n\t<work:title>\n\t\t<common:title>"));
	CHECK(Contains(orcid::WriteDocument("work", w, WriteFormat::Xml), " put-code=\"007\">"));

	// a translated title: its attribute after "value" in the default, sorted before it here
	orcid::WriteRecord t;
	t.fields["title"] = "T";
	t.fields["type"] = "other";
	t.fields["translated_title"] = "U";
	t.fields["translated_title_language"] = "fr";
	CHECK(Contains(Crosswalk("work", t, WriteFormat::Json),
	               "\"translated-title\": {\n      \"language-code\": \"fr\",\n      \"value\": \"U\"\n    }"));
	CHECK(Contains(orcid::WriteDocument("work", t, WriteFormat::Json),
	               "\"translated-title\": {\n      \"value\": \"U\",\n      \"language-code\": \"fr\"\n    }"));
	CHECK(Contains(Crosswalk("work", t, WriteFormat::Xml),
	               "    <common:translated-title language-code=\"fr\">U</common:translated-title>\n"));

	// containers yorc-rs always writes: empty external ids are "external-id": [] (json.rs req_ext_ids)
	orcid::WriteRecord rr;
	rr.fields["title"] = "P";
	rr.fields["host_name"] = "H";
	rr.lists["external_ids"] = {};
	orcid::WriteRecord item;
	item.fields["resource_name"] = "R";
	item.fields["resource_type"] = "equipment";
	item.fields["host_name"] = "H";
	rr.lists["resource_items"] = {item};
	const std::string rj = Crosswalk("research_resource", rr, WriteFormat::Json);
	CHECK(Contains(rj, "  \"proposal\": {\n    \"external-ids\": {\n      \"external-id\": []\n    },\n"
	                   "    \"hosts\": {\n      \"organization\": [\n        {\n"));
	CHECK(Contains(rj, "  \"resource-item\": [\n    {\n      \"external-ids\": {\n        \"external-id\": []\n      },\n"));
	CHECK(!Contains(rj, "\"organization\": []")); // the flat host is the hosts' one organization
	CHECK(Contains(orcid::WriteDocument("research_resource", rr, WriteFormat::Json), "\"external-ids\": {}"));
	const std::string rx = Crosswalk("research_resource", rr, WriteFormat::Xml);
	CHECK(Contains(rx, "\n    <common:external-ids/>\n"));
	CHECK(Contains(rx, "\n  <research-resource:resource-items>\n    <research-resource:resource-item>\n"));
	orcid::WriteRecord pr;
	pr.fields["reviewer_role"] = "reviewer";
	pr.fields["review_type"] = "review";
	pr.fields["completion_year"] = "2012";
	pr.fields["review_group_id"] = "issn:1741-4857";
	pr.fields["convening_org_name"] = "O";
	CHECK(Contains(Crosswalk("peer_review", pr, WriteFormat::Json),
	               "  \"review-identifiers\": {\n    \"external-id\": []\n  },\n"));
	CHECK(Contains(Crosswalk("peer_review", pr, WriteFormat::Xml), "\n  <peer-review:review-identifiers/>\n"));

	// escaping: XML as yorc-rs's `escape` (attributes: " TAB LF too; CR always &#13;)
	orcid::WriteRecord e;
	e.fields["title"] = "a\rb\tc\nd";
	e.fields["type"] = "other";
	e.fields["visibility"] = "pub\"lic<\t";
	const std::string ex = Crosswalk("work", e, WriteFormat::Xml);
	CHECK(Contains(ex, " visibility=\"pub&quot;lic&lt;&#9;\">\n"));
	CHECK(Contains(ex, "    <common:title>a&#13;b\tc\nd</common:title>\n"));
	e.fields["title"] = "bell \x07";
	CHECK(Contains(Message([&] { Crosswalk("work", e, WriteFormat::Xml); }), "XML 1.0"));
	// JSON: DEL raw (serde_json), where the default writes \u007f
	e.fields["title"] = "del \x7f";
	CHECK(Contains(Crosswalk("work", e, WriteFormat::Json), "\"value\": \"del \x7f\""));
	CHECK(Contains(orcid::WriteDocument("work", e, WriteFormat::Json), "\"value\": \"del \\u007f\""));
	e.fields["title"] = "bad \xC3\x28";
	CHECK(Throws([&] { Crosswalk("work", e, WriteFormat::Json); }));

	// a put-code yorc-rs's i64 cannot hold: the default writes it, crosswalk refuses it
	orcid::WriteRecord big;
	big.fields["title"] = "T";
	big.fields["type"] = "other";
	big.fields["put_code"] = "9223372036854775808";
	CHECK(Contains(orcid::WriteDocument("work", big, WriteFormat::Xml), "put-code=\"9223372036854775808\""));
	CHECK(Contains(Message([&] { Crosswalk("work", big, WriteFormat::Json); }),
	               "work: 'put-code' is 9223372036854775808, outside yorc-rs's put-code range"));

	// bulk: bulk:bulk with every namespace on the root; {"bulk": [{"work": ...}]}
	orcid::WriteRecord a, b;
	a.fields["title"] = "A";
	a.fields["type"] = "other";
	b.fields["title"] = "B";
	b.fields["type"] = "other";
	CHECK_EQ(orcid::WriteBulkWorks({a, b}, WriteFormat::Xml, WriteStyle::Crosswalk),
	         std::string("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
	                     "<bulk:bulk xmlns:bulk=\"http://www.orcid.org/ns/bulk\" "
	                     "xmlns:common=\"http://www.orcid.org/ns/common\" xmlns:work=\"http://www.orcid.org/ns/work\">\n"
	                     "  <work:work>\n    <work:title>\n      <common:title>A</common:title>\n    </work:title>\n"
	                     "    <work:type>other</work:type>\n  </work:work>\n"
	                     "  <work:work>\n    <work:title>\n      <common:title>B</common:title>\n    </work:title>\n"
	                     "    <work:type>other</work:type>\n  </work:work>\n"
	                     "</bulk:bulk>\n"));
	CHECK_EQ(orcid::WriteBulkWorks({a, b}, WriteFormat::Json, WriteStyle::Crosswalk),
	         std::string("{\n  \"bulk\": [\n"
	                     "    {\n      \"work\": {\n        \"title\": {\n          \"title\": {\n"
	                     "            \"value\": \"A\"\n          }\n        },\n        \"type\": \"other\"\n      }\n    },\n"
	                     "    {\n      \"work\": {\n        \"title\": {\n          \"title\": {\n"
	                     "            \"value\": \"B\"\n          }\n        },\n        \"type\": \"other\"\n      }\n    }\n"
	                     "  ]\n}\n"));
	CHECK_EQ(orcid::WriteBulkWorks({a, b}, WriteFormat::Json, WriteStyle::Native),
	         orcid::WriteBulkWorks({a, b}, WriteFormat::Json));
	// the writers' checks are the same in both styles
	CHECK(Contains(Message([] { orcid::WriteBulkWorks({}, WriteFormat::Xml, WriteStyle::Crosswalk); }),
	               "at least one work"));
	orcid::WriteRecord untitled;
	untitled.fields["type"] = "other";
	CHECK(Contains(Message([&] { Crosswalk("work", untitled, WriteFormat::Json); }), "'title' is required"));
}

//! Every XML write sample, read into rows, assembled as TestWriterRoundTrips
//! does and written in crosswalk mode, against the bytes in
//! test/data/crosswalk/write_samples (scripts/crosswalk_expected.py: yorc-rs's
//! rules applied to the sample). The authoritative comparison with the real
//! Rust output is the cross-implementation check (BENCHMARKS.md, "Parity").
static void TestCrosswalkSamples() {
	static const std::string EXPECTED = "test/data/crosswalk/write_samples/";
	size_t compared = 0;
	auto compare = [&](const std::string &stem, const std::string &writer, const orcid::WriteRecord *rec,
	                   const std::vector<orcid::WriteRecord> *bulk) {
		for (auto format : {orcid::WriteFormat::Xml, orcid::WriteFormat::Json}) {
			const bool xml = format == orcid::WriteFormat::Xml;
			const std::string want = ReadFile(EXPECTED + stem + (xml ? ".xml" : ".json"));
			std::string got;
			try {
				got = bulk ? orcid::WriteBulkWorks(*bulk, format, orcid::WriteStyle::Crosswalk)
				           : Crosswalk(writer, *rec, format);
			} catch (const std::exception &ex) {
				got = std::string("error: ") + ex.what();
			}
			if (got != want) {
				std::fprintf(stderr, "  crosswalk %s.%s differs:\n--- got ---\n%s--- want ---\n%s", stem.c_str(),
				             xml ? "xml" : "json", got.c_str(), want.c_str());
			}
			CHECK(got == want);
			compared++;
		}
	};
	for (const char *stem : {"work-full-3.0", "work-simple-3.0"}) {
		const std::string doc = ReadFile(WRITE_SAMPLES + stem + ".xml");
		auto w = ExtractAny("works", doc);
		CHECK_EQ(w.rows.size(), size_t(1));
		if (w.rows.size() == 1) {
			orcid::WriteRecord rec = RecordOf(w, 0);
			rec.lists["external_ids"] = RecordsOf(ExtractAny("external_ids", doc));
			rec.lists["contributors"] = RecordsOf(ExtractAny("contributors", doc));
			compare(stem, "work", &rec, nullptr);
		}
	}
	{
		// one external id per work (grep -c '<common:external-id>'), none with contributors
		const std::string doc = ReadFile(WRITE_SAMPLES + "bulk-work-3.0.xml");
		auto w = ExtractAny("works", doc);
		auto ids = RecordsOf(ExtractAny("external_ids", doc));
		CHECK_EQ(w.rows.size(), size_t(2));
		CHECK_EQ(ids.size(), size_t(2));
		if (w.rows.size() == 2 && ids.size() == 2) {
			std::vector<orcid::WriteRecord> works = RecordsOf(w);
			works[0].lists["external_ids"] = {ids[0]};
			works[1].lists["external_ids"] = {ids[1]};
			compare("bulk-work-3.0", "", nullptr, &works);
		}
	}
	{
		const std::string doc = ReadFile(WRITE_SAMPLES + "funding-3.0.xml");
		auto f = ExtractAny("fundings", doc);
		CHECK_EQ(f.rows.size(), size_t(1));
		if (f.rows.size() == 1) {
			orcid::WriteRecord rec = RecordOf(f, 0);
			rec.lists["external_ids"] = RecordsOf(ExtractAny("external_ids", doc));
			rec.lists["contributors"] = RecordsOf(ExtractAny("contributors", doc));
			compare("funding-3.0", "funding", &rec, nullptr);
		}
	}
	for (const char *type : {"distinction", "education", "employment", "invited-position", "membership",
	                         "qualification", "service"}) {
		const std::string stem = std::string(type) + "-3.0";
		const std::string doc = ReadFile(WRITE_SAMPLES + stem + ".xml");
		auto a = ExtractAny("affiliations", doc);
		CHECK_EQ(a.rows.size(), size_t(1));
		if (a.rows.size() == 1) {
			orcid::WriteRecord rec = RecordOf(a, 0);
			rec.lists["external_ids"] = RecordsOf(ExtractAny("external_ids", doc));
			compare(stem, "affiliation", &rec, nullptr);
		}
	}
	for (const char *stem : {"peer-review-full-3.0", "peer-review-full-award-subject-type-3.0", "peer-review-simple-3.0"}) {
		const std::string doc = ReadFile(WRITE_SAMPLES + stem + ".xml");
		auto p = ExtractAny("peer_reviews", doc);
		CHECK_EQ(p.rows.size(), size_t(1));
		if (p.rows.size() == 1) {
			orcid::WriteRecord rec = RecordOf(p, 0);
			rec.lists["external_ids"] = RecordsOf(ExtractAny("external_ids", doc)); // the subject id: container 'peer-review'
			compare(stem, "peer_review", &rec, nullptr);
		}
	}
	{
		// 2 proposal ids, then items of 2 and 1 (as TestWriterRoundTrips); one host each
		const std::string doc = ReadFile(WRITE_SAMPLES + "research-resource-3.0.xml");
		auto r = ExtractAny("research_resources", doc);
		auto items = ExtractAny("research_resource_items", doc);
		auto ids = RecordsOf(ExtractAny("external_ids", doc));
		if (r.rows.size() == 1 && items.rows.size() == 2 && ids.size() == 5) {
			orcid::WriteRecord rec = RecordOf(r, 0);
			rec.lists["external_ids"] = {ids[0], ids[1]};
			orcid::WriteRecord i1 = RecordOf(items, 0), i2 = RecordOf(items, 1);
			i1.lists["external_ids"] = {ids[2], ids[3]};
			i2.lists["external_ids"] = {ids[4]};
			rec.lists["resource_items"] = {i1, i2};
			compare("research-resource-3.0", "research_resource", &rec, nullptr);
		} else {
			CHECK(false);
		}
	}
	for (const char *stem : {"address-3.0", "keyword-3.0", "other-name-3.0", "researcher-url-3.0",
	                         "researcher-url-3.0-no-name", "external-identifier-3.0"}) {
		auto e = ExtractAny("person_items", ReadFile(WRITE_SAMPLES + stem + ".xml"));
		CHECK_EQ(e.rows.size(), size_t(1));
		if (e.rows.size() == 1) {
			const orcid::WriteRecord rec = RecordOf(e, 0);
			compare(stem, "person_item", &rec, nullptr);
		}
	}
	CHECK_EQ(compared, size_t(42)); // the 21 XML write samples, as XML and as JSON
}

//! XML input is checked for UTF-8 where it enters the extractor.
static void TestXmlUtf8() {
	CHECK_EQ(orcid::Utf8ValidPrefix("", 0), size_t(0));
	const std::string ascii(100, 'a');
	CHECK_EQ(orcid::Utf8ValidPrefix(ascii.data(), ascii.size()), ascii.size());
	// the bad byte at every offset around the 8-byte fast path
	for (size_t at = 0; at < 20; at++) {
		std::string t(24, 'x');
		t[at] = '\xFF';
		CHECK_EQ(orcid::Utf8ValidPrefix(t.data(), t.size()), at);
	}
	const std::string mixed = std::string(13, 'a') + "\xC3\xA9" + std::string(9, 'b') + "\xF0\x9F\x98\x80";
	CHECK_EQ(orcid::Utf8ValidPrefix(mixed.data(), mixed.size()), mixed.size());
	const std::string truncated = std::string(10, 'a') + "\xE2\x82";
	CHECK_EQ(orcid::Utf8ValidPrefix(truncated.data(), truncated.size()), size_t(10));
	const std::string surrogate = "ab\xED\xA0\x80";
	CHECK_EQ(orcid::Utf8ValidPrefix(surrogate.data(), surrogate.size()), size_t(2));

	const std::string head = "<work:work xmlns:work=\"http://www.orcid.org/ns/work\" "
	                         "xmlns:common=\"http://www.orcid.org/ns/common\"><work:title><common:title>";
	const std::string tail = "</common:title></work:title><work:type>other</work:type></work:work>";
	// valid UTF-8 reads as before
	CHECK_EQ(Extract("works", head + "caf\xC3\xA9" + tail).Get(0, "title"), std::string("caf\xC3\xA9"));
	// Latin-1 'e acute': an XmlError naming the byte, in text and in an attribute value
	const std::string latin1 = head + "caf\xE9" + tail;
	const std::string m = Message([&] { Extract("works", latin1); });
	CHECK_EQ(m, "invalid UTF-8 at byte " + std::to_string(head.size() + 3));
	CHECK(Throws([&] { Extract("works", "<work:work put-code=\"1\xFF\"/>"); }));
	bool xml_error = false;
	try {
		std::vector<orcid::Row> rows;
		orcid::ExtractDocument(*orcid::FindSection("works"), latin1.data(), latin1.size(), orcid::DocFormat::Xml, "",
		                       rows);
	} catch (const orcid::XmlError &) {
		xml_error = true;
	}
	CHECK(xml_error);
	// character references that name no Unicode scalar value stay literal text
	CHECK_EQ(Extract("works", head + "a&#xD800;b&#x110000;c&#0;" + tail).Get(0, "title"),
	         std::string("a&#xD800;b&#x110000;c&#0;"));
	CHECK_EQ(Extract("works", head + "&#x1F600;" + tail).Get(0, "title"), std::string("\xF0\x9F\x98\x80"));
}

// orcid_dump_sample's sampler: the contract vectors of yorc-rs's
// src/dump/reservoir.rs and yorc-zig's src/reservoir.zig.
static std::vector<uint32_t> ReservoirSample(uint64_t n, uint64_t seed, uint32_t total) {
	orcid::ReservoirSampler r(n, seed);
	std::vector<uint32_t> items;
	for (uint32_t i = 0; i < total; i++) {
		const orcid::Slot slot = r.Offer();
		if (slot.kind == orcid::SlotKind::Append) {
			items.push_back(i);
		} else if (slot.kind == orcid::SlotKind::Replace) {
			items[static_cast<size_t>(slot.index)] = i;
		}
	}
	return items;
}

static std::string Joined(std::vector<uint32_t> v, bool sorted) {
	if (sorted) {
		std::sort(v.begin(), v.end());
	}
	std::string out;
	for (auto x : v) {
		out += (out.empty() ? "" : ",") + std::to_string(x);
	}
	return out;
}

static void TestReservoir() {
	orcid::SplitMix64 g(0);
	CHECK(g.Next() == 0xe220a8397b1dcdafull);
	CHECK(g.Next() == 0x6e789e6aa1b965f4ull);
	CHECK(g.Next() == 0x06c45d188009454full);
	orcid::SplitMix64 h(1234567);
	CHECK(h.Next() == 6457827717110365317ull);
	CHECK(h.Next() == 3203168211198807973ull);
	CHECK(h.Next() == 9817491932198370423ull);
	CHECK(orcid::MulHigh64(~0ull, ~0ull) == ~0ull - 1);
	CHECK(orcid::MulHigh64(1ull << 63, 2) == 1);
	CHECK(orcid::MulHigh64(0x123456789abcdefull, 0x10) == 0);
	orcid::SplitMix64 b(42);
	for (uint64_t n : {1ull, 2ull, 3ull, 10ull, 1000ull, ~0ull}) {
		for (int i = 0; i < 1000; i++) {
			CHECK(b.Below(n) < n);
		}
	}
	CHECK_EQ(Joined(ReservoirSample(3, 42, 10), true), std::string("5,7,9"));
	CHECK_EQ(Joined(ReservoirSample(5, 1, 1000), true), std::string("71,165,177,730,916"));
	CHECK_EQ(Joined(ReservoirSample(5, 7, 10000), false), std::string("8135,5980,1301,5670,6839"));
	CHECK_EQ(Joined(ReservoirSample(10, 1, 5), false), std::string("0,1,2,3,4"));
	CHECK(ReservoirSample(0, 1, 100).empty());
	// the dump fixture's two records, capacity 1 (the walkthroughs, cell 8):
	// seed 42 draws below(2) = 1 (keep the first), seed 7 draws 0 (the second)
	CHECK_EQ(Joined(ReservoirSample(1, 42, 2), false), std::string("0"));
	CHECK_EQ(Joined(ReservoirSample(1, 7, 2), false), std::string("1"));

	std::string id;
	CHECK(orcid::DumpSampleEligible("ORCID_test_summaries/634/0000-0003-0100-6634.xml", id));
	CHECK_EQ(id, std::string("0000-0003-0100-6634"));
	CHECK(orcid::DumpSampleEligible("x/000000030100663 4.XML", id)); // NormalizeId: spaces, any case of .xml
	CHECK_EQ(id, std::string("0000-0003-0100-6634"));
	CHECK(!orcid::DumpSampleEligible("ORCID_test_summaries/README.txt", id));
	CHECK(!orcid::DumpSampleEligible("dir/.xml", id));
	CHECK(!orcid::DumpSampleEligible("dir/0000-0003-0100-6635.xml", id)); // check character
	CHECK(!orcid::DumpSampleEligible("d/and-a-file-name-too-0000-0000-0000-0000.xml", id));
	CHECK(!orcid::DumpSampleEligible("0000-0003-0100-6634.xml/notes", id));
}

int main() {
#if defined(__unix__) || defined(__APPLE__)
	alarm(60); // a looping parser fails the run instead of hanging it
#endif
	TestId();
	TestSha256();
	TestXml();
	TestParsers();
	TestXmlEdgeCases();
	TestSqlTestLiteral();
	TestWorksFixture();
	TestSampleRecord();
	TestSampleWorkFull();
	TestSampleResearchResource();
	TestSamplePeerReviewFull();
	TestSampleCollections();
	TestTar();
	TestTimeline();
	TestChanges();
	TestSummary();
	TestSearchAndErrors();
	TestNotificationsAndGroupIds();
	TestFullActivityItem();
	TestAnchoredItems();
	TestLockedDeprecated();
	TestTarCorruptSizes();
	TestJsonParser();
	TestUtf8AndEscaping();
	TestJsonCrosswalk();
	TestJsonRecordSample();
	TestWriterOutput();
	TestWriterChecks();
	TestWriterRoundTrips();
	TestCrosswalk();
	TestCrosswalkSamples();
	TestXmlUtf8();
	TestReservoir();
	if (failures) {
		std::fprintf(stderr, "core_test: %d check(s) failed\n", failures);
		return 1;
	}
	std::printf("core_test: all checks passed\n");
	return 0;
}
