/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_value.hpp
	Desc: a script value (a Lua table or a JavaScript object) read ONCE, in C++,
	      the same way for both runtimes.

	WHY THIS EXISTS

	A function that takes a table needs a reader per runtime: a lua_next walk in
	sam_lua_runtime.cpp and a samJsGetPropCI walk in sam_js_runtime.cpp. They are
	written twice, by hand, and the second one is where a key gets spelled differently,
	a default gets forgotten, or a type is checked in one language and not the other.
	sam_event.hpp names Lua/JS divergence as this project's most repeated bug.

	Both runtimes can already turn any plain script value into SAMNet's tagged bytes
	(samLuaEncode / samJsEncode, the multiplayer trampoline's encoders) and back
	(samLuaDecode / samJsDecode). So a binding that takes a table does this:

	    SAMNet::Writer w;
	    samLuaEncode(Ls, 2, w, 0, err);          // or samJsEncode(ctx, argv[1], w, 0, err)
	    SamValue spec;
	    SamValue::decode(w.buf, spec);           // one parser, here
	    module.doTheThing(spec);                 // reads spec.str("name", ""), ...

	and a function that answers with a table builds a SamValue and hands back
	`value.encode()` to samLuaDecode / samJsDecode. The Lua and JS twins become a few lines
	each, and they cannot disagree about a key, a default or a type, because neither of
	them reads one.

	THE RULES (the wire's, so nothing here is a new limit)

	  * nil, false/true, a whole number, a real, a string, an array, a keyed map.
	  * Nested at most SAMNet::MAX_DEPTH (4) deep, at most 4096 entries a level.
	  * Map keys are text. An integer key (Lua `{ [3] = "x" }`) reads back as "3",
	    the way samJsDecode already turns it into a property name.
	  * Lookups by key ignore case, like samJsGetPropCI and every spec reader in the
	    project: `Name` and `name` are the same key.
	  * A Lua table that is a sequence 1..n arrives as an array, anything else as a
	    map, so `{}` is an empty ARRAY. Readers that want a map accept an empty array as
	    an empty map (see get()).

	No dependency on Barony: only sam_net.hpp's Writer and Reader.

-------------------------------------------------------------------------------*/

#pragma once

#include <string>
#include <utility>
#include <vector>

class SamValue
{
public:
	enum class Type : unsigned char { Nil, Bool, Int, Num, Str, Arr, Map };

	Type type = Type::Nil;
	bool b = false;
	long long i = 0;
	double n = 0.0;
	std::string s;
	std::vector<SamValue> arr;                              // Type::Arr
	std::vector<std::pair<std::string, SamValue>> map;      // Type::Map, in the order sent

	// ---- building
	static SamValue nil();
	static SamValue boolean(bool v);
	static SamValue integer(long long v);
	static SamValue number(double v);
	static SamValue string(const std::string& v);
	static SamValue array();
	static SamValue object();

	// Add to a map (replacing a key that is already there, ignoring case) or to an array.
	// Both turn a nil value into the container they need, so `SamValue v; v.set(...)` works.
	SamValue& set(const std::string& key, const SamValue& value);
	SamValue& push(const SamValue& value);

	// ---- the wire
	std::string encode() const;
	// False for bytes that are cut short, nested too deep, carry an unknown tag, or have
	// anything left over after the value. `out` is left nil on failure.
	static bool decode(const std::string& bytes, SamValue& out);

	// ---- reading
	bool isNil() const { return type == Type::Nil; }
	bool isNumber() const { return type == Type::Int || type == Type::Num; }
	bool isMap() const { return type == Type::Map || (type == Type::Arr && arr.empty()); }

	// The value under `key` in a map, ignoring case; nullptr for a missing key or when this
	// is not a map. A key that is present with a nil value is returned (it is nil).
	const SamValue* get(const std::string& key) const;

	// Typed reads with a fallback for a missing key, a nil, or a value of the wrong type.
	std::string str(const std::string& key, const std::string& fallback) const;
	long long whole(const std::string& key, long long fallback) const;   // a real truncates
	double real(const std::string& key, double fallback) const;
	bool flag(const std::string& key, bool fallback) const;

	// True when `key` holds a non-nil value that is not `want` (Int and Num both count as a
	// number). Lets a module say "name must be a string" instead of silently using its default.
	bool wrongType(const std::string& key, Type want) const;

	// "string", "number", "table", ... as a modder would call it, for messages.
	static const char* typeName(Type t);
};
