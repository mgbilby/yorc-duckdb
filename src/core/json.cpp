#include "orcid/json.hpp"

#include <cstring>

namespace orcid {

const JsonValue *JsonValue::Find(const std::string &key) const {
	for (auto &m : members) {
		if (m.first == key) {
			return &m.second;
		}
	}
	return nullptr;
}

// ---------------------------------------------------------------------------
// UTF-8 (RFC 3629)
// ---------------------------------------------------------------------------

bool DecodeUtf8(const char *&p, const char *end, uint32_t &cp) {
	if (p >= end) {
		return false;
	}
	const unsigned char *s = reinterpret_cast<const unsigned char *>(p);
	const size_t avail = static_cast<size_t>(end - p);
	const unsigned char c = s[0];
	if (c < 0x80) {
		cp = c;
		p++;
		return true;
	}
	size_t n;
	uint32_t value, min;
	if (c >= 0xC2 && c <= 0xDF) {
		n = 2;
		value = c & 0x1F;
		min = 0x80;
	} else if ((c & 0xF0) == 0xE0) {
		n = 3;
		value = c & 0x0F;
		min = 0x800;
	} else if (c >= 0xF0 && c <= 0xF4) {
		n = 4;
		value = c & 0x07;
		min = 0x10000;
	} else {
		return false; // continuation byte, 0xC0/0xC1 (always overlong) or 0xF5..0xFF
	}
	if (avail < n) {
		return false;
	}
	for (size_t i = 1; i < n; i++) {
		if ((s[i] & 0xC0) != 0x80) {
			return false;
		}
		value = (value << 6) | (s[i] & 0x3F);
	}
	if (value < min || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) {
		return false;
	}
	cp = value;
	p += n;
	return true;
}

size_t Utf8ValidPrefix(const char *data, size_t len) {
	const char *p = data;
	const char *end = data + len;
	while (p < end) {
		// ASCII fast path: eight bytes at a time while none has its high bit set.
		while (static_cast<size_t>(end - p) >= 8) {
			uint64_t word;
			std::memcpy(&word, p, 8);
			if (word & UINT64_C(0x8080808080808080)) {
				break;
			}
			p += 8;
		}
		if (p >= end) {
			break;
		}
		if (static_cast<unsigned char>(*p) < 0x80) {
			p++;
			continue;
		}
		uint32_t cp;
		if (!DecodeUtf8(p, end, cp)) {
			return static_cast<size_t>(p - data);
		}
	}
	return len;
}

bool IsValidUtf8(const char *data, size_t len) {
	return Utf8ValidPrefix(data, len) == len;
}

void AppendCodePoint(uint32_t cp, std::string &out) {
	if (cp < 0x80) {
		out.push_back(static_cast<char>(cp));
	} else if (cp < 0x800) {
		out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
		out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
	} else if (cp < 0x10000) {
		out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
		out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
		out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
	} else {
		out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
		out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
		out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
		out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
	}
}

void AppendJsonString(const std::string &text, std::string &out) {
	static const char HEX[] = "0123456789abcdef";
	out.push_back('"');
	const char *p = text.data();
	const char *end = p + text.size();
	while (p < end) {
		const unsigned char c = static_cast<unsigned char>(*p);
		if (c >= 0x20 && c < 0x7F && c != '"' && c != '\\') {
			out.push_back(static_cast<char>(c));
			p++;
			continue;
		}
		if (c < 0x80) {
			switch (c) {
			case '"':
				out += "\\\"";
				break;
			case '\\':
				out += "\\\\";
				break;
			case '\b':
				out += "\\b";
				break;
			case '\f':
				out += "\\f";
				break;
			case '\n':
				out += "\\n";
				break;
			case '\r':
				out += "\\r";
				break;
			case '\t':
				out += "\\t";
				break;
			default: // other C0 controls and DEL
				out += "\\u00";
				out.push_back(HEX[c >> 4]);
				out.push_back(HEX[c & 0xF]);
				break;
			}
			p++;
			continue;
		}
		const char *start = p;
		uint32_t cp;
		if (!DecodeUtf8(p, end, cp)) {
			throw JsonError("invalid UTF-8 at byte " + std::to_string(start - text.data()) + " of a string value");
		}
		// U+2028/U+2029 are written as themselves, as serde_json (yorc-rs)
		// and Jackson (ORCID's own orcid-model) do; JSON allows them unescaped.
		out.append(start, static_cast<size_t>(p - start));
	}
	out.push_back('"');
}

// ---------------------------------------------------------------------------
// Parser: recursive descent, depth bounded by JSON_MAX_DEPTH
// ---------------------------------------------------------------------------

namespace {

bool IsDigit(char c) {
	return c >= '0' && c <= '9';
}

class Parser {
public:
	Parser(const char *data, size_t len) : begin_(data), p_(data), end_(data + len) {
	}

	JsonValue Document() {
		if (end_ - p_ >= 3 && std::memcmp(p_, "\xEF\xBB\xBF", 3) == 0) {
			p_ += 3;
		}
		SkipSpace();
		JsonValue v;
		ParseValue(v, 0);
		SkipSpace();
		if (p_ != end_) {
			Fail("unexpected content after the JSON value");
		}
		return v;
	}

private:
	[[noreturn]] void Fail(const std::string &what) const {
		throw JsonError(what + " at byte " + std::to_string(p_ - begin_));
	}

	void SkipSpace() {
		while (p_ < end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r')) {
			p_++;
		}
	}

	void ParseValue(JsonValue &v, size_t depth) {
		if (p_ >= end_) {
			Fail("unexpected end of input");
		}
		switch (*p_) {
		case '{':
			ParseObject(v, depth + 1);
			return;
		case '[':
			ParseArray(v, depth + 1);
			return;
		case '"':
			v.type = JsonValue::Type::String;
			ParseString(v.text);
			return;
		case 't':
			Literal("true");
			v.type = JsonValue::Type::Bool;
			v.text = "true";
			return;
		case 'f':
			Literal("false");
			v.type = JsonValue::Type::Bool;
			v.text = "false";
			return;
		case 'n':
			Literal("null");
			v.type = JsonValue::Type::Null;
			return;
		default:
			if (*p_ == '-' || IsDigit(*p_)) {
				ParseNumber(v);
				return;
			}
			Fail(std::string("unexpected character '") + *p_ + "'");
		}
	}

	void Literal(const char *word) {
		const size_t n = std::strlen(word);
		if (static_cast<size_t>(end_ - p_) < n || std::memcmp(p_, word, n) != 0) {
			Fail("invalid literal");
		}
		p_ += n;
	}

	void CheckDepth(size_t depth) const {
		if (depth > JSON_MAX_DEPTH) {
			Fail("nesting deeper than " + std::to_string(JSON_MAX_DEPTH) + " levels");
		}
	}

	void ParseObject(JsonValue &v, size_t depth) {
		CheckDepth(depth);
		v.type = JsonValue::Type::Object;
		p_++; // '{'
		SkipSpace();
		if (p_ < end_ && *p_ == '}') {
			p_++;
			return;
		}
		while (true) {
			SkipSpace();
			if (p_ >= end_ || *p_ != '"') {
				Fail("expected a member name");
			}
			std::string key;
			ParseString(key);
			SkipSpace();
			if (p_ >= end_ || *p_ != ':') {
				Fail("expected ':' after a member name");
			}
			p_++;
			SkipSpace();
			v.members.emplace_back(std::move(key), JsonValue());
			ParseValue(v.members.back().second, depth);
			SkipSpace();
			if (p_ >= end_) {
				Fail("unterminated object");
			}
			if (*p_ == ',') {
				p_++;
				continue;
			}
			if (*p_ == '}') {
				p_++;
				return;
			}
			Fail("expected ',' or '}' in an object");
		}
	}

	void ParseArray(JsonValue &v, size_t depth) {
		CheckDepth(depth);
		v.type = JsonValue::Type::Array;
		p_++; // '['
		SkipSpace();
		if (p_ < end_ && *p_ == ']') {
			p_++;
			return;
		}
		while (true) {
			SkipSpace();
			v.items.emplace_back();
			ParseValue(v.items.back(), depth);
			SkipSpace();
			if (p_ >= end_) {
				Fail("unterminated array");
			}
			if (*p_ == ',') {
				p_++;
				continue;
			}
			if (*p_ == ']') {
				p_++;
				return;
			}
			Fail("expected ',' or ']' in an array");
		}
	}

	void ParseNumber(JsonValue &v) {
		const char *start = p_;
		if (*p_ == '-') {
			p_++;
		}
		if (p_ < end_ && *p_ == '0') {
			p_++;
		} else if (p_ < end_ && IsDigit(*p_)) {
			while (p_ < end_ && IsDigit(*p_)) {
				p_++;
			}
		} else {
			Fail("invalid number");
		}
		if (p_ < end_ && *p_ == '.') {
			p_++;
			if (p_ >= end_ || !IsDigit(*p_)) {
				Fail("invalid number: digits expected after '.'");
			}
			while (p_ < end_ && IsDigit(*p_)) {
				p_++;
			}
		}
		if (p_ < end_ && (*p_ == 'e' || *p_ == 'E')) {
			p_++;
			if (p_ < end_ && (*p_ == '+' || *p_ == '-')) {
				p_++;
			}
			if (p_ >= end_ || !IsDigit(*p_)) {
				Fail("invalid number: digits expected in the exponent");
			}
			while (p_ < end_ && IsDigit(*p_)) {
				p_++;
			}
		}
		v.type = JsonValue::Type::Number;
		v.text.assign(start, p_);
	}

	uint32_t Hex4() {
		if (end_ - p_ < 4) {
			Fail("truncated \\u escape");
		}
		uint32_t value = 0;
		for (int i = 0; i < 4; i++) {
			const char c = *p_++;
			value <<= 4;
			if (c >= '0' && c <= '9') {
				value |= static_cast<uint32_t>(c - '0');
			} else if (c >= 'a' && c <= 'f') {
				value |= static_cast<uint32_t>(c - 'a' + 10);
			} else if (c >= 'A' && c <= 'F') {
				value |= static_cast<uint32_t>(c - 'A' + 10);
			} else {
				Fail("invalid hex digit in a \\u escape");
			}
		}
		return value;
	}

	void ParseEscape(std::string &out) {
		p_++; // '\'
		if (p_ >= end_) {
			Fail("unterminated escape");
		}
		const char e = *p_++;
		switch (e) {
		case '"':
		case '\\':
		case '/':
			out.push_back(e);
			return;
		case 'b':
			out.push_back('\b');
			return;
		case 'f':
			out.push_back('\f');
			return;
		case 'n':
			out.push_back('\n');
			return;
		case 'r':
			out.push_back('\r');
			return;
		case 't':
			out.push_back('\t');
			return;
		case 'u': {
			uint32_t cp = Hex4();
			if (cp >= 0xD800 && cp <= 0xDBFF) {
				if (end_ - p_ < 2 || p_[0] != '\\' || p_[1] != 'u') {
					Fail("unpaired surrogate in a \\u escape");
				}
				p_ += 2;
				const uint32_t low = Hex4();
				if (low < 0xDC00 || low > 0xDFFF) {
					Fail("unpaired surrogate in a \\u escape");
				}
				cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
			} else if (cp >= 0xDC00 && cp <= 0xDFFF) {
				Fail("unpaired surrogate in a \\u escape");
			}
			AppendCodePoint(cp, out);
			return;
		}
		default:
			p_--;
			Fail(std::string("invalid escape '\\") + e + "'");
		}
	}

	void ParseString(std::string &out) {
		p_++; // opening quote
		while (true) {
			const char *run = p_;
			while (p_ < end_) {
				const unsigned char c = static_cast<unsigned char>(*p_);
				if (c < 0x20 || c >= 0x80 || c == '"' || c == '\\') {
					break;
				}
				p_++;
			}
			out.append(run, static_cast<size_t>(p_ - run));
			if (p_ >= end_) {
				Fail("unterminated string");
			}
			const unsigned char c = static_cast<unsigned char>(*p_);
			if (c == '"') {
				p_++;
				return;
			}
			if (c == '\\') {
				ParseEscape(out);
				continue;
			}
			if (c < 0x20) {
				Fail("unescaped control character in a string");
			}
			const char *start = p_;
			uint32_t cp;
			if (!DecodeUtf8(p_, end_, cp)) {
				Fail("invalid UTF-8 in a string");
			}
			out.append(start, static_cast<size_t>(p_ - start));
		}
	}

	const char *begin_;
	const char *p_;
	const char *end_;
};

} // namespace

JsonValue ParseJson(const char *data, size_t len) {
	return Parser(data, len).Document();
}

} // namespace orcid
