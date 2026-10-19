#include "orcid/xml.hpp"

#include "orcid/json.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace orcid {

static bool IsSpace(char c) {
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static bool IsNameEnd(char c) {
	return IsSpace(c) || c == '>' || c == '/' || c == '=';
}

static void AppendUtf8(unsigned long cp, std::string &out) {
	if (cp < 0x80) {
		out.push_back(static_cast<char>(cp));
	} else if (cp < 0x800) {
		out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
		out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
	} else if (cp < 0x10000) {
		out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
		out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
		out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
	} else if (cp <= 0x10FFFF) {
		out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
		out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
		out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
		out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
	}
}

void XmlReader::DecodeInto(const char *begin, const char *end, std::string &out) {
	for (const char *s = begin; s < end;) {
		if (*s != '&') {
			const char *amp = static_cast<const char *>(std::memchr(s, '&', static_cast<size_t>(end - s)));
			const char *stop = amp ? amp : end;
			out.append(s, static_cast<size_t>(stop - s));
			s = stop;
			continue;
		}
		// a reference is at most 12 bytes after its '&': look no further, or a
		// run of '&' would cost quadratic time
		const size_t reach = std::min<size_t>(static_cast<size_t>(end - s), 13);
		const char *semi = static_cast<const char *>(std::memchr(s, ';', reach));
		if (!semi || semi - s > 12) {
			out.push_back('&'); // stray ampersand: keep literally
			s++;
			continue;
		}
		std::string ent(s + 1, semi);
		if (ent == "amp") {
			out.push_back('&');
		} else if (ent == "lt") {
			out.push_back('<');
		} else if (ent == "gt") {
			out.push_back('>');
		} else if (ent == "quot") {
			out.push_back('"');
		} else if (ent == "apos") {
			out.push_back('\'');
		} else if (ent.size() > 1 && ent[0] == '#') {
			bool hex = ent[1] == 'x' || ent[1] == 'X';
			char *parse_end = nullptr;
			unsigned long cp = std::strtoul(ent.c_str() + (hex ? 2 : 1), &parse_end, hex ? 16 : 10);
			if (*parse_end != '\0' || parse_end == ent.c_str() + (hex ? 2 : 1) || cp == 0 || cp > 0x10FFFF ||
			    (cp >= 0xD800 && cp <= 0xDFFF)) {
				// not a character reference to a Unicode scalar value (a surrogate,
				// beyond U+10FFFF, NUL, no digits): keep it literally rather than
				// write bytes that are not UTF-8
				out.append(s, static_cast<size_t>(semi + 1 - s));
			} else {
				AppendUtf8(cp, out);
			}
		} else {
			out.append(s, static_cast<size_t>(semi + 1 - s)); // unknown entity: keep literally
		}
		s = semi + 1;
	}
}

void XmlReader::SkipPast(const char *terminator) {
	size_t tlen = std::strlen(terminator);
	for (const char *s = p_; s + tlen <= end_; s++) {
		if (std::memcmp(s, terminator, tlen) == 0) {
			p_ = s + tlen;
			return;
		}
	}
	throw XmlError(std::string("unterminated construct, expected '") + terminator + "'");
}

const char *XmlReader::LocalName() const {
	auto colon = name_.find(':');
	return name_.c_str() + (colon == std::string::npos ? 0 : colon + 1);
}

bool XmlReader::Attribute(const char *qname, std::string &out) const {
	for (auto &kv : attrs_) {
		if (kv.first == qname) {
			out = kv.second;
			return true;
		}
	}
	return false;
}

// p_ points just past '<' of a start tag.
void XmlReader::ParseTag() {
	attrs_.clear();
	const char *s = p_;
	while (s < end_ && !IsNameEnd(*s)) {
		s++;
	}
	name_.assign(p_, s);
	if (name_.empty()) {
		throw XmlError("empty element name");
	}
	while (true) {
		while (s < end_ && IsSpace(*s)) {
			s++;
		}
		if (s >= end_) {
			throw XmlError("unterminated start tag <" + name_ + ">");
		}
		if (*s == '>') {
			p_ = s + 1;
			return;
		}
		if (*s == '/') {
			if (s + 1 >= end_ || s[1] != '>') {
				throw XmlError("malformed self-closing tag <" + name_ + ">");
			}
			p_ = s + 2;
			pending_end_ = true;
			return;
		}
		const char *attr_start = s;
		while (s < end_ && !IsNameEnd(*s)) {
			s++;
		}
		std::string attr_name(attr_start, s);
		while (s < end_ && IsSpace(*s)) {
			s++;
		}
		if (s >= end_ || *s != '=') {
			throw XmlError("attribute without value in <" + name_ + ">");
		}
		s++;
		while (s < end_ && IsSpace(*s)) {
			s++;
		}
		if (s >= end_ || (*s != '"' && *s != '\'')) {
			throw XmlError("unquoted attribute value in <" + name_ + ">");
		}
		char quote = *s++;
		const char *value_end = static_cast<const char *>(std::memchr(s, quote, static_cast<size_t>(end_ - s)));
		if (!value_end) {
			throw XmlError("unterminated attribute value in <" + name_ + ">");
		}
		std::string value;
		DecodeInto(s, value_end, value);
		attrs_.emplace_back(std::move(attr_name), std::move(value));
		s = value_end + 1;
	}
}

XmlEvent XmlReader::Next() {
	if (pending_end_) {
		pending_end_ = false;
		return XmlEvent::EndElement;
	}
	while (p_ < end_) {
		if (*p_ != '<') {
			const char *lt = static_cast<const char *>(std::memchr(p_, '<', static_cast<size_t>(end_ - p_)));
			const char *stop = lt ? lt : end_;
			text_.clear();
			DecodeInto(p_, stop, text_);
			p_ = stop;
			return XmlEvent::Text;
		}
		size_t remaining = static_cast<size_t>(end_ - p_);
		if (remaining >= 4 && std::memcmp(p_, "<!--", 4) == 0) {
			p_ += 4;
			SkipPast("-->");
		} else if (remaining >= 9 && std::memcmp(p_, "<![CDATA[", 9) == 0) {
			const char *start = p_ + 9;
			p_ = start;
			SkipPast("]]>");
			text_.assign(start, p_ - 3);
			return XmlEvent::Text;
		} else if (remaining >= 2 && p_[1] == '?') {
			p_ += 2;
			SkipPast("?>");
		} else if (remaining >= 2 && p_[1] == '!') {
			// DOCTYPE, possibly with an internal subset [...]
			int depth = 0;
			for (p_ += 2; p_ < end_; p_++) {
				if (*p_ == '[') {
					depth++;
				} else if (*p_ == ']') {
					depth--;
				} else if (*p_ == '>' && depth <= 0) {
					break;
				}
			}
			if (p_ >= end_) {
				throw XmlError("unterminated <! declaration");
			}
			p_++;
		} else if (remaining >= 2 && p_[1] == '/') {
			const char *s = p_ + 2;
			const char *gt = static_cast<const char *>(std::memchr(s, '>', static_cast<size_t>(end_ - s)));
			if (!gt) {
				throw XmlError("unterminated end tag");
			}
			const char *name_end = gt;
			while (name_end > s && IsSpace(name_end[-1])) {
				name_end--;
			}
			name_.assign(s, name_end);
			p_ = gt + 1;
			return XmlEvent::EndElement;
		} else {
			p_++;
			ParseTag();
			return XmlEvent::StartElement;
		}
	}
	return XmlEvent::End;
}

void AppendXmlEscaped(const std::string &text, bool attribute, std::string &out) {
	const char *p = text.data();
	const char *end = p + text.size();
	while (p < end) {
		const unsigned char c = static_cast<unsigned char>(*p);
		if (c >= 0x80) {
			const char *start = p;
			uint32_t cp;
			if (!DecodeUtf8(p, end, cp)) {
				throw XmlError("invalid UTF-8 at byte " + std::to_string(start - text.data()) + " of a value");
			}
			if (cp == 0xFFFE || cp == 0xFFFF) {
				throw XmlError("character U+" + std::string(cp == 0xFFFE ? "FFFE" : "FFFF") +
				               " cannot be written in XML 1.0");
			}
			out.append(start, static_cast<size_t>(p - start));
			continue;
		}
		p++;
		switch (c) {
		case '&':
			out += "&amp;";
			break;
		case '<':
			out += "&lt;";
			break;
		case '>':
			out += "&gt;";
			break;
		case '"':
			out += attribute ? "&quot;" : "\"";
			break;
		case '\r':
			out += "&#13;";
			break;
		case '\t':
			out += attribute ? "&#9;" : "\t";
			break;
		case '\n':
			out += attribute ? "&#10;" : "\n";
			break;
		default:
			if (c < 0x20) {
				char buf[8];
				std::snprintf(buf, sizeof(buf), "%02X", static_cast<unsigned>(c));
				throw XmlError(std::string("control character U+00") + buf + " cannot be written in XML 1.0");
			}
			out.push_back(static_cast<char>(c));
			break;
		}
	}
}

} // namespace orcid
