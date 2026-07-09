#ifndef AETHER_NET_H
#define AETHER_NET_H

#include <stddef.h>

typedef struct TcpSocket TcpSocket;
typedef struct TcpServer TcpServer;
typedef struct TcpReceiveResult {
    void* _0;
    int _1;
    const char* _2;
} TcpReceiveResult;

// TCP Client
TcpSocket* tcp_connect_raw(const char* host, int port);
int tcp_send_raw(TcpSocket* sock, const char* data);
int tcp_send_n_raw(TcpSocket* sock, const char* data, int length);
char* tcp_receive_raw(TcpSocket* sock, int max_bytes);
TcpReceiveResult tcp_receive_n_raw(TcpSocket* sock, int max_bytes);
int tcp_close(TcpSocket* sock);

// TCP Server
TcpServer* tcp_listen_raw(int port);
TcpSocket* tcp_accept_raw(TcpServer* server);
int tcp_server_close(TcpServer* server);

// OS-level descriptor inside a handle, or -1 if null/closed. For
// capability plumbing (capsicum.rights_limit before capsicum.enter);
// owned by the handle — do not close() it. Issue #1003.
int tcp_fd_raw(TcpSocket* sock);
int tcp_server_fd_raw(TcpServer* server);

#endif
