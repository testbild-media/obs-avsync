// Minimal HTTP + WebSocket server. Serves the phone page (embedded copy of web/)
// in the local network and offers a precise clock sync over WebSocket.
#pragma once

#include "mini-json.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace avs {

struct EmbeddedFile {
	const char *path; // e.g. "/index.html"
	const char *mime;
	const unsigned char *data;
	size_t size;
};

// Implemented in the generated web-assets.cpp
const EmbeddedFile *findEmbeddedFile(const std::string &path);

struct WsClientInfo {
	int id = 0;
	std::string address;
	std::string name;
	std::string userAgent;
	double connectedUtcMs = 0;
	double lastSeenUtcMs = 0;
	json::Object status; // last "status" message of the page
};

class WebServer {
public:
	using Query = std::map<std::string, std::string>;

	struct Hooks {
		std::function<double()> utcNowMs;
		std::function<std::string()> apiStatus; // body for GET /api/status
		// other /api/... requests: return false for 404
		std::function<bool(const std::string &path, const Query &query, std::string &body)> apiCommand;
	};

	WebServer();
	~WebServer();

	bool start(uint16_t port, Hooks hooks, std::string &error, bool loopbackOnly = false);
	void stop();
	bool running() const { return running_; }
	uint16_t port() const { return port_; }

	void broadcast(const std::string &text);
	std::vector<WsClientInfo> clients() const;

	// IPv4 addresses of active network adapters (most likely LAN address first)
	static std::vector<std::string> localIPv4Addresses();

private:
	struct Conn;

	void acceptLoop();
	void handle(std::shared_ptr<Conn> c);
	void serveHttp(Conn &c, const std::string &method, const std::string &path, const std::string &query,
		       const std::string &rangeHeader);
	void webSocketLoop(Conn &c);
	void onText(Conn &c, const std::string &text);
	static bool sendAll(Conn &c, const char *data, size_t len);
	static bool sendFrame(Conn &c, int opcode, const std::string &payload);

	Hooks hooks_;
	uint16_t port_ = 0;
	std::atomic<bool> running_{false};
	uintptr_t listenSock_ = 0;
	std::thread acceptThread_;

	mutable std::mutex connMutex_;
	std::vector<std::shared_ptr<Conn>> conns_;
	int nextId_ = 1;
};

} // namespace avs
