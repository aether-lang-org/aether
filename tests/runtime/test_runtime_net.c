#include "test_harness.h"
#include "../../std/net/aether_net.h"
#include "../../std/string/aether_string.h"
#include <errno.h>

#ifndef _WIN32
#include <sys/socket.h>
#include <unistd.h>
#endif

TEST_CATEGORY(socket_null_handling, TEST_CATEGORY_NETWORK) {
    int result = tcp_send_raw(NULL, NULL);
    ASSERT_EQ(-1, result);

    result = tcp_send_n_raw(NULL, NULL, 0);
    ASSERT_EQ(-1, result);

    char* received = tcp_receive_raw(NULL, 1024);
    ASSERT_NULL(received);

    TcpReceiveResult received_n = tcp_receive_n_raw(NULL, 1024);
    ASSERT_NULL(received_n._0);
    ASSERT_EQ(0, received_n._1);
    ASSERT_STRNE("", received_n._2);

    result = tcp_close(NULL);
    ASSERT_EQ(-1, result);
}

TEST_CATEGORY(server_null_handling, TEST_CATEGORY_NETWORK) {
    TcpSocket* sock = tcp_accept_raw(NULL);
    ASSERT_NULL(sock);

    int result = tcp_server_close(NULL);
    ASSERT_EQ(-1, result);
}

TEST_CATEGORY(socket_connect_invalid_host, TEST_CATEGORY_NETWORK) {
#ifndef _WIN32
    TcpSocket* sock = tcp_connect_raw("invalid.host.that.does.not.exist.12345", 80);
    ASSERT_NULL(sock);
#else
    ASSERT_TRUE(1);  // DNS resolution can hang on Windows
#endif
}

TEST_CATEGORY(server_create_invalid_port, TEST_CATEGORY_NETWORK) {
    TcpServer* server = tcp_listen_raw(-1);
    ASSERT_NULL(server);
}

#ifndef _WIN32
// Mirror the private runtime layout so this unit test can wrap a POSIX
// socketpair without depending on a bindable TCP port.
struct TcpSocket {
    int fd;
    int connected;
};

TEST_CATEGORY(tcp_binary_nul_roundtrip, TEST_CATEGORY_NETWORK) {
    int fds[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds));

    TcpSocket left = { fds[0], 1 };
    TcpSocket right = { fds[1], 1 };

    const char payload[3] = { 'A', '\0', 'B' };
    errno = 0;
    int sent = tcp_send_n_raw(&left, payload, 3);
    if (sent < 0) {
        if (errno == EPERM) {
            printf("  SKIP tcp_binary_nul_roundtrip: socket send denied by sandbox\n");
            close(fds[0]);
            close(fds[1]);
            return;
        }
    }
    ASSERT_EQ(3, sent);

    TcpReceiveResult got = tcp_receive_n_raw(&right, 16);
    ASSERT_NOT_NULL(got._0);
    ASSERT_EQ(3, got._1);

    const char* data = aether_string_data(got._0);
    ASSERT_EQ('A', data[0]);
    ASSERT_EQ('\0', data[1]);
    ASSERT_EQ('B', data[2]);
    string_release(got._0);

    close(fds[0]);
    close(fds[1]);
}
#endif
