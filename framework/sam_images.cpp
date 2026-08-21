/*-------------------------------------------------------------------------------
	S.A.M Framework -- mod-supplied pictures on screen. See sam_images.hpp.
-------------------------------------------------------------------------------*/

#include "sam_images.hpp"
#include "sam_logger.hpp"
#include "sam_errors.hpp"
#include "sam_workshop.hpp"

#include <fstream>
#include <map>
#include <string>

#ifndef EDITOR
#	include "main.hpp"
#	include "game.hpp"
#	include "net.hpp"
#	include "player.hpp"
#	include "ui/Frame.hpp"   // Frame::virtualScreenX/Y -- the HUD coordinate space
#	include "ui/Image.hpp"
#	define SAM_IMAGES_HAVE_BARONY 1
#endif

namespace
{
	const char* MOD = "IMAGES";

	// "ns:name" -> a path Image::get can load. Empty in vanilla.
	std::map<std::string, std::string> s_registry;

	std::string joinPath(const std::string& dir, const std::string& file)
	{
		if ( dir.empty() ) { return file; }
		if ( file.empty() ) { return dir; }
		const char last = dir[dir.size() - 1];
		if ( last == '/' || last == '\\' ) { return dir + file; }
		return dir + "/" + file;
	}

	bool fileExists(const std::string& path)
	{
		std::ifstream f(path.c_str(), std::ios::binary);
		return f.good();
	}

	// Barony's own path separator convention: forward slashes, so a Workshop mod loaded
	// from a Windows path still resolves the same string on every machine.
	std::string normalizeSlashes(std::string s)
	{
		for ( char& c : s ) { if ( c == '\\' ) { c = '/'; } }
		return s;
	}

	// The folder a namespace was loaded from, or "" if that mod is not loaded here.
	std::string modPathFor(const std::string& ns)
	{
		if ( ns.empty() ) { return ""; }
		for ( const auto& m : SAMWorkshop::manifests() )
		{
			if ( m.ns == ns ) { return m.modPath; }
		}
		return "";
	}

#ifdef SAM_IMAGES_HAVE_BARONY
	// One overlay per player. `path` empty means nothing is showing -- that is the state
	// every player is in until a mod calls sam_show_image, and it is what makes
	// drawOverlay() free in vanilla.
	struct Overlay
	{
		std::string path;
		std::string ns;      // kept so a re-send to a late-joining client can re-resolve
		std::string name;
		Uint32 startMs = 0;
		Uint32 durMs   = 0;  // 0 = until hidden
		Uint8  alpha   = 255;
		Uint8  fit     = 0;
		int x = 0, y = 0, w = 0, h = 0;
	};
	Overlay s_overlay[MAXPLAYERS];

	// Tell the client that owns `player` to show/hide a picture.
	//
	// Guarded exactly like samSendMoveSpeed: player 0 would index net_clients[-1], and a
	// local or splitscreen player already has the state written directly.
	void sendOverlay(int player, bool showIt, const Overlay& o)
	{
		if ( multiplayer != SERVER ) { return; }
		if ( player <= 0 || player >= MAXPLAYERS ) { return; }
		if ( !players[player] || players[player]->isLocalPlayer() ) { return; }
		if ( client_disconnected[player] ) { return; }
		if ( o.ns.size() > SAMImages::SAM_IMAGE_MAX_NS ) { return; }
		if ( o.name.size() > SAMImages::SAM_IMAGE_MAX_NAME ) { return; }

		strcpy((char*)net_packet->data, "SAMI");
		net_packet->data[4] = (Uint8)player;
		net_packet->data[5] = (Uint8)(showIt ? 0 : 1);
		// Duration is milliseconds in a Uint16: 65 seconds is far past what an overlay is
		// for, and anything longer should be "until hidden" (0) anyway.
		SDLNet_Write16((Uint16)(o.durMs > 65535 ? 65535 : o.durMs), &net_packet->data[6]);
		net_packet->data[8] = o.alpha;
		net_packet->data[9] = o.fit;
		SDLNet_Write16((Uint16)(Sint16)o.x, &net_packet->data[10]);
		SDLNet_Write16((Uint16)(Sint16)o.y, &net_packet->data[12]);
		SDLNet_Write16((Uint16)(Sint16)o.w, &net_packet->data[14]);
		SDLNet_Write16((Uint16)(Sint16)o.h, &net_packet->data[16]);
		net_packet->data[18] = (Uint8)o.ns.size();
		net_packet->data[19] = (Uint8)o.name.size();
		if ( !o.ns.empty() )   { memcpy(&net_packet->data[20], o.ns.data(), o.ns.size()); }
		if ( !o.name.empty() ) { memcpy(&net_packet->data[20 + o.ns.size()], o.name.data(), o.name.size()); }
		net_packet->address.host = net_clients[player - 1].host;
		net_packet->address.port = net_clients[player - 1].port;
		net_packet->len = 20 + o.ns.size() + o.name.size();
		sendPacketSafe(net_sock, -1, net_packet, player - 1);
	}
#endif
}

// ---- registry ------------------------------------------------------------------------

void SAMImages::loadFromManifest(const SAMModManifest& manifest)
{
	for ( const auto& kv : manifest.images )
	{
		const std::string& id   = kv.first;
		const std::string& file = kv.second;
		if ( id.empty() || file.empty() ) { continue; }
		if ( SAMErrors::relPathEscapes(file) )
		{
			SAM_WARN(MOD, "Image [" + id + "] path '" + file + "' escapes the mod folder -- ignored.");
			continue;
		}
		const std::string path = normalizeSlashes(joinPath(manifest.modPath, file));
		if ( !fileExists(path) )
		{
			// The whole reason to declare images in the manifest: a typo says so HERE,
			// once, at load, instead of drawing nothing forever with no explanation.
			SAM_ERROR(MOD, "Image file not found: " + path + " (declared as [" + id
				+ "] by [" + manifest.ns + "]) -- that image will not draw.");
			continue;
		}
		auto existing = s_registry.find(id);
		if ( existing != s_registry.end() && existing->second != path )
		{
			SAM_WARN(MOD, "Image id [" + id + "] declared twice -- keeping the first.");
			continue;
		}
		s_registry[id] = path;
	}
	if ( !manifest.images.empty() )
	{
		SAM_INFO(MOD, "[" + manifest.ns + "] registered "
			+ std::to_string(manifest.images.size()) + " image(s).");
	}
}

void SAMImages::clear()
{
	s_registry.clear();
	resetAll();
}

int SAMImages::count() { return (int)s_registry.size(); }

std::string SAMImages::resolve(const std::string& ns, const std::string& nameOrPath)
{
	if ( nameOrPath.empty() ) { return ""; }

	// 1) an explicit "ns:name" id.
	if ( nameOrPath.find(':') != std::string::npos )
	{
		auto it = s_registry.find(nameOrPath);
		if ( it != s_registry.end() ) { return it->second; }
		SAM_WARN(MOD, "No image registered as [" + nameOrPath
			+ "]. Declare it in mod.json \"images\", or pass a path inside your mod folder.");
		return "";
	}

	// 2) a bare name, meaning one of the calling mod's own declared images.
	if ( !ns.empty() )
	{
		auto it = s_registry.find(ns + ":" + nameOrPath);
		if ( it != s_registry.end() ) { return it->second; }
	}

	// 3) a path relative to the calling mod's folder.
	if ( SAMErrors::relPathEscapes(nameOrPath) )
	{
		SAM_WARN(MOD, "Image path '" + nameOrPath + "' escapes the mod folder -- refused.");
		return "";
	}
	const std::string base = modPathFor(ns);
	if ( base.empty() )
	{
		SAM_WARN(MOD, "Cannot resolve image '" + nameOrPath + "': no mod folder for namespace ["
			+ ns + "].");
		return "";
	}
	const std::string path = normalizeSlashes(joinPath(base, nameOrPath));
	if ( !fileExists(path) )
	{
		SAM_WARN(MOD, "Image file not found: " + path);
		return "";
	}
	return path;
}

bool SAMImages::size(const std::string& ns, const std::string& nameOrPath, int& w, int& h)
{
	w = 0; h = 0;
#ifndef SAM_IMAGES_HAVE_BARONY
	(void)ns; (void)nameOrPath;
	return false;
#else
	const std::string path = resolve(ns, nameOrPath);
	if ( path.empty() ) { return false; }
	Image* img = Image::get(path.c_str());
	if ( !img || img->getWidth() == 0 || img->getHeight() == 0 ) { return false; }
	w = (int)img->getWidth();
	h = (int)img->getHeight();
	return true;
#endif
}

// ---- overlay -------------------------------------------------------------------------

bool SAMImages::show(int player, const std::string& ns, const std::string& nameOrPath,
	int durationMs, int alpha, int fit, int x, int y, int w, int h)
{
#ifndef SAM_IMAGES_HAVE_BARONY
	(void)player; (void)ns; (void)nameOrPath; (void)durationMs; (void)alpha;
	(void)fit; (void)x; (void)y; (void)w; (void)h;
	return false;
#else
	if ( player < 0 || player >= MAXPLAYERS ) { return false; }
	if ( nameOrPath.size() > SAM_IMAGE_MAX_NAME || ns.size() > SAM_IMAGE_MAX_NS )
	{
		SAM_WARN(MOD, "sam_show_image: name too long to send over the wire -- refused.");
		return false;
	}

	// Resolve on the machine that ASKED, so a bad name is reported once, here, with the
	// calling mod's namespace attached -- rather than silently on someone else's client.
	const std::string path = resolve(ns, nameOrPath);
	if ( path.empty() ) { return false; }

	Overlay& o = s_overlay[player];
	o.path    = path;
	o.ns      = ns;
	o.name    = nameOrPath;
	o.startMs = SDL_GetTicks();
	o.durMs   = (Uint32)(durationMs > 0 ? (durationMs > 65535 ? 65535 : durationMs) : 0);
	o.alpha   = (Uint8)(alpha < 0 ? 0 : (alpha > 255 ? 255 : alpha));
	o.fit     = (Uint8)(fit == FIT_CONTAIN ? FIT_CONTAIN : (fit == FIT_RECT ? FIT_RECT : FIT_STRETCH));
	o.x = x; o.y = y; o.w = w; o.h = h;

	// A fully transparent overlay is a hide, not an invisible one that still expires.
	if ( o.alpha == 0 ) { return hide(player); }

	sendOverlay(player, true, o);
	return true;
#endif
}

bool SAMImages::hide(int player)
{
#ifndef SAM_IMAGES_HAVE_BARONY
	(void)player;
	return false;
#else
	if ( player < 0 || player >= MAXPLAYERS ) { return false; }
	Overlay& o = s_overlay[player];
	const bool had = !o.path.empty();
	// Send before clearing so the client learns which picture to drop, then clear.
	if ( had ) { sendOverlay(player, false, o); }
	o.path.clear();
	o.ns.clear();
	o.name.clear();
	o.durMs = 0;
	return had;
#endif
}

void SAMImages::applyRemote(int player, bool showIt, const std::string& ns, const std::string& name,
	int durationMs, int alpha, int fit, int x, int y, int w, int h)
{
#ifndef SAM_IMAGES_HAVE_BARONY
	(void)player; (void)showIt; (void)ns; (void)name; (void)durationMs; (void)alpha;
	(void)fit; (void)x; (void)y; (void)w; (void)h;
#else
	if ( player < 0 || player >= MAXPLAYERS ) { return; }
	if ( !showIt )
	{
		Overlay& o = s_overlay[player];
		o.path.clear(); o.ns.clear(); o.name.clear(); o.durMs = 0;
		return;
	}
	// Resolve against THIS machine's copy of the mod. The host sent a name, not pixels,
	// so a client missing the mod simply logs and draws nothing instead of desyncing.
	const std::string path = resolve(ns, name);
	if ( path.empty() ) { return; }

	Overlay& o = s_overlay[player];
	o.path    = path;
	o.ns      = ns;
	o.name    = name;
	o.startMs = SDL_GetTicks();
	o.durMs   = (Uint32)(durationMs > 0 ? durationMs : 0);
	o.alpha   = (Uint8)(alpha < 0 ? 0 : (alpha > 255 ? 255 : alpha));
	o.fit     = (Uint8)(fit == FIT_CONTAIN ? FIT_CONTAIN : (fit == FIT_RECT ? FIT_RECT : FIT_STRETCH));
	o.x = x; o.y = y; o.w = w; o.h = h;
	if ( o.alpha == 0 ) { o.path.clear(); }
#endif
}

void SAMImages::drawOverlay(int player, int viewX, int viewY, int viewW, int viewH)
{
#ifndef SAM_IMAGES_HAVE_BARONY
	(void)player; (void)viewX; (void)viewY; (void)viewW; (void)viewH;
#else
	if ( player < 0 || player >= MAXPLAYERS ) { return; }
	Overlay& o = s_overlay[player];
	if ( o.path.empty() ) { return; }   // the vanilla path: one branch, then out
	// Never over a menu. A script could leave a durationless overlay up and then the player
	// quits; the mod-unload clear would tidy it, but not before it covered the main menu.
	if ( intro ) { return; }

	// Expiry is wall-clock, like the impact flash, so an overlay lasts as long as the
	// script asked no matter what the frame rate is doing. Unsigned, so it is safe
	// across the SDL_GetTicks wrap.
	if ( o.durMs > 0 && (SDL_GetTicks() - o.startMs) >= o.durMs )
	{
		o.path.clear(); o.ns.clear(); o.name.clear(); o.durMs = 0;
		return;
	}

	Image* img = Image::get(o.path.c_str());
	if ( !img || img->getWidth() == 0 || img->getHeight() == 0 ) { return; }

	SDL_Rect dest{ viewX, viewY, viewW, viewH };
	if ( o.fit == FIT_RECT )
	{
		// Script-placed: coordinates are virtual-screen pixels (the space the HUD uses),
		// scaled into this player's viewport so a fixed layout survives any resolution.
		const double sx = (double)viewW / (double)Frame::virtualScreenX;
		const double sy = (double)viewH / (double)Frame::virtualScreenY;
		const int rw = ( o.w > 0 ) ? o.w : (int)img->getWidth();
		const int rh = ( o.h > 0 ) ? o.h : (int)img->getHeight();
		dest.x = viewX + (int)(o.x * sx);
		dest.y = viewY + (int)(o.y * sy);
		dest.w = (int)(rw * sx);
		dest.h = (int)(rh * sy);
	}
	else if ( o.fit == FIT_CONTAIN )
	{
		// Largest rect that fits the viewport with the picture's own aspect kept, centred.
		const double ia = (double)img->getWidth() / (double)img->getHeight();
		const double va = (double)viewW / (double)viewH;
		if ( ia > va ) { dest.h = (int)(viewW / ia); dest.y = viewY + (viewH - dest.h) / 2; }
		else           { dest.w = (int)(viewH * ia); dest.x = viewX + (viewW - dest.w) / 2; }
	}
	if ( dest.w <= 0 || dest.h <= 0 ) { return; }

	// White at the requested alpha = the picture's own colours, faded. drawColor MIXES
	// the colour in, so anything other than white would tint the art.
	img->drawColor(nullptr, dest, SDL_Rect{ 0, 0, xres, yres },
		makeColor(255, 255, 255, o.alpha));
#endif
}

void SAMImages::resetAll()
{
#ifdef SAM_IMAGES_HAVE_BARONY
	for ( int c = 0; c < MAXPLAYERS; ++c )
	{
		s_overlay[c].path.clear();
		s_overlay[c].ns.clear();
		s_overlay[c].name.clear();
		s_overlay[c].durMs = 0;
	}
#endif
}
