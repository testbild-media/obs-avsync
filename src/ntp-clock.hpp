// Reference clock: maps the monotonic OBS clock to real UTC time using SNTP.
// Falls back to the system clock when no NTP server is reachable.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace avs {

struct ClockStatus {
	bool ntpSynced = false;
	std::string server;      // server of the best recent sample
	double errorMs = 0.0;    // estimated uncertainty (half round trip + fit residual)
	double rttMs = 0.0;      // best round trip
	double systemOffsetMs = 0.0; // system clock - reference UTC (positive: system clock ahead)
	double driftPpm = 0.0;
	int samples = 0;
	std::string lastError;
};

class NtpClock {
public:
	using MonoNs = std::function<uint64_t()>;

	explicit NtpClock(MonoNs mono);
	~NtpClock();

	void start(const std::vector<std::string> &servers);
	void stop();

	// UTC milliseconds (double) for a monotonic timestamp
	double utcMsAt(uint64_t monoNs) const;
	double nowUtcMs() const { return utcMsAt(mono_()); }
	uint64_t monoNow() const { return mono_(); }

	ClockStatus status() const;

	// Wall clock of the OS in UTC milliseconds.
	static double systemUtcMs();

private:
	struct Sample {
		double monoMs;
		double offsetMs; // utc - mono
		double delayMs;
		int server;
	};

	void run();
	bool query(int serverIndex, std::string &err);
	void refit();

	MonoNs mono_;
	std::vector<std::string> servers_;
	std::thread thread_;
	std::atomic<bool> stop_{false};
	std::mutex waitMutex_;
	std::condition_variable waitCv_;

	mutable std::mutex mutex_;
	std::vector<Sample> samples_;
	bool haveFit_ = false;
	double fitA_ = 0, fitB_ = 0, fitRef_ = 0;
	// small corrections are slewed in (no jumps the phone page would have to follow)
	double slewStartMs_ = 0.0, slewDeltaMs_ = 0.0;

	double fitUtcLocked(double monoMs) const;
	ClockStatus status_;
};

} // namespace avs
