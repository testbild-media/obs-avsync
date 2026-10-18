// Tiny JSON helpers for the WebSocket protocol (flat objects only).
#pragma once

#include <map>
#include <string>

namespace avs::json {

struct Value {
	enum class Type { Null, Bool, Number, String } type = Type::Null;
	bool b = false;
	double num = 0.0;
	std::string str;
};

using Object = std::map<std::string, Value>;

// Parses a flat JSON object. Nested objects/arrays are skipped. Returns false on syntax errors.
bool parseObject(const std::string &text, Object &out);

std::string escape(const std::string &s);

inline double number(const Object &o, const char *key, double def = 0.0)
{
	auto it = o.find(key);
	return (it != o.end() && it->second.type == Value::Type::Number) ? it->second.num : def;
}

inline std::string string(const Object &o, const char *key, const std::string &def = "")
{
	auto it = o.find(key);
	return (it != o.end() && it->second.type == Value::Type::String) ? it->second.str : def;
}

inline bool boolean(const Object &o, const char *key, bool def = false)
{
	auto it = o.find(key);
	return (it != o.end() && it->second.type == Value::Type::Bool) ? it->second.b : def;
}

} // namespace avs::json
