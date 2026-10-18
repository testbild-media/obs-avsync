#include "mini-json.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace avs::json {

namespace {

struct Parser {
	const std::string &s;
	size_t i = 0;

	void ws()
	{
		while (i < s.size() && std::isspace((unsigned char)s[i]))
			i++;
	}

	bool lit(const char *w)
	{
		size_t n = 0;
		while (w[n])
			n++;
		if (s.compare(i, n, w) != 0)
			return false;
		i += n;
		return true;
	}

	static void appendUtf8(std::string &out, unsigned cp)
	{
		if (cp < 0x80) {
			out += (char)cp;
		} else if (cp < 0x800) {
			out += (char)(0xC0 | (cp >> 6));
			out += (char)(0x80 | (cp & 0x3F));
		} else {
			out += (char)(0xE0 | (cp >> 12));
			out += (char)(0x80 | ((cp >> 6) & 0x3F));
			out += (char)(0x80 | (cp & 0x3F));
		}
	}

	bool str(std::string &out)
	{
		if (i >= s.size() || s[i] != '"')
			return false;
		i++;
		while (i < s.size()) {
			char c = s[i++];
			if (c == '"')
				return true;
			if (c != '\\') {
				out += c;
				continue;
			}
			if (i >= s.size())
				return false;
			char e = s[i++];
			switch (e) {
			case 'n':
				out += '\n';
				break;
			case 't':
				out += '\t';
				break;
			case 'r':
				out += '\r';
				break;
			case 'b':
				out += '\b';
				break;
			case 'f':
				out += '\f';
				break;
			case 'u': {
				if (i + 4 > s.size())
					return false;
				unsigned cp = (unsigned)std::strtoul(s.substr(i, 4).c_str(), nullptr, 16);
				i += 4;
				appendUtf8(out, cp);
				break;
			}
			default:
				out += e;
			}
		}
		return false;
	}

	bool skipValue()
	{
		ws();
		if (i >= s.size())
			return false;
		char c = s[i];
		if (c == '"') {
			std::string tmp;
			return str(tmp);
		}
		if (c == '{' || c == '[') {
			char open = c, close = c == '{' ? '}' : ']';
			int depth = 0;
			while (i < s.size()) {
				if (s[i] == '"') {
					std::string tmp;
					if (!str(tmp))
						return false;
					continue;
				}
				if (s[i] == open)
					depth++;
				else if (s[i] == close && --depth == 0) {
					i++;
					return true;
				}
				i++;
			}
			return false;
		}
		while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']')
			i++;
		return true;
	}

	bool value(Value &v)
	{
		ws();
		if (i >= s.size())
			return false;
		char c = s[i];
		if (c == '"') {
			v.type = Value::Type::String;
			return str(v.str);
		}
		if (lit("true")) {
			v.type = Value::Type::Bool;
			v.b = true;
			return true;
		}
		if (lit("false")) {
			v.type = Value::Type::Bool;
			v.b = false;
			return true;
		}
		if (lit("null")) {
			v.type = Value::Type::Null;
			return true;
		}
		if (c == '-' || std::isdigit((unsigned char)c)) {
			char *end = nullptr;
			v.type = Value::Type::Number;
			v.num = std::strtod(s.c_str() + i, &end);
			if (!end || end == s.c_str() + i)
				return false;
			i = (size_t)(end - s.c_str());
			return true;
		}
		v.type = Value::Type::Null;
		return skipValue();
	}
};

} // namespace

bool parseObject(const std::string &text, Object &out)
{
	Parser p{text};
	p.ws();
	if (p.i >= text.size() || text[p.i] != '{')
		return false;
	p.i++;
	p.ws();
	if (p.i < text.size() && text[p.i] == '}')
		return true;
	while (p.i < text.size()) {
		p.ws();
		std::string key;
		if (!p.str(key))
			return false;
		p.ws();
		if (p.i >= text.size() || text[p.i] != ':')
			return false;
		p.i++;
		Value v;
		if (!p.value(v))
			return false;
		out[key] = v;
		p.ws();
		if (p.i < text.size() && text[p.i] == ',') {
			p.i++;
			continue;
		}
		if (p.i < text.size() && text[p.i] == '}')
			return true;
		return false;
	}
	return false;
}

std::string escape(const std::string &s)
{
	std::string out;
	out.reserve(s.size() + 2);
	for (unsigned char c : s) {
		switch (c) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
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
		default:
			if (c < 0x20) {
				char buf[8];
				snprintf(buf, sizeof(buf), "\\u%04x", c);
				out += buf;
			} else {
				out += (char)c;
			}
		}
	}
	return out;
}

} // namespace avs::json
