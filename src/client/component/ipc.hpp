#pragma once

#include <string>

namespace ipc
{
	// Queue a single-line JSON message for the launcher (newline appended). Safe from any thread;
	// dropped if the pipe is down (messages are not persisted across reconnects).
	void send_message(std::string line);

	// Push a presence update on the next main-thread frame instead of waiting for the 5s tick.
	void flush_presence();
}
