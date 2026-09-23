/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_settings.cpp
	Desc: see sam_settings.hpp.

-------------------------------------------------------------------------------*/

// Barony headers pull in <windows.h>; stop it defining min()/max() macros.
// Must precede every include.
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "sam_settings.hpp"
#include "sam_logger.hpp"
#include "sam_errors.hpp"       // writeFileAtomic: the same writer sam_save_data uses
#include "sam_event.hpp"        // mod.on_setting_changed
#include "sam_lua_runtime.hpp"  // SAMLua::modDataDir: the mod's own data folder
#include "sam_loader.hpp"       // SAMLoader::getManifest: a mod's display name
#include "sam_workshop.hpp"     // SAMModManifest

#include "nlohmann/json.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <set>

#include "main.hpp"         // SDL_GetKeyFromName / SDL_GetKeyName
#include "ui/MainMenu.hpp"  // MainMenu::emptyBinding

static const char* MOD = "SETTINGS";

namespace SAMSettings
{
namespace
{
	// ---------------------------------------------------------------- actions

	std::deque<Action> s_actions;
	std::vector<std::string> s_actionNs;
	unsigned s_actionGen = 1;
	// Constant-initialised on purpose: MainMenu.cpp installs the hook from a static
	// initialiser of its own, and a dynamic initialiser here could run after it and wipe it.
	SeedActionFn s_seed = nullptr;

	// ---------------------------------------------------------------- settings

	std::deque<Setting> s_settings;
	std::vector<std::string> s_settingNs;
	// The parsed settings file of each mod, read once per load and written back whole.
	std::map<std::string, nlohmann::json> s_files;

	// The Settings window's copy. Keyed by "ns:id".
	struct Staged
	{
		Value value;
		Source source = Source::Ui;
	};
	bool s_editing = false;
	std::map<std::string, Staged> s_staged;
	// What each number/text row of the open window showed when it was built (showText), keyed
	// like s_staged.
	std::map<std::string, std::string> s_shown;

	// ---------------------------------------------------------------- helpers

	std::string trim(const std::string& s)
	{
		std::size_t a = 0, b = s.size();
		while ( a < b && (s[a] == ' ' || s[a] == '\t') ) { ++a; }
		while ( b > a && (s[b - 1] == ' ' || s[b - 1] == '\t') ) { --b; }
		return s.substr(a, b - a);
	}

	std::string lower(const std::string& s)
	{
		std::string o = s;
		for ( char& c : o ) { c = (char)std::tolower((unsigned char)c); }
		return o;
	}

	bool equalsCI(const std::string& a, const char* b)
	{
		return lower(a) == lower(b);
	}

	// An id becomes part of an action name, a config.json key, a widget name and a JSON key,
	// so it is kept to the characters every one of those is happy with.
	bool validId(const std::string& id, std::string& why, const char* what)
	{
		if ( id.empty() ) { why = std::string(what) + " id is empty"; return false; }
		if ( id.size() > 64 ) { why = std::string(what) + " id '" + id + "' is longer than 64 characters"; return false; }
		for ( unsigned char c : id )
		{
			const bool ok = ( c >= 'A' && c <= 'Z' ) || ( c >= 'a' && c <= 'z' ) || ( c >= '0' && c <= '9' )
				|| c == '_' || c == '-' || c == '.';
			if ( !ok )
			{
				why = std::string(what) + " id '" + id + "' may only use letters, digits, '_', '-' and '.'";
				return false;
			}
		}
		return true;
	}

	std::string key(const std::string& ns, const std::string& id) { return ns + ":" + id; }

	Setting* findMutable(const std::string& ns, const std::string& id)
	{
		for ( Setting& s : s_settings )
		{
			if ( s.ns == ns && s.id == id ) { return &s; }
		}
		return nullptr;
	}

	bool finite(double d) { return d == d && d <= 1.0e300 && d >= -1.0e300; }

	Kind kindFor(Type t)
	{
		switch ( t )
		{
			case Type::Slider: case Type::Number: return Kind::Number;
			case Type::Toggle: return Kind::Bool;
			default: return Kind::String;
		}
	}

	// Kill the float noise a snap leaves behind (0.1 * 3 is not 0.3), at the precision the
	// slider is shown with.
	double roundTo(double v, int decimals)
	{
		double p = 1.0;
		for ( int i = 0; i < decimals; ++i ) { p *= 10.0; }
		return std::round(v * p) / p;
	}

	double snapTo(const Setting& s, double v)
	{
		if ( !finite(v) ) { return s.min; }
		if ( s.step > 0.0 )
		{
			v = s.min + std::round((v - s.min) / s.step) * s.step;
			v = roundTo(v, sliderDecimals(s));
		}
		if ( v < s.min ) { v = s.min; }
		if ( v > s.max ) { v = s.max; }
		return v;
	}

	bool same(const Setting& s, const Value& a, const Value& b)
	{
		switch ( kindFor(s.type) )
		{
			case Kind::Number: return a.number == b.number;
			case Kind::Bool: return a.flag == b.flag;
			default: return a.text == b.text;
		}
	}

	// Validate `v` against the spec and normalise it (a slider onto its step). The reason a
	// script sees when it is refused is in `why`.
	bool validate(const Setting& s, Value& v, std::string& why)
	{
		const Kind want = kindFor(s.type);
		if ( v.kind != want )
		{
			why = "'" + s.id + "' is a " + typeName(s.type) + " and takes a "
				+ ( want == Kind::Number ? "number" : want == Kind::Bool ? "boolean" : "string" );
			return false;
		}
		switch ( s.type )
		{
			case Type::Slider:
				if ( !finite(v.number) ) { why = "'" + s.id + "': not a finite number"; return false; }
				if ( v.number < s.min || v.number > s.max )
				{
					why = "'" + s.id + "': " + valueText(s, v) + " is outside " + std::to_string(s.min) + ".." + std::to_string(s.max);
					return false;
				}
				v.number = snapTo(s, v.number);
				return true;
			case Type::Number:
				if ( !finite(v.number) ) { why = "'" + s.id + "': not a finite number"; return false; }
				if ( s.hasMin && v.number < s.min ) { why = "'" + s.id + "': " + valueText(s, v) + " is below the minimum " + valueText(s, Value{ Kind::Number, s.min, false, "" }); return false; }
				if ( s.hasMax && v.number > s.max ) { why = "'" + s.id + "': " + valueText(s, v) + " is above the maximum " + valueText(s, Value{ Kind::Number, s.max, false, "" }); return false; }
				return true;
			case Type::Toggle:
				return true;
			case Type::Dropdown:
				for ( const std::string& o : s.options ) { if ( o == v.text ) { return true; } }
				why = "'" + s.id + "': \"" + v.text + "\" is not one of its options";
				return false;
			case Type::Text:
				if ( (int)v.text.size() > kTextMax )
				{
					why = "'" + s.id + "': a text setting holds at most " + std::to_string(kTextMax) + " characters";
					return false;
				}
				return true;
		}
		return false;
	}

	// ---------------------------------------------------------------- the file

	std::string filePath(const std::string& ns)
	{
		return SAMLua::modDataDir(ns) + "/settings.sam";
	}

	// The most a settings file may be before it is left unread. This module writes a line of
	// under two hundred bytes per setting, so a file past this is not one it wrote, and the
	// cap is what bounds the parse (a whole-file read into memory) for a file another hand
	// made.
	constexpr std::streamoff kFileMax = 64 * 1024;

	nlohmann::json& fileFor(const std::string& ns)
	{
		auto it = s_files.find(ns);
		if ( it != s_files.end() ) { return it->second; }
		nlohmann::json j = nlohmann::json::object();
		std::ifstream f(filePath(ns), std::ios::binary);
		if ( f.is_open() )
		{
			f.seekg(0, std::ios::end);
			const std::streamoff size = f.tellg();
			f.seekg(0, std::ios::beg);
			if ( size > kFileMax )
			{
				SAM_WARN(MOD, "The settings file of [" + ns + "] is " + std::to_string((long long)size) + " bytes, more than the "
					+ std::to_string((long long)kFileMax) + " this module would ever write; its settings start from their defaults and the file is rewritten at the next change.");
				return s_files.emplace(ns, std::move(j)).first->second;
			}
			const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
			nlohmann::json parsed = nlohmann::json::parse(text, nullptr, false);
			if ( parsed.is_object() )
			{
				// Only the scalars this module writes are carried. An array or an object under
				// some key was put there by another hand, and persist()'s dump() recurses once
				// per level of nesting: deep enough (the parser and the destructor are
				// iterative, so it loads fine) it overflows the stack on the first Confirm.
				// Dropped here, so nothing nested ever reaches dump().
				int dropped = 0;
				for ( auto e = parsed.begin(); e != parsed.end(); )
				{
					if ( e->is_number() || e->is_boolean() || e->is_string() ) { ++e; }
					else { e = parsed.erase(e); ++dropped; }
				}
				if ( dropped > 0 )
				{
					SAM_WARN(MOD, "The settings file of [" + ns + "] held " + std::to_string(dropped) + ( dropped == 1 ? " entry that is" : " entries that are" )
						+ " not a number, a boolean or a string; dropped, and gone from the file at the next change.");
				}
				j = std::move(parsed);
			}
			else if ( !text.empty() )
			{
				SAM_WARN(MOD, "The settings file of [" + ns + "] could not be read; its settings start from their defaults and the file is rewritten at the next change.");
			}
		}
		return s_files.emplace(ns, std::move(j)).first->second;
	}

	nlohmann::json toJson(const Setting& s, const Value& v)
	{
		switch ( kindFor(s.type) )
		{
			case Kind::Number: return nlohmann::json(v.number);
			case Kind::Bool: return nlohmann::json(v.flag);
			default: return nlohmann::json(v.text);
		}
	}

	bool fromJson(const Setting& s, const nlohmann::json& j, Value& out)
	{
		switch ( kindFor(s.type) )
		{
			case Kind::Number:
				if ( !j.is_number() ) { return false; }
				out.kind = Kind::Number; out.number = j.get<double>(); return true;
			case Kind::Bool:
				if ( !j.is_boolean() ) { return false; }
				out.kind = Kind::Bool; out.flag = j.get<bool>(); return true;
			default:
				if ( !j.is_string() ) { return false; }
				out.kind = Kind::String; out.text = j.get<std::string>(); return true;
		}
	}

	// Read-modify-write, so a key the mod no longer registers (a setting it dropped in an
	// update, or one only an older version had) is carried along rather than lost.
	void persist(const std::string& ns)
	{
		nlohmann::json& file = fileFor(ns);
		for ( const Setting& s : s_settings )
		{
			if ( s.ns == ns ) { file[s.id] = toJson(s, s.value); }
		}
		std::string text;
		try
		{
			text = file.dump(1, '\t', false, nlohmann::json::error_handler_t::replace);
		}
		catch ( ... )
		{
			SAM_ERROR(MOD, "Could not encode the settings of [" + ns + "]; nothing written.");
			return;
		}
		if ( !SAMErrors::writeFileAtomic(filePath(ns), text) )
		{
			SAM_ERROR(MOD, "Cannot write " + filePath(ns));
		}
	}

	// The persisted value for a setting being registered for the first time this load, if
	// the file has one that the spec accepts.
	void loadPersisted(Setting& s)
	{
		const nlohmann::json& file = fileFor(s.ns);
		auto it = file.find(s.id);
		if ( it == file.end() ) { return; }
		Value v;
		std::string why;
		if ( !fromJson(s, *it, v) || !validate(s, v, why) )
		{
			SAM_WARN(MOD, "[" + s.ns + "] saved setting '" + s.id + "' no longer fits its declaration ("
				+ ( why.empty() ? std::string("wrong kind") : why ) + "); using the default.");
			return;
		}
		s.value = v;
	}

	const char* sourceName(Source src)
	{
		switch ( src )
		{
			case Source::Ui: return "ui";
			case Source::Script: return "script";
			default: return "reset";
		}
	}

	// A change that is real: written, then announced. The event goes out AFTER the value is
	// in force, so a handler that asks sam_get_setting gets the new answer.
	void apply(Setting& s, const Value& v, Source src)
	{
		const std::string oldText = valueText(s, s.value);
		s.value = v;
		persist(s.ns);
		SamEvent ev("mod.on_setting_changed");
		ev.s("mod", s.ns).s("id", s.id).s("type", typeName(s.type))
			.s("old", oldText).s("new", valueText(s, s.value)).s("source", sourceName(src));
		ev.fire();
	}
}

// -------------------------------------------------------------------- actions

bool canonicalKeyName(const std::string& in, std::string& out, std::string& why)
{
	const std::string s = trim(in);
	if ( s.empty() || equalsCI(s, MainMenu::emptyBinding) ) { out = MainMenu::emptyBinding; return true; }
	const std::string l = lower(s);
	if ( l.compare(0, 3, "pad") == 0 || l.compare(0, 3, "joy") == 0 )
	{
		why = "'" + s + "' names a controller input; the keyboard/mouse default goes in the third argument and a controller default in the fourth";
		return false;
	}
	if ( l.compare(0, 5, "mouse") == 0 )
	{
		std::string rest;
		for ( std::size_t i = 5; i < s.size(); ++i ) { if ( s[i] != ' ' ) { rest += s[i]; } }
		if ( equalsCI(rest, "wheelup") ) { out = "MouseWheelUp"; return true; }
		if ( equalsCI(rest, "wheeldown") ) { out = "MouseWheelDown"; return true; }
		bool digits = !rest.empty();
		for ( char c : rest ) { if ( c < '0' || c > '9' ) { digits = false; } }
		const int n = digits ? std::atoi(rest.c_str()) : 0;
		if ( digits && n >= 1 && n <= 15 ) { out = "Mouse" + std::to_string(n); return true; }
		why = "'" + s + "' is not a mouse input; those are Mouse1..Mouse15, MouseWheelUp and MouseWheelDown";
		return false;
	}
	const SDL_Keycode code = SDL_GetKeyFromName(s.c_str());
	if ( code == SDLK_UNKNOWN )
	{
		why = "'" + s + "' is not a key the game knows. Use the name the Bindings page shows for it: a letter or digit (\"F\", \"3\"), \"F1\"..\"F12\", \"Keypad 0\"..\"Keypad 9\", \"Space\", \"Return\", \"Tab\", \"Left Shift\", \"Left Ctrl\", \"Left Alt\", \"Escape\", \"Backspace\", \"Up\"/\"Down\"/\"Left\"/\"Right\", \"Home\", \"End\", \"Page Up\"/\"Page Down\", \"Insert\", \"Delete\", a punctuation key as its character (\"`\", \"-\", \"=\", \"[\", \"]\", \";\", \"'\", \",\", \".\", \"/\"), \"Mouse1\"..\"Mouse15\", \"MouseWheelUp\", \"MouseWheelDown\", or \"[unbound]\"";
		return false;
	}
	// The canonical spelling, which is also what the game stores when the player rebinds:
	// "keypad 0" becomes "Keypad 0" and "f" becomes "F".
	const char* canon = SDL_GetKeyName(code);
	out = ( canon && canon[0] ) ? canon : s;
	return true;
}

bool canonicalPadName(const std::string& in, std::string& out, std::string& why)
{
	static const char* const kTokens[] = {
		"ButtonA", "ButtonB", "ButtonX", "ButtonY", "ButtonBack", "ButtonStart",
		"ButtonLeftBumper", "ButtonRightBumper", "ButtonLeftStick", "ButtonRightStick",
		"LeftTrigger", "RightTrigger",
		"DpadX+", "DpadX-", "DpadY+", "DpadY-",
		"StickLeftX+", "StickLeftX-", "StickLeftY+", "StickLeftY-",
		"StickRightX+", "StickRightX-", "StickRightY+", "StickRightY-",
	};
	std::string s = trim(in);
	if ( s.empty() || equalsCI(s, MainMenu::emptyBinding) ) { out = MainMenu::emptyBinding; return true; }
	// "Pad Y", "Pad0ButtonA": the seat prefix is the engine's, never the mod's.
	if ( lower(s).compare(0, 3, "pad") == 0 )
	{
		std::size_t i = 3;
		while ( i < s.size() && (s[i] == ' ' || (s[i] >= '0' && s[i] <= '9')) ) { ++i; }
		s = s.substr(i);
	}
	std::string compact;
	for ( char c : s ) { if ( c != ' ' ) { compact += c; } }
	if ( compact.size() == 1 )
	{
		const char c = (char)std::toupper((unsigned char)compact[0]);
		if ( c == 'A' || c == 'B' || c == 'X' || c == 'Y' ) { compact = std::string("Button") + c; }
	}
	for ( const char* t : kTokens )
	{
		if ( equalsCI(compact, t) ) { out = t; return true; }
	}
	why = "'" + in + "' is not a controller input the game knows. Those are ButtonA, ButtonB, ButtonX, ButtonY, ButtonBack, ButtonStart, ButtonLeftBumper, ButtonRightBumper, ButtonLeftStick, ButtonRightStick, LeftTrigger, RightTrigger, DpadX+, DpadX-, DpadY+, DpadY-, StickLeftX+, StickLeftX-, StickLeftY+, StickLeftY-, StickRightX+, StickRightX-, StickRightY+, StickRightY-, or \"[unbound]\"";
	return false;
}

bool registerAction(const std::string& ns, const std::string& id, const std::string& label,
	const std::string& keyboard, const std::string& gamepad, std::string* why)
{
	std::string reason, kb, pad;
	if ( ns.empty() ) { reason = "no owning mod namespace"; }
	const bool ok = reason.empty()
		&& validId(id, reason, "action")
		&& canonicalKeyName(keyboard, kb, reason)
		&& canonicalPadName(gamepad, pad, reason);
	if ( !ok )
	{
		if ( why ) { *why = reason; }
		return false;
	}
	const std::string name = key(ns, id);
	const std::string shown = label.empty() ? id : label;
	for ( Action& a : s_actions )
	{
		if ( a.name == name )
		{
			// The same action again (a second call, or the same script on a later event):
			// refresh what the page shows and what Restore Defaults will use, add nothing.
			a.label = shown;
			a.keyboard = kb;
			a.gamepad = pad;
			return true;
		}
	}
	Action a;
	a.ns = ns;
	a.id = id;
	a.name = name;
	a.label = shown;
	a.keyboard = kb;
	a.gamepad = pad;
	s_actions.push_back(a);
	bool seenNs = false;
	for ( const std::string& n : s_actionNs ) { if ( n == ns ) { seenNs = true; break; } }
	if ( !seenNs ) { s_actionNs.push_back(ns); }
	++s_actionGen;
	if ( s_seed ) { s_seed(s_actions.back()); }
	SAM_INFO(MOD, "[" + ns + "] action '" + name + "' (" + shown + "): keyboard " + kb + ", controller " + pad + ".");
	return true;
}

const std::deque<Action>& actions() { return s_actions; }
const std::vector<std::string>& actionNamespaces() { return s_actionNs; }

const char* actionLabel(const char* actionName)
{
	if ( !actionName || s_actions.empty() ) { return nullptr; }
	for ( const Action& a : s_actions )
	{
		if ( a.name == actionName ) { return a.label.c_str(); }
	}
	return nullptr;
}

std::string modHeaderForAction(const char* actionName)
{
	if ( !actionName || s_actions.empty() ) { return std::string(); }
	std::set<std::string> seen;
	for ( const Action& a : s_actions )
	{
		const bool first = seen.insert(a.ns).second;
		if ( a.name == actionName ) { return first ? displayName(a.ns) : std::string(); }
	}
	return std::string();
}

unsigned actionGeneration() { return s_actionGen; }

void setSeedAction(SeedActionFn fn) { s_seed = fn; }

// -------------------------------------------------------------------- settings

const char* typeName(Type t)
{
	switch ( t )
	{
		case Type::Slider: return "slider";
		case Type::Toggle: return "toggle";
		case Type::Dropdown: return "dropdown";
		case Type::Number: return "number";
		default: return "text";
	}
}

bool typeFromName(const std::string& name, Type& out)
{
	const std::string l = lower(trim(name));
	if ( l == "slider" ) { out = Type::Slider; return true; }
	if ( l == "toggle" || l == "bool" || l == "boolean" || l == "checkbox" ) { out = Type::Toggle; return true; }
	if ( l == "dropdown" || l == "choice" || l == "select" ) { out = Type::Dropdown; return true; }
	if ( l == "number" ) { out = Type::Number; return true; }
	if ( l == "text" || l == "string" ) { out = Type::Text; return true; }
	return false;
}

std::string valueText(const Setting& s, const Value& v)
{
	switch ( kindFor(s.type) )
	{
		case Kind::Bool:
			return v.flag ? "true" : "false";
		case Kind::Number:
		{
			if ( !finite(v.number) ) { return "0"; }
			const double r = std::round(v.number);
			if ( r == v.number && std::fabs(r) < 1.0e15 )
			{
				char buf[32];
				snprintf(buf, sizeof(buf), "%lld", (long long)r);   // plain: the engine's headers make snprintf a macro
				return buf;
			}
			char buf[32];
			snprintf(buf, sizeof(buf), "%.10g", v.number);
			return buf;
		}
		default:
			return v.text;
	}
}

int sliderDecimals(const Setting& s)
{
	auto whole = [](double d) { return std::fabs(d - std::round(d)) < 1.0e-9; };
	if ( s.step > 0.0 )
	{
		if ( whole(s.step) && whole(s.min) ) { return 0; }
		if ( whole(s.step * 10.0) && whole(s.min * 10.0) ) { return 1; }
		return 2;
	}
	return ( whole(s.min) && whole(s.max) && (s.max - s.min) >= 10.0 ) ? 0 : 2;
}

int sliderSteps(const Setting& s)
{
	if ( s.step > 0.0 ) { return (int)std::lround((s.max - s.min) / s.step); }   // 1..kSliderStepsMax: registerSetting saw to it
	// Continuous. A whole range shown without decimals moves by one, so no two positions print
	// the same number; anything else gets one position per percent of the range.
	if ( sliderDecimals(s) == 0 )
	{
		const double n = std::round(s.max - s.min);
		return n > (double)kSliderStepsMax ? kSliderStepsMax : (int)n;
	}
	return 100;
}

double sliderPosition(const Setting& s, double value)
{
	if ( !finite(value) ) { return 0.0; }
	const int n = sliderSteps(s);
	double p = ( s.step > 0.0 ) ? std::round((value - s.min) / s.step) : (value - s.min) / (s.max - s.min) * n;
	if ( p < 0.0 ) { p = 0.0; }
	if ( p > n ) { p = n; }
	return p;
}

double sliderValue(const Setting& s, double position)
{
	// The nearest whole position, so the handle, the number beside it and the value staged
	// agree, and a key press from wherever the mouse left the handle lands on the grid.
	if ( !finite(position) ) { return s.min; }
	const int n = sliderSteps(s);
	double p = std::round(position);
	if ( p < 0.0 ) { p = 0.0; }
	if ( p > n ) { p = n; }
	const double v = ( s.step > 0.0 ) ? s.min + p * s.step : s.min + p / n * (s.max - s.min);
	return snapTo(s, v);
}

std::string sliderText(const Setting& s, double position)
{
	const double v = sliderValue(s, position);   // within +-kSliderRangeMax, so the long long below holds it
	char buf[32];
	switch ( sliderDecimals(s) )
	{
		case 0: snprintf(buf, sizeof(buf), "%lld", (long long)std::llround(v)); break;
		case 1: snprintf(buf, sizeof(buf), "%.1f", v); break;
		default: snprintf(buf, sizeof(buf), "%.2f", v); break;
	}
	return buf;
}

bool registerSetting(const std::string& ns, const Spec& in, std::string* why)
{
	std::string reason;
	Spec spec = in;
	spec.label = trim(spec.label);
	if ( spec.label.empty() ) { spec.label = spec.id; }
	if ( ns.empty() ) { reason = "no owning mod namespace"; }
	else if ( !validId(spec.id, reason, "setting") ) {}
	else
	{
		switch ( spec.type )
		{
			case Type::Slider:
				if ( !spec.hasMin || !spec.hasMax ) { reason = "'" + spec.id + "': a slider needs min and max"; }
				else if ( !finite(spec.min) || !finite(spec.max) || spec.max <= spec.min ) { reason = "'" + spec.id + "': a slider's max must be above its min"; }
				else if ( std::fabs(spec.min) > kSliderRangeMax || std::fabs(spec.max) > kSliderRangeMax )
				{
					// finite() admits anything up to 1e300, but the General tab prints the
					// value in a 16-character field and converts it in float on the way to
					// the handle; past this it is drawn nowhere. A number setting takes any
					// finite value.
					reason = "'" + spec.id + "': a slider's min and max must lie within -1000000000..1000000000; a number setting takes larger values";
				}
				else if ( spec.step < 0.0 || !finite(spec.step) || spec.step > (spec.max - spec.min) ) { reason = "'" + spec.id + "': step must be 0 (continuous) or a positive number no larger than the range"; }
				else if ( spec.step > 0.0 && (spec.max - spec.min) / spec.step > (double)kSliderStepsMax )
				{
					// The widget counts the steps in float, which holds whole numbers only so
					// far, and its handle walks them one at a time under a key or a stick.
					reason = "'" + spec.id + "': a slider may have at most 1000000 steps between min and max; use a larger step or a number setting";
				}
				break;
			case Type::Number:
				if ( (spec.hasMin && !finite(spec.min)) || (spec.hasMax && !finite(spec.max)) ) { reason = "'" + spec.id + "': min and max must be finite numbers"; }
				else if ( spec.hasMin && spec.hasMax && spec.max < spec.min ) { reason = "'" + spec.id + "': max is below min"; }
				break;
			case Type::Dropdown:
				if ( spec.options.empty() ) { reason = "'" + spec.id + "': a dropdown needs at least one option"; }
				for ( const std::string& o : spec.options )
				{
					if ( o.empty() ) { reason = "'" + spec.id + "': a dropdown option is empty"; }
				}
				break;
			default:
				break;
		}
	}
	if ( reason.empty() )
	{
		// A default is required for a slider, a number, a toggle and a dropdown; a text
		// setting may leave it out and starts empty.
		if ( spec.def.kind == Kind::None )
		{
			if ( spec.type == Type::Text ) { spec.def.kind = Kind::String; }
			else if ( spec.type == Type::Dropdown ) { spec.def.kind = Kind::String; spec.def.text = spec.options.front(); }
			else { reason = "'" + spec.id + "': no default given"; }
		}
	}
	Setting probe;
	if ( reason.empty() )
	{
		static_cast<Spec&>(probe) = spec;
		probe.ns = ns;
		if ( !validate(probe, spec.def, reason) ) { reason = "the default " + reason; }
	}
	if ( !reason.empty() )
	{
		if ( why ) { *why = reason; }
		return false;
	}

	if ( Setting* s = findMutable(ns, spec.id) )
	{
		// Registered again this load: take the new declaration, keep the value in force if
		// the new declaration still accepts it, else fall back to the new default. No event:
		// nothing the player chose has changed.
		Value keep = s->value;
		static_cast<Spec&>(*s) = spec;
		std::string ignored;
		s->value = validate(*s, keep, ignored) ? keep : spec.def;
		return true;
	}
	Setting s;
	static_cast<Spec&>(s) = spec;
	s.ns = ns;
	s.widgetName = "sam:" + ns + ":" + spec.id;
	s.value = spec.def;
	loadPersisted(s);
	s_settings.push_back(s);
	bool seenNs = false;
	for ( const std::string& n : s_settingNs ) { if ( n == ns ) { seenNs = true; break; } }
	if ( !seenNs ) { s_settingNs.push_back(ns); }
	SAM_INFO(MOD, "[" + ns + "] setting '" + spec.id + "' (" + typeName(spec.type) + ") = " + valueText(s_settings.back(), s_settings.back().value) + ".");
	return true;
}

const std::deque<Setting>& settings() { return s_settings; }
const std::vector<std::string>& settingNamespaces() { return s_settingNs; }

const Setting* find(const std::string& ns, const std::string& id)
{
	return findMutable(ns, id);
}

bool anySettings() { return !s_settings.empty(); }

std::string displayName(const std::string& ns)
{
	if ( const SAMModManifest* m = SAMLoader::getManifest(ns) )
	{
		if ( !m->name.empty() ) { return m->name; }
	}
	return ns;
}

bool set(const std::string& ns, const std::string& id, const Value& value, Source source, std::string* why)
{
	Setting* s = findMutable(ns, id);
	if ( !s )
	{
		if ( why ) { *why = "no setting '" + id + "' is registered by [" + ns + "]"; }
		return false;
	}
	Value v = value;
	std::string reason;
	if ( !validate(*s, v, reason) )
	{
		if ( why ) { *why = reason; }
		return false;
	}
	if ( same(*s, s->value, v) ) { return true; }
	apply(*s, v, source);
	return true;
}

// -------------------------------------------------------------------- the editing session

void beginEdit()
{
	s_staged.clear();
	s_shown.clear();
	s_editing = true;
}

const Value& staged(const Setting& s)
{
	if ( s_editing )
	{
		auto it = s_staged.find(key(s.ns, s.id));
		if ( it != s_staged.end() ) { return it->second.value; }
	}
	return s.value;
}

namespace
{
	void stage(const Setting& s, const Value& v, Source src)
	{
		if ( !s_editing ) { beginEdit(); }
		Staged& st = s_staged[key(s.ns, s.id)];
		st.value = v;
		st.source = src;
	}
}

void stageNumber(const std::string& ns, const std::string& id, double v)
{
	const Setting* s = find(ns, id);
	if ( !s || kindFor(s->type) != Kind::Number ) { return; }
	Value val; val.kind = Kind::Number; val.number = v;
	stage(*s, val, Source::Ui);
}

void stageFlag(const std::string& ns, const std::string& id, bool v)
{
	const Setting* s = find(ns, id);
	if ( !s || kindFor(s->type) != Kind::Bool ) { return; }
	Value val; val.kind = Kind::Bool; val.flag = v;
	stage(*s, val, Source::Ui);
}

void stageText(const std::string& ns, const std::string& id, const std::string& v)
{
	const Setting* s = find(ns, id);
	if ( !s || kindFor(s->type) != Kind::String ) { return; }
	Value val; val.kind = Kind::String; val.text = v;
	stage(*s, val, Source::Ui);
}

void showText(const std::string& ns, const std::string& id, const std::string& text)
{
	const Setting* s = find(ns, id);
	if ( !s ) { return; }
	if ( !s_editing ) { beginEdit(); }
	s_shown[key(ns, id)] = text;
}

bool stageFromText(const std::string& ns, const std::string& id, const std::string& text)
{
	const Setting* s = find(ns, id);
	if ( !s ) { return false; }
	// The row untouched, or typed back to what it showed: nothing of the player's to stage.
	// Without this a value that changed underneath the open window (a script's
	// sam_set_setting; a timer runs while the game is paused) was re-staged from the stale
	// text and undone on Confirm with source "ui", and a number whose text does not parse
	// back to the same double counted as an edit the moment the tab opened. An edit staged
	// in between is dropped, since the field no longer shows it; what Restore Defaults staged
	// is what the rebuilt row shows, so it stays.
	const std::string k = key(ns, id);
	auto shown = s_shown.find(k);
	if ( shown != s_shown.end() && shown->second == text )
	{
		auto st = s_staged.find(k);
		if ( st != s_staged.end() && valueText(*s, st->second.value) != text ) { s_staged.erase(st); }
		return false;
	}
	Value val;
	if ( s->type == Type::Number )
	{
		const std::string t = trim(text);
		if ( t.empty() ) { return false; }
		char* end = nullptr;
		const double d = std::strtod(t.c_str(), &end);
		if ( !end || *end != '\0' || !finite(d) ) { return false; }   // half-typed: "-", "1.", "1e"
		val.kind = Kind::Number;
		val.number = d;
		if ( s->hasMin && val.number < s->min ) { val.number = s->min; }
		if ( s->hasMax && val.number > s->max ) { val.number = s->max; }
	}
	else if ( s->type == Type::Text )
	{
		val.kind = Kind::String;
		val.text = text.substr(0, (std::size_t)kTextMax);
	}
	else
	{
		return false;
	}
	if ( same(*s, staged(*s), val) ) { return false; }
	stage(*s, val, Source::Ui);
	return true;
}

double snapSlider(const std::string& ns, const std::string& id, double v)
{
	const Setting* s = find(ns, id);
	if ( !s || s->type != Type::Slider ) { return v; }
	return snapTo(*s, v);
}

void stageDefaults()
{
	for ( const Setting& s : s_settings ) { stage(s, s.def, Source::Reset); }
}

int commitEdit()
{
	if ( !s_editing ) { return 0; }
	int changed = 0;
	// By index: a handler of the event a commit fires may register a setting, and a deque
	// keeps its elements in place but not its iterators.
	for ( std::size_t i = 0; i < s_settings.size(); ++i )
	{
		Setting& s = s_settings[i];
		auto it = s_staged.find(key(s.ns, s.id));
		if ( it == s_staged.end() ) { continue; }
		Value v = it->second.value;
		std::string why;
		if ( !validate(s, v, why) )
		{
			SAM_WARN(MOD, "[" + s.ns + "] '" + s.id + "': the edited value was refused (" + why + "); keeping " + valueText(s, s.value) + ".");
			continue;
		}
		if ( same(s, s.value, v) ) { continue; }
		apply(s, v, it->second.source);
		++changed;
	}
	s_staged.clear();
	s_shown.clear();
	s_editing = false;
	return changed;
}

void discardEdit()
{
	s_staged.clear();
	s_shown.clear();
	s_editing = false;
}

// -------------------------------------------------------------------- teardown

void clear()
{
	s_actions.clear();
	s_actionNs.clear();
	++s_actionGen;
	s_settings.clear();
	s_settingNs.clear();
	s_files.clear();
	s_staged.clear();
	s_shown.clear();
	s_editing = false;
}
}
