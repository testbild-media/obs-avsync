#include "plugin.hpp"
#include "program-probes.hpp"

#include <obs-frontend-api.h>
#include <util/platform.h>

#include <QDateTime>
#include <QDir>
#include <QTime>
#include <QRegularExpression>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace avs {

namespace {

std::vector<std::string> splitServers(const std::string &s)
{
	std::vector<std::string> out;
	std::string cur;
	for (char c : s) {
		if (c == ',' || c == ';' || c == ' ' || c == '\n') {
			if (!cur.empty())
				out.push_back(cur);
			cur.clear();
		} else {
			cur += c;
		}
	}
	if (!cur.empty())
		out.push_back(cur);
	return out;
}

std::string num(double v, int decimals = 3)
{
	if (!std::isfinite(v))
		return "null";
	char buf[64];
	snprintf(buf, sizeof(buf), "%.*f", decimals, v);
	return buf;
}

void tickCallback(void *param, float)
{
	static_cast<Plugin *>(param)->onTick();
}

} // namespace

Plugin &Plugin::instance()
{
	static Plugin plugin;
	return plugin;
}

void Plugin::load()
{
	clock_ = std::make_unique<NtpClock>([] { return os_gettime_ns(); });
	engine_ = std::make_unique<Engine>([this] { return clock_->nowUtcMs(); }, 1000.0);
	loadConfig();
	{
		std::lock_guard<std::mutex> lock(mutex_);
		engine_->setMaxLatencyMs(config_.maxLatencyMs);
		engine_->setLiveWindow(config_.liveWindow);
	}
	restartNtp();
	restartServer();
	program_ = std::make_unique<ProgramProbes>(*engine_, *clock_);
	obs_add_tick_callback(tickCallback, this);
	loaded_ = true;
}

void Plugin::unload()
{
	if (!loaded_)
		return;
	loaded_ = false;
	obs_remove_tick_callback(tickCallback, this);
	if (program_)
		program_->stop();
	server_.stop();
	if (clock_)
		clock_->stop();
	// engine_ and clock_ stay alive: libobs destroys sources (and our filters) after module unload.
}

void Plugin::startProgramProbes()
{
	Config c = config();
	if (program_)
		program_->configure(c.programVideo, c.programAudio, c.programTrack);
}

Config Plugin::config() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return config_;
}

void Plugin::applyConfig(const Config &c)
{
	Config old;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		old = config_;
		config_ = c;
	}
	saveConfig();
	engine_->setMaxLatencyMs(c.maxLatencyMs);
	engine_->setLiveWindow(c.liveWindow);
	if (old.port != c.port || old.serverEnabled != c.serverEnabled)
		restartServer();
	if (old.ntpEnabled != c.ntpEnabled || old.ntpServers != c.ntpServers)
		restartNtp();
	if (program_ && (old.programVideo != c.programVideo || old.programAudio != c.programAudio ||
			 old.programTrack != c.programTrack))
		program_->configure(c.programVideo, c.programAudio, c.programTrack);
}

std::string Plugin::serverError() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return serverError_;
}

std::vector<std::string> Plugin::phoneUrls() const
{
	std::vector<std::string> out;
	if (!server_.running())
		return out;
	for (const auto &ip : WebServer::localIPv4Addresses())
		out.push_back("http://" + ip + ":" + std::to_string(server_.port()) + "/");
	return out;
}

void Plugin::setPair(int videoId, int audioId)
{
	std::lock_guard<std::mutex> lock(mutex_);
	pairVideo_ = videoId;
	pairAudio_ = audioId;
}

void Plugin::resolvedPair(int &videoId, int &audioId) const
{
	int v, a;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		v = pairVideo_;
		a = pairAudio_;
	}
	auto snap = engine_->snapshot(0);
	auto exists = [&](int id, Kind kind) {
		return std::any_of(snap.begin(), snap.end(), [&](const ProbeSnapshot &p) { return p.id == id && p.kind == kind; });
	};
	auto fresh = [](const ProbeSnapshot &p) { return p.lastAgeMs >= 0 && p.lastAgeMs < 5000; };
	auto pick = [&](Kind kind, const std::string &preferUuid) {
		// 1. same source as the other half of the pair, 2. source filters, 3. program
		// - each time probes with current measurements first
		for (bool needFresh : {true, false}) {
			if (!preferUuid.empty())
				for (const auto &p : snap)
					if (p.kind == kind && p.sourceUuid == preferUuid && (!needFresh || fresh(p)))
						return p.id;
			for (const char *origin : {"filter", "program"})
				for (const auto &p : snap)
					if (p.kind == kind && p.origin == origin && (!needFresh || fresh(p)))
						return p.id;
		}
		return -1;
	};
	videoId = exists(v, Kind::Video) ? v : pick(Kind::Video, "");
	std::string videoUuid;
	for (const auto &p : snap)
		if (p.id == videoId)
			videoUuid = p.sourceUuid;
	audioId = exists(a, Kind::Audio) ? a : pick(Kind::Audio, videoUuid);
}

std::string Plugin::lastCsvPath() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return lastCsvPath_;
}

std::string Plugin::lastCsvError() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return lastCsvError_;
}

void Plugin::restartServer()
{
	server_.stop();
	Config c = config();
	std::string err;
	if (c.serverEnabled) {
		WebServer::Hooks hooks;
		hooks.utcNowMs = [this] { return clock_->nowUtcMs(); };
		hooks.apiStatus = [this] { return statusJson(); };
		hooks.apiCommand = [this](const std::string &path, const WebServer::Query &q, std::string &body) {
			return apiCommand(path, q, body);
		};
		if (!server_.start((uint16_t)c.port, hooks, err))
			blog(LOG_WARNING, "[obs-avsync] web server: %s", err.c_str());
		else
			blog(LOG_INFO, "[obs-avsync] phone page on port %d", c.port);
	}
	std::lock_guard<std::mutex> lock(mutex_);
	serverError_ = err;
}

void Plugin::restartNtp()
{
	Config c = config();
	clock_->stop();
	if (c.ntpEnabled)
		clock_->start(splitServers(c.ntpServers));
}

void Plugin::loadConfig()
{
	char *path = obs_module_config_path("config.json");
	obs_data_t *d = path ? obs_data_create_from_json_file_safe(path, "bak") : nullptr;
	bfree(path);
	Config c;
	c.csvFolder = QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
			      .filePath("AV-Sync")
			      .toStdString();
	if (d) {
		auto getInt = [&](const char *k, int def) { return obs_data_has_user_value(d, k) ? (int)obs_data_get_int(d, k) : def; };
		auto getBool = [&](const char *k, bool def) { return obs_data_has_user_value(d, k) ? obs_data_get_bool(d, k) : def; };
		auto getDouble = [&](const char *k, double def) { return obs_data_has_user_value(d, k) ? obs_data_get_double(d, k) : def; };
		auto getString = [&](const char *k, const std::string &def) {
			return obs_data_has_user_value(d, k) ? std::string(obs_data_get_string(d, k)) : def;
		};
		c.port = getInt("port", c.port);
		c.serverEnabled = getBool("server_enabled", c.serverEnabled);
		c.ntpEnabled = getBool("ntp_enabled", c.ntpEnabled);
		c.ntpServers = getString("ntp_servers", c.ntpServers);
		c.programVideo = getBool("program_video", c.programVideo);
		c.programAudio = getBool("program_audio", c.programAudio);
		c.programTrack = getInt("program_track", c.programTrack);
		c.maxLatencyMs = getDouble("max_latency_ms", c.maxLatencyMs);
		c.liveWindow = getInt("live_window", c.liveWindow);
		c.csvFolder = getString("csv_folder", c.csvFolder);
		c.csvGerman = getBool("csv_german", c.csvGerman);
		c.publicPageUrl = getString("public_page_url", c.publicPageUrl);
		c.sessionDurationS = getInt("session_duration_s", c.sessionDurationS);
		obs_data_release(d);
	}
	std::lock_guard<std::mutex> lock(mutex_);
	config_ = c;
}

void Plugin::saveConfig() const
{
	Config c = config();
	char *dir = obs_module_config_path("");
	if (dir)
		os_mkdirs(dir);
	bfree(dir);
	char *path = obs_module_config_path("config.json");
	if (!path)
		return;
	obs_data_t *d = obs_data_create();
	obs_data_set_int(d, "port", c.port);
	obs_data_set_bool(d, "server_enabled", c.serverEnabled);
	obs_data_set_bool(d, "ntp_enabled", c.ntpEnabled);
	obs_data_set_string(d, "ntp_servers", c.ntpServers.c_str());
	obs_data_set_bool(d, "program_video", c.programVideo);
	obs_data_set_bool(d, "program_audio", c.programAudio);
	obs_data_set_int(d, "program_track", c.programTrack);
	obs_data_set_double(d, "max_latency_ms", c.maxLatencyMs);
	obs_data_set_int(d, "live_window", c.liveWindow);
	obs_data_set_string(d, "csv_folder", c.csvFolder.c_str());
	obs_data_set_bool(d, "csv_german", c.csvGerman);
	obs_data_set_string(d, "public_page_url", c.publicPageUrl.c_str());
	obs_data_set_int(d, "session_duration_s", c.sessionDurationS);
	obs_data_save_json_safe(d, path, "tmp", "bak");
	obs_data_release(d);
	bfree(path);
}

void Plugin::writeSessionCsv(const SessionResult &res)
{
	Config c = config();
	std::string err;
	std::string path;
	QDir dir(QString::fromStdString(c.csvFolder));
	if (!dir.exists() && !dir.mkpath(".")) {
		err = "Ordner kann nicht angelegt werden: " + c.csvFolder;
	} else {
		QString label = QString::fromStdString(res.label).trimmed();
		label.replace(QRegularExpression("[^A-Za-z0-9_\\-]+"), "_");
		QString name = "AV-Sync_" + QDateTime::fromMSecsSinceEpoch((qint64)res.startUtcMs).toString("yyyy-MM-dd_HH-mm-ss");
		if (!label.isEmpty())
			name += "_" + label;
		path = dir.filePath(name + ".csv").toStdString();
		std::ofstream f(std::filesystem::u8path(path), std::ios::binary);
		if (!f) {
			err = "Datei kann nicht geschrieben werden: " + path;
		} else {
			f << "\xEF\xBB\xBF" << Engine::sessionCsv(res, c.csvGerman);
		}
	}
	if (err.empty())
		blog(LOG_INFO, "[obs-avsync] session saved: %s", path.c_str());
	else
		blog(LOG_WARNING, "[obs-avsync] %s", err.c_str());
	std::lock_guard<std::mutex> lock(mutex_);
	lastCsvPath_ = err.empty() ? path : "";
	lastCsvError_ = err;
}

void Plugin::onTick()
{
	if (engine_->tick())
		writeSessionCsv(engine_->session());

	// The first NTP sync moves the reference clock (often by hundreds of ms on PCs whose
	// clock is not synchronised). Results measured before that are not comparable.
	const bool synced = clock_->status().ntpSynced;
	if (synced && !lastNtpSynced_) {
		blog(LOG_INFO, "[obs-avsync] reference clock synchronised via NTP, resetting statistics");
		engine_->resetStats();
	}
	lastNtpSynced_ = synced;

	const uint64_t now = os_gettime_ns();
	if (now - lastBroadcastNs_ < 1000000000ULL || !server_.running())
		return;
	lastBroadcastNs_ = now;
	if (server_.clients().empty())
		return;

	int v, a;
	resolvedPair(v, a);
	auto snap = engine_->snapshot(0);
	double vLat = NAN, aLat = NAN;
	std::string vName, aName;
	for (const auto &p : snap) {
		const bool fresh = p.lastAgeMs >= 0 && p.lastAgeMs < 5000;
		if (p.id == v && p.live.count && fresh) {
			vLat = p.live.median;
			vName = p.name;
		}
		if (p.id == a && p.live.count && fresh) {
			aLat = p.live.median;
			aName = p.name;
		}
	}
	PairStats ps = engine_->pair(v, a, 10);
	double off = (ps.offset.count && std::isfinite(vLat) && std::isfinite(aLat)) ? ps.offset.median : NAN;
	std::string msg = "{\"type\":\"results\",\"video\":" + num(vLat, 1) + ",\"audio\":" + num(aLat, 1) +
			  ",\"offset\":" + num(off, 1) + ",\"videoName\":\"" + json::escape(vName) +
			  "\",\"audioName\":\"" + json::escape(aName) + "\"}";
	server_.broadcast(msg);
}

bool Plugin::startSession(double startUtcMs, double durationMs, const std::string &label)
{
	if (startUtcMs <= 0.0)
		startUtcMs = std::ceil(nowUtcMs() / 1000.0) * 1000.0 + 1000.0;
	int v, a;
	resolvedPair(v, a);
	engine_->setSessionPair(v, a);
	return engine_->scheduleSession(startUtcMs, durationMs, label);
}

bool Plugin::apiCommand(const std::string &path, const WebServer::Query &q, std::string &body)
{
	auto get = [&](const char *k, const std::string &def = "") {
		auto it = q.find(k);
		return it == q.end() ? def : it->second;
	};
	if (path == "/api/reset") {
		engine_->resetStats();
		body = "{\"ok\":true}";
		return true;
	}
	if (path == "/api/session/cancel") {
		engine_->cancelSession();
		body = "{\"ok\":true}";
		return true;
	}
	if (path == "/api/session/start") {
		const double seconds = std::atof(get("seconds", std::to_string(config().sessionDurationS)).c_str());
		double start = 0.0;
		const std::string at = get("at");
		if (!at.empty()) {
			QTime t = QTime::fromString(QString::fromStdString(at), "H:mm:ss");
			if (!t.isValid())
				t = QTime::fromString(QString::fromStdString(at), "H:mm");
			if (!t.isValid()) {
				body = "{\"ok\":false,\"error\":\"at=HH:MM[:SS] erwartet\"}";
				return true;
			}
			const double now = nowUtcMs();
			QDateTime dt(QDateTime::fromMSecsSinceEpoch((qint64)now).date(), t);
			if (dt.toMSecsSinceEpoch() <= (qint64)now)
				dt = dt.addDays(1);
			start = (double)dt.toMSecsSinceEpoch();
		}
		const bool ok = startSession(start, seconds * 1000.0, get("label"));
		SessionResult r = engine_->session();
		body = "{\"ok\":" + std::string(ok ? "true" : "false") + ",\"startUtcMs\":" + num(r.startUtcMs) +
		       ",\"endUtcMs\":" + num(r.endUtcMs) + "}";
		return true;
	}
	if (path == "/api/session") {
		SessionResult r = engine_->session();
		std::ostringstream os;
		os << "{\"state\":" << (int)r.state << ",\"label\":\"" << json::escape(r.label)
		   << "\",\"startUtcMs\":" << num(r.startUtcMs) << ",\"endUtcMs\":" << num(r.endUtcMs) << ",\"probes\":[";
		for (size_t i = 0; i < r.probes.size(); i++) {
			const auto &p = r.probes[i];
			os << (i ? "," : "") << "{\"name\":\"" << json::escape(p.name) << "\",\"kind\":\""
			   << (p.kind == Kind::Video ? "video" : "audio") << "\",\"count\":" << p.stats.count
			   << ",\"mean\":" << num(p.stats.mean) << ",\"median\":" << num(p.stats.median)
			   << ",\"stddev\":" << num(p.stats.stddev) << "}";
		}
		os << "],\"offset\":{\"count\":" << r.pairOffset.count << ",\"median\":" << num(r.pairOffset.median)
		   << "},\"csv\":\"" << json::escape(lastCsvPath()) << "\"}";
		body = os.str();
		return true;
	}
	return false;
}

std::string Plugin::statusJson()
{
	std::ostringstream os;
	ClockStatus cs = clock_->status();
	os << "{\"version\":\"1.0.0\",\"utcMs\":" << num(clock_->nowUtcMs()) << ",\"clock\":{\"ntp\":"
	   << (cs.ntpSynced ? "true" : "false") << ",\"server\":\"" << json::escape(cs.server)
	   << "\",\"errorMs\":" << num(cs.errorMs) << ",\"rttMs\":" << num(cs.rttMs)
	   << ",\"systemOffsetMs\":" << num(cs.systemOffsetMs) << ",\"driftPpm\":" << num(cs.driftPpm)
	   << ",\"samples\":" << cs.samples << "},\"probes\":[";
	auto snap = engine_->snapshot(30);
	bool first = true;
	auto stats = [&](const Stats &s) {
		std::ostringstream st;
		st << "{\"count\":" << s.count << ",\"last\":" << num(s.last) << ",\"mean\":" << num(s.mean)
		   << ",\"median\":" << num(s.median) << ",\"stddev\":" << num(s.stddev) << ",\"min\":" << num(s.min)
		   << ",\"max\":" << num(s.max) << "}";
		return st.str();
	};
	for (const auto &p : snap) {
		os << (first ? "" : ",") << "{\"id\":" << p.id << ",\"kind\":\""
		   << (p.kind == Kind::Video ? "video" : "audio") << "\",\"name\":\"" << json::escape(p.name)
		   << "\",\"origin\":\"" << p.origin << "\",\"locked\":" << (p.locked ? "true" : "false")
		   << ",\"signal\":{\"ok\":" << (p.signal.ok ? "true" : "false") << ",\"text\":\""
		   << json::escape(p.signal.text) << "\",\"level\":" << num(p.signal.level, 1)
		   << "},\"lastAgeMs\":" << num(p.lastAgeMs, 0) << ",\"live\":" << stats(p.live)
		   << ",\"total\":" << stats(p.total) << ",\"recent\":[";
		for (size_t i = 0; i < p.recent.size(); i++) {
			const auto &m = p.recent[i];
			os << (i ? "," : "") << "{\"seq\":" << m.seq << ",\"latencyMs\":" << num(m.latencyMs)
			   << ",\"marker\":" << (m.marker ? "true" : "false")
			   << ",\"verified\":" << (m.verified ? "true" : "false") << "}";
		}
		os << "]}";
		first = false;
	}
	int v, a;
	resolvedPair(v, a);
	PairStats ps = engine_->pair(v, a, 20);
	os << "],\"pair\":{\"video\":" << v << ",\"audio\":" << a << ",\"offset\":" << stats(ps.offset) << "}";
	os << ",\"phones\":[";
	first = true;
	for (const auto &c : server_.clients()) {
		os << (first ? "" : ",") << "{\"name\":\"" << json::escape(c.name) << "\",\"address\":\"" << c.address
		   << "\",\"syncErrMs\":" << num(json::number(c.status, "syncErrMs", NAN))
		   << ",\"refreshHz\":" << num(json::number(c.status, "refreshHz", NAN), 1)
		   << ",\"running\":" << (json::boolean(c.status, "running") ? "true" : "false") << "}";
		first = false;
	}
	SessionResult sr = engine_->session();
	os << "],\"session\":{\"state\":" << (int)sr.state << ",\"startUtcMs\":" << num(sr.startUtcMs)
	   << ",\"endUtcMs\":" << num(sr.endUtcMs) << ",\"csv\":\"" << json::escape(lastCsvPath()) << "\"}}";
	return os.str();
}

} // namespace avs

// ---------------------------------------------------------------- module entry points

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-avsync", "en-US")

MODULE_EXPORT const char *obs_module_name(void)
{
	return "AV-Sync Latenzmessung";
}

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Misst Audio- und Video-Latenz gegen echte UTC-Zeit (Blitz + Chirp vom Handy).";
}

bool obs_module_load(void)
{
	avs::Plugin::instance().load();
	avs::registerVideoSensor();
	avs::registerAudioSensor();
	avs::registerTestSource();
	blog(LOG_INFO, "[obs-avsync] loaded (version 1.0.0)");
	return true;
}

void obs_module_post_load(void)
{
	avs::Plugin::instance().startProgramProbes();
	avs::createDock();
}

void obs_module_unload(void)
{
	avs::destroyDock();
	avs::Plugin::instance().unload();
}
