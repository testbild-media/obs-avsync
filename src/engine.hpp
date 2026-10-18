// Measurement engine: matches detections to the nominal event schedule
// (event k happens at UTC k * period), keeps statistics and runs timed sessions.
// Independent of OBS so it can be unit tested.
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace avs {

enum class Kind { Video = 0, Audio = 1 };

struct Measurement {
	int64_t seq = 0;        // event index, event time = seq * period (UTC)
	double latencyMs = 0.0; // detection time - event time
	double detUtcMs = 0.0;
	bool marker = false;
	bool verified = false; // confirmed by a marker event (unambiguous)
	double quality = 0.0;
};

struct Stats {
	int count = 0;
	double last = 0, mean = 0, median = 0, stddev = 0, min = 0, max = 0;
};

Stats computeStats(const std::vector<double> &values);

struct ProbeSignal {
	bool ok = false;      // detector has a usable signal (ROI locked / level ok)
	std::string text;     // short human readable state
	double level = 0.0;   // contrast (video) or dBFS (audio)
};

struct ProbeSnapshot {
	int id = 0;
	Kind kind = Kind::Video;
	std::string name;
	std::string origin; // "filter", "program", "test"
	std::string sourceUuid;
	ProbeSignal signal;
	bool locked = false;
	double lastAgeMs = -1.0; // time since last measurement, -1 = none
	double lastDetectionAgeMs = -1.0; // time since the last detection (also unmatched ones)
	int unmatched = 0;       // recent detections without a matching event (latency > max)
	Stats live;
	Stats total;
	std::vector<Measurement> recent;
};

struct PairStats {
	Stats offset; // audio latency - video latency (positive = audio later)
	std::vector<std::pair<int64_t, double>> recent; // seq, offset
	// true: no shared events (e.g. a played back recording), audio and video detections were
	// paired directly with each other. Only the A/V offset is meaningful then.
	bool relative = false;
};

enum class SessionState { Idle, Scheduled, Running, Finished };

struct SessionResult {
	SessionState state = SessionState::Idle;
	std::string label;
	double periodMs = 1000.0;
	double startUtcMs = 0, endUtcMs = 0;
	int64_t seqBegin = 0, seqEnd = 0; // [begin, end)
	struct ProbeRow {
		int id;
		Kind kind;
		std::string name;
		Stats stats;
		std::map<int64_t, double> latencies;
	};
	std::vector<ProbeRow> probes;
	int pairVideo = -1, pairAudio = -1;
	Stats pairOffset;
};

class Engine {
public:
	using UtcClock = std::function<double()>;

	explicit Engine(UtcClock clock, double periodMs = 1000.0);

	double periodMs() const { return periodMs_; }
	double utcNowMs() const { return clock_(); }

	void setMaxLatencyMs(double v);
	void setLiveWindow(int n);
	double maxLatencyMs() const;

	int addProbe(Kind kind, const std::string &name, const std::string &origin, const std::string &sourceUuid);
	void removeProbe(int id);
	void setProbeName(int id, const std::string &name);
	void setProbeSource(int id, const std::string &sourceUuid);
	void setProbeSignal(int id, const ProbeSignal &sig);

	// Returns the stored measurement (seq < 0 if it was rejected).
	Measurement reportDetection(int id, double detUtcMs, bool marker, double quality);

	std::vector<ProbeSnapshot> snapshot(size_t recentCount) const;
	PairStats pair(int videoId, int audioId, size_t window) const;
	void resetStats();

	// Timed sessions ("geplante Messung")
	bool scheduleSession(double startUtcMs, double durationMs, const std::string &label);
	void cancelSession();
	void setSessionPair(int videoId, int audioId);
	SessionResult session() const;
	// Must be called regularly. Returns true exactly once when a session finished.
	bool tick();

	static std::string sessionCsv(const SessionResult &res, bool germanFormat);

private:
	struct Probe {
		int id;
		Kind kind;
		std::string name, origin, uuid;
		ProbeSignal signal;
		std::map<int64_t, Measurement> results;
		std::vector<double> totals; // all latencies since reset (bounded)
		bool locked = false;
		bool havePrior = false;
		double prior = 0.0;
		int mismatches = 0;
		bool pendingRelock = false;
		double pendingLat = 0.0;
		double lastUtc = 0.0;
		struct Raw {
			double det;
			bool marker;
			bool matched;
		};
		std::deque<Raw> raw; // every detection of the last minute
		double lastDetUtc = 0.0;
	};

	Probe *find(int id);
	const Probe *find(int id) const;
	SessionResult buildSessionResult() const;

	UtcClock clock_;
	double periodMs_;
	double maxLatencyMs_ = 5000.0;
	double minLatencyMs_ = -300.0;
	int liveWindow_ = 20;
	int nextId_ = 1;
	std::vector<Probe> probes_;

	SessionState sessionState_ = SessionState::Idle;
	std::string sessionLabel_;
	double sessionStart_ = 0, sessionEnd_ = 0;
	int pairVideo_ = -1, pairAudio_ = -1;
	SessionResult lastResult_;

	mutable std::mutex mutex_;
};

} // namespace avs
