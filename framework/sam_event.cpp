#include "sam_event.hpp"

bool SamEvent::anyScripts()
{
#ifndef SAM_LUA_NO_JS
	return SAMLua::scriptCount() > 0 || SAMJs::scriptCount() > 0;
#else
	return SAMLua::scriptCount() > 0;
#endif
}

SamEvent::SamEvent(const char* name)
	: active(anyScripts())
{
	// Asked ONCE, here. Everything else in this class is gated on it, so a site with no mods
	// loaded never builds a payload it is about to throw away.
	if ( !active ) { return; }
	lua.setName(name ? name : "");
#ifndef SAM_LUA_NO_JS
	js.setName(name ? name : "");
#endif
}

SamEvent::SamEvent()
	: active(anyScripts())
{
}

SamEvent& SamEvent::setName(const char* name)
{
	if ( !active || !name ) { return *this; }
	lua.setName(name);
#ifndef SAM_LUA_NO_JS
	js.setName(name);
#endif
	return *this;
}

SamEvent& SamEvent::i(const char* key, long long value)
{
	if ( !active || !key ) { return *this; }
	lua.i(key, value);
#ifndef SAM_LUA_NO_JS
	js.i(key, value);
#endif
	return *this;
}

SamEvent& SamEvent::s(const char* key, const std::string& value)
{
	if ( !active || !key ) { return *this; }
	lua.s(key, value);
#ifndef SAM_LUA_NO_JS
	js.s(key, value);
#endif
	return *this;
}

bool SamEvent::fire()
{
	if ( !active ) { return true; }   // nothing listening: the game carries on
	// Lua first, then JS. Both write into one shared store, and the JS pass seeds its objects
	// from that store, so an edit made in either language is visible to the other and to the
	// engine site afterwards.
	SAMLua::dispatchEvent(lua);

	// Capture Lua's verdict HERE, before the JS pass. The two runtimes keep SEPARATE cancel
	// latches, and SAMLua::dispatchEvent resets its own on entry -- so a JS handler that
	// calls sam_fire_hook re-enters the Lua dispatcher and wipes the answer a Lua handler
	// just gave. Reading it afterwards would silently lose that veto.
	bool cancelled = SAMLua::lastDispatchCancelled();
#ifndef SAM_LUA_NO_JS
	SAMJs::dispatchEvent(js);
	// And ask JS too. Returning only Lua's answer would mean a JS mod's `return false` did
	// nothing while the byte-identical Lua mod worked -- the exact divergence this type was
	// built to make impossible, sitting in the one method that decides anything.
	if ( SAMJs::lastDispatchCancelled() ) { cancelled = true; }
#endif
	return !cancelled;
}

long long SamEvent::get(const char* key, long long fallback) const
{
	if ( !active ) { return fallback; }
	return SAMLua::lastEventInt(key, fallback);
}

std::string SamEvent::getStr(const char* key, const std::string& fallback) const
{
	if ( !active ) { return fallback; }
	return SAMLua::lastEventString(key, fallback);
}
