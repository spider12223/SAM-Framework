/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_settings.hpp
	Desc: what a mod adds to the game's own Settings screens -- a rebindable action in
	      the Bindings page, and a slider, toggle, dropdown, number or text row in the
	      General tab -- and where the player's choices for them are kept.

	WHY THIS EXISTS

	A mod that wants "press a key to do X" has had two choices: claim a raw key
	(on_key_pressed), which collides with whatever the player bound there and cannot be
	changed without editing the script, or watch a vanilla action ("Cast Spell"), which
	the player CAN rebind but which then does two things at once. Neither is what the
	Game Speed Control mod does: it puts four rows of its own in Settings > Controls >
	Bindings, and the player rebinds them like any other action. That is what
	registerAction gives every mod.

	The same mod has four speed sliders. sam_ui cannot draw a slider, a toggle or a
	dropdown, so a panel of a mod's own could never look like the game's settings; the
	General tab can, and it is the one tab that exists in both the main menu and the pause
	menu. registerSetting puts a MOD SETTINGS section there. It is NOT inside the Bindings
	window (the screenshot's layout): that window's Confirm, Discard and Restore Defaults
	know only the binding table, and threading a second kind of value through the three of
	them buys nothing the General tab does not already give.

	THE TWO STORES, AND WHY THEY DIFFER

	  * An ACTION's binding lives in config.json beside the vanilla rows. Barony's binding
	    table is a string-keyed map with no allow-list: a name it does not know is read,
	    kept and written back like any other, so the player's rebinding survives a restart
	    and survives the mod being unloaded. The mod contributes only the DEFAULT, and it
	    is seeded with emplace: whatever config.json already holds wins.

	  * A SETTING's value lives in the mod's own data folder,
	    savegames/sam_mod_data/<ns>/settings.sam, written with the same atomic writer
	    sam_save_data uses and holding the same JSON. It is deliberately not in config.json:
	    AllSettings is a fixed struct with a version number, and a free-form map in it would
	    mean a version bump, a custom serializer, and the vanilla exe rewriting the file
	    without it. The file has no .json suffix so it can never be the file behind one of
	    the mod's own sam_save_data keys (those are always <encoded key>.json) and never
	    appears in sam_list_data_keys.

	Registration is IDEMPOTENT and the registry is cleared on every load and unload, because
	mods load long after config.json is read, load again on every Play and on /sam_reload, and
	a second registration of the same thing must be a no-op rather than a duplicate row.

	THE IRON RULE. With no mod loaded the registry is empty: the Bindings page is byte-identical
	to vanilla, the General tab has no MOD SETTINGS section, and the action poller watches the
	twelve vanilla names it always watched.

-------------------------------------------------------------------------------*/

#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace SAMSettings
{
	// ---- mod actions ------------------------------------------------------------------
	//
	// One row in Settings > Controls > Bindings, under the mod's name. The registered action
	// NAME is "<ns>:<id>": that is the key in config.json, the `action` field of
	// on_action_pressed / on_action_released, and the string sam_is_action_held and
	// sam_get_action_binding take.

	struct Action
	{
		std::string ns;        // the mod that registered it
		std::string id;        // the mod's own short name for it
		std::string name;      // ns + ":" + id
		std::string label;     // what the Bindings page shows on the row
		std::string keyboard;  // default keyboard/mouse input, canonical SDL spelling; "[unbound]" for none
		std::string gamepad;   // default controller input without the seat prefix ("ButtonY"); "[unbound]" for none
	};

	// False, with the reason in `why`, for an empty namespace, an id that is not
	// [A-Za-z0-9_.-], or a key name Input::bind could not decode. Registering an action that
	// already exists refreshes its label and defaults and returns true; nothing is duplicated.
	bool registerAction(const std::string& ns, const std::string& id, const std::string& label,
		const std::string& keyboard, const std::string& gamepad, std::string* why);

	// Every registered action, in registration order. A deque, which never moves an element
	// when another is appended, so a reference held across a registration stays good; nothing
	// outside this module keeps a pointer into it past a call (the Bindings page copies the
	// names it shows, since clear() empties the deque under any pointer that survived).
	const std::deque<Action>& actions();
	// The mods that registered an action, in the order they first did. The Bindings page
	// groups its mod rows by this.
	const std::vector<std::string>& actionNamespaces();
	// The label for a registered action name, or nullptr when it is not a mod action. The
	// Bindings page asks this FIRST: its own label lookup counts along a language table and,
	// for a name it does not know, returns whichever line sits past the last vanilla entry.
	const char* actionLabel(const char* actionName);
	// The display name of the mod whose FIRST action this is (registration order), else "".
	// The Bindings page puts a header above that row.
	std::string modHeaderForAction(const char* actionName);
	// Bumped by every registration that adds a name and by clear(). The action poller
	// (sam_mp_input.cpp) rebuilds its list when this changes.
	unsigned actionGeneration();

	// The forms a default accepts, and the canonical spelling that is stored.
	//   keyboard/mouse: any SDL key name ("Keypad 0", "F5", "Space", "Left Shift", "A"),
	//                   "Mouse1".."Mouse15", "MouseWheelUp", "MouseWheelDown", "" or "[unbound]".
	//   controller:     "ButtonA/B/X/Y", "ButtonBack", "ButtonStart", "ButtonLeftBumper",
	//                   "ButtonRightBumper", "ButtonLeftStick", "ButtonRightStick",
	//                   "LeftTrigger", "RightTrigger", "DpadX+/X-/Y+/Y-", "StickLeftX+"...
	//                   "StickRightY-", "" or "[unbound]". A bare A, B, X or Y means the button.
	bool canonicalKeyName(const std::string& in, std::string& out, std::string& why);
	bool canonicalPadName(const std::string& in, std::string& out, std::string& why);

	// The engine's half. The maps the Settings window edits (allSettings) are a file-static of
	// Barony/src/ui/MainMenu.cpp, so the one step that must write into them -- seeding a newly
	// registered action into every seat's maps and into the live Input table -- lives there and
	// is installed here at static-initialisation time, the way SAMNet::setItemRouter is. With
	// nothing installed, registration still records the action; it simply binds nothing.
	using SeedActionFn = void (*)(const Action& action);
	void setSeedAction(SeedActionFn fn);

	// ---- mod settings -----------------------------------------------------------------

	enum class Type : std::uint8_t { Slider, Toggle, Dropdown, Number, Text };
	const char* typeName(Type t);                          // "slider", "toggle", ...
	bool typeFromName(const std::string& name, Type& out);  // case-insensitive

	// A value, tagged with what the script handed over. A setting's Type says which member
	// counts: number for Slider and Number, flag for Toggle, text for Dropdown and Text.
	enum class Kind : std::uint8_t { None, Number, Bool, String };
	struct Value
	{
		Kind kind = Kind::None;
		double number = 0.0;
		bool flag = false;
		std::string text;
	};

	// What a mod declares. `min`/`max` are required for a slider and optional for a number
	// (hasMin/hasMax); `step` is the slider's grid, 0 meaning continuous. A text setting holds
	// at most kTextMax characters: the settings field it is edited in holds no more. A slider's
	// min and max lie within +-kSliderRangeMax and its grid has at most kSliderStepsMax steps:
	// the widget counts positions in float and its handle walks them one at a time, so a range
	// or a grid beyond those is refused at registration, where the script can see why, rather
	// than drawn nowhere. A number setting takes any finite value.
	constexpr int kTextMax = 31;
	constexpr double kSliderRangeMax = 1.0e9;
	constexpr int kSliderStepsMax = 1000000;
	struct Spec
	{
		std::string id;
		std::string label;
		std::string tooltip;
		Type type = Type::Slider;
		double min = 0.0;
		double max = 0.0;
		double step = 0.0;
		bool hasMin = false;
		bool hasMax = false;
		std::vector<std::string> options;   // dropdown choices, in order
		Value def;
	};

	struct Setting : Spec
	{
		std::string ns;
		std::string widgetName;   // "sam:<ns>:<id>", the stem of every widget the General tab makes for it
		Value value;              // the value in force (what sam_get_setting answers)
	};

	// False, with the reason in `why`, for an empty namespace, a bad id, a type the spec
	// does not name, a slider without a range or with one the widget cannot hold (see
	// kSliderRangeMax and kSliderStepsMax), a number with a bound that is not a finite number,
	// a dropdown without options, or a default of the wrong kind or outside the range.
	// Registering an id again refreshes the spec and
	// keeps the current value when it is still valid under it. The first registration of an
	// id reads the persisted value from the mod's settings file (the file wins over the
	// default).
	bool registerSetting(const std::string& ns, const Spec& spec, std::string* why);

	const std::deque<Setting>& settings();                  // registration order
	const std::vector<std::string>& settingNamespaces();    // mods with a setting, first-seen order
	const Setting* find(const std::string& ns, const std::string& id);
	bool anySettings();                                     // false: the General tab shows no section
	std::string displayName(const std::string& ns);         // the mod.json name, else the namespace

	// Where a change came from; the event says which.
	enum class Source : std::uint8_t { Ui, Script, Reset };

	// Change a value now: validated against the spec (a slider is snapped to its step; a
	// value out of range, an option not in the list, a value of the wrong kind or a text
	// longer than kTextMax is refused with `why`), written to the mod's file, and announced
	// with mod.on_setting_changed. A set to the value already in force is true and silent.
	bool set(const std::string& ns, const std::string& id, const Value& value, Source source, std::string* why);

	// The value's text: "true"/"false", a number ("2", "0.5"), or the string itself. This is
	// what the event carries in `old` and `new` (an event field is an integer or a string,
	// and a slider's 0.5 is neither), and what a number/text field shows.
	std::string valueText(const Setting& s, const Value& v);
	// How many decimals a slider's number should be shown with: 0, 1 or 2, from its step.
	int sliderDecimals(const Setting& s);

	// The General tab's slider widget. The engine's Slider moves a selected handle by exactly
	// 1.0 per keyboard or controller repeat, so a widget built over the mod's own range cannot
	// land between whole numbers: 0.1..8 step 0.1 would offer 0.1, 1.1, 2.1... and never the
	// 0.5 default, and a 0..1 slider would be a two-position switch. The window builds a mod
	// slider in STEP units instead, positions 0..sliderSteps, and converts at the edges: the
	// number beside the handle and the value it stages are in the mod's own units. A
	// continuous slider (step 0) gets one position per whole number when its range is whole
	// and shown without decimals, else a hundred, one per percent of the range.
	int sliderSteps(const Setting& s);
	double sliderPosition(const Setting& s, double value);       // value -> position, clamped
	double sliderValue(const Setting& s, double position);       // nearest whole position -> value, snapped and clamped
	std::string sliderText(const Setting& s, double position);   // the number beside the handle, at sliderDecimals

	// ---- the Settings window's editing session ------------------------------------------
	//
	// The window edits a COPY and makes it real on Confirm; Discard throws the copy away.
	// beginEdit starts the copy (the window opening), the stage* calls write into it from the
	// widgets, stageDefaults is Restore Defaults, commitEdit is Confirm: every staged value
	// that differs from the one in force goes through set(), with the source the stage
	// recorded (Ui, or Reset for one Restore Defaults put there). discardEdit is Discard.
	// set() never reads the copy, so a script changing a setting while the window is open
	// is not held up by it; if the player also changed that setting, Confirm wins.
	void beginEdit();
	const Value& staged(const Setting& s);   // the copy's value, else the one in force
	void stageNumber(const std::string& ns, const std::string& id, double v);
	void stageFlag(const std::string& ns, const std::string& id, bool v);
	void stageText(const std::string& ns, const std::string& id, const std::string& v);
	// The text a number or text row shows when the window builds it. stageFromText treats
	// the row as untouched while its text still reads exactly this, so a value that changes
	// underneath the open window (a script's sam_set_setting while the tab is open) is not put
	// back from the stale text on Confirm, and a number whose text does not parse back to the
	// same double (2/3 shows as 0.6666666667) does not count as an edit the moment the tab
	// opens.
	void showText(const std::string& ns, const std::string& id, const std::string& text);
	// A number or text field's contents, every tick: true only when it parsed to a value
	// DIFFERENT from what is staged (a number is clamped into the range, a text cut to
	// kTextMax), so the window's "modified" mark is set once and not fifty times a second.
	// Text equal to what showText recorded stages nothing, and drops an edit staged in between
	// (the player typed back what the row showed).
	bool stageFromText(const std::string& ns, const std::string& id, const std::string& text);
	// A slider's raw position, snapped to the step and clamped into the range.
	double snapSlider(const std::string& ns, const std::string& id, double v);
	void stageDefaults();
	int commitEdit();   // how many values changed
	// Discard, and any other way out of the window that is not Confirm: the copy is dropped
	// and nothing changes. Harmless when nothing was staged. beginEdit also starts clean, so
	// a window closed by the game itself (a disconnect) leaves nothing behind either.
	void discardEdit();

	// Drop everything: both loader paths, like every other registry. The files on disk and
	// the bindings in config.json stay, so a mod loaded again finds the player's choices.
	void clear();
}
