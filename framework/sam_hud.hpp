/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	sam_hud.hpp — a script-driven HUD layer.

	Barony has a real retained UI framework (Frame/Field/Button), and `gui` is a public
	root Frame that already processes and draws whatever is parented to it. So a mod HUD
	does not need a new UI system; it needs a handful of bindings that put widgets under
	that root and take them away again.

	The model is deliberately NOT immediate-mode in the draw sense. A script calls
	sam_hud_text("hp", ...) whenever the value changes and the widget persists until it is
	changed or cleared, which is what a mod actually wants (update on an event, not 50
	times a second) and costs nothing per frame.

	Everything lives under ONE container frame named "sam_hud". That matters for the
	no-op rule: with no mod loaded the container is never created, so vanilla pays nothing
	and there is no way for a stale widget to outlive an unloaded mod -- clearAll() drops
	the whole container in one call and the loader invokes it on unload.

	Game build only: the editor links the UI too, but has no scripts to drive this.

-------------------------------------------------------------------------------*/

#pragma once

#include <string>

class SAMHud
{
public:
	// Show (or update) a line of text. `id` is the mod's own name for it; calling again
	// with the same id moves/retitles the existing widget rather than stacking a new one.
	// Coordinates are virtual screen pixels, the same space the vanilla HUD uses.
	static bool text(const std::string& ns, const std::string& id, int x, int y,
		const std::string& value, unsigned int rgba);

	// Show (or update) a horizontal bar. `frac` is clamped to 0..1.
	static bool bar(const std::string& ns, const std::string& id, int x, int y, int w, int h,
		double frac, unsigned int rgba);

	// Show (or update) a PICTURE the mod ships. `path` is already resolved (SAMImages
	// does that) so this module stays a pure widget layer. w/h of 0 means "the picture's
	// own pixel size". `rgba` is mixed into the image, so white is untinted -- use the
	// alpha channel to fade it.
	static bool image(const std::string& ns, const std::string& id, int x, int y, int w, int h,
		const std::string& path, unsigned int rgba);

	// Remove one element. Returns false if that id was not showing.
	//
	// EVERY call takes the OWNING MOD'S NAMESPACE. Ids are per-mod, not global: two mods can
	// both call their bar "hp" without colliding, and one mod clearing its HUD cannot take
	// another mod's widgets down with it. That was the behaviour before, and it only went
	// unnoticed because this API is new enough that no two mods use it yet.
	static bool clear(const std::string& ns, const std::string& id);

	// Remove everything ONE mod drew. This is what a script's sam_hud_clear() with no
	// argument does -- it means "clear my HUD", never "clear everybody's".
	static void clearNamespace(const std::string& ns);

	// Remove every element and the container itself, across all mods. Reserved for the
	// loader: this runs on mod load and unload, where dropping everything is the point.
	// Scripts get clearNamespace instead.
	static void clearAll();

	// Put the HUD back if the engine threw the UI away.
	//
	// Every floor change runs Frame::guiDestroy() + guiInit() (createLevelLoadScreen passes a
	// null background, and baseCreateLoadingScreen wipes the tree in that case), which deletes
	// our container along with everything else. Nothing tells us. So the engine calls this once
	// a frame and it repaints the retained widgets whenever it finds the container gone.
	//
	// Free in vanilla: with no widgets recorded it returns on an empty() test.
	static void ensure();

	// How many elements the script has asked for (0 in vanilla, always). This counts the
	// RETAINED widgets, not live Frames, so it stays correct across a floor change.
	static int count();
};
