/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_camera.cpp
	Desc: script control of the camera. See sam_camera.hpp for why it is an
	      override the engine asks for rather than a setter.

-------------------------------------------------------------------------------*/

#include "sam_camera.hpp"

#include <cmath>
#include <string>

#include "sam_logger.hpp"
#include "sam_net.hpp"     // ClearHook: a rig does not outlive the game it was set in

#include "main.hpp"        // umbrella
#include "game.hpp"        // cameras[]
#include "stat.hpp"
#include "entity.hpp"      // Entity, uidToEntity, actPlayer
#include "player.hpp"      // players[], MAXPLAYERS
#include "net.hpp"

namespace SAMCamera
{
	// One tile of height is 32 units of camera z, and z runs the OTHER WAY: the renderer
	// puts the camera at world height -camera.z. The floor of a level is at world -16.
	// Both numbers are read straight off the shader input and the map mesh builder, so a
	// height in tiles converts as: world = -16 + tiles*32, and z = -world.
	static const double SAM_Z_PER_TILE = 32.0;
	static const double SAM_FLOOR_WORLD = -16.0;

	enum PosMode { POS_NONE = 0, POS_ORBIT = 1, POS_ABSOLUTE = 2 };
	enum AngMode { ANG_PLAYER = 0, ANG_FIXED = 1, ANG_TARGET = 2 };

	struct Rig
	{
		PosMode pos = POS_NONE;
		AngMode ang = ANG_PLAYER;
		double back = 0.0, up = 0.0, right = 0.0;   // orbit, tiles
		double ax = 0.0, ay = 0.0, ah = 0.0;        // absolute, tiles (ah above the floor)
		double yaw = 0.0, pitch = 0.0;              // fixed look
		long long target = 0;
		bool collide = true;
		bool active() const { return pos != POS_NONE || ang != ANG_PLAYER; }
	};

	static Rig s_rig[MAXPLAYERS];
	static bool s_any = false;

	// Whose body flag WE turned on. Only these are put back, so a player already on the death
	// camera, or one who typed /thirdperson themselves, is left exactly as we found them.
	static bool s_ownedBody[MAXPLAYERS] = { false };

	// Where the camera ended up last frame, so sam_get_camera reports what is on screen
	// rather than what was asked for -- those differ the moment the boom hits a wall.
	static double s_lastX[MAXPLAYERS] = { 0.0 };
	static double s_lastY[MAXPLAYERS] = { 0.0 };
	static double s_lastZ[MAXPLAYERS] = { 0.0 };
	static double s_lastYaw[MAXPLAYERS] = { 0.0 };
	static double s_lastPitch[MAXPLAYERS] = { 0.0 };

	// sam_get_camera has said once, for this slot, that the camera it asked about is not here.
	static bool s_warnedGet[MAXPLAYERS] = { false };

	static void refreshAny()
	{
		s_any = false;
		for ( int i = 0; i < MAXPLAYERS; ++i )
		{
			if ( s_rig[i].active() ) { s_any = true; return; }
		}
	}

	bool any() { return s_any; }

	// A camera belongs to the machine that draws it. Another player's view lives on their
	// own computer and nothing written here could ever reach it, so a call naming one is a
	// mistake worth saying out loud rather than a no-op that looks like it worked.
	static bool samLocal(int player, const char* who)
	{
		if ( player < 0 || player >= MAXPLAYERS || !players[player] )
		{
			SAM_ERROR("CAMERA", std::string(who) + ": " + std::to_string(player)
				+ " is not a player slot.");
			return false;
		}
		if ( !players[player]->isLocalPlayer() )
		{
			SAM_WARN("CAMERA", std::string(who) + ": player " + std::to_string(player)
				+ "'s camera is on their own machine, and nothing this one draws can move it."
				" Run this in a script on their side instead.");
			return false;
		}
		return true;
	}

	static bool samFinite3(double a, double b, double c, const char* who)
	{
		if ( std::isfinite(a) && std::isfinite(b) && std::isfinite(c) ) { return true; }
		SAM_ERROR("CAMERA", std::string(who) + ": every value has to be a finite number.");
		return false;
	}

	bool setOffset(int player, double back, double up, double right)
	{
		if ( !samLocal(player, "sam_set_camera_offset") ) { return false; }
		if ( !samFinite3(back, up, right, "sam_set_camera_offset") ) { return false; }
		// 64 tiles is far past any Barony room and well beyond the draw distance; a bigger
		// number is a units mistake, and left alone it would put the view outside the level
		// where there is nothing to see and no way to tell why.
		const double lim = 64.0;
		if ( back > lim || back < -lim || up > lim || up < -lim || right > lim || right < -lim )
		{
			SAM_WARN("CAMERA", "sam_set_camera_offset: clamped to 64 tiles. Past that the camera"
				" leaves the level entirely and the screen goes empty with nothing to explain it.");
			if ( back > lim ) { back = lim; } if ( back < -lim ) { back = -lim; }
			if ( up > lim ) { up = lim; }     if ( up < -lim ) { up = -lim; }
			if ( right > lim ) { right = lim; } if ( right < -lim ) { right = -lim; }
		}
		Rig& r = s_rig[player];
		r.pos = POS_ORBIT;
		r.back = back; r.up = up; r.right = right;
		refreshAny();
		return true;
	}

	bool setPosition(int player, double x, double y, double height)
	{
		if ( !samLocal(player, "sam_set_camera_position") ) { return false; }
		if ( !samFinite3(x, y, height, "sam_set_camera_position") ) { return false; }
		Rig& r = s_rig[player];
		r.pos = POS_ABSOLUTE;
		r.ax = x; r.ay = y; r.ah = height;
		refreshAny();
		return true;
	}

	bool setAngle(int player, double yaw, double pitch)
	{
		if ( !samLocal(player, "sam_set_camera_angle") ) { return false; }
		if ( !samFinite3(yaw, pitch, 0.0, "sam_set_camera_angle") ) { return false; }
		// Pitch past a right angle turns the view upside down and the controls with it. The
		// engine clamps the player's own pitch for exactly this reason (actplayer.cpp), so a
		// script-driven camera is held to the same limit rather than being allowed to roll over.
		const double lim = PI / 2.0 - 0.01;
		if ( pitch > lim ) { pitch = lim; }
		if ( pitch < -lim ) { pitch = -lim; }
		yaw = std::fmod(yaw, 2.0 * PI);
		if ( yaw < 0.0 ) { yaw += 2.0 * PI; }
		Rig& r = s_rig[player];
		r.ang = ANG_FIXED;
		r.yaw = yaw; r.pitch = pitch;
		refreshAny();
		return true;
	}

	bool setTarget(int player, long long uid)
	{
		if ( !samLocal(player, "sam_set_camera_target") ) { return false; }
		Rig& r = s_rig[player];
		if ( uid == 0 )
		{
			if ( r.ang == ANG_TARGET ) { r.ang = ANG_PLAYER; }
			r.target = 0;
			refreshAny();
			return true;
		}
		r.ang = ANG_TARGET;
		r.target = uid;
		refreshAny();
		return true;
	}

	bool clearAngle(int player)
	{
		if ( !samLocal(player, "sam_set_camera_angle") ) { return false; }
		s_rig[player].ang = ANG_PLAYER;
		s_rig[player].target = 0;
		refreshAny();
		return true;
	}

	bool setCollision(int player, bool on)
	{
		if ( !samLocal(player, "sam_set_camera_collision") ) { return false; }
		s_rig[player].collide = on;
		return true;
	}

	bool showOwnBody(int player, bool on)
	{
		if ( !samLocal(player, "sam_show_own_body") ) { return false; }
		Entity* e = players[player]->entity;
		if ( !e ) { return false; }
		// 2 belongs to the death camera and the project-spirit effect. Taking it would strand a
		// dying player, so it is refused rather than overwritten.
		if ( e->skill[3] == 2 )
		{
			SAM_WARN("CAMERA", "sam_show_own_body: player " + std::to_string(player) + " is on the"
				" death camera, which owns this switch. Refused rather than stranding them.");
			return false;
		}
		if ( on )
		{
			// Only claim it if the engine was not already using it -- somebody may have typed
			// /thirdperson themselves, and we must not switch that off underneath them later.
			if ( e->skill[3] == 0 ) { s_ownedBody[player] = true; }
			e->skill[3] = 1;
		}
		else
		{
			if ( e->skill[3] == 1 ) { e->skill[3] = 0; }
			s_ownedBody[player] = false;
		}
		return true;
	}

	// Give the flag back, but only where we took it. Safe to call when there is no entity: a
	// rebuilt one starts at 0 anyway, which is the state we would be restoring.
	static void samReleaseBody(int player)
	{
		if ( player < 0 || player >= MAXPLAYERS ) { return; }
		if ( !s_ownedBody[player] ) { return; }
		s_ownedBody[player] = false;
		if ( players[player] && players[player]->entity && players[player]->entity->skill[3] == 1 )
		{
			players[player]->entity->skill[3] = 0;
		}
	}

	bool reset(int player)
	{
		if ( player < 0 || player >= MAXPLAYERS ) { return false; }
		const bool was = s_rig[player].active() || s_ownedBody[player];
		s_rig[player] = Rig();
		samReleaseBody(player);
		refreshAny();
		return was;
	}

	bool get(int player, double* x, double* y, double* height, double* yaw, double* pitch, int* mode)
	{
		if ( player < 0 || player >= MAXPLAYERS || !players[player] ) { return false; }
		// Only a player on THIS machine has a camera here worth reporting. For anyone else,
		// cameras[] holds the engine's guess built from their replicated position (or stale
		// defaults with interpolation off) and "mode" is always vanilla: their real view -- a
		// third-person rig, the death camera -- lives on their own machine and is never sent.
		// Nothing rather than a confident wrong answer. Said once per slot, not through samLocal,
		// because this is a reader a script may poll every tick.
		if ( !players[player]->isLocalPlayer() )
		{
			if ( !s_warnedGet[player] )
			{
				s_warnedGet[player] = true;
				SAM_WARN("CAMERA", "sam_get_camera: player " + std::to_string(player) + "'s camera is on"
					" their own machine, so this one cannot say where it is. Returning nothing.");
			}
			return false;
		}
		// Read from the live camera, not from the rig. They disagree whenever the boom has
		// been shortened by a wall, and what a script wants to know is where the view IS.
		if ( x ) { *x = cameras[player].x; }
		if ( y ) { *y = cameras[player].y; }
		if ( height ) { *height = (-cameras[player].z - SAM_FLOOR_WORLD) / SAM_Z_PER_TILE; }
		if ( yaw ) { *yaw = cameras[player].ang; }
		if ( pitch ) { *pitch = cameras[player].vang; }
		if ( mode ) { *mode = (int)s_rig[player].pos; }
		(void)s_lastX; (void)s_lastY; (void)s_lastZ; (void)s_lastYaw; (void)s_lastPitch;
		return true;
	}

	bool apply(int player, double* x, double* y, double* z, double* ang, double* vang)
	{
		if ( !s_any || player < 0 || player >= MAXPLAYERS ) { return false; }
		Rig& r = s_rig[player];
		if ( !r.active() ) { return false; }
		if ( !players[player] ) { return false; }

		Entity* e = players[player]->entity;

		double cx = *x, cy = *y, cz = *z, cang = *ang, cvang = *vang;

		// ---- where it looks, worked out FIRST, because an orbit camera is placed along it.
		if ( r.ang == ANG_FIXED )
		{
			cang = r.yaw; cvang = r.pitch;
		}
		else if ( r.ang == ANG_TARGET )
		{
			Entity* t = ( r.target > 0 && r.target <= 0x7FFFFFFFLL )
				? uidToEntity((Sint32)r.target) : nullptr;
			if ( t )
			{
				// Aimed from where the camera is ABOUT to be, not from where it was, or a
				// tracking shot lags one frame behind its subject and visibly swims.
				double fromX = cx * 16.0, fromY = cy * 16.0;
				if ( r.pos == POS_ABSOLUTE ) { fromX = r.ax * 16.0; fromY = r.ay * 16.0; }
				else if ( r.pos == POS_ORBIT && e ) { fromX = e->x; fromY = e->y; }
				cang = std::atan2(t->y - fromY, t->x - fromX);
				const double flat = std::sqrt((t->x - fromX) * (t->x - fromX)
					+ (t->y - fromY) * (t->y - fromY));
				// Barony's pitch is positive DOWNWARD, and camera z is negative upward, so a
				// target below the camera needs a positive pitch. Both inversions cancel here.
				const double camWorld = -cz;
				const double tgtWorld = -((t->z * 2.0) - 2.5);
				cvang = ( flat > 0.01 ) ? std::atan2(camWorld - tgtWorld, flat) : cvang;
			}
			else if ( r.target != 0 )
			{
				// The subject is gone. Hold the last angle rather than snapping to a default,
				// which is what a camera operator would do, and clear the target so this is
				// not recomputed every frame for something that will never come back.
				r.target = 0;
				r.ang = ANG_PLAYER;
				refreshAny();
			}
		}
		else if ( e )
		{
			cang = e->yaw; cvang = e->pitch;
		}

		// ---- where it sits
		if ( r.pos == POS_ABSOLUTE )
		{
			cx = r.ax; cy = r.ay;
			cz = -(SAM_FLOOR_WORLD + r.ah * SAM_Z_PER_TILE);
		}
		else if ( r.pos == POS_ORBIT )
		{
			if ( !e ) { return false; }   // nothing to orbit; leave the camera alone

			// The anchor is computed from the entity rather than from whatever is currently in
			// the camera, because the vanilla writers SKIP a player whose skill[3] is set --
			// which is exactly the state a third-person mod puts them in. Depending on the
			// camera's previous value would work until the moment somebody showed their body,
			// and then freeze.
			double bx = e->x / 16.0, by = e->y / 16.0;
			if ( TimerExperiments::bUseTimerInterpolation && e->bUseRenderInterpolation )
			{
				// The same source the engine's own per-frame writer uses, so the camera is
				// smooth between game ticks instead of stepping at tick rate.
				bx = e->lerpRenderState.x.position;
				by = e->lerpRenderState.y.position;
			}
			const double bz = (e->z * 2.0) - 2.5;   // the vanilla eye setpoint

			// The boom swings with pitch: looking down lifts the camera and pulls it in, which
			// is what makes an orbit camera feel like one instead of a sled.
			double back = r.back;
			if ( r.collide && back > 0.0 )
			{
				// WALLS ONLY, and marched by hand rather than handed to lineTrace.
				//
				// lineTrace's `entities` argument is a BITFIELD of LINETRACE_* flags passed
				// through to findEntityInLine -- 0 does not mean "ignore entities", it means
				// "default handling". So it stops at the first CREATURE behind the player, and a
				// rat wandering into shot would jerk the camera into the player's back. A camera
				// should be stopped by level geometry and nothing else.
				//
				// Stepped at a quarter tile, which is finer than the thinnest thing Barony
				// builds, so the march cannot hop a wall the way a single destination test can.
				const double stepPx = 4.0;
				const double dx = -std::cos(cang) * stepPx;
				const double dy = -std::sin(cang) * stepPx;
				double px = e->x, py = e->y;
				double travelled = 0.0;
				const double range = back * 16.0;
				while ( travelled + stepPx <= range )
				{
					px += dx; py += dy;
					const int tx = (int)(px / 16.0);
					const int ty = (int)(py / 16.0);
					if ( tx < 0 || ty < 0 || tx >= map.width || ty >= map.height ) { break; }
					if ( map.tiles && map.tiles[OBSTACLELAYER + ty * MAPLAYERS
						+ tx * MAPLAYERS * map.height] ) { break; }
					travelled += stepPx;
				}
				// Stop a little short of whatever was hit. A camera sitting exactly on a wall
				// sees through it, because the near plane is in front of the position.
				double allowed = ( travelled < range ) ? ( travelled - 6.0 ) : range;
				if ( allowed < 0.0 ) { allowed = 0.0; }
				back = allowed / 16.0;
			}

			const double flat = back * std::cos(cvang);
			cx = bx - std::cos(cang) * flat;
			cy = by - std::sin(cang) * flat;
			cz = bz - (r.up * SAM_Z_PER_TILE) - (std::sin(cvang) * back * SAM_Z_PER_TILE);

			// "right" is the player's right: the perpendicular to their facing.
			if ( r.right != 0.0 )
			{
				cx += -std::sin(cang) * r.right;
				cy += std::cos(cang) * r.right;
			}
		}

		if ( !std::isfinite(cx) || !std::isfinite(cy) || !std::isfinite(cz)
			|| !std::isfinite(cang) || !std::isfinite(cvang) )
		{
			return false;
		}

		*x = cx; *y = cy; *z = cz; *ang = cang; *vang = cvang;
		s_lastX[player] = cx; s_lastY[player] = cy; s_lastZ[player] = cz;
		s_lastYaw[player] = cang; s_lastPitch[player] = cvang;
		return true;
	}

	void clear()
	{
		for ( int i = 0; i < MAXPLAYERS; ++i )
		{
			s_rig[i] = Rig();
			// The body flag goes back with the camera. Leaving it set would hand the player the
			// exact frozen camera this module was written to fix, with no mod left to blame.
			samReleaseBody(i);
			s_warnedGet[i] = false;
		}
		s_any = false;
	}

	// A game ended (doEndgame runs SAMNet::clear, which runs this). A rig is keyed by player SLOT,
	// and a machine's slot changes between games -- a client that was player 2 may be player 1
	// next time -- so a rig left behind would drive somebody else's view, or answer
	// sam_get_camera for a slot that is no longer this machine's. The player entities are still
	// alive at that point, so the body flag we took is put back on a real entity.
	static SAMNet::ClearHook s_clearHook(&SAMCamera::clear);
}
