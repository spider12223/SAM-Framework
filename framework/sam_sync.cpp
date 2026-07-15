/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_sync.cpp
	Desc: implementation of the multiplayer mod-fingerprint sync checker.

	Wire format (mirrors Barony's "CSCN" custom-scenario packet):
	  data[0..3] = "SAMF"
	  data[4]    = sequence (low 4 bits, 1-based) | numchunks << 4 (high 4 bits)
	  data[5..]  = fingerprint chunk (max CHUNK_SIZE chars)
	  numchunks == 0  ->  host runs S.A.M with ZERO mods (empty fingerprint).

	Sent host -> client via sendPacketSafe() (reliable, all transports), right
	after the join handshake alongside SVFL/CSCN. Game build only — no EDITOR
	guards needed because this file is only in GAME_SOURCES.

-------------------------------------------------------------------------------*/

// Barony headers pull in <windows.h>; stop it defining min()/max() macros.
// Must precede every include.
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "sam_sync.hpp"
#include "sam_workshop.hpp"
#include "sam_logger.hpp"

#include "main.hpp"   // multiplayer/SERVER/CLIENT, MAXPLAYERS, net_packet, net_clients, net_sock, client_disconnected, stringCopy
#include "net.hpp"    // sendPacketSafe

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

static const char* MOD = "SYNC";

// Chunking (mirrors sendCustomScenarioOverNet): 4-bit sequence/count fields,
// so at most 15 chunks of CHUNK_SIZE chars each.
static const int CHUNK_SIZE = 256;
static const int MAX_CHUNKS = 15;

// --- client-side reassembly + comparison state ---
static std::map<int, std::string> s_chunks;      // sequence -> chunk text
static int s_expectedChunks = 0;                 // numchunks of the transmission being assembled
static bool s_haveHostFingerprint = false;
static std::string s_hostFingerprint;
static bool s_mismatch = false;
static std::string s_mismatchDetails;
// Anti-spam: distinct fingerprints already processed + a hard prompt cap, so an
// attacker on unauthenticated direct-IP can't alternate two fingerprints to defeat
// the single-value dedup and re-fire the mismatch prompt endlessly.
static std::map<std::string, bool> s_seenFingerprints;
static int s_fpPromptCount = 0;

/*-------------------------------------------------------------------------------
	Local helpers
-------------------------------------------------------------------------------*/

// FNV-1a 32-bit — compact hash for log lines only (comparison is string-level).
static unsigned int fnv1a(const std::string& s)
{
	unsigned int h = 2166136261u;
	for ( size_t i = 0; i < s.size(); ++i )
	{
		h ^= static_cast<unsigned char>(s[i]);
		h *= 16777619u;
	}
	return h;
}

static std::string hashString(const std::string& fp)
{
	char buf[16];
	snprintf(buf, sizeof(buf), "%08x", fnv1a(fp));
	return std::string(buf);
}

// Parse "ns@version;ns@version;..." into a ns -> version map.
static std::map<std::string, std::string> parseFingerprint(const std::string& fp)
{
	std::map<std::string, std::string> mods;
	size_t start = 0;
	while ( start < fp.size() )
	{
		size_t end = fp.find(';', start);
		if ( end == std::string::npos )
		{
			end = fp.size();
		}
		const std::string entry = fp.substr(start, end - start);
		const size_t at = entry.find('@');
		if ( at != std::string::npos && at > 0 )
		{
			mods[entry.substr(0, at)] = entry.substr(at + 1);
		}
		start = end + 1;
	}
	return mods;
}

// Compare the received host fingerprint against our own; fill the mismatch
// state and log the outcome.
static void compareFingerprints()
{
	const std::string local = SAMSync::generateFingerprint();

	s_mismatch = (local != s_hostFingerprint);
	s_mismatchDetails.clear();

	if ( !s_mismatch )
	{
		const int n = static_cast<int>(parseFingerprint(local).size());
		SAM_INFO(MOD, "Mod fingerprints MATCH host (" + std::to_string(n)
			+ " mod(s), hash " + hashString(local) + ").");
		return;
	}

	const std::map<std::string, std::string> hostMods = parseFingerprint(s_hostFingerprint);
	const std::map<std::string, std::string> localMods = parseFingerprint(local);

	std::vector<std::string> issues;
	for ( const auto& kv : hostMods )
	{
		auto it = localMods.find(kv.first);
		if ( it == localMods.end() )
		{
			issues.push_back("missing [" + kv.first + " v" + kv.second + "]");
		}
		else if ( it->second != kv.second )
		{
			issues.push_back("version [" + kv.first + ": host v" + kv.second
				+ ", you v" + it->second + "]");
		}
	}
	for ( const auto& kv : localMods )
	{
		if ( hostMods.find(kv.first) == hostMods.end() )
		{
			issues.push_back("extra [" + kv.first + " v" + kv.second + "]");
		}
	}

	// The raw strings differ but the per-namespace maps agree — duplicate
	// namespaces or a truncated oversized list. Never warn with empty details.
	if ( issues.empty() )
	{
		issues.push_back("mod lists differ [host hash " + hashString(s_hostFingerprint)
			+ ", yours " + hashString(local) + "] (duplicate namespaces or oversized mod list)");
	}

	SAM_WARN(MOD, "S.A.M mod MISMATCH with host — host hash " + hashString(s_hostFingerprint)
		+ " (" + std::to_string(hostMods.size()) + " mod(s)), local hash " + hashString(local)
		+ " (" + std::to_string(localMods.size()) + " mod(s)), " + std::to_string(issues.size())
		+ " issue(s):");
	for ( size_t i = 0; i < issues.size(); ++i )
	{
		SAM_WARN(MOD, "  " + issues[i]);
		if ( !s_mismatchDetails.empty() )
		{
			s_mismatchDetails += "\n";
		}
		s_mismatchDetails += issues[i];
	}
}

/*-------------------------------------------------------------------------------
	SAMSync
-------------------------------------------------------------------------------*/

std::string SAMSync::generateFingerprint()
{
	std::vector<std::string> entries;
	for ( const SAMModManifest& m : SAMWorkshop::manifests() )
	{
		entries.push_back(m.ns + "@" + m.version);
	}
	std::sort(entries.begin(), entries.end());

	std::string fp;
	for ( size_t i = 0; i < entries.size(); ++i )
	{
		if ( i > 0 )
		{
			fp += ";";
		}
		fp += entries[i];
	}
	return fp;
}

void SAMSync::sendFingerprint(int player)
{
	if ( multiplayer != SERVER )
	{
		return; // singleplayer / client: sync never runs
	}
	if ( player <= 0 || player >= MAXPLAYERS || client_disconnected[player] )
	{
		return;
	}

	std::string fp = generateFingerprint();
	const std::string hash = hashString(fp);
	const int numMods = static_cast<int>(parseFingerprint(fp).size());

	int numchunks = fp.empty() ? 0 : (1 + static_cast<int>((fp.size() - 1) / CHUNK_SIZE));
	if ( numchunks > MAX_CHUNKS )
	{
		SAM_WARN(MOD, "Fingerprint too long (" + std::to_string(fp.size()) + " chars, "
			+ std::to_string(numMods) + " mods) — truncating to " + std::to_string(MAX_CHUNKS)
			+ " chunks. Clients will report a mismatch.");
		fp.resize(MAX_CHUNKS * CHUNK_SIZE);
		numchunks = MAX_CHUNKS;
	}

	if ( numchunks == 0 )
	{
		// empty fingerprint: we run S.A.M but no mods are loaded
		memcpy(net_packet->data, "SAMF", 4);
		net_packet->data[4] = 0;
		net_packet->len = 5;
		net_packet->address.host = net_clients[player - 1].host;
		net_packet->address.port = net_clients[player - 1].port;
		sendPacketSafe(net_sock, -1, net_packet, player - 1);
		SAM_INFO(MOD, "Sent EMPTY mod fingerprint to player " + std::to_string(player)
			+ " (no S.A.M mods loaded).");
		return;
	}

	int sequence = 0;
	for ( size_t c = 0; c < fp.size(); c += CHUNK_SIZE )
	{
		sequence += 1;
		const std::string chunk = fp.substr(c, CHUNK_SIZE);

		memcpy(net_packet->data, "SAMF", 4);
		net_packet->data[4] = static_cast<Uint8>(sequence & 0xF);
		net_packet->data[4] |= static_cast<Uint8>((numchunks & 0xF) << 4);
		stringCopy((char*)net_packet->data + 5, chunk.c_str(), CHUNK_SIZE + 1, chunk.size());
		net_packet->len = 5 + static_cast<int>(chunk.size());
		net_packet->address.host = net_clients[player - 1].host;
		net_packet->address.port = net_clients[player - 1].port;
		sendPacketSafe(net_sock, -1, net_packet, player - 1);
	}

	SAM_INFO(MOD, "Sent mod fingerprint to player " + std::to_string(player) + " ("
		+ std::to_string(numMods) + " mod(s), hash " + hash + ", "
		+ std::to_string(numchunks) + " chunk(s)).");
}

bool SAMSync::receiveFingerprint(const char* data, int len)
{
	if ( !data || len < 1 )
	{
		return false;
	}

	const unsigned char head = static_cast<unsigned char>(data[0]);
	const int sequence = head & 0xF;
	const int numchunks = (head >> 4) & 0xF;

	if ( numchunks == 0 )
	{
		// host runs S.A.M with zero mods
		s_chunks.clear();
		s_expectedChunks = 0;
		if ( s_haveHostFingerprint && s_hostFingerprint.empty() )
		{
			return false; // duplicate transmission, nothing new — don't re-fire the prompt
		}
		s_hostFingerprint.clear();
		s_haveHostFingerprint = true;
		SAM_INFO(MOD, "Received EMPTY mod fingerprint from host (host has no S.A.M mods).");
		compareFingerprints();
		return true;
	}

	if ( sequence < 1 || sequence > numchunks )
	{
		SAM_WARN(MOD, "Ignoring malformed SAMF chunk (sequence " + std::to_string(sequence)
			+ " of " + std::to_string(numchunks) + ").");
		return false;
	}

	// A different chunk count means a NEW transmission — drop stale partials.
	// Deliberately NOT keyed on sequence==1: Barony safe packets are reliable
	// but UNORDERED, so chunk 1 can legitimately arrive after later chunks of
	// the same transmission (resetting there would wedge reassembly forever,
	// because acked chunks are never re-sent).
	if ( numchunks != s_expectedChunks )
	{
		s_chunks.clear();
		s_expectedChunks = numchunks;
	}

	// copy the chunk payload (bounded; net packets are <= 512 bytes total)
	char buf[512];
	const int payloadLen = std::min(len - 1, static_cast<int>(sizeof(buf)) - 1);
	memcpy(buf, data + 1, payloadLen);
	buf[payloadLen] = '\0';
	s_chunks[sequence] = buf;
	SAM_DEBUG(MOD, "Received SAMF chunk " + std::to_string(sequence) + "/"
		+ std::to_string(numchunks) + " (" + std::to_string(payloadLen) + " chars).");

	if ( static_cast<int>(s_chunks.size()) < numchunks )
	{
		return false; // waiting for more chunks
	}

	// all chunks in — reassemble strictly in order; the range guard + count
	// reset above ensure keys are exactly [1..numchunks], but never fabricate
	// an empty chunk if that invariant is somehow violated
	std::string assembled;
	for ( int i = 1; i <= numchunks; ++i )
	{
		auto it = s_chunks.find(i);
		if ( it == s_chunks.end() )
		{
			return false; // missing chunk — keep waiting
		}
		assembled += it->second;
	}
	s_chunks.clear();
	s_expectedChunks = 0;

	if ( s_haveHostFingerprint && assembled == s_hostFingerprint )
	{
		SAM_DEBUG(MOD, "Duplicate fingerprint transmission (unchanged) — ignoring.");
		return false; // nothing new — don't re-fire the prompt
	}

	s_hostFingerprint = assembled;
	s_haveHostFingerprint = true;

	// Only prompt for a fingerprint we have NOT already processed this session, and
	// cap total prompts. This defeats the alternating-A/B spam that slips past the
	// single-value dedup above, while still surfacing a genuine first-time change.
	static const int MAX_FP_PROMPTS = 8;
	if ( s_seenFingerprints.count(assembled) || s_fpPromptCount >= MAX_FP_PROMPTS )
	{
		SAM_DEBUG(MOD, "Fingerprint already seen or prompt cap reached — not re-firing.");
		return false;
	}
	s_seenFingerprints[assembled] = true;
	++s_fpPromptCount;
	compareFingerprints();
	return true;
}

void SAMSync::requestFingerprint()
{
	if ( multiplayer != CLIENT )
	{
		return; // only a connected client asks the host
	}
	// Ask the host to (re)send its fingerprint. Covers the join-time send being
	// swallowed by the pre-lobby HELO filter (safe packets can arrive reordered
	// on direct connections). Mirrors the client-side SVFL/CSCN request pattern.
	memcpy(net_packet->data, "SAMF", 4);
	net_packet->data[4] = static_cast<Uint8>(clientnum);
	net_packet->len = 5;
	net_packet->address.host = net_server.host;
	net_packet->address.port = net_server.port;
	sendPacketSafe(net_sock, -1, net_packet, 0);
	SAM_DEBUG(MOD, "Requested mod fingerprint from host.");
}

bool SAMSync::hasMismatch()
{
	return s_haveHostFingerprint && s_mismatch;
}

std::string SAMSync::getMismatchDetails()
{
	return s_mismatchDetails;
}

void SAMSync::clear()
{
	s_chunks.clear();
	s_expectedChunks = 0;
	s_hostFingerprint.clear();
	s_haveHostFingerprint = false;
	s_mismatch = false;
	s_mismatchDetails.clear();
	s_seenFingerprints.clear();
	s_fpPromptCount = 0;
}
