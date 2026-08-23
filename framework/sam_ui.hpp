/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	sam_ui.hpp - interactive panels a mod can open, and that the player can click.

	WHY THIS EXISTS. sam_hud_* draws; it does not listen. Its container is deliberately
	setHollow(true) so a mod HUD can never steal input from the game. That is correct for a
	HUD and it is the whole ceiling for everything else: a mod could show you a list but not
	let you pick from it, could show a value but not let you change it.

	That ceiling is the single largest gap in the framework. Across the most-installed mods in
	Minecraft and Terraria, the top of both charts is interface work - a searchable item index
	is the #1 Minecraft mod of all time, and Terraria's equivalent has 85% of the install base
	of the biggest content mod ever made for that game. See SAM_MODDING_FREEDOM_STUDY.md.

	WHAT THIS IS NOT. It is not a new UI system. Barony already has a real retained-mode one
	(Frame / Button / Field) with the entire input path built: Button::setCallback,
	Frame::process(), capturesMouse(), scrolling. GameUI.cpp calls gui->process() every frame
	during play. This module is bindings onto that, plus the two things scripts cannot do
	themselves: routing a C callback back into Lua/JS, and surviving the engine throwing the
	widget tree away.

	THE LIFETIME PROBLEM, ALREADY SOLVED ONCE. Every floor change runs Frame::guiDestroy() +
	guiInit() and deletes everything parented to `gui`. sam_hud learned this the hard way; the
	fix was to make a retained description the source of truth and repaint from it. This does
	the same, and for the same reason: a panel that vanishes at a ladder with no event and no
	log line is indistinguishable from a broken mod.

	PANELS ARE PER-MOD. Ids are scoped by the calling mod's namespace, so two mods can both
	have a panel called "main" and neither can close or clobber the other's.

	Game build only.

-------------------------------------------------------------------------------*/

#pragma once

#include <string>
#include <vector>

namespace SAMUi
{
	// ---- panels --------------------------------------------------------------------

	// Open (or re-open) a panel. Coordinates are virtual screen pixels, the same space
	// sam_hud_* uses, so a layout survives any resolution.
	//
	// `modal` makes the panel capture the mouse: the cursor is freed and clicks go to the
	// panel rather than to the game. That is what a browser or a shop screen wants. A
	// non-modal panel is clickable but does not take the cursor, for a docked side panel.
	bool open(const std::string& ns, const std::string& id,
		int x, int y, int w, int h, const std::string& title, bool modal);

	// Close one panel, or every panel this mod opened. False if it was not open.
	bool close(const std::string& ns, const std::string& id);
	void closeNamespace(const std::string& ns);

	// Drop every panel from every mod, and the container. For the loader on mod (un)load:
	// a panel must never outlive the mod that opened it.
	void closeAll();

	bool isOpen(const std::string& ns, const std::string& id);

	// Remove every widget from a panel but leave the panel itself. This is how a script
	// redraws a list after the player types in a search box, without the panel flickering.
	bool clearWidgets(const std::string& ns, const std::string& id);

	// ---- widgets -------------------------------------------------------------------
	// All coordinates are relative to the panel. Adding a widget with an id that already
	// exists updates it in place rather than stacking a duplicate.

	// Text. `w` is the box the text is laid out in, NOT the length of the text -- pass 0 and
	// it fills the panel's whole width, which will run underneath anything to its right. A
	// text box draws a background and a label does not, so an overlap is invisible until the
	// label's text grows long enough to reach it. Size labels to their content, or stack
	// widgets in rows rather than sitting them side by side.
	bool label(const std::string& ns, const std::string& panel, const std::string& id,
		int x, int y, int w, const std::string& text, unsigned int rgba);

	// A real button. Clicking it fires the ui.on_click event with the panel and widget ids.
	bool button(const std::string& ns, const std::string& panel, const std::string& id,
		int x, int y, int w, int h, const std::string& text);

	// A picture, resolved by SAMImages the same way sam_hud_image resolves one.
	bool image(const std::string& ns, const std::string& panel, const std::string& id,
		int x, int y, int w, int h, const std::string& path, unsigned int rgba);

	// A scrolling, clickable list. This is the widget a browser is made of: 200 items you can
	// scroll and pick from. Clicking a row fires ui.on_select with the row's id in .value.
	//
	// Rows are added separately so a script can rebuild the contents on every keystroke of a
	// search box without recreating the list itself.
	bool list(const std::string& ns, const std::string& panel, const std::string& id,
		int x, int y, int w, int h);
	bool listAdd(const std::string& ns, const std::string& panel, const std::string& id,
		const std::string& rowId, const std::string& text, unsigned int rgba);
	bool listClear(const std::string& ns, const std::string& panel, const std::string& id);

	// An editable text box. Pressing enter fires ui.on_submit with the text in .value.
	// This plus list() is what makes a searchable index possible at all.
	bool input(const std::string& ns, const std::string& panel, const std::string& id,
		int x, int y, int w, int h, const std::string& text);

	// Read what the player typed, without waiting for them to press enter.
	std::string inputText(const std::string& ns, const std::string& panel, const std::string& id);

	// ---- appearance ------------------------------------------------------------------
	//
	// Nothing here should look like "the S.A.M style". A mod's panel is the mod's, so every
	// colour, the font and the row height are settable, and the defaults are only defaults.

	// Panel background and border. Either may be 0 for "leave it alone".
	bool panelStyle(const std::string& ns, const std::string& panel,
		unsigned int bg, unsigned int border, int borderWidth);

	// Font for one widget, or for the whole panel when `id` is empty (which also becomes the
	// default for widgets added afterwards).
	//
	// The engine's default is fonts/pixel_maz.ttf#32#2 -- a 32px face, which is why anything
	// dense clips. It ships smaller ones: fonts/pixel_maz_multiline.ttf#16#2,
	// fonts/pixelmix.ttf#16#2 and fonts/kongtext.ttf#16#2. Panels default to the 16px
	// multiline face because a list at 32px fits about four rows.
	bool font(const std::string& ns, const std::string& panel, const std::string& id,
		const std::string& fontName);

	// Pixel height of a list's rows. Must match the font or rows overlap or leave gaps.
	bool listRowHeight(const std::string& ns, const std::string& panel, const std::string& id, int px);

	// Measure text before placing it. This is the answer to the whole class of bug where a
	// label silently runs underneath the widget next to it: ask how wide it is, then lay out.
	// Returns false if the font could not be loaded.
	bool textSize(const std::string& text, const std::string& fontName, int& w, int& h);

	// ---- engine hooks ---------------------------------------------------------------

	// Repaint any panel whose Frame the engine destroyed. Called once a frame; free when no
	// mod has opened anything, which is every game without a UI mod.
	// True while one of this mod's text boxes has keyboard focus.
	//
	// Barony feeds the raw keyboard to gameplay bindings whether or not SDL text input is
	// active, so without an explicit gate, typing a search term into a panel also walks the
	// player around and fires their hotbar. The engine already has the right hook for an
	// outside system taking the keyboard -- it is what the ImGui debug overlay uses -- and
	// game.cpp consults this the same way.
	bool keyboardCaptured();

	void ensure();

	// True while any modal panel is open, so the engine can free the cursor and the game can
	// decide not to act on clicks that belong to the panel.
	bool modalActive();

	// How many panels are open (0 in vanilla, always).
	int count();
}
