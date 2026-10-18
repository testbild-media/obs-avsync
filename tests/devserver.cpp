// Stand-alone test server: NTP clock + phone page + WebSocket clock sync, without OBS.
// Usage: avsync-devserver [port] [seconds]
#include "../src/mini-json.hpp"
#include "../src/ntp-clock.hpp"
#include "../src/web-server.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <thread>

using namespace avs;

static uint64_t monoNs()
{
	return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
		       std::chrono::steady_clock::now().time_since_epoch())
		.count();
}

int main(int argc, char **argv)
{
	uint16_t port = argc > 1 ? (uint16_t)atoi(argv[1]) : 8765;
	int seconds = argc > 2 ? atoi(argv[2]) : 0;
	bool loopback = argc > 3 && std::string(argv[3]) == "local";

	NtpClock clock(monoNs);
	clock.start({"time.cloudflare.com", "time.google.com", "pool.ntp.org"});

	WebServer server;
	WebServer::Hooks hooks;
	hooks.utcNowMs = [&] { return clock.nowUtcMs(); };
	hooks.apiStatus = [&] {
		ClockStatus st = clock.status();
		std::ostringstream os;
		os.precision(15);
		os << "{\"clock\":{\"ntp\":" << (st.ntpSynced ? "true" : "false") << ",\"server\":\""
		   << json::escape(st.server) << "\",\"errorMs\":" << st.errorMs << ",\"rttMs\":" << st.rttMs
		   << ",\"systemOffsetMs\":" << st.systemOffsetMs << ",\"utcMs\":" << clock.nowUtcMs() << "},\"clients\":[";
		bool first = true;
		for (const auto &c : server.clients()) {
			os << (first ? "" : ",") << "{\"name\":\"" << json::escape(c.name) << "\",\"address\":\""
			   << c.address << "\",\"syncErrMs\":" << json::number(c.status, "syncErrMs", -1)
			   << ",\"refreshHz\":" << json::number(c.status, "refreshHz", 0) << "}";
			first = false;
		}
		os << "]}";
		return os.str();
	};
	std::string err;
	if (!server.start(port, hooks, err, loopback)) {
		fprintf(stderr, "server: %s\n", err.c_str());
		return 1;
	}
	printf("listening on port %u, addresses:", port);
	for (const auto &ip : WebServer::localIPv4Addresses())
		printf(" http://%s:%u/", ip.c_str(), port);
	printf("\n");
	fflush(stdout);

	auto start = std::chrono::steady_clock::now();
	while (seconds <= 0 || std::chrono::steady_clock::now() - start < std::chrono::seconds(seconds)) {
		std::this_thread::sleep_for(std::chrono::seconds(2));
		ClockStatus st = clock.status();
		printf("ntp=%d server=%s err=%.2fms rtt=%.2fms sysOffset=%+.1fms samples=%d clients=%zu %s\n",
		       st.ntpSynced, st.server.c_str(), st.errorMs, st.rttMs, st.systemOffsetMs, st.samples,
		       server.clients().size(), st.lastError.c_str());
		for (const auto &c : server.clients())
			printf("   client %s (%s) syncErr=%.2f rtt=%.2f hz=%.1f\n", c.name.c_str(), c.address.c_str(),
			       json::number(c.status, "syncErrMs", -1), json::number(c.status, "rttMs", -1),
			       json::number(c.status, "refreshHz", 0));
		char res[160];
		snprintf(res, sizeof(res), "{\"type\":\"results\",\"video\":%.1f,\"audio\":%.1f,\"offset\":%.1f}", 101.5,
			 22.0, -79.5);
		server.broadcast(res);
		fflush(stdout);
	}
	server.stop();
	clock.stop();
	return 0;
}
