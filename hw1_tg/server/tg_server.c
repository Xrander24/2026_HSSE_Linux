#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define FIFO_REQ "/tmp/tg_request"
#define FIFO_RESP "/tmp/tg_response"
#define NUM_CHATS 3
#define MAX_MSGS 10
#define MAX_MSG_LEN 256
#define BUF_SIZE 8192

struct message {
  char text[MAX_MSG_LEN];
};
struct chat {
  struct message msgs[MAX_MSGS];
  int count;
};

static struct chat chats[NUM_CHATS];

static const char *uid_to_name(unsigned int uid) {
  switch (uid) {
  case 0:
    return "root";
  case 1000:
    return "Мария";
  case 1001:
    return "Владимир";
  case 1002:
    return "Алиса";
  default:
    return NULL;
  }
}

static void init_chats(void) {
  strcpy(chats[0].msgs[0].text, "[Мария] Привет!");
  chats[0].count = 1;
  strcpy(chats[1].msgs[0].text, "[Алиса] Всем привет!");
  chats[1].count = 1;

  strcpy(chats[2].msgs[0].text, "[Максим] Кто идёт на митап?");
  strcpy(chats[2].msgs[1].text, "[Анна] Я иду!");
  chats[2].count = 2;
}

static void chat_add(int id, const char *text) {
  struct chat *c = &chats[id];
  int i;
  if (c->count == MAX_MSGS) {
    for (i = 1; i < MAX_MSGS; i++)
      c->msgs[i - 1] = c->msgs[i];
    c->count--;
  }
  strncpy(c->msgs[c->count].text, text, MAX_MSG_LEN - 1);
  c->msgs[c->count].text[MAX_MSG_LEN - 1] = '\0';
  c->count++;
}

static void process(const char *req, char *resp, int resp_max) {
  printf("[server] запрос: %s\n", req);
  fflush(stdout);

  if (strncmp(req, "READ:", 5) == 0) {
    int id = atoi(req + 5);
    int pos = 0, i;

    if (id < 0 || id >= NUM_CHATS) {
      snprintf(resp, resp_max, "ERR:bad chat id\n");
      return;
    }

    struct chat *c = &chats[id];
    for (i = 0; i < c->count; i++)
      pos += snprintf(resp + pos, resp_max - pos, "%s\n", c->msgs[i].text);

    if (pos == 0)
      snprintf(resp, resp_max, "(нет сообщений)\n");
    return;
  }

  if (strncmp(req, "WRITE:", 6) == 0) {
    char tmp[BUF_SIZE];
    strncpy(tmp, req + 6, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    char *p = tmp;

    char *c1 = strchr(p, ':');
    if (!c1) {
      snprintf(resp, resp_max, "ERR:bad format\n");
      return;
    }
    *c1 = '\0';
    int id = atoi(p);

    char *c2 = strchr(c1 + 1, ':');
    if (!c2) {
      snprintf(resp, resp_max, "ERR:bad format\n");
      return;
    }
    *c2 = '\0';
    unsigned int uid = (unsigned int)atoi(c1 + 1);

    char *c3 = strchr(c2 + 1, ':');
    if (!c3) {
      snprintf(resp, resp_max, "ERR:bad format\n");
      return;
    }
    *c3 = '\0';
    char *comm = c2 + 1;

    char *msg = c3 + 1;

    if (id < 0 || id >= NUM_CHATS) {
      snprintf(resp, resp_max, "ERR:bad chat id\n");
      return;
    }

    if (strlen(msg) == 0) {
      snprintf(resp, resp_max, "ERR:empty message\n");
      return;
    }

    const char *name = uid_to_name(uid);
    if (name == NULL)
      name = comm;

    char full_msg[MAX_MSG_LEN];
    snprintf(full_msg, sizeof(full_msg), "[%s] %s", name, msg);

    chat_add(id, full_msg);
    printf("[server] chat_%d ← %s\n", id, full_msg);
    fflush(stdout);
    snprintf(resp, resp_max, "OK\n");
    return;
  }

  snprintf(resp, resp_max, "ERR:unknown command\n");
}

static volatile int running = 1;
static void on_signal(int s) {
  (void)s;
  running = 0;
}

int main(void) {
  char req[BUF_SIZE];
  char resp[BUF_SIZE];
  int req_fd, resp_fd, n;

  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);

  init_chats();

  mkfifo(FIFO_REQ, 0666);
  mkfifo(FIFO_RESP, 0666);

  printf("[server] запущен\n");
  printf("[server] чатов: %d", NUM_CHATS);
  printf("[server] ожидаем запросы от модуля...\n");
  fflush(stdout);

  while (running) {
    req_fd = open(FIFO_REQ, O_RDONLY);
    if (req_fd < 0) {
      if (running)
        perror("open req");
      break;
    }

    n = read(req_fd, req, sizeof(req) - 1);
    close(req_fd);

    if (n <= 0)
      continue;
    req[n] = '\0';
    if (n > 0 && req[n - 1] == '\n')
      req[--n] = '\0';

    process(req, resp, sizeof(resp));

    resp_fd = open(FIFO_RESP, O_WRONLY);
    if (resp_fd < 0) {
      if (running)
        perror("open resp");
      break;
    }
    write(resp_fd, resp, strlen(resp));
    close(resp_fd);

    printf("[server] ответ: %s", resp);
    fflush(stdout);
  }

  unlink(FIFO_REQ);
  unlink(FIFO_RESP);
  printf("[server] остановлен\n");
  return 0;
}
