// server.c
//
// プロトコル
//   要求: [4byte 長さ(ビッグエンディアン)] + "T:" + データ      (T は J / 1 / 2 / X)
//   応答: [4byte 長さ(ビッグエンディアン)] + [1byte 状態] + 本文  (状態 0=成功, 1=エラー)
//   点訳(1:/2:)の成功時の本文は、末尾に \x7f を付加。
//
// macOS ビルド例:
// gcc -Wall -Werror -Wextra -I/opt/homebrew/include -I/opt/homebrew/include/liblouis -I/opt/homebrew/include/mecab -L/opt/homebrew/lib /opt/homebrew/lib/liblouis.a -lunistring -lmecab -g -O2 -o server_mac server.c
//
// static
// g++ -I/opt/homebrew/include -I/opt/homebrew/include/liblouis -I/opt/homebrew/include/mecab -L/opt/homebrew/lib /opt/homebrew/lib/liblouis.a /opt/homebrew/Cellar/mecab/0.996/lib/libmecab.a /opt/homebrew/opt/libunistring/lib/libunistring.a -liconv -g -O2 -o server_mac server.c
//
// MSYS2 UCRT64 ビルド例:
// gcc -Wall -Werror -Wextra server.c -o server_win.exe -llouis -lunistring -lmecab -lws2_32
//
// static
// g++ -I/ucrt64/include/liblouis -I/ucrt64/include/mecab -Wall -Werror -Wextra server.c -o server_win.exe /ucrt64/lib/liblouis.a /ucrt64/lib/libunistring.a /ucrt64/lib/libmecab.a -lws2_32 -liconv -static

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>
#include <signal.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  typedef SOCKET socket_t;
  typedef int sock_len_t;
  #define CLOSE_SOCKET(s) closesocket(s)
  #define IS_INVALID_SOCKET(s) ((s) == INVALID_SOCKET)
  #define INVALID_SOCK INVALID_SOCKET
#else
  #include <unistd.h>
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <sys/select.h>
  #include <sys/time.h>
  #include <sys/stat.h>
  #include <sys/file.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <fcntl.h>
  typedef int socket_t;
  typedef socklen_t sock_len_t;
  #define CLOSE_SOCKET(s) close(s)
  #define IS_INVALID_SOCKET(s) ((s) < 0)
  #define INVALID_SOCK (-1)
#endif

#include <mecab.h>
#include <liblouis.h>
#include <unistr.h>

// ★環境に合わせて変更してください
#define SERVER_PORT 8080
#define SERVER_ADDR "127.0.0.1"

#define MAXSTRING 65536
#define BUF_SIZE  (MAXSTRING + 2)              // 要求 1 件の最大バイト数 ("T:" 込み)
#define MAX_FRAME ((size_t)16 * 1024 * 1024)   // 応答 1 件の最大バイト数
#define CLIENT_IO_TIMEOUT_SEC 30               // 接続後の送受信タイムアウト(無言クライアント対策)
#define LOCK_FILE "./brl_server.lock"

#define LOUIS_TABLE1 "./jmbraille/ueb/brl_en-ueb-g1.ctb"
#define LOUIS_TABLE2 "./jmbraille/ueb/brl_en-ueb-g2.ctb"
#define MECAB_OPTS "-r ./jmbraille/dic/mecabrc -Obraille -p"

#define LOG(...) do { printf(__VA_ARGS__); fflush(stdout); } while (0)

#ifdef MSG_NOSIGNAL
  #define SEND_FLAGS MSG_NOSIGNAL
#else
  #define SEND_FLAGS 0
#endif

static const char *g_lock_file = LOCK_FILE;
static volatile sig_atomic_t g_stop = 0;

static void handle_signal(int sig) {
    (void)sig;
    g_stop = 1;   // フラグを立てるだけ (メインループが 1 秒ごとに確認する)
}

static void sock_perror(const char *what) {
#ifdef _WIN32
    fprintf(stderr, "Error: %s failed (WSA error %d)\n", what, WSAGetLastError());
#else
    perror(what);
#endif
}

static int is_transient_error(void) {
#ifdef _WIN32
    int e = WSAGetLastError();
    return e == WSAEINTR || e == WSAECONNRESET || e == WSAECONNABORTED;
#else
    return errno == EINTR || errno == ECONNABORTED;
#endif
}

// ----------------------------------------------------------------
// 二重起動禁止 (ロックファイル制御)
// ----------------------------------------------------------------
#ifdef _WIN32
static HANDLE g_lock_handle = INVALID_HANDLE_VALUE;

static int acquire_lock(const char *lock_file) {
    // 共有なし(排他) + 閉じたら自動削除
    g_lock_handle = CreateFileA(lock_file, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                                OPEN_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    return (g_lock_handle == INVALID_HANDLE_VALUE) ? -1 : 0;
}

static void release_lock(const char *lock_file) {
    (void)lock_file;
    if (g_lock_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(g_lock_handle);   // DELETE_ON_CLOSE により削除される
        g_lock_handle = INVALID_HANDLE_VALUE;
    }
}
#else
static int g_lock_fd = -1;

static int acquire_lock(const char *lock_file) {
    // 「開く→ロック」の間に前の持ち主が unlink したファイルを掴まないよう、
    // ロック後にパスと fd が同一ファイルか確認する
    for (int attempt = 0; attempt < 5; attempt++) {
        int fd = open(lock_file, O_CREAT | O_RDWR, 0666);
        if (fd == -1) return -1;
        if (flock(fd, LOCK_EX | LOCK_NB) == -1) {
            close(fd);
            return -1;
        }
        struct stat a, b;
        if (fstat(fd, &a) == 0 && stat(lock_file, &b) == 0 &&
            a.st_ino == b.st_ino && a.st_dev == b.st_dev) {
            g_lock_fd = fd;
            return 0;
        }
        close(fd);
    }
    return -1;
}

static void release_lock(const char *lock_file) {
    if (g_lock_fd != -1) {
        unlink(lock_file);   // 先に unlink してから close (ロック解放後の隙間をなくす)
        close(g_lock_fd);
        g_lock_fd = -1;
    }
}
#endif

// ----------------------------------------------------------------
// ソケット送受信 (部分送受信・EINTR 対応) とフレーム処理
// ----------------------------------------------------------------
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

// 戻り値: 1=len バイト受信完了 / 0=最初の 1 バイト前に相手が切断 / -1=エラー・タイムアウト・途中切断
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

static uint32_t get_u32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static int send_frame(socket_t s, uint8_t status, const void *data, size_t len) {
    uint8_t hdr[5];
    if (len > MAX_FRAME) return -1;
    hdr[0] = (uint8_t)(len >> 24);
    hdr[1] = (uint8_t)(len >> 16);
    hdr[2] = (uint8_t)(len >> 8);
    hdr[3] = (uint8_t)len;
    hdr[4] = status;
    if (send_all(s, hdr, sizeof(hdr)) < 0) return -1;
    if (len > 0 && send_all(s, data, len) < 0) return -1;
    return 0;
}

static int send_error(socket_t s, const char *msg) {
    return send_frame(s, 1, msg, strlen(msg));
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

// X: (停止コマンド) は同一ホストからの接続のみ許可する
static int is_local_peer(socket_t s) {
    struct sockaddr_in peer, local;
    sock_len_t pl = (sock_len_t)sizeof(peer);
    sock_len_t ll = (sock_len_t)sizeof(local);
    if (getpeername(s, (struct sockaddr *)&peer, &pl) != 0) return 0;
    if (getsockname(s, (struct sockaddr *)&local, &ll) != 0) return 0;
    if (peer.sin_family != AF_INET) return 0;
    uint32_t p = ntohl(peer.sin_addr.s_addr);
    uint32_t l = ntohl(local.sin_addr.s_addr);
    return ((p >> 24) == 127) || (p == l);
}

// ----------------------------------------------------------------
// 点訳
// ----------------------------------------------------------------
static int add_to_inbuf(uint32_t cp, formtype font,
                        widechar *inbuf, formtype *typeform, int *w_idx) {
    if (sizeof(widechar) == 2 && cp >= 0x10000) {
        if (*w_idx + 2 >= MAXSTRING) return -1;
        uint32_t offset = cp - 0x10000;
        inbuf[*w_idx]    = (widechar)(0xD800 | (offset >> 10));
        typeform[*w_idx] = font;
        (*w_idx)++;
        inbuf[*w_idx]    = (widechar)(0xDC00 | (offset & 0x3FF));
        typeform[*w_idx] = font;
        (*w_idx)++;
    } else {
        if (*w_idx + 1 >= MAXSTRING) return -1;
        inbuf[*w_idx]    = (widechar)cp;
        typeform[*w_idx] = font;
        (*w_idx)++;
    }
    return 0;
}

// s[i] から UTF-8 の 1 文字を読み、コードポイントと消費バイト数を返す
static int read_cp(const char *s, size_t n, size_t i, uint32_t *cp) {
    ucs4_t c = 0;
    int bytes = u8_mbtouc(&c, (const uint8_t *)s + i, n - i);
    if (bytes <= 0) {
        c = 0xFFFD;
        bytes = 1;
    }
    *cp = (uint32_t)c;
    return bytes;
}

// 16進 4 桁を厳密に解釈する (符号・空白・0x は不可)
static int parse_hex4(const char *s, uint32_t *out) {
    uint32_t v = 0;
    for (int k = 0; k < 4; k++) {
        unsigned char c = (unsigned char)s[k];
        int d;
        if (c >= '0' && c <= '9')      d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return -1;
        v = (v << 4) | (uint32_t)d;
    }
    *out = v;
    return 0;
}

// 成功時 0 (出力は malloc された UTF-8、末尾に \x7f 付き。呼び出し側で free)、失敗時 -1 (*err に理由)
static int run_liblouis(const char *in, size_t in_len, const char *table,
                        uint8_t **out, size_t *out_len, const char **err) {
    widechar *inbuf    = (widechar *)malloc(sizeof(widechar) * MAXSTRING);
    formtype *typeform = (formtype *)malloc(sizeof(formtype) * MAXSTRING);
    widechar *outbuf   = (widechar *)malloc(sizeof(widechar) * MAXSTRING);
    uint8_t  *u8       = NULL;
    uint8_t  *grown    = NULL;
    size_t    u8len    = 0;
    size_t    i        = 0;
    int       w_idx    = 0;
    int       inlen    = 0;
    int       outlen   = 0;
    int       rc       = -1;
    uint32_t  cp       = 0;
    formtype  font     = 0;

    *err = "Error: translation failed.";
    if (inbuf == NULL || typeform == NULL || outbuf == NULL) {
        *err = "Error: out of memory.";
        goto done;
    }

    // --- 1. 入力文字列をパースして inbuf/typeform を構築 ---
    while (i < in_len) {
        if (in[i] == '\\' && i + 1 < in_len) {
            unsigned char next = (unsigned char)in[i + 1];

            if ('0' <= next && next <= '<') {
                // フォント属性トグル (\0 〜 \<)
                font ^= (formtype)(1u << (next - '0'));
                i += 2;

            } else if (next == 'x' || next == 'X') {
                // \xHHHH: 16進 4 桁エスケープ
                if (i + 6 > in_len || parse_hex4(in + i + 2, &cp) < 0) {
                    *err = "Error: invalid or incomplete hex escape (\\xHHHH).";
                    goto done;
                }
                if (cp >= 0xD800 && cp <= 0xDFFF) {
                    *err = "Error: surrogate code point in hex escape.";
                    goto done;
                }
                if (add_to_inbuf(cp, font, inbuf, typeform, &w_idx) < 0) {
                    *err = "Error: input buffer overflow.";
                    goto done;
                }
                i += 6;

            } else {
                // その他のエスケープ: 直後の 1 文字 (マルチバイト可) をそのまま投入
                int b = read_cp(in, in_len, i + 1, &cp);
                if (add_to_inbuf(cp, font, inbuf, typeform, &w_idx) < 0) {
                    *err = "Error: input buffer overflow.";
                    goto done;
                }
                i += 1 + (size_t)b;
            }

        } else {
            // 通常の文字 (末尾の孤立バックスラッシュもここで通る)
            int b = read_cp(in, in_len, i, &cp);
            if (add_to_inbuf(cp, font, inbuf, typeform, &w_idx) < 0) {
                *err = "Error: input buffer overflow.";
                goto done;
            }
            i += (size_t)b;
        }
    }
    inbuf[w_idx]    = 0;
    typeform[w_idx] = 0;
    inlen  = w_idx;
    outlen = MAXSTRING;

    // --- 2. liblouis による点字翻訳 ---
    if (inlen > 0) {
        int consumed = inlen;
        if (!lou_translateString(table, inbuf, &consumed, outbuf, &outlen, typeform, NULL, 0)) {
            *err = "Error: translation failure.";
            goto done;
        }
        if (consumed != inlen) {   // 出力バッファ不足で途中までしか変換されていない
            *err = "Error: output too long.";
            goto done;
        }

        // --- 3. widechar → UTF-8 ---
        if (sizeof(widechar) == 2) {
            u8 = u16_to_u8((const uint16_t *)outbuf, (size_t)outlen, NULL, &u8len);
        } else {
            u8 = u32_to_u8((const uint32_t *)outbuf, (size_t)outlen, NULL, &u8len);
        }
        if (u8 == NULL) {
            *err = "Error: UTF-8 output conversion failed.";
            goto done;
        }
    }

    // デリミタ \x7f を末尾に付加
    grown = (uint8_t *)realloc(u8, u8len + 1);
    if (grown == NULL) {
        *err = "Error: out of memory.";
        goto done;
    }
    u8 = grown;
    u8[u8len] = 0x7f;
    *out     = u8;
    *out_len = u8len + 1;
    u8 = NULL;   // 所有権を呼び出し側へ移した
    rc = 0;

done:
    free(inbuf);
    free(typeform);
    free(outbuf);
    free(u8);
    return rc;
}

// ----------------------------------------------------------------
// クライアントとの通信・処理の振り分け。X: を受理したら 1 を返す
// ----------------------------------------------------------------
static int communicate(socket_t c_sock, mecab_t *mecab, const char *table_g1, const char *table_g2) {
    char *req = (char *)malloc(BUF_SIZE + 1);
    int shutdown_req = 0;

    if (req == NULL) {
        send_error(c_sock, "Error: out of memory.");
        return 0;
    }

    while (!shutdown_req && !g_stop) {
        uint8_t hdr[4];
        if (recv_all(c_sock, hdr, sizeof(hdr)) != 1) break;   // 切断・タイムアウト

        size_t len = get_u32(hdr);
        if (len < 2 || len > BUF_SIZE) {
            send_error(c_sock, "Error: invalid request length.");
            break;   // 本文を読んでいないのでストリームの同期が取れない。切断する
        }
        if (recv_all(c_sock, req, len) != 1) break;
        req[len] = '\0';

        const char *data = req + 2;
        size_t dlen = len - 2;

        if (req[1] != ':') {
            send_error(c_sock, "Error: Invalid command format. Use J:, 1:, 2:, or X:");
            continue;
        }

        if (req[0] == 'X') {
            if (!is_local_peer(c_sock)) {
                send_error(c_sock, "Error: shutdown is only accepted from the local host.");
                continue;
            }
            LOG("[Server] Shutdown command received.\n");
            send_frame(c_sock, 0, NULL, 0);
            shutdown_req = 1;

        } else if (req[0] == 'J') {
            // MeCab 形態素解析 (長さ指定版なので埋め込み NUL があっても安全)
            const char *result = mecab_sparse_tostr2(mecab, data, dlen);
            if (result == NULL) {
                send_error(c_sock, "Error: translation failed.");
            } else {
                send_frame(c_sock, 0, result, strlen(result));
            }

        } else if (req[0] == '1' || req[0] == '2') {
            // Liblouis 点字翻訳。末尾の CR/LF を除去
            while (dlen > 0 && (data[dlen - 1] == '\n' || data[dlen - 1] == '\r')) dlen--;

            uint8_t *out = NULL;
            size_t out_len = 0;
            const char *err = NULL;
            if (run_liblouis(data, dlen, (req[0] == '1') ? table_g1 : table_g2,
                             &out, &out_len, &err) < 0) {
                send_error(c_sock, err);
            } else {
                send_frame(c_sock, 0, out, out_len);
                free(out);
            }

        } else {
            send_error(c_sock, "Error: Invalid command format. Use J:, 1:, 2:, or X:");
        }
    }

    free(req);
    return shutdown_req;
}

// ----------------------------------------------------------------
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

static int run_server(int argc, char *argv[]) {
    int rc = 1;
    int timeout = 0;
    const char *table_g1 = LOUIS_TABLE1;
    const char *table_g2 = LOUIS_TABLE2;
    const char *mecab_opts = MECAB_OPTS;
    char bind_addr[128];
    int bind_port = SERVER_PORT;
    int lock_held = 0;
    socket_t s_sock = INVALID_SOCK;
    mecab_t *mecab = NULL;
    struct sockaddr_in addr;
    int opt = 1;
    int shutdown_flag = 0;
    time_t last_activity;
    int nfds = 0;

    snprintf(bind_addr, sizeof(bind_addr), "%s", SERVER_ADDR);

    // 引数解析
    for (int i = 1; i < argc; i++) {
        int has_val = (i + 1 < argc);
        if (strcmp(argv[i], "-t") == 0 && has_val) {
            timeout = atoi(argv[++i]);
            if (timeout < 0) {
                fprintf(stderr, "Error: timeout must be >= 0.\n");
                return 1;
            }
        } else if (strcmp(argv[i], "-l") == 0 && has_val) {
            g_lock_file = argv[++i];
        } else if (strcmp(argv[i], "-m") == 0 && has_val) {
            mecab_opts = argv[++i];
        } else if (strcmp(argv[i], "-g1") == 0 && has_val) {
            table_g1 = argv[++i];
        } else if (strcmp(argv[i], "-g2") == 0 && has_val) {
            table_g2 = argv[++i];
        } else if (strcmp(argv[i], "-a") == 0 && has_val) {
            if (parse_addr(argv[++i], bind_addr, sizeof(bind_addr), &bind_port) < 0) return 1;
        } else {
            fprintf(stderr, "Error: invalid option or missing value: %s\n"
                            "Usage: %s [-t minutes] [-l lockfile] [-m mecab_opts] [-g1 table] [-g2 table] [-a addr:port]\n",
                    argv[i], argv[0]);
            return 1;
        }
    }

    // 点字テーブルを起動時に検証 (パス間違いを最初の翻訳まで持ち越さない)
    if (!lou_checkTable(table_g1)) {
        fprintf(stderr, "Error: cannot load braille table: %s\n", table_g1);
        return 1;
    }
    if (!lou_checkTable(table_g2)) {
        fprintf(stderr, "Error: cannot load braille table: %s\n", table_g2);
        lou_free();
        return 1;
    }

    // 二重起動禁止
    if (acquire_lock(g_lock_file) == -1) {
        fprintf(stderr, "Error: Server is already running or cannot create lock file.\n");
        lou_free();
        return 1;
    }
    lock_held = 1;

    // 終了シグナル (フラグを立てるだけ。メインループが確認して正常終了する)
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);   // 切断済みクライアントへの send でプロセスが死なないように
#endif

    // ソケットの作成と設定
    s_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (IS_INVALID_SOCKET(s_sock)) {
        sock_perror("socket()");
        goto cleanup;
    }

#ifdef _WIN32
    // Windows の SO_REUSEADDR は他プロセスとの同一ポート共有を許してしまうため使わない
    setsockopt(s_sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&opt, sizeof(opt));
#else
    setsockopt(s_sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));
#endif

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((unsigned short)bind_port);
    if (inet_pton(AF_INET, bind_addr, &addr.sin_addr) != 1) {
        fprintf(stderr, "Error: invalid address: %s\n", bind_addr);
        goto cleanup;
    }

    if (bind(s_sock, (const struct sockaddr *)&addr, sizeof(addr)) == -1) {
        sock_perror("bind()");
        goto cleanup;
    }
    if (listen(s_sock, 8) == -1) {
        sock_perror("listen()");
        goto cleanup;
    }

    // MeCab 初期化
    mecab = mecab_new2(mecab_opts);
    if (mecab == NULL) {
        fprintf(stderr, "Exception: Failed to initialize MeCab: %s\n", mecab_strerror(NULL));
        goto cleanup;
    }

    LOG("[Server] Listening on %s:%d\n", bind_addr, bind_port);
    LOG("[Server] Mecab: %s\n", mecab_opts);
    LOG("[Server] UEBG1: %s\n", table_g1);
    LOG("[Server] UEBG2: %s\n", table_g2);
    if (timeout > 0) {
        LOG("[Server] Automatic shutdown after %d minute(s) of inactivity.\n", timeout);
    }

#ifndef _WIN32
    nfds = (int)s_sock + 1;
#endif
    last_activity = time(NULL);
    rc = 0;

    // accept を 1 秒ごとに起こし、停止シグナルとアイドルタイムアウトを本流で処理する
    while (!shutdown_flag && !g_stop) {
        fd_set rfds;
        struct timeval tv;
        FD_ZERO(&rfds);
        FD_SET(s_sock, &rfds);
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int sr = select(nfds, &rfds, NULL, NULL, &tv);
        if (sr < 0) {
            if (is_transient_error()) continue;
            sock_perror("select()");
            rc = 1;
            break;
        }
        if (sr == 0) {
            if (timeout > 0 && (time(NULL) - last_activity) >= (time_t)timeout * 60) {
                LOG("[Server] Timeout reached. Shutting down automatically.\n");
                break;
            }
            continue;
        }

        socket_t c_sock = accept(s_sock, NULL, NULL);
        if (IS_INVALID_SOCKET(c_sock)) {
            if (is_transient_error()) continue;
            sock_perror("accept()");
            rc = 1;
            break;
        }
        set_io_timeout(c_sock, CLIENT_IO_TIMEOUT_SEC);
        shutdown_flag = communicate(c_sock, mecab, table_g1, table_g2);
        CLOSE_SOCKET(c_sock);
        last_activity = time(NULL);
    }

    if (g_stop) {
        LOG("[Server] Termination signal received.\n");
    }

cleanup:
    if (mecab != NULL) mecab_destroy(mecab);
    lou_free();
    if (!IS_INVALID_SOCKET(s_sock)) CLOSE_SOCKET(s_sock);
    if (lock_held) release_lock(g_lock_file);
    if (rc == 0) {
        LOG("[Server] Server stopped cleanly.\n");
    }
    return rc;
}

int main(int argc, char *argv[]) {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        fprintf(stderr, "Error: WSAStartup failed.\n");
        return 1;
    }
#endif
    int rc = run_server(argc, argv);
#ifdef _WIN32
    WSACleanup();
#endif
    return rc;
}
