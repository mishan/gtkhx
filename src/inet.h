/* Socket-state helpers. Originally adapted from XChat 1.8.6.
 *
 * The Win32 winsock branch was removed (Phase 1.6) — gtkhx has never
 * built on Windows, and the historical fallback path was a strict
 * downgrade (ioctlsocket vs fcntl, WSAGetLastError vs errno).
 */

#ifndef __gtkhx_inet_h
#define __gtkhx_inet_h 1

#define set_blocking(sok) fcntl (sok, F_SETFL, 0)
#define set_nonblocking(sok) fcntl (sok, F_SETFL, O_NONBLOCK)
#define would_block_again() (errno == EAGAIN || errno == EWOULDBLOCK)
#define would_block() (errno == EWOULDBLOCK)
#define sock_error() (errno)

#endif
