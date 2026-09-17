// server.c
// macOS ビルド例:
// gcc -Wall -Werror -Wextra -I/opt/homebrew/include -I/opt/homebrew/include/liblouis -I/opt/homebrew/include/mecab -L/opt/homebrew/lib /opt/homebrew/lib/liblouis.a -lunistring -lmecab -g -O2 -o server_mac server.c
//
// static
// g++ -I/opt/homebrew/include -I/opt/homebrew/include/liblouis -I/opt/homebrew/include/mecab -L/opt/homebrew/lib /opt/homebrew/lib/liblouis.a /opt/homebrew/Cellar/mecab/0.996/lib/libmecab.a /opt/homebrew/opt/libunistring/lib/libunistring.a -liconv -g -O2 -o server_mac server.c
//
// MSYS2 UCRT64 ビルド例:
// gcc -Wall -Werror -Wextra server.c -o server_win.exe -llouis -lunistring -lmecab -lws2_32

// static
// g++ -I/ucrt64/include/liblouis -I/ucrt64/include/mecab -Wall -Werror -Wextra server.c -o server_win.exe /ucrt64/lib/liblouis.a /ucrt64/lib/libunistring.a /ucrt64/lib/libmecab.a -lws2_32 -liconv -static


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  #include <io.h>
  typedef SOCKET socket_t;
  #define CLOSE_SOCKET(s) closesocket(s)
  #define IS_INVALID_SOCKET(s) ((s) == INVALID_SOCKET)
#else
  #include <unistd.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <sys/file.h>
  #include <signal.h>
  #include <fcntl.h>
  typedef int socket_t;
  #define CLOSE_SOCKET(s) close(s)
  #define IS_INVALID_SOCKET(s) ((s) < 0)
#endif

#include <mecab.h>
#include <liblouis.h>
#include <unistr.h>

// ★環境に合わせて変更してください
#define SERVER_PORT 8080
#define SERVER_ADDR "127.0.0.1"

#define MAXSTRING 65536
#define BUF_SIZE  (MAXSTRING + 2)
#define LOCK_FILE "./brl_server.lock"

#define LOUIS_TABLE1 "./jmbraille/ueb/brl_en-ueb-g1.ctb"
#define LOUIS_TABLE2 "./jmbraille/ueb/brl_en-ueb-g2.ctb"
#define MECAB_OPTS "-r ./jmbraille/dic/mecabrc -Obraille -p"

// シグナル・タイマーから参照するためグローバルで保持
static const char *g_lock_file = LOCK_FILE;

// ----------------------------------------------------------------
// 二重起動禁止 (ロックファイル制御) のクロスプラットフォーム抽象化
// ----------------------------------------------------------------
#ifdef _WIN32
static HANDLE g_lock_handle = INVALID_HANDLE_VALUE;

static int acquire_lock(const char *lock_file) {
    g_lock_handle = CreateFileA(
        lock_file,
        GENERIC_READ | GENERIC_WRITE,
        0, // 他プロセスのアクセスを禁止 (排他ロック)
        NULL,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );
    if (g_lock_handle == INVALID_HANDLE_VALUE) {
        return -1;
    }
    return 0;
}

static void release_lock(const char *lock_file) {
    if (g_lock_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(g_lock_handle);
        g_lock_handle = INVALID_HANDLE_VALUE;
    }
    DeleteFileA(lock_file);
}
#else
static int g_lock_fd = -1;

static int acquire_lock(const char *lock_file) {
    g_lock_fd = open(lock_file, O_CREAT | O_RDWR, 0666);
    if (g_lock_fd == -1) return -1;
    if (flock(g_lock_fd, LOCK_EX | LOCK_NB) == -1) {
        close(g_lock_fd);
        g_lock_fd = -1;
        return -1;
    }
    return 0;
}

static void release_lock(const char *lock_file) {
    if (g_lock_fd != -1) {
        close(g_lock_fd);
        g_lock_fd = -1;
    }
    unlink(lock_file);
}
#endif

// ----------------------------------------------------------------
// タイマー終了処理のクロスプラットフォーム抽象化
// ----------------------------------------------------------------
#ifdef _WIN32
static HANDLE g_timer_thread = NULL;

static DWORD WINAPI timer_thread_proc(LPVOID param) {
    int minutes = *(int*)param;
    Sleep((DWORD)minutes * 60 * 1000);
    printf("\n[Server] Timeout reached. Shutting down automatically.\n");
    release_lock(g_lock_file);
    WSACleanup();
    exit(0);
    return 0;
}

static void set_timer(int timeout) {
    if (timeout <= 0) return;
    if (g_timer_thread != NULL) {
        TerminateThread(g_timer_thread, 0);
        CloseHandle(g_timer_thread);
        g_timer_thread = NULL;
    }
    static int t_val;
    t_val = timeout;
    g_timer_thread = CreateThread(NULL, 0, timer_thread_proc, &t_val, 0, NULL);
}
#else
static void handle_alarm(int sig) {
    (void)sig;
    printf("\n[Server] Timeout reached. Shutting down automatically.\n");
    release_lock(g_lock_file);
    exit(0);
}

static void set_timer(int timeout) {
    if (timeout > 0) {
        signal(SIGALRM, handle_alarm);
        alarm((unsigned int)(timeout * 60));
    }
}
#endif

// ----------------------------------------------------------------
// 1コードポイントを inbuf/typeform に書き込む
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

// ----------------------------------------------------------------
static int run_liblouis(const char *input_utf8, char *output_utf8, const char *table_path) {
    widechar inbuf[MAXSTRING];
    formtype typeform[MAXSTRING];

    int      in_len = (int)strlen(input_utf8);
    int      w_idx  = 0;
    int      i      = 0;
    formtype font   = 0;

    // --- 1. 入力文字列をパースして inbuf/typeform を直接構築 ---
    while (i < in_len) {
        if (input_utf8[i] == '\\') {

            if (i + 1 >= in_len) {
                // 末尾の孤立バックスラッシュはそのまま投入
                if (add_to_inbuf('\\', font, inbuf, typeform, &w_idx) < 0) {
                    fprintf(stderr, "Error: input buffer overflow.\n");
                    return -1;
                }
                i++;
                continue;
            }

            unsigned char next = (unsigned char)input_utf8[i + 1];

            if ('0' <= next && next <= '<') {
                // フォント属性トグル (\0 〜 \<)
                font ^= (formtype)(1 << (next - '0'));
                i += 2;

            } else if (next == 'x' || next == 'X') {
                // \xHHHH: 16進4桁エスケープ
                if (i + 6 > in_len) {
                    fprintf(stderr, "Error: incomplete hex escape sequence.\n");
                    return -1;
                }
                char hex[5];
                memcpy(hex, &input_utf8[i + 2], 4);
                hex[4] = '\0';

                char *endptr;
                unsigned long cp = strtoul(hex, &endptr, 16);
                if (*endptr != '\0') {
                    fprintf(stderr, "Error: invalid hex escape \\x%s\n", hex);
                    return -1;
                }
                if (cp > 0x10FFFF) {
                    fprintf(stderr,
                            "Error: code point U+%04lX out of Unicode range.\n", cp);
                    return -1;
                }
                if (add_to_inbuf((uint32_t)cp, font, inbuf, typeform, &w_idx) < 0) {
                    fprintf(stderr, "Error: input buffer overflow.\n");
                    return -1;
                }
                i += 6;

            } else {
                // その他のエスケープ: next 1文字を投入
                if (add_to_inbuf((uint32_t)next, font, inbuf, typeform, &w_idx) < 0) {
                    fprintf(stderr, "Error: input buffer overflow.\n");
                    return -1;
                }
                i += 2;
            }

        } else {
            // 通常のUTF-8文字: u8_mbtouc でコードポイントを取り出す
            ucs4_t cp;
            int bytes = u8_mbtouc(&cp, (const uint8_t *)&input_utf8[i],
                                  (size_t)(in_len - i));
            if (bytes <= 0) {
                // 不正バイト: 1バイトとして救済
                cp    = (unsigned char)input_utf8[i];
                bytes = 1;
            }
            if (add_to_inbuf((uint32_t)cp, font, inbuf, typeform, &w_idx) < 0) {
                fprintf(stderr, "Error: input buffer overflow.\n");
                return -1;
            }
            i += bytes;
        }
    }

    // ヌル終端
    inbuf[w_idx]    = 0;
    typeform[w_idx] = 0;
    int inlen = w_idx;

    // --- 2. liblouis による点字翻訳 ---
    widechar outbuf[MAXSTRING];
    int outlen = MAXSTRING;
    int result = lou_translateString(table_path, inbuf, &inlen,
                                     outbuf, &outlen, typeform, NULL, 0);
    if (!result) {
        fprintf(stderr, "Error: translation failure.\n");
        return -1;
    }

    // --- 3. widechar → UTF-8 変換 ---
    uint8_t *tmpbuf;
    size_t   tmplen;
    if (sizeof(widechar) == 2) {
        tmpbuf = u16_to_u8((const uint16_t *)outbuf, (size_t)outlen,
                           NULL, &tmplen);
    } else {
        tmpbuf = u32_to_u8((const uint32_t *)outbuf, (size_t)outlen,
                           NULL, &tmplen);
    }
    if (tmpbuf == NULL) {
        fprintf(stderr, "Error: UTF-8 output conversion failed.\n");
        return -1;
    }

    // BUF_SIZE に収まるか確認 (-1 はデリミタ \x7f の分)
    if ((int)tmplen >= BUF_SIZE - 1) {
        fprintf(stderr, "Error: output too long.\n");
        free(tmpbuf);
        return -1;
    }

    // デリミタ \x7f を末尾に付加して output_utf8 に書き込む
    memcpy(output_utf8, tmpbuf, tmplen);
    output_utf8[tmplen]     = '\x7f';
    output_utf8[tmplen + 1] = '\0';
    free(tmpbuf);

    return (int)tmplen + 1; // デリミタ込みのバイト数
}

// ----------------------------------------------------------------
// クライアントとの通信・処理の振り分け
// ----------------------------------------------------------------
static int communicate(socket_t c_sock, mecab_t *mecab, const char *table_g1, const char *table_g2) {
    char recv_buf[BUF_SIZE];
    char send_buf[BUF_SIZE];

    while (1) {
        int recv_size = recv(c_sock, recv_buf, BUF_SIZE - 1, 0);
        if (recv_size <= 0) break;

        // recv_size で末尾を確定し、念のため NUL 終端を保証
        recv_buf[recv_size] = '\0';

        if (strncmp(recv_buf, "X:", 2) == 0) {
            // サーバー終了コマンド
            printf("[Server] Shutdown command received.\n");
            send_buf[0] = '\0';
            send(c_sock, send_buf, 1, 0);
            return 1;

        } else if (strncmp(recv_buf, "J:", 2) == 0) {
            // MeCab 形態素解析
            const char *input_data = recv_buf + 2;
            const char *result = mecab_sparse_tostr(mecab, input_data);
            if (!(result)) {
                snprintf(send_buf, BUF_SIZE, "Error: translation failed.");
                int out_bytes = (int)strlen(send_buf);
                send(c_sock, send_buf, out_bytes, 0);
            } else {
                send(c_sock, result, (int)strlen(result) + 1, 0);
            }

        } else {
            // 末尾の CR/LF を除去
            int len = recv_size;
            while (len > 0 &&
                   (recv_buf[len - 1] == '\0' || recv_buf[len - 1] == '\n' || recv_buf[len - 1] == '\r')) {
                recv_buf[--len] = '\0';
            }
            if (strncmp(recv_buf, "1:", 2) == 0) {
                // Liblouis 点字翻訳
                const char *input_data = recv_buf + 2;
                int out_bytes = run_liblouis(input_data, send_buf, table_g1);
                if (out_bytes < 0) {
                    snprintf(send_buf, BUF_SIZE, "Error: translation failed.\x7f");
                    out_bytes = (int)strlen(send_buf);
                }
                send(c_sock, send_buf, out_bytes, 0);
            } else if (strncmp(recv_buf, "2:", 2) == 0) {
                // Liblouis 点字翻訳
                const char *input_data = recv_buf + 2;
                int out_bytes = run_liblouis(input_data, send_buf, table_g2);
                if (out_bytes < 0) {
                    snprintf(send_buf, BUF_SIZE, "Error: translation failed.\x7f");
                    out_bytes = (int)strlen(send_buf);
                }
                send(c_sock, send_buf, out_bytes, 0);

            } else {
                snprintf(send_buf, BUF_SIZE,
                         "Error: Invalid command format. Use J:, 1:, 2:, or X:\n");
                send(c_sock, send_buf, (int)strlen(send_buf) + 1, 0);
            }
        }
    }
    return 0;
}

// ----------------------------------------------------------------
// main
// ----------------------------------------------------------------
int main(int argc, char *argv[]) {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        fprintf(stderr, "Error: WSAStartup failed.\n");
        return -1;
    }
#endif

    int timeout = 0;
    const char *table_g1 = LOUIS_TABLE1;
    const char *table_g2 = LOUIS_TABLE2;
    const char *mecab_opts = MECAB_OPTS;
    char bind_addr[128]; // format-truncation エラー回避のためサイズを拡張
    snprintf(bind_addr, sizeof(bind_addr), "%s", SERVER_ADDR);
    int bind_port = SERVER_PORT;

    // 引数解析
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            timeout = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) {
            g_lock_file = argv[++i];
        } else if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            mecab_opts = argv[++i];
        } else if (strcmp(argv[i], "-g1") == 0 && i + 1 < argc) {
            table_g1 = argv[++i];
        } else if (strcmp(argv[i], "-g2") == 0 && i + 1 < argc) {
            table_g2 = argv[++i];
        } else if (strcmp(argv[i], "-a") == 0 && i + 1 < argc) {
            char tmp[128];
            snprintf(tmp, sizeof(tmp), "%s", argv[++i]);
            char *colon = strrchr(tmp, ':');
            if (colon == NULL) {
                fprintf(stderr, "Error: address must be in addr:port format (e.g. 127.0.0.1:12345)\n");
#ifdef _WIN32
                WSACleanup();
#endif
                return -1;
            }
            *colon = '\0';
            bind_port = atoi(colon + 1);
            if (bind_port <= 0 || bind_port > 65535) {
                fprintf(stderr, "Error: invalid port number: %s\n", colon + 1);
#ifdef _WIN32
                WSACleanup();
#endif
                return -1;
            }
            snprintf(bind_addr, sizeof(bind_addr), "%s", tmp);
        } else {
            fprintf(stderr, "Error: invalid option: %s.\n", argv[i]);
        }
    }

    // 二重起動禁止
    if (acquire_lock(g_lock_file) == -1) {
        fprintf(stderr, "Error: Server is already running or cannot create lock file.\n");
#ifdef _WIN32
        WSACleanup();
#endif
        return -1;
    }

    // タイムアウトの設定
    if (timeout > 0) {
        printf("[Server] Automatic shutdown timer set for %d minutes.\n", timeout);
        set_timer(timeout);
    }

    // ソケットの作成と設定
    socket_t s_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (IS_INVALID_SOCKET(s_sock)) {
        perror("Error: socket()");
        release_lock(g_lock_file);
#ifdef _WIN32
        WSACleanup();
#endif
        return -1;
    }

    int opt = 1;
    setsockopt(s_sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = inet_addr(bind_addr);
    addr.sin_port        = htons((unsigned short)bind_port);
    if (addr.sin_addr.s_addr == INADDR_NONE) {
        fprintf(stderr, "Error: invalid address: %s\n", bind_addr);
        CLOSE_SOCKET(s_sock);
        release_lock(g_lock_file);
#ifdef _WIN32
        WSACleanup();
#endif
        return -1;
    }

    if (bind(s_sock, (const struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("Error: bind()");
        CLOSE_SOCKET(s_sock);
        release_lock(g_lock_file);
#ifdef _WIN32
        WSACleanup();
#endif
        return -1;
    }

    if (listen(s_sock, 3) == -1) {
        perror("Error: listen()");
        CLOSE_SOCKET(s_sock);
        release_lock(g_lock_file);
#ifdef _WIN32
        WSACleanup();
#endif
        return -1;
    }

    // MeCab 初期化
    mecab_t *mecab = mecab_new2(mecab_opts);
    if (!mecab) {
        fprintf(stderr, "Exception: Failed to initialize MeCab.\n");
        CLOSE_SOCKET(s_sock);
        release_lock(g_lock_file);
#ifdef _WIN32
        WSACleanup();
#endif
        return -1;
    }

    printf("[Server] Listening on %s:%d\n", bind_addr, bind_port);
    printf("[Server] Mecab: %s\n", mecab_opts);
    printf("[Server] UEBG1: %s\n", table_g1);
    printf("[Server] UEBG2: %s\n", table_g2);

    int shutdown_flag = 0;
    while (!shutdown_flag) {
        socket_t c_sock = accept(s_sock, NULL, NULL);
        if (IS_INVALID_SOCKET(c_sock)) break;
        if (timeout > 0) {
            printf("[Server] Activity detected. Timer reset to %d minutes.\n", timeout);
            set_timer(timeout);
        }
        shutdown_flag = communicate(c_sock, mecab, table_g1, table_g2);
        CLOSE_SOCKET(c_sock);
    }

    // クリーンアップ
    lou_free();
    mecab_destroy(mecab);
    CLOSE_SOCKET(s_sock);
    release_lock(g_lock_file);

#ifdef _WIN32
    WSACleanup();
#endif
    printf("[Server] Server stopped cleanly.\n");

    return 0;
}
