// client.c
// macOS / Linux: gcc -Wall -Werror -Wextra client.c -o client
// MSYS2 UCRT64 : gcc -Wall -Werror -Wextra client.c -o client.exe -lws2_32

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  typedef int socket_t;
  #define CLOSE_SOCKET(s) close(s)
  #define IS_INVALID_SOCKET(s) ((s) < 0)
  #define SLEEP_SEC(s) sleep(s)
#endif

#define SERVER_PORT 8080
#define SERVER_ADDR "127.0.0.1"

#define MAXSTRING 65536
#define BUF_SIZE  (MAXSTRING + 2)

// 再試行設定（定数）
#define MAX_RETRIES 5       // 最大試行回数
#define RETRY_DELAY_SEC 2   // 再試行までの待ち時間（秒）

int communication(socket_t sock, const char type) {
  char send_buf[BUF_SIZE];
  char recv_buf[BUF_SIZE];

  // サーバーに送る文字列を取得
  send_buf[0] = type;
  send_buf[1] = ':';
  if (scanf("%65535[\x01-\xff]", send_buf + 2) != 1) {
      fprintf(stderr, "Error: failed to read input.\n");
      return -1;
  }
  // 文字列を送信
  int send_size = send(sock, send_buf, (int)strlen(send_buf) + 1, 0);
  if (send_size == -1) {
      perror("Error: send()");
      return -1;
  }

  // サーバーから文字列を受信
  int recv_size = recv(sock, recv_buf, BUF_SIZE - 1, 0);
  if (recv_size == -1) {
      perror("Error: recv()");
      return -1;
  }

  // recv_size で末尾を確定し、NUL 終端を保証
  recv_buf[recv_size] = '\0';

  // 応答が0バイト目にNULの場合はサーバー終了応答 (X:)
  if (recv_buf[0] == '\0') {
      printf("Exited from the connection to a server\n");
      return 0;
  }

  printf("%s", recv_buf);
  return 0;
}

int main(int argc, char *argv[]) {
#ifdef _WIN32
  WSADATA wsaData;
  if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
    fprintf(stderr, "Error: WSAStartup failed.\n");
    return -1;
  }
#endif

  char bind_addr[128];
  snprintf(bind_addr, sizeof(bind_addr), "%s", SERVER_ADDR);
  int bind_port = SERVER_PORT;

  if (argc < 2 || argc > 3) {
    fprintf(stderr, "Usage: %s J|1|2|X [addr:port]\n", argv[0]);
#ifdef _WIN32
    WSACleanup();
#endif
    return -1;
  }

  char *type = argv[1];

  if (strlen(argv[1]) != 1 || (type[0] != 'J' && type[0] != '1' && type[0] != '2' && type[0] != 'X')) {
    fprintf(stderr, "Error: type must be J, 1, 2 or X.\n");
#ifdef _WIN32
    WSACleanup();
#endif
    return -1;
  }

  if (argc == 3) {
    char tmp[128];
    snprintf(tmp, sizeof(tmp), "%s", argv[2]);
    char *colon = strrchr(tmp, ':'); // IPv4 前提で最後の ':' を区切りとする
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
  }

  // 構造体を全て0にセット
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family      = AF_INET;
  addr.sin_addr.s_addr = inet_addr(bind_addr);
  addr.sin_port        = htons((unsigned short)bind_port);

  if (addr.sin_addr.s_addr == INADDR_NONE) {
    fprintf(stderr, "Error: invalid address: %s\n", bind_addr);
#ifdef _WIN32
    WSACleanup();
#endif
    return -1;
  }

  // サーバー接続リトライループ
  socket_t sock = IS_INVALID_SOCKET(0);
  int connected = 0;

  for (int attempt = 1; attempt <= MAX_RETRIES; attempt++) {
    // 接続試行ごとにソケットを新規作成
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (IS_INVALID_SOCKET(sock)) {
      perror("Error: socket()");
#ifdef _WIN32
      WSACleanup();
#endif
      return -1;
    }

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
      connected = 1;
      break; // 接続成功
    }

    // 接続失敗時：ソケットを破棄して待機
    CLOSE_SOCKET(sock);

    if (attempt < MAX_RETRIES) {
      fprintf(stderr, "[Client] Server not ready. Retrying in %d second(s)... (%d/%d)\n",
              RETRY_DELAY_SEC, attempt, MAX_RETRIES);
      SLEEP_SEC(RETRY_DELAY_SEC);
    }
  }

  if (!connected) {
    fprintf(stderr, "Error: Could not connect to server after %d attempts.\n", MAX_RETRIES);
#ifdef _WIN32
    WSACleanup();
#endif
    return -1;
  }

  // 接続済のソケットでデータのやり取り
  communication(sock, type[0]);

  // ソケット通信をクローズ
  CLOSE_SOCKET(sock);

#ifdef _WIN32
  WSACleanup();
#endif
  return 0;
}
