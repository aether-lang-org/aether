#include "aether_net.h"
#include "../../runtime/config/aether_optimization_config.h"
#include "../../runtime/aether_sandbox.h"
#include "../../runtime/aether_resource_caps.h"

#if !AETHER_HAS_NETWORKING
TcpSocket* tcp_connect_raw(const char* h, int p) { (void)h; (void)p; return NULL; }
int tcp_send_raw(TcpSocket* s, const char* d) { (void)s; (void)d; return -1; }
char* tcp_receive_raw(TcpSocket* s, int m) { (void)s; (void)m; return NULL; }
int tcp_close(TcpSocket* s) { (void)s; return 0; }
TcpServer* tcp_listen_raw(int p) { (void)p; return NULL; }
TcpSocket* tcp_accept_raw(TcpServer* s) { (void)s; return NULL; }
int tcp_server_close(TcpServer* s) { (void)s; return 0; }
#else

#include <stdlib.h>
#include <string.h>
#include <stdio.h>   /* snprintf (getaddrinfo port string) */

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #ifdef _MSC_VER
        #pragma comment(lib, "ws2_32.lib")
    #endif
    #define close closesocket
    typedef int socklen_t;
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <netdb.h>
    #include <unistd.h>
    #include <arpa/inet.h>
#endif

struct TcpSocket {
    int fd;
    int connected;
};

struct TcpServer {
    int fd;
    int port;
};

static int net_initialized = 0;

static void net_init() {
    if (net_initialized) return;
    #ifdef _WIN32
    WSADATA wsa_data;
    WSAStartup(MAKEWORD(2, 2), &wsa_data);
    #endif
    net_initialized = 1;
}

// Set read/write timeouts on a socket so blocking operations don't hang
// indefinitely on dead or slow peers.  Default: 30 seconds.
static void net_set_socket_timeouts(int fd, int timeout_sec) {
#ifdef _WIN32
    DWORD timeout_ms = (DWORD)timeout_sec * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout_ms, sizeof(timeout_ms));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout_ms, sizeof(timeout_ms));
#else
    struct timeval tv;
    tv.tv_sec = timeout_sec;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

TcpSocket* tcp_connect_raw(const char* host, int port) {
    // Sandbox check: is TCP connect to this host allowed?
    if (!aether_sandbox_check("tcp", host)) return NULL;

    net_init();

    /* getaddrinfo, not gethostbyname: the latter returns a pointer into a
     * shared, process-static struct, so concurrent connects on different
     * threads race and corrupt each other's resolved address. getaddrinfo is
     * thread-safe and returns caller-owned memory. Pinned to AF_INET to match
     * the sockaddr_in the rest of this function builds. */
    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    {
        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%d", port);
        struct addrinfo hints;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo* res = NULL;
        if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
            return NULL;
        }
        memcpy(&serv_addr, res->ai_addr, sizeof(struct sockaddr_in));
        freeaddrinfo(res);
    }
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        return NULL;
    }

    if (connect(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        close(sockfd);
        return NULL;
    }

    net_set_socket_timeouts(sockfd, 30);

    TcpSocket* sock = (TcpSocket*)malloc(sizeof(TcpSocket));
    if (!sock) { close(sockfd); return NULL; }
    sock->fd = sockfd;
    sock->connected = 1;
    return sock;
}

int tcp_send_raw(TcpSocket* sock, const char* data) {
    if (!sock || !sock->connected || !data) return -1;

    int sent = send(sock->fd, data, strlen(data), 0);
    return sent;
}

char* tcp_receive_raw(TcpSocket* sock, int max_bytes) {
    if (!sock || !sock->connected || max_bytes <= 0) return NULL;

    /* Cap-aware (#343): max_bytes is caller-supplied (program-
     * controlled, but plugin-host paths surface it as untrusted).
     * Caller frees with plain libc free per the caller-owned-return
     * contract (aether_resource_caps.h:89-94). */
    char* buffer = (char*)aether_caps_malloc((size_t)max_bytes + 1);
    if (!buffer) return NULL;
    int received = recv(sock->fd, buffer, max_bytes, 0);

    if (received <= 0) {
        aether_caps_free(buffer, (size_t)max_bytes + 1);
        sock->connected = 0;
        return NULL;
    }

    buffer[received] = '\0';
    return buffer;
}

int tcp_close(TcpSocket* sock) {
    if (!sock) return -1;

    if (sock->connected) {
        close(sock->fd);
        sock->connected = 0;
    }

    free(sock);
    return 0;
}

TcpServer* tcp_listen_raw(int port) {
    // Sandbox check: is listening on this port allowed?
    if (!aether_sandbox_check("tcp_listen", "*")) return NULL;

    net_init();

    // Validate port range (1-65535)
    if (port < 1 || port > 65535) {
        return NULL;
    }

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        return NULL;
    }

    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(port);

    if (bind(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        close(sockfd);
        return NULL;
    }

    if (listen(sockfd, 5) < 0) {
        close(sockfd);
        return NULL;
    }

    TcpServer* server = (TcpServer*)malloc(sizeof(TcpServer));
    if (!server) { close(sockfd); return NULL; }
    server->fd = sockfd;
    server->port = port;
    return server;
}

TcpSocket* tcp_accept_raw(TcpServer* server) {
    if (!server) return NULL;

    struct sockaddr_in cli_addr;
    socklen_t clilen = sizeof(cli_addr);

    int newsockfd = accept(server->fd, (struct sockaddr*)&cli_addr, &clilen);
    if (newsockfd < 0) {
        return NULL;
    }

    net_set_socket_timeouts(newsockfd, 30);

    TcpSocket* sock = (TcpSocket*)malloc(sizeof(TcpSocket));
    if (!sock) { close(newsockfd); return NULL; }
    sock->fd = newsockfd;
    sock->connected = 1;
    return sock;
}

int tcp_server_close(TcpServer* server) {
    if (!server) return -1;

    close(server->fd);
    free(server);
    return 0;
}

#endif // AETHER_HAS_NETWORKING
