// Global plugin state: reference clock, measurement engine, web server, program probes, config.
#pragma once

#include "engine.hpp"
#include "ntp-clock.hpp"
#include "web-server.hpp"

#include <obs-module.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace avs {

struct Config {
	int port = 8765;
	bool serverEnabled = true;
	bool ntpEnabled = true;
	std::string ntpServers = "time.cloudflare.com, time.google.com, pool.ntp.org";
	bool programVideo = true;
	bool programAudio = true;
	int programTrack = 1; // 1..6
	double maxLatencyMs = 3000.0;
	int liveWindow = 20;
	std::string csvFolder;
	bool csvGerman = true;
	std::string publicPageUrl = "https://testbild-media.github.io/obs-avsync/";
	int sessionDurationS = 30;
	bool overlayHint = true;
};

class ProgramProbes;

class Plugin {
public:
	static Plugin &instance();

	void load();
	void unload();
	void startProgramProbes();

	Engine &engine() { return *engine_; }
	NtpClock &clock() { return *clock_; }
	WebServer &server() { return server_; }

	double utcMs(uint64_t obsNs) const { return clock_->utcMsAt(obsNs); }
	double nowUtcMs() const { return clock_->nowUtcMs(); }

	Config config() const;
	void applyConfig(const Config &c);

	std::string serverError() const;
	std::vector<std::string> phoneUrls() const;

	// A/V pair used for the offset display, the phone feedback and sessions.
	void setPair(int videoId, int audioId);
	void resolvedPair(int &videoId, int &audioId) const;

	std::string statusJson();
	bool apiCommand(const std::string &path, const WebServer::Query &query, std::string &body);
	// start a measurement session; startUtcMs <= 0 means: at the next full second
	bool startSession(double startUtcMs, double durationMs, const std::string &label);
	std::string lastCsvPath() const;
	std::string lastCsvError() const;

	// tick callback (graphics thread, once per frame)
	void onTick();

private:
	Plugin() = default;
	void loadConfig();
	void saveConfig() const;
	void restartServer();
	void restartNtp();
	void writeSessionCsv(const SessionResult &res);

	std::unique_ptr<NtpClock> clock_;
	std::unique_ptr<Engine> engine_;
	WebServer server_;
	std::unique_ptr<ProgramProbes> program_;

	mutable std::mutex mutex_;
	Config config_;
	std::string serverError_;
	int pairVideo_ = -1, pairAudio_ = -1;
	std::string lastCsvPath_, lastCsvError_;
	uint64_t lastBroadcastNs_ = 0;
	bool lastNtpSynced_ = false;
	bool loaded_ = false;
};

// Registration helpers (implemented in the respective files)
void registerVideoSensor();
void registerAudioSensor();
void registerTestSource();
void createDock();
void destroyDock();

} // namespace avs
