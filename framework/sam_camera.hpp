/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_camera.hpp
	Desc: script control of where a player's camera is and what it looks at.

	WHY THIS CANNOT BE A PLAIN SETTER.

	`cameras[n]` is rewritten from the player entity every single frame, in two
	different places, and which one runs depends on a setting:

	  * actplayer.cpp writes it on the game tick, gated on `!PLAYER_DEBUGCAM`
	  * TimerExperiments::renderCameras (game.cpp) writes it on every RENDERED
	    frame from the interpolated render state, when interpolation is on

	So a binding that assigned `cameras[n].x` would be erased before anything was
	drawn. This is the same shape as a creature's z, which its own species code
	rewrites every frame. The override therefore lives here and the engine ASKS
	for it, once per rendered frame, at the one point after both writers and
	before the screen shake is added -- so a modded camera still shakes.

	WHAT BARONY'S "THIRD PERSON" ACTUALLY IS.

	The `/thirdperson` console command sets `skill[3] = 1`, and BOTH camera
	writers skip a player whose skill[3] is non-zero. Nothing else moves the
	camera, so it simply stops where it was: the body becomes visible and the
	view stays behind. It is a detached camera, not a third-person camera. This
	module supplies the half that was missing.

	UNITS. Everything crossing this API is in TILES, like the rest of the
	framework. Internally camera x and y are already tiles, but the vertical is
	a separate scale that runs the other way: the renderer places the camera at
	world height `-camera.z`, the floor of a level sits at world -16 and a tile
	is 32 world units, so one tile of height is 32 units of z and MORE NEGATIVE
	IS HIGHER. Every conversion is done here so no caller has to know that.

-------------------------------------------------------------------------------*/

#pragma once

namespace SAMCamera
{
	// ---- the rig ------------------------------------------------------------------------
	//
	// An ORBIT camera is derived from the player every frame, offset in the player's own
	// frame of reference: `back` tiles behind them, `up` tiles above, `right` tiles to the
	// side. It follows their yaw and pitch, so looking down swings the camera up and over,
	// the way a third-person camera should. back = 0 restores the eye position.
	bool setOffset(int player, double back, double up, double right);

	// An ABSOLUTE camera is pinned in the world and does not follow the player at all:
	// a security camera, a cutscene, a fixed isometric view. x and y are tile coordinates,
	// height is in tiles ABOVE THE FLOOR (a standing eye is about 0.64).
	bool setPosition(int player, double x, double y, double height);

	// ---- where it looks -----------------------------------------------------------------
	//
	// By default the camera keeps the player's own yaw and pitch, so the mouse still turns
	// it. These override that. setTarget makes it track an entity, recomputed every frame;
	// a uid of 0 clears it. clearAngle returns to following the player.
	bool setAngle(int player, double yaw, double pitch);
	bool setTarget(int player, long long uid);
	bool clearAngle(int player);

	// ---- the boom ------------------------------------------------------------------------
	//
	// Whether an ORBIT camera shortens when a wall is in the way, so the view never ends up
	// inside geometry. On by default, and it uses the engine's own line trace, so it agrees
	// with what the level actually blocks.
	bool setCollision(int player, bool on);

	// ---- seeing yourself ------------------------------------------------------------------
	//
	// Whether the local player's own character is drawn (and the first-person weapon hidden).
	// This is the engine's own switch -- the one /thirdperson flips -- and it lives HERE rather
	// than in a loose binding because it shares the camera's lifetime: both of the engine's
	// camera writers skip a player who has it set, so a flag left behind when a mod unloads
	// would strand them in the frozen-camera state this module exists to fix.
	//
	// clear() and reset() put back whatever the engine had.
	bool showOwnBody(int player, bool on);

	// Back to vanilla for one player: the camera AND the body flag. Returns true if anything
	// was overridden.
	bool reset(int player);

	// ---- reads ----------------------------------------------------------------------------
	//
	// Where the camera actually IS right now, after everything above has been applied, in
	// the same tile units the setters take. mode is 0 vanilla, 1 orbit, 2 absolute.
	bool get(int player, double* x, double* y, double* height, double* yaw, double* pitch, int* mode);

	// ---- the engine side --------------------------------------------------------------------
	//
	// Called once per rendered frame per viewport. Returns true when it changed something, so
	// the caller only writes back when it has to. `any()` is a single bool test so a game with
	// no mod loaded pays nothing.
	bool any();
	bool apply(int player, double* x, double* y, double* z, double* ang, double* vang);

	// Drop every override. Both loader paths, like every other registry.
	void clear();
}
