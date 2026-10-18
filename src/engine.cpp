#include "engine.hpp"
#include "signal-spec.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <sstream>

namespace avs {

namespace {

constexpr size_t kMaxResultsPerProbe = 20000;
constexpr double kRawKeepMs = 60000.0;
constexpr double kPriorAlpha = 0.3;

std::string formatNumber(double v, bool german, int decimals = 2)
{
	char buf[64];
	snprintf(buf, sizeof(buf), "%.*f", decimals, v);
	std::string s = buf;
	if (german)
		std::replace(s.begin(), s.end(), '.', ',');
	return s;
}

std::string formatUtcIso(double utcMs)
{
	double secs = std::floor(utcMs / 1000.0);
	int ms = (int)std::llround(utcMs - secs * 1000.0);
	if (ms >= 1000) {
		ms -= 1000;
		secs += 1.0;
	}
	std::time_t t = (std::time_t)secs;
	std::tm tmv{};
#ifdef _WIN32
	gmtime_s(&tmv, &t);
#else
	gmtime_r(&t, &tmv);
#endif
	char buf[64];
	snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tmv.tm_year + 1900, tmv.tm_mon + 1,
		 tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec, ms);
	return buf;
}

std::string csvEscape(const std::string &s, char sep)
{
	if (s.find(sep) == std::string::npos && s.find('"') == std::string::npos)
		return s;
	std::string out = "\"";
	for (char c : s) {
		if (c == '"')
			out += "\"\"";
		else
			out += c;
	}
	return out + "\"";
}

} // namespace

Stats computeStats(const std::vector<double> &values)
{
	Stats s;
	s.count = (int)values.size();
	if (values.empty())
		return s;
	s.last = values.back();
	double sum = 0.0;
	s.min = s.max = values.front();
	for (double v : values) {
		sum += v;
		s.min = std::min(s.min, v);
		s.max = std::max(s.max, v);
	}
	s.mean = sum / (double)values.size();
	double var = 0.0;
	for (double v : values)
		var += (v - s.mean) * (v - s.mean);
	s.stddev = values.size() > 1 ? std::sqrt(var / (double)(values.size() - 1)) : 0.0;
	std::vector<double> sorted = values;
	std::sort(sorted.begin(), sorted.end());
	size_t n = sorted.size();
	s.median = (n % 2) ? sorted[n / 2] : 0.5 * (sorted[n / 2 - 1] + sorted[n / 2]);
	return s;
}

Engine::Engine(UtcClock clock, double periodMs) : clock_(std::move(clock)), periodMs_(periodMs) {}

void Engine::setMaxLatencyMs(double v)
{
	std::lock_guard<std::mutex> lock(mutex_);
	maxLatencyMs_ = std::max(100.0, v);
}

double Engine::maxLatencyMs() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return maxLatencyMs_;
}

void Engine::setLiveWindow(int n)
{
	std::lock_guard<std::mutex> lock(mutex_);
	liveWindow_ = std::max(1, n);
}

Engine::Probe *Engine::find(int id)
{
	for (auto &p : probes_)
		if (p.id == id)
			return &p;
	return nullptr;
}

const Engine::Probe *Engine::find(int id) const
{
	for (const auto &p : probes_)
		if (p.id == id)
			return &p;
	return nullptr;
}

int Engine::addProbe(Kind kind, const std::string &name, const std::string &origin, const std::string &sourceUuid)
{
	std::lock_guard<std::mutex> lock(mutex_);
	Probe p;
	p.id = nextId_++;
	p.kind = kind;
	p.name = name;
	p.origin = origin;
	p.uuid = sourceUuid;
	probes_.push_back(std::move(p));
	return probes_.back().id;
}

void Engine::removeProbe(int id)
{
	std::lock_guard<std::mutex> lock(mutex_);
	probes_.erase(std::remove_if(probes_.begin(), probes_.end(), [id](const Probe &p) { return p.id == id; }),
		      probes_.end());
}

void Engine::setProbeName(int id, const std::string &name)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (Probe *p = find(id))
		p->name = name;
}

void Engine::setProbeSource(int id, const std::string &sourceUuid)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (Probe *p = find(id))
		p->uuid = sourceUuid;
}

void Engine::setProbeSignal(int id, const ProbeSignal &sig)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (Probe *p = find(id))
		p->signal = sig;
}

Measurement Engine::reportDetection(int id, double det, bool marker, double quality)
{
	std::lock_guard<std::mutex> lock(mutex_);
	Measurement rejected;
	rejected.seq = -1;

	Probe *p = find(id);
	if (!p)
		return rejected;

	const double nowUtc = clock_();
	p->lastDetUtc = nowUtc;
	p->raw.push_back({det, marker, false});
	while (!p->raw.empty() && p->raw.front().det < det - kRawKeepMs)
		p->raw.pop_front();

	const double P = periodMs_;
	const int64_t kMin = (int64_t)std::ceil((det - maxLatencyMs_) / P);
	const int64_t kMax = (int64_t)std::floor((det - minLatencyMs_) / P);

	struct Cand {
		int64_t k;
		double lat;
	};
	std::vector<Cand> all, markers;
	for (int64_t k = kMin; k <= kMax; k++) {
		Cand c{k, det - (double)k * P};
		all.push_back(c);
		if (spec::isMarker(k))
			markers.push_back(c);
	}
	if (all.empty())
		return rejected;

	auto nearest = [](const std::vector<Cand> &v, double target) {
		Cand b = v.front();
		for (const auto &c : v)
			if (std::fabs(c.lat - target) < std::fabs(b.lat - target))
				b = c;
		return b;
	};
	auto smallest = [](const std::vector<Cand> &v) {
		Cand b = v.front();
		for (const auto &c : v)
			if (c.lat < b.lat)
				b = c;
		return b;
	};

	Cand best = all.front();
	bool relock = false;
	if (marker && !markers.empty()) {
		// A marker identifies its event unambiguously (within the marker spacing). If it
		// contradicts the current assumption it may also be a misclassified pulse, so it
		// only takes effect once the next marker confirms it.
		Cand mc = smallest(markers);
		const double ref = p->havePrior ? p->prior : smallest(all).lat;
		if (std::fabs(mc.lat - ref) < P / 2 ||
		    (p->pendingRelock && std::fabs(mc.lat - p->pendingLat) < P / 2)) {
			best = mc;
			relock = true;
			p->pendingRelock = false;
		} else {
			p->pendingRelock = true;
			p->pendingLat = mc.lat;
			best = p->havePrior ? nearest(all, p->prior) : smallest(all);
		}
	} else if (p->havePrior) {
		best = nearest(all, p->prior);
	} else {
		// No reference yet: assume the most recent event (latency < period). Marker events are
		// included on purpose: a marker pulse that was only partly seen looks like a normal one.
		best = smallest(all);
	}

	if (relock) {
		double shift = p->havePrior ? best.lat - p->prior : 0.0;
		p->locked = true;
		p->havePrior = true;
		p->prior = best.lat;
		p->mismatches = 0;
		if (std::fabs(shift) >= P / 2) {
			// recent results were assigned to the wrong event -> move them
			const int64_t since = best.k - 2 * spec::kMarkerEvery;
			std::map<int64_t, Measurement> fixed;
			for (auto &kv : p->results) {
				Measurement m = kv.second;
				int64_t n = (int64_t)std::llround((best.lat - m.latencyMs) / P);
				if (n != 0 && (!m.verified || m.seq >= since)) {
					m.latencyMs += (double)n * P;
					m.seq -= n;
				}
				fixed[m.seq] = m;
			}
			p->results.swap(fixed);
		}
	} else {
		if (p->havePrior && std::fabs(best.lat - p->prior) > 0.4 * P) {
			if (++p->mismatches >= 3) {
				p->locked = false;
				p->havePrior = false;
				p->mismatches = 0;
			}
		} else {
			p->mismatches = 0;
		}
		if (!p->havePrior) {
			p->prior = best.lat;
			p->havePrior = true;
		} else {
			p->prior += kPriorAlpha * (best.lat - p->prior);
		}
	}

	if (p->results.count(best.k))
		return rejected; // second detection for the same event (echo, double flash)
	p->raw.back().matched = true;

	Measurement m;
	m.seq = best.k;
	m.latencyMs = best.lat;
	m.detUtcMs = det;
	m.marker = marker;
	m.verified = p->locked;
	m.quality = quality;
	p->results[m.seq] = m;
	p->lastUtc = clock_();
	while (p->results.size() > kMaxResultsPerProbe)
		p->results.erase(p->results.begin());
	return m;
}

std::vector<ProbeSnapshot> Engine::snapshot(size_t recentCount) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	const double now = clock_();
	std::vector<ProbeSnapshot> out;
	out.reserve(probes_.size());
	for (const auto &p : probes_) {
		ProbeSnapshot s;
		s.id = p.id;
		s.kind = p.kind;
		s.name = p.name;
		s.origin = p.origin;
		s.sourceUuid = p.uuid;
		s.signal = p.signal;
		s.locked = p.locked;
		s.lastAgeMs = p.results.empty() ? -1.0 : now - p.lastUtc;
		s.lastDetectionAgeMs = p.raw.empty() ? -1.0 : now - p.lastDetUtc;
		for (auto it = p.raw.rbegin(); it != p.raw.rend() && it - p.raw.rbegin() < 10; ++it)
			s.unmatched += it->matched ? 0 : 1;

		std::vector<double> all;
		all.reserve(p.results.size());
		for (const auto &kv : p.results)
			all.push_back(kv.second.latencyMs);
		s.total = computeStats(all);
		size_t nLive = std::min(all.size(), (size_t)liveWindow_);
		s.live = computeStats(std::vector<double>(all.end() - (ptrdiff_t)nLive, all.end()));

		size_t nRecent = std::min(p.results.size(), recentCount);
		auto it = p.results.end();
		std::advance(it, -(ptrdiff_t)nRecent);
		for (; it != p.results.end(); ++it)
			s.recent.push_back(it->second);
		out.push_back(std::move(s));
	}
	return out;
}

PairStats Engine::pair(int videoId, int audioId, size_t window) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	PairStats ps;
	const Probe *v = find(videoId);
	const Probe *a = find(audioId);
	if (!v || !a)
		return ps;
	std::vector<double> vals;
	for (auto it = a->results.rbegin(); it != a->results.rend() && ps.recent.size() < window; ++it) {
		auto vit = v->results.find(it->first);
		if (vit == v->results.end())
			continue;
		double off = it->second.latencyMs - vit->second.latencyMs;
		ps.recent.push_back({it->first, off});
	}
	std::reverse(ps.recent.begin(), ps.recent.end());

	if (ps.recent.size() < 3) {
		// No shared events (e.g. a recording played back in OBS): pair every audio detection
		// with the nearest video detection. Unambiguous while |offset| < period / 2.
		std::vector<std::pair<int64_t, double>> rel;
		for (auto it = a->raw.rbegin(); it != a->raw.rend() && rel.size() < window; ++it) {
			const Probe::Raw *bestV = nullptr;
			for (const auto &vr : v->raw)
				if (!bestV || std::fabs(vr.det - it->det) < std::fabs(bestV->det - it->det))
					bestV = &vr;
			if (!bestV || std::fabs(bestV->det - it->det) >= periodMs_ / 2)
				continue;
			rel.push_back({(int64_t)std::llround(it->det / periodMs_), it->det - bestV->det});
		}
		if (rel.size() >= 3) {
			std::reverse(rel.begin(), rel.end());
			ps.recent = std::move(rel);
			ps.relative = true;
		}
	}

	for (const auto &r : ps.recent)
		vals.push_back(r.second);
	ps.offset = computeStats(vals);
	return ps;
}

void Engine::resetStats()
{
	std::lock_guard<std::mutex> lock(mutex_);
	for (auto &p : probes_) {
		p.results.clear();
		p.locked = false;
		p.havePrior = false;
		p.mismatches = 0;
		p.pendingRelock = false;
		p.raw.clear();
	}
}

bool Engine::scheduleSession(double startUtcMs, double durationMs, const std::string &label)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (durationMs < periodMs_)
		return false;
	sessionStart_ = startUtcMs;
	sessionEnd_ = startUtcMs + durationMs;
	sessionLabel_ = label;
	sessionState_ = clock_() >= startUtcMs ? SessionState::Running : SessionState::Scheduled;
	return true;
}

void Engine::cancelSession()
{
	std::lock_guard<std::mutex> lock(mutex_);
	sessionState_ = SessionState::Idle;
}

void Engine::setSessionPair(int videoId, int audioId)
{
	std::lock_guard<std::mutex> lock(mutex_);
	pairVideo_ = videoId;
	pairAudio_ = audioId;
}

SessionResult Engine::buildSessionResult() const
{
	SessionResult r;
	r.state = sessionState_;
	r.label = sessionLabel_;
	r.periodMs = periodMs_;
	r.startUtcMs = sessionStart_;
	r.endUtcMs = sessionEnd_;
	r.seqBegin = (int64_t)std::ceil(sessionStart_ / periodMs_);
	r.seqEnd = (int64_t)std::ceil(sessionEnd_ / periodMs_);
	r.pairVideo = pairVideo_;
	r.pairAudio = pairAudio_;
	for (const auto &p : probes_) {
		SessionResult::ProbeRow row;
		row.id = p.id;
		row.kind = p.kind;
		row.name = p.name;
		std::vector<double> vals;
		for (auto it = p.results.lower_bound(r.seqBegin); it != p.results.end() && it->first < r.seqEnd; ++it) {
			row.latencies[it->first] = it->second.latencyMs;
			vals.push_back(it->second.latencyMs);
		}
		row.stats = computeStats(vals);
		r.probes.push_back(std::move(row));
	}
	const SessionResult::ProbeRow *pv = nullptr, *pa = nullptr;
	for (const auto &row : r.probes) {
		if (row.id == pairVideo_)
			pv = &row;
		if (row.id == pairAudio_)
			pa = &row;
	}
	if (pv && pa) {
		std::vector<double> offs;
		for (const auto &kv : pa->latencies) {
			auto it = pv->latencies.find(kv.first);
			if (it != pv->latencies.end())
				offs.push_back(kv.second - it->second);
		}
		r.pairOffset = computeStats(offs);
	}
	return r;
}

SessionResult Engine::session() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (sessionState_ == SessionState::Finished)
		return lastResult_;
	return buildSessionResult();
}

bool Engine::tick()
{
	std::lock_guard<std::mutex> lock(mutex_);
	const double now = clock_();
	if (sessionState_ == SessionState::Scheduled && now >= sessionStart_)
		sessionState_ = SessionState::Running;
	if (sessionState_ == SessionState::Running && now >= sessionEnd_ + maxLatencyMs_ + 500.0) {
		sessionState_ = SessionState::Finished;
		lastResult_ = buildSessionResult();
		return true;
	}
	return false;
}

std::string Engine::sessionCsv(const SessionResult &res, bool german)
{
	const char sep = german ? ';' : ',';
	std::ostringstream os;
	const SessionResult::ProbeRow *pv = nullptr, *pa = nullptr;
	for (const auto &row : res.probes) {
		if (row.id == res.pairVideo)
			pv = &row;
		if (row.id == res.pairAudio)
			pa = &row;
	}

	os << "# AV-Sync" << sep << csvEscape(res.label, sep) << "\n";
	os << "# Start (UTC)" << sep << formatUtcIso(res.startUtcMs) << sep << "Ende (UTC)" << sep
	   << formatUtcIso(res.endUtcMs) << "\n";
	os << "Ereignis" << sep << "Zeit (UTC)" << sep << "Marker";
	for (const auto &row : res.probes)
		os << sep << csvEscape(std::string(row.kind == Kind::Video ? "Video: " : "Audio: ") + row.name + " [ms]", sep);
	if (pv && pa)
		os << sep << "A/V-Versatz [ms]";
	os << "\n";

	for (int64_t k = res.seqBegin; k < res.seqEnd; k++) {
		os << k << sep << formatUtcIso((double)k * res.periodMs) << sep << (spec::isMarker(k) ? "1" : "0");
		for (const auto &row : res.probes) {
			os << sep;
			auto it = row.latencies.find(k);
			if (it != row.latencies.end())
				os << formatNumber(it->second, german);
		}
		if (pv && pa) {
			os << sep;
			auto iv = pv->latencies.find(k), ia = pa->latencies.find(k);
			if (iv != pv->latencies.end() && ia != pa->latencies.end())
				os << formatNumber(ia->second - iv->second, german);
		}
		os << "\n";
	}

	os << "\n";
	struct Line {
		const char *name;
		double Stats::*field;
	};
	const Line lines[] = {{"Mittelwert", &Stats::mean}, {"Median", &Stats::median},
			      {"Standardabweichung", &Stats::stddev}, {"Minimum", &Stats::min},
			      {"Maximum", &Stats::max}};
	for (const auto &line : lines) {
		os << line.name << sep << sep;
		for (const auto &row : res.probes)
			os << sep << (row.stats.count ? formatNumber(row.stats.*line.field, german) : "");
		if (pv && pa)
			os << sep << (res.pairOffset.count ? formatNumber(res.pairOffset.*line.field, german) : "");
		os << "\n";
	}
	os << "Anzahl" << sep << sep;
	for (const auto &row : res.probes)
		os << sep << row.stats.count;
	if (pv && pa)
		os << sep << res.pairOffset.count;
	os << "\n";
	return os.str();
}

} // namespace avs
