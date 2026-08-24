// sam_event.hpp -- one event, fired once, to both script runtimes.
//
// WHY THIS EXISTS
//
// Every engine site that talks to a script used to say the same thing twice:
//
//     SAMLua::Event ev;
//     ev.setName("player.on_x").i("a", 1).i("b", 2);
//     SAMLua::dispatchEvent(ev);
//
//     SAMJs::Event jsev;
//     jsev.setName("player.on_x").i("a", 1).i("b", 2);
//     SAMJs::dispatchEvent(jsev);
//
// There are 79 of each, across 26 files. Two near-identical blocks per site is not just
// noise -- it is the reason a new interception point costs an engine edit rather than a
// line, and it is where a field gets added to one runtime and forgotten in the other.
// Lua/JS divergence is this project's single most repeated bug.
//
// It also cost something at runtime. Both blocks built their payload unconditionally, so
// every site allocated two events' worth of strings on every fire even with no mods loaded.
//
// THE SHAPE
//
//     SamEvent e("player.on_x");
//     e.i("player", n).i("damage", dmg);
//     if ( !e.fire() ) { return; }              // a handler said no
//     dmg = (int)e.get("damage", dmg);          // a handler proposed a new value
//
// FREE WHEN NOTHING IS LISTENING. The constructor asks once whether any script exists at
// all; if none does, every i()/s() is a no-op, fire() returns true immediately, and get()
// hands back the fallback. So a site adopting this costs one bool test in vanilla -- less
// than the two allocations it replaces.
#pragma once

#include <string>

#include "sam_lua_runtime.hpp"
#ifndef SAM_LUA_NO_JS
#include "sam_js_runtime.hpp"
#endif

class SamEvent
{
public:
	explicit SamEvent(const char* name);
	// Default-construct + setName mirrors the shape the old two-block sites already used,
	// so converting one is a deletion rather than a rewrite.
	SamEvent();
	SamEvent& setName(const char* name);

	SamEvent& i(const char* key, long long value);
	SamEvent& s(const char* key, const std::string& value);

	// Deliver to every loaded script in both languages. Returns FALSE if any handler asked
	// the game not to do the thing -- so the natural shape at a site is
	// `if ( !e.fire() ) { return; }`.
	bool fire();

	// What a handler proposed for a field, or `fallback` if none did. Only meaningful after
	// fire(), and only for fields this event actually carried.
	long long   get(const char* key, long long fallback) const;
	std::string getStr(const char* key, const std::string& fallback) const;

	// True while any script is loaded. Sites with expensive payloads can test this before
	// doing the work of gathering them.
	static bool anyScripts();

private:
	bool active;
	SAMLua::Event lua;
#ifndef SAM_LUA_NO_JS
	SAMJs::Event js;
#endif
};
