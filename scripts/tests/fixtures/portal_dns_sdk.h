// Socket boundary of the complete production portal-DNS translation unit.
#pragma once
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

namespace network {
int openSocket(int, int, int);
int bindSocket(int, const sockaddr*, socklen_t);
int configureSocket(int, int, int, const void*, socklen_t);
int closeSocket(int);
ssize_t receive(int, void*, std::size_t, int, sockaddr*, socklen_t*);
ssize_t send(int, const void*, std::size_t, int, const sockaddr*, socklen_t);
}  // namespace network
#define socket network::openSocket
#define bind network::bindSocket
#define setsockopt network::configureSocket
#define close network::closeSocket
#define recvfrom network::receive
#define sendto network::send
