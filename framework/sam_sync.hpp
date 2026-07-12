/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_sync.hpp
	Desc: multiplayer mod-fingerprint sync checker.

	After a client joins a lobby, the host sends a "SAMF" packet carrying its
	canonical mod list ("namespace@version;..." sorted) — chunked exactly like
	Barony's own "CSCN" scenario packet, over the transport-unified
	sendPacketSafe() (direct IP, Steam P2P, EOS). The client reassembles it,
	compares against its own list, and on mismatch WARNS the player (prompt +
	sam_log.txt detail) — it never blocks the connection; the player decides.

	- Singleplayer is never touched: sending is gated on multiplayer == SERVER,
	  and receiving only happens in the client packet handler.
	- A host running S.A.M with zero mods sends an empty fingerprint (numchunks
	  == 0) so modded clients still get a warning.
	- A VANILLA host (no S.A.M exe) never sends "SAMF" at all — a modded client
	  cannot distinguish that from "no packet yet", so no warning fires there.
	- A vanilla CLIENT receiving "SAMF" just logs Barony's "mystery packet"
	  line and continues — no crash.

	Delivery robustness: Barony safe packets are reliable but UNORDERED, and a
	"SAMF" reordered ahead of "HELO" during the join handshake gets acked-then-
	dropped by the pre-lobby filter. Two mitigations: reassembly tolerates any
	chunk arrival order (never resets on chunk 1), and the client re-REQUESTS
	the fingerprint from its lobby card ("SAMF" client->host, mirroring the
	SVFL/CSCN request pattern), so a swallowed join-time send self-heals.

	Game build only (GAME_SOURCES) — the editor has no networking.

-------------------------------------------------------------------------------*/

#pragma once

#include <string>

class SAMSync
{
public:
	// Canonical fingerprint of the currently loaded S.A.M mod list:
	// "ns@version;ns@version;..." sorted ascending. Empty string = no mods.
	static std::string generateFingerprint();

	// Host only (multiplayer == SERVER): send our fingerprint to the given
	// connected player slot (1..MAXPLAYERS-1) as chunked "SAMF" safe packets.
	static void sendFingerprint(int player);

	// Client only (multiplayer == CLIENT): ask the host to (re)send its
	// fingerprint. Called from the lobby card in case the join-time send was
	// swallowed during the handshake. Duplicate deliveries are deduped in
	// receiveFingerprint, so this never re-fires an unchanged warning.
	static void requestFingerprint();

	// Client side: feed the payload of one received "SAMF" packet
	// (pass &net_packet->data[4], net_packet->len - 4). Returns true once the
	// full fingerprint has been reassembled and compared — after which
	// hasMismatch()/getMismatchDetails() are valid for this transmission.
	static bool receiveFingerprint(const char* data, int len);

	// True if the last complete fingerprint comparison found a difference.
	static bool hasMismatch();

	// Human-readable mismatch report, one issue per line:
	//   missing [darkblade v1.0.0]
	//   version [darkblade: host v1.0.0, you v1.1.0]
	//   extra [foo v2.0.0]
	// Empty string when there is no mismatch.
	static std::string getMismatchDetails();

	// Reset all reassembly + comparison state (called on mod load/unload).
	static void clear();
};
