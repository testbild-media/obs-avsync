#include "web-server.hpp"
#include "net-compat.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <sstream>

#ifdef _WIN32
#include <iphlpapi.h>
#else
#include <ifaddrs.h>
#include <net/if.h>
#endif

namespace avs {

namespace {

// ---------------------------------------------------------------- SHA-1 / Base64 (WebSocket handshake)

std::string sha1(const std::string &msg)
{
	uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476, h4 = 0xC3D2E1F0;
	std::string data = msg;
	uint64_t bitLen = (uint64_t)msg.size() * 8;
	data += (char)0x80;
	while (data.size() % 64 != 56)
		data += (char)0;
	for (int i = 7; i >= 0; i--)
		data += (char)((bitLen >> (i * 8)) & 0xFF);

	auto rol = [](uint32_t v, int n) { return (v << n) | (v >> (32 - n)); };
	for (size_t chunk = 0; chunk < data.size(); chunk += 64) {
		uint32_t w[80];
		for (int i = 0; i < 16; i++) {
			const unsigned char *p = (const unsigned char *)data.data() + chunk + i * 4;
			w[i] = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
		}
		for (int i = 16; i < 80; i++)
			w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
		uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
		for (int i = 0; i < 80; i++) {
			uint32_t f, k;
			if (i < 20) {
				f = (b & c) | (~b & d);
				k = 0x5A827999;
			} else if (i < 40) {
				f = b ^ c ^ d;
				k = 0x6ED9EBA1;
			} else if (i < 60) {
				f = (b & c) | (b & d) | (c & d);
				k = 0x8F1BBCDC;
			} else {
				f = b ^ c ^ d;
				k = 0xCA62C1D6;
			}
			uint32_t t = rol(a, 5) + f + e + k + w[i];
			e = d;
			d = c;
			c = rol(b, 30);
			b = a;
			a = t;
		}
		h0 += a;
		h1 += b;
		h2 += c;
		h3 += d;
		h4 += e;
	}
	std::string out;
	for (uint32_t v : {h0, h1, h2, h3, h4})
		for (int i = 3; i >= 0; i--)
			out += (char)((v >> (i * 8)) & 0xFF);
	return out;
}

std::string base64(const std::string &in)
{
	static const char *tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string out;
	size_t i = 0;
	while (i + 2 < in.size()) {
		uint32_t v = ((uint8_t)in[i] << 16) | ((uint8_t)in[i + 1] << 8) | (uint8_t)in[i + 2];
		out += tbl[(v >> 18) & 63];
		out += tbl[(v >> 12) & 63];
		out += tbl[(v >> 6) & 63];
		out += tbl[v & 63];
		i += 3;
	}
	if (i + 1 == in.size()) {
		uint32_t v = (uint8_t)in[i] << 16;
		out += tbl[(v >> 18) & 63];
		out += tbl[(v >> 12) & 63];
		out += "==";
	} else if (i + 2 == in.size()) {
		uint32_t v = ((uint8_t)in[i] << 16) | ((uint8_t)in[i + 1] << 8);
		out += tbl[(v >> 18) & 63];
		out += tbl[(v >> 12) & 63];
		out += tbl[(v >> 6) & 63];
		out += '=';
	}
	return out;
}

std::string lower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	return s;
}

std::string trim(const std::string &s)
{
	size_t a = s.find_first_not_of(" \t\r\n");
	size_t b = s.find_last_not_of(" \t\r\n");
	return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

const char *statusText(int code)
{
	switch (code) {
	case 200:
		return "OK";
	case 206:
		return "Partial Content";
	case 404:
		return "Not Found";
	case 405:
		return "Method Not Allowed";
	case 416:
		return "Range Not Satisfiable";
	default:
		return "Error";
	}
}

} // namespace

struct WebServer::Conn {
	socket_t sock = kInvalidSocket;
	int id = 0;
	std::string address;
	std::thread thread;
	std::mutex sendMutex;
	std::atomic<bool> done{false};
	bool isWs = false;
	WsClientInfo info; // guarded by WebServer::connMutex_
};

WebServer::WebServer() {}

WebServer::~WebServer()
{
	stop();
}

bool WebServer::start(uint16_t port, Hooks hooks, std::string &error, bool loopbackOnly)
{
	stop();
	if (!netInit()) {
		error = "Netzwerk konnte nicht initialisiert werden";
		return false;
	}
	hooks_ = std::move(hooks);

	socket_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s == kInvalidSocket) {
		error = "socket() fehlgeschlagen";
		return false;
	}
#ifdef _WIN32
	BOOL excl = TRUE;
	setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&excl, sizeof(excl));
#else
	int reuse = 1;
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(loopbackOnly ? INADDR_LOOPBACK : INADDR_ANY);
	addr.sin_port = htons(port);
	if (bind(s, (sockaddr *)&addr, sizeof(addr)) != 0) {
		error = "Port " + std::to_string(port) + " ist bereits belegt";
		closeSocket(s);
		return false;
	}
	if (listen(s, 16) != 0) {
		error = "listen() fehlgeschlagen";
		closeSocket(s);
		return false;
	}
	listenSock_ = (uintptr_t)s;
	port_ = port;
	running_ = true;
	acceptThread_ = std::thread([this] { acceptLoop(); });
	return true;
}

void WebServer::stop()
{
	if (!running_ && !acceptThread_.joinable())
		return;
	running_ = false;
	closeSocket((socket_t)listenSock_);
	if (acceptThread_.joinable())
		acceptThread_.join();

	std::vector<std::shared_ptr<Conn>> list;
	{
		std::lock_guard<std::mutex> lock(connMutex_);
		list.swap(conns_);
	}
	for (auto &c : list) {
#ifdef _WIN32
		shutdown(c->sock, SD_BOTH);
#else
		shutdown(c->sock, SHUT_RDWR);
#endif
	}
	for (auto &c : list)
		if (c->thread.joinable())
			c->thread.join();
}

void WebServer::acceptLoop()
{
	const socket_t ls = (socket_t)listenSock_;
	while (running_) {
		sockaddr_in addr{};
#ifdef _WIN32
		int len = sizeof(addr);
#else
		socklen_t len = sizeof(addr);
#endif
		socket_t s = accept(ls, (sockaddr *)&addr, &len);
		if (s == kInvalidSocket) {
			if (!running_)
				break;
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
			continue;
		}
		int one = 1;
		setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));

		auto c = std::make_shared<Conn>();
		c->sock = s;
		char ip[INET_ADDRSTRLEN] = {};
		inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
		c->address = ip;

		std::lock_guard<std::mutex> lock(connMutex_);
		// reap finished connections
		for (auto it = conns_.begin(); it != conns_.end();) {
			if ((*it)->done) {
				if ((*it)->thread.joinable())
					(*it)->thread.join();
				it = conns_.erase(it);
			} else {
				++it;
			}
		}
		c->id = nextId_++;
		conns_.push_back(c);
		c->thread = std::thread([this, c] {
			handle(c);
			c->done = true;
		});
	}
}

bool WebServer::sendAll(Conn &c, const char *data, size_t len)
{
	while (len > 0) {
		int n = send(c.sock, data, (int)std::min<size_t>(len, 1 << 20), 0);
		if (n <= 0)
			return false;
		data += n;
		len -= (size_t)n;
	}
	return true;
}

bool WebServer::sendFrame(Conn &c, int opcode, const std::string &payload)
{
	std::string frame;
	frame += (char)(0x80 | (opcode & 0x0F));
	const size_t n = payload.size();
	if (n < 126) {
		frame += (char)n;
	} else if (n < 65536) {
		frame += (char)126;
		frame += (char)((n >> 8) & 0xFF);
		frame += (char)(n & 0xFF);
	} else {
		frame += (char)127;
		for (int i = 7; i >= 0; i--)
			frame += (char)(((uint64_t)n >> (i * 8)) & 0xFF);
	}
	frame += payload;
	std::lock_guard<std::mutex> lock(c.sendMutex);
	return sendAll(c, frame.data(), frame.size());
}

void WebServer::handle(std::shared_ptr<Conn> c)
{
	setRecvTimeout(c->sock, 10000);
	std::string req;
	char buf[4096];
	while (req.find("\r\n\r\n") == std::string::npos && req.size() < 16384) {
		int n = recv(c->sock, buf, sizeof(buf), 0);
		if (n <= 0) {
			closeSocket(c->sock);
			return;
		}
		req.append(buf, (size_t)n);
	}

	std::istringstream is(req);
	std::string line;
	std::getline(is, line);
	std::string method, target, version;
	{
		std::istringstream ls(line);
		ls >> method >> target >> version;
	}
	std::map<std::string, std::string> headers;
	while (std::getline(is, line)) {
		line = trim(line);
		if (line.empty())
			break;
		size_t colon = line.find(':');
		if (colon != std::string::npos)
			headers[lower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
	}
	const size_t qpos = target.find('?');
	std::string path = target.substr(0, qpos);
	std::string query = qpos == std::string::npos ? std::string() : target.substr(qpos + 1);

	if (path == "/ws" && lower(headers["upgrade"]) == "websocket" && !headers["sec-websocket-key"].empty()) {
		std::string accept = base64(sha1(headers["sec-websocket-key"] + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
		std::string resp = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
				   "Sec-WebSocket-Accept: " +
				   accept + "\r\n\r\n";
		if (sendAll(*c, resp.data(), resp.size())) {
			{
				std::lock_guard<std::mutex> lock(connMutex_);
				c->isWs = true;
				c->info.id = c->id;
				c->info.address = c->address;
				c->info.userAgent = headers["user-agent"];
				c->info.connectedUtcMs = hooks_.utcNowMs ? hooks_.utcNowMs() : 0.0;
				c->info.lastSeenUtcMs = c->info.connectedUtcMs;
			}
			webSocketLoop(*c);
		}
	} else {
		serveHttp(*c, method, path, query, headers["range"]);
	}

#ifdef _WIN32
	shutdown(c->sock, SD_SEND);
#else
	shutdown(c->sock, SHUT_WR);
#endif
	closeSocket(c->sock);
}

namespace {

std::string urlDecode(const std::string &s)
{
	std::string out;
	for (size_t i = 0; i < s.size(); i++) {
		if (s[i] == '+') {
			out += ' ';
		} else if (s[i] == '%' && i + 2 < s.size()) {
			out += (char)std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
			i += 2;
		} else {
			out += s[i];
		}
	}
	return out;
}

WebServer::Query parseQuery(const std::string &q)
{
	WebServer::Query out;
	size_t start = 0;
	while (start <= q.size()) {
		size_t amp = q.find('&', start);
		std::string part = q.substr(start, amp == std::string::npos ? std::string::npos : amp - start);
		if (!part.empty()) {
			size_t eq = part.find('=');
			out[urlDecode(part.substr(0, eq))] = eq == std::string::npos ? "" : urlDecode(part.substr(eq + 1));
		}
		if (amp == std::string::npos)
			break;
		start = amp + 1;
	}
	return out;
}

} // namespace

void WebServer::serveHttp(Conn &c, const std::string &method, const std::string &rawPath, const std::string &query,
			  const std::string &range)
{
	auto respond = [&](int code, const char *mime, const char *body, size_t len, const std::string &extra) {
		std::string head = "HTTP/1.1 " + std::to_string(code) + " " + statusText(code) + "\r\n";
		head += std::string("Content-Type: ") + mime + "\r\n";
		head += "Content-Length: " + std::to_string(len) + "\r\n";
		head += "Cache-Control: no-cache\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n";
		head += extra;
		head += "\r\n";
		if (!sendAll(c, head.data(), head.size()))
			return;
		if (method != "HEAD" && len)
			sendAll(c, body, len);
	};

	if (method != "GET" && method != "HEAD") {
		respond(405, "text/plain", "", 0, "");
		return;
	}
	std::string path = rawPath == "/" ? "/index.html" : rawPath;

	if (path == "/avsync-server.json") {
		std::string body = "{\"server\":\"obs-avsync\",\"version\":\"1.0.0\",\"ws\":\"/ws\"}";
		respond(200, "application/json", body.data(), body.size(), "");
		return;
	}
	if (path == "/api/status") {
		std::string body = hooks_.apiStatus ? hooks_.apiStatus() : "{}";
		respond(200, "application/json; charset=utf-8", body.data(), body.size(), "");
		return;
	}

	if (path.rfind("/api/", 0) == 0) {
		std::string body;
		if (hooks_.apiCommand && hooks_.apiCommand(path, parseQuery(query), body)) {
			respond(200, "application/json; charset=utf-8", body.data(), body.size(), "");
		} else {
			const char *msg = "{\"error\":\"unknown command\"}";
			respond(404, "application/json", msg, strlen(msg), "");
		}
		return;
	}

	const EmbeddedFile *f = findEmbeddedFile(path);
	if (!f) {
		const char *msg = "Not found";
		respond(404, "text/plain", msg, strlen(msg), "");
		return;
	}

	// Byte ranges: Safari on iOS requires them for <video>.
	if (range.rfind("bytes=", 0) == 0 && f->size > 0) {
		std::string spec = range.substr(6);
		size_t dash = spec.find('-');
		if (dash != std::string::npos && spec.find(',') == std::string::npos) {
			std::string a = spec.substr(0, dash), b = spec.substr(dash + 1);
			size_t first, last;
			if (a.empty()) {
				size_t suffix = (size_t)std::strtoull(b.c_str(), nullptr, 10);
				suffix = std::min(suffix, f->size);
				first = f->size - suffix;
				last = f->size - 1;
			} else {
				first = (size_t)std::strtoull(a.c_str(), nullptr, 10);
				last = b.empty() ? f->size - 1 : std::min((size_t)std::strtoull(b.c_str(), nullptr, 10), f->size - 1);
			}
			if (first >= f->size || first > last) {
				respond(416, "text/plain", "", 0, "Content-Range: bytes */" + std::to_string(f->size) + "\r\n");
				return;
			}
			std::string extra = "Accept-Ranges: bytes\r\nContent-Range: bytes " + std::to_string(first) + "-" +
					    std::to_string(last) + "/" + std::to_string(f->size) + "\r\n";
			respond(206, f->mime, (const char *)f->data + first, last - first + 1, extra);
			return;
		}
	}
	respond(200, f->mime, (const char *)f->data, f->size, "Accept-Ranges: bytes\r\n");
}

void WebServer::webSocketLoop(Conn &c)
{
	setRecvTimeout(c.sock, 30000);
	auto recvExact = [&](void *dst, size_t n) {
		char *p = (char *)dst;
		while (n > 0) {
			int got = recv(c.sock, p, (int)n, 0);
			if (got <= 0)
				return false;
			p += got;
			n -= (size_t)got;
		}
		return true;
	};

	std::string message;
	int msgOpcode = 0;
	while (running_) {
		uint8_t h[2];
		if (!recvExact(h, 2))
			break;
		const bool fin = (h[0] & 0x80) != 0;
		const int op = h[0] & 0x0F;
		const bool masked = (h[1] & 0x80) != 0;
		uint64_t len = h[1] & 0x7F;
		if (len == 126) {
			uint8_t e[2];
			if (!recvExact(e, 2))
				break;
			len = ((uint64_t)e[0] << 8) | e[1];
		} else if (len == 127) {
			uint8_t e[8];
			if (!recvExact(e, 8))
				break;
			len = 0;
			for (int i = 0; i < 8; i++)
				len = (len << 8) | e[i];
		}
		if (len > (1u << 20))
			break;
		uint8_t mask[4] = {};
		if (masked && !recvExact(mask, 4))
			break;
		std::string payload((size_t)len, '\0');
		if (len && !recvExact(&payload[0], (size_t)len))
			break;
		if (masked)
			for (size_t i = 0; i < payload.size(); i++)
				payload[i] = (char)(payload[i] ^ mask[i % 4]);

		if (op == 0x8) {
			sendFrame(c, 0x8, payload.substr(0, 2));
			break;
		}
		if (op == 0x9) {
			sendFrame(c, 0xA, payload);
			continue;
		}
		if (op == 0xA)
			continue;
		if (op == 0x1 || op == 0x2) {
			message = payload;
			msgOpcode = op;
		} else if (op == 0x0) {
			message += payload;
		}
		if (fin && msgOpcode == 0x1) {
			onText(c, message);
			message.clear();
		}
	}
	std::lock_guard<std::mutex> lock(connMutex_);
	c.isWs = false;
}

void WebServer::onText(Conn &c, const std::string &text)
{
	json::Object o;
	if (!json::parseObject(text, o))
		return;
	const std::string type = json::string(o, "type");
	if (type == "ping") {
		// answer immediately: the reply time is the reference for the page's clock
		const double id = json::number(o, "id");
		const double utc = hooks_.utcNowMs ? hooks_.utcNowMs() : 0.0;
		char buf[160];
		snprintf(buf, sizeof(buf), "{\"type\":\"pong\",\"id\":%.0f,\"utc\":%.3f}", id, utc);
		sendFrame(c, 0x1, buf);
		std::lock_guard<std::mutex> lock(connMutex_);
		c.info.lastSeenUtcMs = utc;
		return;
	}

	std::lock_guard<std::mutex> lock(connMutex_);
	c.info.lastSeenUtcMs = hooks_.utcNowMs ? hooks_.utcNowMs() : 0.0;
	if (type == "hello") {
		c.info.name = json::string(o, "name", c.info.name);
		c.info.userAgent = json::string(o, "ua", c.info.userAgent);
	} else if (type == "status") {
		c.info.status = o;
		std::string name = json::string(o, "name");
		if (!name.empty())
			c.info.name = name;
	}
}

void WebServer::broadcast(const std::string &text)
{
	std::vector<std::shared_ptr<Conn>> list;
	{
		std::lock_guard<std::mutex> lock(connMutex_);
		for (auto &c : conns_)
			if (c->isWs && !c->done)
				list.push_back(c);
	}
	for (auto &c : list)
		sendFrame(*c, 0x1, text);
}

std::vector<WsClientInfo> WebServer::clients() const
{
	std::vector<WsClientInfo> out;
	std::lock_guard<std::mutex> lock(connMutex_);
	for (const auto &c : conns_)
		if (c->isWs && !c->done)
			out.push_back(c->info);
	return out;
}

std::vector<std::string> WebServer::localIPv4Addresses()
{
	std::vector<std::string> preferred, other;
#ifdef _WIN32
	ULONG size = 16 * 1024;
	std::vector<uint8_t> buf(size);
	const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER |
			    GAA_FLAG_INCLUDE_GATEWAYS;
	ULONG ret = GetAdaptersAddresses(AF_INET, flags, nullptr, (IP_ADAPTER_ADDRESSES *)buf.data(), &size);
	if (ret == ERROR_BUFFER_OVERFLOW) {
		buf.resize(size);
		ret = GetAdaptersAddresses(AF_INET, flags, nullptr, (IP_ADAPTER_ADDRESSES *)buf.data(), &size);
	}
	if (ret != NO_ERROR)
		return {};
	for (auto *a = (IP_ADAPTER_ADDRESSES *)buf.data(); a; a = a->Next) {
		if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
			continue;
		const bool gateway = a->FirstGatewayAddress != nullptr;
		for (auto *u = a->FirstUnicastAddress; u; u = u->Next) {
			if (u->Address.lpSockaddr->sa_family != AF_INET)
				continue;
			char ip[INET_ADDRSTRLEN] = {};
			inet_ntop(AF_INET, &((sockaddr_in *)u->Address.lpSockaddr)->sin_addr, ip, sizeof(ip));
			std::string s = ip;
			if (s.rfind("169.254.", 0) == 0)
				continue;
			(gateway ? preferred : other).push_back(s);
		}
	}
#else
	ifaddrs *list = nullptr;
	if (getifaddrs(&list) != 0)
		return {};
	for (ifaddrs *i = list; i; i = i->ifa_next) {
		if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || (i->ifa_flags & IFF_LOOPBACK) ||
		    !(i->ifa_flags & IFF_UP))
			continue;
		char ip[INET_ADDRSTRLEN] = {};
		inet_ntop(AF_INET, &((sockaddr_in *)i->ifa_addr)->sin_addr, ip, sizeof(ip));
		std::string s = ip;
		if (s.rfind("169.254.", 0) != 0)
			preferred.push_back(s);
	}
	freeifaddrs(list);
#endif
	preferred.insert(preferred.end(), other.begin(), other.end());
	return preferred;
}

} // namespace avs
