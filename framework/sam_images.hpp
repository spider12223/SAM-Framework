/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	sam_images.hpp -- let mods put their OWN pictures on the screen.

	WHY THIS EXISTS. A mod could already replace any of Barony's ~4,700 UI images by
	shadowing the file through PhysFS, but it could not ADD one: there was no way to say
	"draw my picture, here, now, for this long". Every image the game drew had to already
	be an image the game knew about. So the whole class of mods that punctuate a moment
	with art -- a jumpscare, a title card, a portrait on dialogue, a full-screen death
	splash, a custom minimap overlay -- was simply not buildable.

	Two layers, because the two uses want opposite lifetimes:

	  OVERLAY  (sam_show_image / sam_show_image_at / sam_hide_image)
	           One picture per player, drawn OVER the world and over the HUD, with a
	           duration in milliseconds after which it removes itself. This is the
	           jumpscare / title-card layer: fire and forget, no bookkeeping.

	  HUD      (sam_hud_image, in sam_hud.hpp)
	           A persistent positioned picture that lives with the rest of the script HUD
	           and is cleared by sam_hud_clear / on mod unload.

	RESOLVING A PICTURE. Scripts name images three ways, in this order:
	  "ns:name"   an id from another mod's (or this mod's) manifest "images" list
	  "name"      shorthand for "<calling mod's namespace>:name"
	  "art/x.png" a path relative to the calling mod's own folder
	The manifest form is worth preferring: it is checked when the mod loads, so a typo
	is a line in the log at load time rather than a picture that silently never appears.

	MULTIPLAYER. Scripts run on the host, but a picture has to appear on the screen of
	the player it is aimed at. When the host targets a remote player the overlay is
	forwarded over a 'SAMI' packet carrying the id, not the pixels -- the client already
	has the mod, so it resolves the same name against its own copy. Host->client only:
	the host never accepts one, exactly like 'SAMS' (move speed).

	NO-OP GUARANTEE. The registry is empty and every player's overlay is inactive in
	vanilla, so drawOverlay() returns on its first branch and nothing is loaded, decoded
	or drawn. Nothing here runs until a mod asks for it.

	Game build only.

-------------------------------------------------------------------------------*/

#pragma once

#include <string>
#include <vector>

struct SAMModManifest;  // from sam_workshop.hpp

namespace SAMImages
{
	// ---- registry ------------------------------------------------------------------

	// Register every image a mod declares in its manifest "images" list. Each entry is
	// { "id": "ns:name", "file": "art/whatever.png" }. Missing files are reported here,
	// at load, rather than at the moment a script tries to draw one.
	void loadFromManifest(const SAMModManifest& manifest);

	// Drop the id->path map (mod unload / reload).
	void clear();

	// How many images are registered. 0 in vanilla, always.
	int count();

	// Turn a script's name for a picture into a loadable path, using the rules in the
	// header comment. Returns "" when it cannot be resolved (already warned).
	std::string resolve(const std::string& ns, const std::string& nameOrPath);

	// Natural pixel size of a picture, so a script can place it without guessing.
	// Loads the image if it is not cached yet. False if it could not be loaded.
	bool size(const std::string& ns, const std::string& nameOrPath, int& w, int& h);

	// ---- overlay -------------------------------------------------------------------

	// Fit modes for the overlay rect.
	enum Fit
	{
		FIT_STRETCH = 0,  // fill the player's viewport, ignoring aspect ratio
		FIT_CONTAIN = 1,  // largest size that fits the viewport, aspect preserved
		FIT_RECT    = 2   // the explicit x/y/w/h given by the script
	};

	// Show a picture over `player`'s view. durationMs <= 0 means "until hidden".
	// alpha is 0..255. Returns false if the picture could not be resolved.
	// Host-side this also forwards to the owning client when that player is remote.
	bool show(int player, const std::string& ns, const std::string& nameOrPath,
		int durationMs, int alpha, int fit, int x, int y, int w, int h);

	// Take it away early. False if nothing was showing.
	bool hide(int player);

	// Apply an overlay that arrived over the wire ('SAMI'). Never forwards onward.
	void applyRemote(int player, bool showIt, const std::string& ns, const std::string& name,
		int durationMs, int alpha, int fit, int x, int y, int w, int h);

	// Called once per local player from the engine's post-HUD draw pass. Returns
	// immediately when that player has no overlay, which is every player in vanilla.
	void drawOverlay(int player, int viewX, int viewY, int viewW, int viewH);

	// Clear every player's overlay (new game, mod unload).
	void resetAll();

	// Wire limits for 'SAMI'. The receiving handler in net.cpp bounds against these.
	static const unsigned int SAM_IMAGE_MAX_NS   = 64;
	static const unsigned int SAM_IMAGE_MAX_NAME = 160;
}
