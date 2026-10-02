// client.c
// macOS / Linux: gcc -Wall -Werror -Wextra client.c -o client_mac or client_lin
// MSYS2 UCRT64 : gcc -Wall -Werror -Wextra client.c -o client_win.exe -lws2_32
//
// 使い方:  client J|1|2|X [addr:port]
//   J/1/2 : 標準入力から読み取った内容をサーバーへ送り、結果を標準出力へ書く
//   X     : サーバー停止 (標準入力は読まない)
// 終了コード: 0=成功, 1=失敗

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <signal.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  typedef SOCKET socket_t;
  #define CLOSE_SOCKET(s) closesocket(s)
  #define IS_INVALID_SOCKET(s) ((s) == INVALID_SOCKET)
  #define SLEEP_SEC(s) Sleep((s) * 1000)
#else
  #include <unistd.h>
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <sys/time.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  typedef int socket_t;
  #define CLOSE_SOCKET(s) close(s)
  #define IS_INVALID_SOCKET(s) ((s) < 0)
  #define SLEEP_SEC(s) sleep(s)
#endif

#define SERVER_PORT 8080
#define SERVER_ADDR "127.0.0.1"

#define MAXSTRING 65536
#define BUF_SIZE  (MAXSTRING + 2)
#define MAX_INPUT (MAXSTRING - 1)                // 入力の最大バイト数
#define MAX_FRAME ((size_t)16 * 1024 * 1024)     // 応答 1 件の最大バイト数

// 再試行設定（定数）
#define MAX_RETRIES 5       // 最大試行回数
#define RETRY_DELAY_SEC 2   // 再試行までの待ち時間（秒）
#define IO_TIMEOUT_SEC 120  // 応答待ちのタイムアウト（秒）

#ifdef MSG_NOSIGNAL
  #define SEND_FLAGS MSG_NOSIGNAL
#else
  #define SEND_FLAGS 0
#endif

static void sock_perror(const char *what) {
#ifdef _WIN32
    fprintf(stderr, "Error: %s failed (WSA error %d)\n", what, WSAGetLastError());
#else
    perror(what);
#endif
}

static int send_all(socket_t s, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    while (len > 0) {
        int chunk = (len > 0x40000000) ? 0x40000000 : (int)len;
        int n = (int)send(s, p, chunk, SEND_FLAGS);
        if (n < 0) {
#ifndef _WIN32
            if (errno == EINTR) continue;
#endif
            return -1;
        }
        if (n == 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

// 戻り値: 1=len バイト受信完了 / 0=最初の 1 バイト前に切断 / -1=エラー・タイムアウト・途中切断
static int recv_all(socket_t s, void *buf, size_t len) {
    char *p = (char *)buf;
    size_t got = 0;
    while (got < len) {
        size_t rest = len - got;
        int want = (rest > 0x40000000) ? 0x40000000 : (int)rest;
        int n = (int)recv(s, p + got, want, 0);
        if (n == 0) return (got == 0) ? 0 : -1;
        if (n < 0) {
#ifndef _WIN32
            if (errno == EINTR) continue;
#endif
            return -1;
        }
        got += (size_t)n;
    }
    return 1;
}

static void set_io_timeout(socket_t s, int sec) {
#ifdef _WIN32
    DWORD ms = (DWORD)sec * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms, sizeof(ms));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&ms, sizeof(ms));
#else
    struct timeval tv;
    tv.tv_sec = sec;
    tv.tv_usec = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv));
#endif
}

// 標準入力を最大 cap バイト読む。cap を超える入力はエラー (黙って切り捨てない)
static int read_stdin(char *buf, size_t cap, size_t *out_len) {
    size_t n = fread(buf, 1, cap, stdin);
    if (ferror(stdin)) {
        fprintf(stderr, "Error: failed to read input.\n");
        return -1;
    }
    if (n == cap && fgetc(stdin) != EOF) {
        fprintf(stderr, "Error: input is too long (max %d bytes).\n", (int)cap);
        return -1;
    }
    *out_len = n;
    return 0;
}

static int communication(socket_t sock, char type) {
    char *req = (char *)malloc(BUF_SIZE);
    char *resp = NULL;
    size_t dlen = 0;
    size_t total;
    size_t rlen;
    uint8_t hdr[5];
    int rc = -1;

    if (req == NULL) {
        fprintf(stderr, "Error: out of memory.\n");
        return -1;
    }
    req[0] = type;
    req[1] = ':';

    // サーバーに送る文字列を取得 (X は入力不要)
    if (type != 'X') {
        if (read_stdin(req + 2, MAX_INPUT, &dlen) < 0) goto done;
        if (dlen == 0) {
            fprintf(stderr, "Error: no input.\n");
            goto done;
        }
    }
    total = dlen + 2;

    // 要求送信: [4byte 長さ] + "T:" + データ
    hdr[0] = (uint8_t)(total >> 24);
    hdr[1] = (uint8_t)(total >> 16);
    hdr[2] = (uint8_t)(total >> 8);
    hdr[3] = (uint8_t)total;
    if (send_all(sock, hdr, 4) < 0 || send_all(sock, req, total) < 0) {
        sock_perror("send()");
        goto done;
    }

    // 応答受信: [4byte 長さ] [1byte 状態] + 本文
    if (recv_all(sock, hdr, 5) != 1) {
        fprintf(stderr, "Error: no response from server.\n");
        goto done;
    }
    rlen = ((size_t)hdr[0] << 24) | ((size_t)hdr[1] << 16) | ((size_t)hdr[2] << 8) | (size_t)hdr[3];
    if (rlen > MAX_FRAME) {
        fprintf(stderr, "Error: response too large.\n");
        goto done;
    }
    resp = (char *)malloc(rlen + 1);
    if (resp == NULL) {
        fprintf(stderr, "Error: out of memory.\n");
        goto done;
    }
    if (rlen > 0 && recv_all(sock, resp, rlen) != 1) {
        fprintf(stderr, "Error: connection lost while receiving response.\n");
        goto done;
    }
    resp[rlen] = '\0';

    if (hdr[4] != 0) {
        fprintf(stderr, "%s\n", resp);   // サーバー側エラー
        goto done;
    }
    if (type == 'X') {
        printf("Exited from the connection to a server\n");
    } else {
        fwrite(resp, 1, rlen, stdout);
        fflush(stdout);
    }
    rc = 0;

done:
    free(req);
    free(resp);
    return rc;
}

static int parse_addr(const char *arg, char *host, size_t host_size, int *port) {
    const char *colon = strrchr(arg, ':');   // IPv4 前提で最後の ':' を区切りとする
    if (colon == NULL) {
        fprintf(stderr, "Error: address must be in addr:port format (e.g. 127.0.0.1:12345)\n");
        return -1;
    }
    size_t hl = (size_t)(colon - arg);
    if (hl == 0 || hl >= host_size) {
        fprintf(stderr, "Error: invalid address: %s\n", arg);
        return -1;
    }
    char *end = NULL;
    long p = strtol(colon + 1, &end, 10);
    if (colon[1] == '\0' || *end != '\0' || p <= 0 || p > 65535) {
        fprintf(stderr, "Error: invalid port number: %s\n", colon + 1);
        return -1;
    }
    memcpy(host, arg, hl);
    host[hl] = '\0';
    *port = (int)p;
    return 0;
}

// IPv4 のドット表記またはホスト名 (localhost など) を解決する
static int resolve_ipv4(const char *host, int port, struct sockaddr_in *out) {
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    char portstr[16];
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(portstr, sizeof(portstr), "%d", port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || res == NULL) {
        return -1;
    }
    if ((size_t)res->ai_addrlen < sizeof(*out)) {
        freeaddrinfo(res);
        return -1;
    }
    memcpy(out, res->ai_addr, sizeof(*out));
    freeaddrinfo(res);
    return 0;
}

static int run_client(int argc, char *argv[]) {
    char host[128];
    int port = SERVER_PORT;
    snprintf(host, sizeof(host), "%s", SERVER_ADDR);

    if (argc < 2 || argc > 3) {
        fprintf(stderr, "Usage: %s J|1|2|X [addr:port]\n", argv[0]);
        return 1;
    }
    char type = argv[1][0];
    if (strlen(argv[1]) != 1 || (type != 'J' && type != '1' && type != '2' && type != 'X')) {
        fprintf(stderr, "Error: type must be J, 1, 2 or X.\n");
        return 1;
    }
    if (argc == 3 && parse_addr(argv[2], host, sizeof(host), &port) < 0) {
        return 1;
    }

    struct sockaddr_in addr;
    if (resolve_ipv4(host, port, &addr) < 0) {
        fprintf(stderr, "Error: invalid address: %s\n", host);
        return 1;
    }

    // サーバー接続リトライループ
    socket_t sock = 0;
    int connected = 0;
    for (int attempt = 1; attempt <= MAX_RETRIES; attempt++) {
        // 接続試行ごとにソケットを新規作成
        sock = socket(AF_INET, SOCK_STREAM, 0);
        if (IS_INVALID_SOCKET(sock)) {
            sock_perror("socket()");
            return 1;
        }
        if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            connected = 1;
            break;
        }
        CLOSE_SOCKET(sock);
        if (attempt < MAX_RETRIES) {
            fprintf(stderr, "[Client] Server not ready. Retrying in %d second(s)... (%d/%d)\n",
                    RETRY_DELAY_SEC, attempt, MAX_RETRIES);
            SLEEP_SEC(RETRY_DELAY_SEC);
        }
    }
    if (!connected) {
        fprintf(stderr, "Error: Could not connect to server after %d attempts.\n", MAX_RETRIES);
        return 1;
    }

    set_io_timeout(sock, IO_TIMEOUT_SEC);
    int rc = communication(sock, type);
    CLOSE_SOCKET(sock);
    return (rc == 0) ? 0 : 1;
}

int main(int argc, char *argv[]) {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        fprintf(stderr, "Error: WSAStartup failed.\n");
        return 1;
    }
#else
    signal(SIGPIPE, SIG_IGN);
#endif
    int rc = run_client(argc, argv);
#ifdef _WIN32
    WSACleanup();
#endif
    return rc;
}
