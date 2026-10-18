// Small socket portability layer (Winsock / BSD sockets).
#pragma once

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
static constexpr socket_t kInvalidSocket = INVALID_SOCKET;
inline int closeSocket(socket_t s)
{
	return closesocket(s);
}
inline int lastSocketError()
{
	return WSAGetLastError();
}
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <errno.h>
using socket_t = int;
static constexpr socket_t kInvalidSocket = -1;
inline int closeSocket(socket_t s)
{
	return ::close(s);
}
inline int lastSocketError()
{
	return errno;
}
#endif

namespace avs {

// Call once per process before using sockets (no-op outside Windows).
bool netInit();

// Receive timeout in milliseconds.
void setRecvTimeout(socket_t s, int ms);

} // namespace avs
