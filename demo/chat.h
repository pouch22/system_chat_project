#ifndef CHAT_H
#define CHAT_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>

/* =========================================================================
 * 1. 시스템 통신 기본 설정
 * ========================================================================= */
#define SOCKET_PATH     "/tmp/sysprog_chat.sock"
#define HISTORY_FILE    "chat_history.log"
#define MAX_CLIENTS     16
#define MAX_NAME_LEN    32
#define MAX_MSG_LEN     512
#define RECENT_HISTORY  20   /* 신규 입장 시 불러올 최근 대화 개수 */

/* =========================================================================
 * 2. 메시지 종류
 * ========================================================================= */
typedef enum {
    MSG_TYPE_JOIN = 1,     /* 방 입장 */
    MSG_TYPE_CHAT,         /* 일반 전체 채팅 */
    MSG_TYPE_WHISPER,      /* 귓속말 (/w 닉네임 내용) */
    MSG_TYPE_LEAVE,        /* 방 퇴장 */
    MSG_TYPE_NOTICE        /* 시스템 공지 */
} MessageType;

/* =========================================================================
 * 3. 데이터 패킷 구조체 (고정 크기 규격)
 * ========================================================================= */
typedef struct {
    MessageType type;
    char sender[MAX_NAME_LEN];
    char receiver[MAX_NAME_LEN];
    char content[MAX_MSG_LEN];
    time_t timestamp;
} Packet;

#define PACKET_SIZE sizeof(Packet)

/* =========================================================================
 * 4. 안전한 송수신을 위한 도우미 함수 (부분 전송 및 시그널 방어)
 * ========================================================================= */
static inline ssize_t send_all(int sock_fd, const void *buf, size_t len) {
    size_t total_sent = 0;
    const char *ptr = (const char *)buf;

    while (total_sent < len) {
        ssize_t sent = write(sock_fd, ptr + total_sent, len - total_sent);
        if (sent <= 0) {
            if (sent < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            return -1;
        }
        total_sent += sent;
    }
    return (ssize_t)total_sent;
}

static inline ssize_t recv_all(int sock_fd, void *buf, size_t len) {
    size_t total_received = 0;
    char *ptr = (char *)buf;

    while (total_received < len) {
        ssize_t received = read(sock_fd, ptr + total_received, len - total_received);
        if (received <= 0) {
            if (received < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            return received;
        }
        total_received += received;
    }
    return (ssize_t)total_received;
}

/* =========================================================================
 * 5. 파일 기록 모듈 (old_chat.c) 연동 함수 선언
 * ========================================================================= */
int log_message(const Packet *pkt);
int load_recent_history(Packet *out_packets, int max_count);

#endif /* CHAT_H */