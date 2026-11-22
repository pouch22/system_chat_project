#include "chat.h"
#include <sys/epoll.h>

#define MAX_EVENTS 32

typedef struct {
    int fd;
    char nickname[MAX_NAME_LEN];
    int active;
} ClientInfo;

static ClientInfo clients[MAX_CLIENTS];
static int server_fd = -1;
static int epoll_fd = -1;

/* Ctrl+C 종료 시 소켓 파일을 깔끔하게 지우는 시그널 핸들러 */
static void handle_sigint(int sig) {
    (void)sig;
    printf("\n[서버] 종료 신호 감지. 자원을 정리합니다...\n");
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active) close(clients[i].fd);
    }
    if (server_fd != -1) close(server_fd);
    if (epoll_fd != -1) close(epoll_fd);
    unlink(SOCKET_PATH);
    exit(0);
}

/* 특정 클라이언트를 제외한 모두에게 패킷 전송 (exclude_fd가 -1이면 전원에게 전송) */
static void broadcast_packet(const Packet *pkt, int exclude_fd) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && clients[i].fd != exclude_fd) {
            send_all(clients[i].fd, pkt, PACKET_SIZE);
        }
    }
}

/* 261122 JM 추가, chat.h에도 패킷 추가 완료(MSG_TYPE_USERLIST) */
/* 현재 접속 중인 모든 사용자 닉네임을 모아서 전원에게 전송 */
static void broadcast_user_list(void) {
    Packet pkt;
    memset(&pkt, 0, PACKET_SIZE);
    pkt.type = MSG_TYPE_USERLIST;
    strncpy(pkt.sender, "시스템", MAX_NAME_LEN - 1);

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active) {
            size_t cur_len = strlen(pkt.content);
            snprintf(pkt.content + cur_len, MAX_MSG_LEN - cur_len, "%s\n", clients[i].nickname);
        }
    }
    printf("\n접속자 명단\n==========\n%s\n", pkt.content);
    broadcast_packet(&pkt, -1);
}


/* 귓속말 전송 */
static void send_whisper(const Packet *pkt, int sender_fd) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && strcmp(clients[i].nickname, pkt->receiver) == 0) {
            send_all(clients[i].fd, pkt, PACKET_SIZE);
            return;
        }
    }
    /* 상대를 못 찾은 경우 보낸 사람에게 안내 */
    Packet err_pkt;
    memset(&err_pkt, 0, PACKET_SIZE);
    err_pkt.type = MSG_TYPE_NOTICE;
    strncpy(err_pkt.sender, "시스템", MAX_NAME_LEN - 1);
    snprintf(err_pkt.content, MAX_MSG_LEN, "'%s' 님을 찾을 수 없습니다.", pkt->receiver);
    send_all(sender_fd, &err_pkt, PACKET_SIZE);
}

/* 클라이언트 퇴장 및 정리 */
static void remove_client(int fd) {
    char left_name[MAX_NAME_LEN] = {0};

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && clients[i].fd == fd) {
            strncpy(left_name, clients[i].nickname, MAX_NAME_LEN - 1);
            clients[i].active = 0;
            clients[i].fd = -1;
            clients[i].nickname[0] = '\0';
            break;
        }
    }

    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
    close(fd);

    if (strlen(left_name) > 0) {
        printf("[서버] 퇴장: %s (fd: %d)\n", left_name, fd);
        Packet notice;
        memset(&notice, 0, PACKET_SIZE);
        notice.type = MSG_TYPE_NOTICE;
        strncpy(notice.sender, "시스템", MAX_NAME_LEN - 1);
        snprintf(notice.content, MAX_MSG_LEN, "%s 님이 퇴장하셨습니다.", left_name);
        notice.timestamp = time(NULL);
        broadcast_packet(&notice, -1);
    }
    broadcast_user_list(); /* 261122 JM 추가: 누군가 나가면 목록 갱신 */
}

int main(void) {
    /* 클라이언트가 갑자기 종료될 때 서버가 죽지 않도록 SIGPIPE 무시 */
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, handle_sigint);
    signal(SIGTERM, handle_sigint);

    memset(clients, 0, sizeof(clients));

    /* 1. 유닉스 도메인 소켓 생성 */
    server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("서버 소켓 생성 실패");
        exit(EXIT_FAILURE);
    }

    unlink(SOCKET_PATH); /* 기존 잔여 소켓 파일 제거 */

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("바인드 실패");
        exit(EXIT_FAILURE);
    }

    /* 같은 서버의 다른 계정 팀원들도 접속할 수 있도록 권한 부여 */
    chmod(SOCKET_PATH, 0666);

    if (listen(server_fd, MAX_CLIENTS) < 0) {
        perror("리슨 실패");
        exit(EXIT_FAILURE);
    }

    /* 2. epoll 인스턴스 생성 및 서버 소켓 등록 */
    epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        perror("epoll 생성 실패");
        exit(EXIT_FAILURE);
    }

    struct epoll_event ev, events[MAX_EVENTS];
    ev.events = EPOLLIN;
    ev.data.fd = server_fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev);

    printf("==================================================\n");
    printf(" 리눅스 터미널 채팅 서버 가동 중\n");
    printf(" 소켓 경로: %s\n", SOCKET_PATH);
    printf(" 종료하려면 Ctrl+C\n");
    printf("==================================================\n");

    /* 3. 메인 이벤트 루프 */
    while (1) {
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait 오류");
            break;
        }

        for (int i = 0; i < nfds; i++) {
            int curr_fd = events[i].data.fd;

            /* A. 새로운 클라이언트 접속 요청 */
            if (curr_fd == server_fd) {
                int client_fd = accept(server_fd, NULL, NULL);
                if (client_fd < 0) continue;

                int slot = -1;
                for (int j = 0; j < MAX_CLIENTS; j++) {
                    if (!clients[j].active) {
                        slot = j;
                        break;
                    }
                }

                if (slot == -1) {
                    printf("[서버] 인원 초과로 접속 거부 (fd: %d)\n", client_fd);
                    close(client_fd);
                    continue;
                }

                clients[slot].fd = client_fd;
                clients[slot].active = 1;
                strcpy(clients[slot].nickname, "익명");

                ev.events = EPOLLIN;
                ev.data.fd = client_fd;
                epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev);
            }
            /* B. 기존 클라이언트의 메시지 수신 */
            else {
                Packet pkt;
                ssize_t n = recv_all(curr_fd, &pkt, PACKET_SIZE);
                if (n <= 0) {
                    remove_client(curr_fd);
                    continue;
                }

                if (pkt.type == MSG_TYPE_JOIN) {
                    /* 닉네임 등록 */
                    for (int j = 0; j < MAX_CLIENTS; j++) {
                        if (clients[j].active && clients[j].fd == curr_fd) {
                            strncpy(clients[j].nickname, pkt.sender, MAX_NAME_LEN - 1);
                            break;
                        }
                    }
                    printf("[서버] 입장: %s (fd: %d)\n", pkt.sender, curr_fd);

                    /* 1) 새로 들어온 사람에게 최근 대화 기록(최대 20개) 전송 */
                    Packet history_pkts[RECENT_HISTORY];
                    int hist_cnt = load_recent_history(history_pkts, RECENT_HISTORY);
                    for (int h = 0; h < hist_cnt; h++) {
                        send_all(curr_fd, &history_pkts[h], PACKET_SIZE);
                    }

                    /* 2) 전체에게 입장 공지 전송 */
                    Packet notice;
                    memset(&notice, 0, PACKET_SIZE);
                    notice.type = MSG_TYPE_NOTICE;
                    strncpy(notice.sender, "시스템", MAX_NAME_LEN - 1);
                    snprintf(notice.content, MAX_MSG_LEN, "%s 님이 입장하셨습니다.", pkt.sender);
                    notice.timestamp = time(NULL);
                    broadcast_packet(&notice, -1);
                    broadcast_user_list(); /* 2612 JM추가 누군가 들어오면 목록 갱신 */

                } else if (pkt.type == MSG_TYPE_CHAT) {
                    printf("[채팅] [%s]: %s\n", pkt.sender, pkt.content);
                    /* 파일에 기록 후 다른 사람들에게 브로드캐스트 */
                    log_message(&pkt);
                    broadcast_packet(&pkt, curr_fd);

                } else if (pkt.type == MSG_TYPE_WHISPER) {
                    printf("[귓속말] [%s -> %s]: %s\n", pkt.sender, pkt.receiver, pkt.content);
                    send_whisper(&pkt, curr_fd);

                } else if (pkt.type == MSG_TYPE_LEAVE) {
                    remove_client(curr_fd);
                }
            }
        }
    }

    handle_sigint(0);
    return 0;
}
