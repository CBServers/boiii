#pragma once

#include <string>

namespace nat
{
	// The active host token, or "" when not hosting (session lifecycle is internal).
	std::string current_token();

	// The host's reachable endpoint ("ip:port") for the join-secret fallback, or "".
	std::string get_host_endpoint();

	// The rendezvous server the joiner punches through (defaults if dvars not yet registered).
	void get_rendezvous(std::string& host, int& port);

	// Joiner: punch toward the host; on failure connect fallback_address, else error.
	void begin_join(const std::string& token, const std::string& fallback_address);
}
