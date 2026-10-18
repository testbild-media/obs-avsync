#include "ntp-clock.hpp"
#include "net-compat.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <map>
#include <random>

namespace avs {

bool netInit()
{
#ifdef _WIN32
	static const bool ok = [] {
		WSADATA data;
		return WSAStartup(MAKEWORD(2, 2), &data) == 0;
	}();
	return ok;
#else
	return true;
#endif
}

void setRecvTimeout(socket_t s, int ms)
{
#ifdef _WIN32
	DWORD tv = (DWORD)ms;
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
#else
	timeval tv;
	tv.tv_sec = ms / 1000;
	tv.tv_usec = (ms % 1000) * 1000;
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

namespace {

constexpr double kNtpUnixDeltaS = 2208988800.0;
constexpr double kSampleMaxAgeMs = 30.0 * 60.0 * 1000.0;
constexpr size_t kMaxSamples = 500;
constexpr double kBucketMs = 15000.0;
constexpr double kMinFitSpanMs = 60000.0;
constexpr double kMaxDrift = 500e-6;
constexpr double kSlewDurationMs = 20000.0;
constexpr double kMaxSlewMs = 100.0;

uint32_t be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

double ntpToUnixMs(const uint8_t *p)
{
	double sec = (double)be32(p);
	double frac = (double)be32(p + 4);
	return (sec - kNtpUnixDeltaS) * 1000.0 + frac * 1000.0 / 4294967296.0;
}

} // namespace

double NtpClock::systemUtcMs()
{
#ifdef _WIN32
	FILETIME ft;
	GetSystemTimePreciseAsFileTime(&ft);
	uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime; // 100 ns since 1601
	return (double)(t - 116444736000000000ULL) / 10000.0;
#else
	timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
#endif
}

NtpClock::NtpClock(MonoNs mono) : mono_(std::move(mono)) {}

NtpClock::~NtpClock()
{
	stop();
}

void NtpClock::start(const std::vector<std::string> &servers)
{
	stop();
	servers_ = servers;
	if (servers_.empty())
		return;
	netInit();
	stop_ = false;
	thread_ = std::thread([this] { run(); });
}

void NtpClock::stop()
{
	stop_ = true;
	waitCv_.notify_all();
	if (thread_.joinable())
		thread_.join();
}

double NtpClock::fitUtcLocked(double monoMs) const
{
	double utc = monoMs + fitA_ + fitB_ * (monoMs - fitRef_);
	if (slewDeltaMs_ != 0.0) {
		const double f = monoMs <= slewStartMs_ ? 1.0 : std::max(0.0, 1.0 - (monoMs - slewStartMs_) / kSlewDurationMs);
		utc += slewDeltaMs_ * f;
	}
	return utc;
}

double NtpClock::utcMsAt(uint64_t monoNs) const
{
	const double monoMs = (double)monoNs / 1e6;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (haveFit_)
			return fitUtcLocked(monoMs);
	}
	// fallback: operating system clock
	const double sysNow = systemUtcMs();
	const double monoNow = (double)mono_() / 1e6;
	return monoMs + (sysNow - monoNow);
}

ClockStatus NtpClock::status() const
{
	ClockStatus st;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		st = status_;
	}
	const double sysNow = systemUtcMs();
	st.systemOffsetMs = sysNow - utcMsAt(mono_());
	return st;
}

void NtpClock::run()
{
	int n = 0;
	size_t server = 0;
	while (!stop_) {
		std::string err;
		if (!query((int)server, err)) {
			std::lock_guard<std::mutex> lock(mutex_);
			status_.lastError = servers_[server] + ": " + err;
		}
		server = (server + 1) % servers_.size();
		n++;
		int waitMs = n < 8 ? 300 : n < 40 ? 8000 : 32000;
		std::unique_lock<std::mutex> lock(waitMutex_);
		waitCv_.wait_for(lock, std::chrono::milliseconds(waitMs), [this] { return stop_.load(); });
	}
}

bool NtpClock::query(int serverIndex, std::string &err)
{
	addrinfo hints{};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_protocol = IPPROTO_UDP;
	addrinfo *res = nullptr;
	if (getaddrinfo(servers_[serverIndex].c_str(), "123", &hints, &res) != 0 || !res) {
		err = "DNS-Aufloesung fehlgeschlagen";
		return false;
	}

	static thread_local std::mt19937_64 rng{std::random_device{}()};
	bool ok = false;
	for (addrinfo *ai = res; ai && !ok && !stop_; ai = ai->ai_next) {
		socket_t s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (s == kInvalidSocket)
			continue;
		setRecvTimeout(s, 1000);

		uint8_t pkt[48] = {};
		pkt[0] = 0x23; // LI 0, version 4, mode 3 (client)
		uint64_t cookie = rng();
		for (int i = 0; i < 8; i++)
			pkt[40 + i] = (uint8_t)(cookie >> (56 - 8 * i));

		const uint64_t t0 = mono_();
		int sent = sendto(s, (const char *)pkt, sizeof(pkt), 0, ai->ai_addr, (int)ai->ai_addrlen);
		uint8_t resp[128];
		int got = sent == (int)sizeof(pkt) ? recv(s, (char *)resp, sizeof(resp), 0) : -1;
		const uint64_t t3 = mono_();
		closeSocket(s);

		if (got < 48) {
			err = "keine Antwort";
			continue;
		}
		const int li = resp[0] >> 6, mode = resp[0] & 7, stratum = resp[1];
		if (mode != 4 || stratum < 1 || stratum > 15 || li == 3 || std::memcmp(resp + 24, pkt + 40, 8) != 0) {
			err = "ungueltige Antwort";
			continue;
		}
		const double T2 = ntpToUnixMs(resp + 32);
		const double T3 = ntpToUnixMs(resp + 40);
		const double t0ms = (double)t0 / 1e6, t3ms = (double)t3 / 1e6;
		double delay = (t3ms - t0ms) - (T3 - T2);
		if (delay < 0)
			delay = 0;
		Sample smp;
		smp.monoMs = (t0ms + t3ms) / 2.0;
		smp.offsetMs = (T2 + T3) / 2.0 - smp.monoMs;
		smp.delayMs = delay;
		smp.server = serverIndex;

		std::lock_guard<std::mutex> lock(mutex_);
		samples_.push_back(smp);
		refit();
		ok = true;
	}
	freeaddrinfo(res);
	return ok;
}

void NtpClock::refit()
{
	if (samples_.empty())
		return;
	const double newest = samples_.back().monoMs;
	samples_.erase(std::remove_if(samples_.begin(), samples_.end(),
				      [&](const Sample &s) { return newest - s.monoMs > kSampleMaxAgeMs; }),
		       samples_.end());
	if (samples_.size() > kMaxSamples)
		samples_.erase(samples_.begin(), samples_.begin() + (ptrdiff_t)(samples_.size() - kMaxSamples));

	double minDelay = 1e9;
	for (const auto &s : samples_)
		minDelay = std::min(minDelay, s.delayMs);

	std::map<int64_t, Sample> buckets;
	for (const auto &s : samples_) {
		int64_t b = (int64_t)std::floor(s.monoMs / kBucketMs);
		auto it = buckets.find(b);
		if (it == buckets.end() || s.delayMs < it->second.delayMs)
			buckets[b] = s;
	}
	std::vector<Sample> pts;
	for (const auto &kv : buckets)
		if (kv.second.delayMs <= minDelay * 1.5 + 2.0)
			pts.push_back(kv.second);

	const double ref = newest;
	double a = 0.0, b = 0.0;
	if (pts.size() >= 4 && pts.back().monoMs - pts.front().monoMs >= kMinFitSpanMs) {
		double sw = 0, st = 0, so = 0, stt = 0, sto = 0;
		for (const auto &p : pts) {
			double w = 1.0 / (p.delayMs + 1.0);
			double dt = p.monoMs - ref;
			sw += w;
			st += w * dt;
			so += w * p.offsetMs;
			stt += w * dt * dt;
			sto += w * dt * p.offsetMs;
		}
		double den = sw * stt - st * st;
		b = den > 0 ? (sw * sto - st * so) / den : 0.0;
		b = std::clamp(b, -kMaxDrift, kMaxDrift);
		a = (so - b * st) / sw;
	} else {
		double sum = 0;
		int n = 0;
		for (const auto &s : samples_) {
			if (s.delayMs <= minDelay + 1.0) {
				sum += s.offsetMs;
				n++;
			}
		}
		a = sum / std::max(1, n);
	}

	double res = 0;
	for (const auto &p : pts) {
		double e = p.offsetMs - (a + b * (p.monoMs - ref));
		res += e * e;
	}
	double rms = pts.empty() ? 0.0 : std::sqrt(res / (double)pts.size());

	const double nowMono = (double)mono_() / 1e6;
	const double before = haveFit_ ? fitUtcLocked(nowMono) : 0.0;
	const bool hadFit = haveFit_;
	haveFit_ = true;
	fitA_ = a;
	fitB_ = b;
	fitRef_ = ref;
	slewDeltaMs_ = 0.0;
	if (hadFit) {
		const double delta = before - fitUtcLocked(nowMono);
		if (std::fabs(delta) < kMaxSlewMs) {
			slewStartMs_ = nowMono;
			slewDeltaMs_ = delta;
		}
	}

	const Sample *best = &samples_.front();
	for (const auto &s : samples_)
		if (s.delayMs < best->delayMs)
			best = &s;
	status_.ntpSynced = true;
	status_.server = servers_[(size_t)best->server];
	status_.rttMs = minDelay;
	status_.errorMs = minDelay / 2.0 + rms;
	status_.driftPpm = b * 1e6;
	status_.samples = (int)samples_.size();
	status_.lastError.clear();
}

} // namespace avs
