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
#define SOCKET_PATH     "/tmp/test.sock"                /* 통신에 사용할 도메인 소켓 경로 ex /tmp/sysprog_chat.sock */ 
#define HISTORY_FILE    "testchat.log"                  /* 대화 기록을 저장할 파일 이름 ex chat_history.log */
#define MAX_CLIENTS     16                              /* 최대 동시 접속 인원 */
#define MAX_NAME_LEN    32                              /* 닉네임 최대 길이 (바이트) */
#define MAX_MSG_LEN     512                             /* 한 번에 보낼 수 있는 메시지 최대 길이 */
#define RECENT_HISTORY  20                              /* 261122 JM 추가, 신규 입장 시 불러올 최근 대화 개수 */

/* =========================================================================
 * 2. 메시지 종류 
 * ========================================================================= */
typedef enum {
    MSG_TYPE_JOIN = 1,         /* 누군가 방에 처음 들어옴 (입장) */
    MSG_TYPE_CHAT,             /* 모두에게 보내는 일반 단톡방 채팅 */
    MSG_TYPE_WHISPER,          /* 특정 사람에게만 보내는 귓속말 */
    MSG_TYPE_LEAVE,            /* 정상적으로 방을 나감 (퇴장) */
    MSG_TYPE_NOTICE,            /* 서버가 전체에게 띄우는 시스템 공지 */
    MSG_TYPE_USERLIST          /* 261122 JM추가. 접속자 리스트 동기화를 위한 패킷 */
} MessageType;

/* =========================================================================
 * 3. 데이터 패킷 구조체 (데이터 주고받는건 이걸로)
 * ========================================================================= */
typedef struct {
    MessageType type;                 /* 메시지 종류 (입장, 채팅, 공지 등) */
    char sender[MAX_NAME_LEN];        /* 보낸 사람의 닉네임 */
    char receiver[MAX_NAME_LEN];      /* 받는 사람의 닉네임 (귓속말) */
    char content[MAX_MSG_LEN];        /* 실제 채팅 내용 */
    time_t timestamp;                 /* 편지를 보낸 시간 (서버가 기록할 때 사용) */
} Packet;

#define PACKET_SIZE sizeof(Packet)    /* 패킷의 전체 크기 (고정 크기) */

/* =========================================================================
 * 4. 안전한 송수신을 위한 도우미 함수 (부분 전송 방지)
 * - 네트워크 지연으로 데이터가 잘려서 오는 것을 막아줍니다.
 * - 클라이언트/서버 모두 write/read 대신 아래 함수를 사용하세요.
 * - 사실 필요하지는 않을 수도 있지만 사용중에 터미널을 늘리거나 인터럽트가 발생하게 되면 에러가 날 수 있으니 예외처리 차원에서 사용합니다.
 * ========================================================================= */

/* 패킷 전체를 한 번에 안전하게 보내는 함수 */
static inline ssize_t send_all(int sock_fd, const void *buf, size_t len) {
    size_t total_sent = 0; // 옮긴 데이터 수
    const char *ptr = (const char *)buf; // 옮길 데이터가 있는 주소

    // 전부 데이터 옮길때까지 반복
    while (total_sent < len) {
        // 남은 데이터만 보내기
        ssize_t sent = write(sock_fd, ptr + total_sent, len - total_sent);
        // 에러처리
        if (sent <= 0) {
            // 만약 EINTR에러면 계속 진행 인터럽트 에러
            if (sent < 0 && (errno ==    EINTR || errno == EAGAIN)) continue;
            return -1; /* 에러 발생 */
        }
        // 옮긴 데이터 누적
        total_sent += sent;
    }
    return (ssize_t)total_sent; /* 전송 성공한 바이트 수 반환 */
}
/*
send 예시
Packet my_box;                              // 1. 빈 패킷 생성
memset(&my_box, 0, PACKET_SIZE);            //    초기화
my_box.type = MSG_TYPE_CHAT;                // 2. 내용물 채우기
strcpy(my_box.sender, "JM");                //    (보내는 사람 이름 적기)
strcpy(my_box.content, "Hello world!");     //    (채팅 내용 적기)
my_box.timestamp = time(NULL);              //    (지금 시간 적기)

// 3. 서버(sock_fd)로 패킹 던지기
send_all(sock_fd, &my_box, PACKET_SIZE);
*/




/* 패킷 전체를 한 번에 안전하게 읽어오는 함수 */
static inline ssize_t recv_all(int sock_fd, void *buf, size_t len) {
    size_t total_received = 0;
    char *ptr = (char *)buf;

    while (total_received < len) {
        ssize_t received = read(sock_fd, ptr + total_received, len - total_received);
        if (received <= 0) {
            if (received < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            return received; /* 0: 상대방이 나감, -1: 에러 발생 */
        }
        total_received += received;
    }
    return (ssize_t)total_received; /* 수신 성공한 바이트 수 반환 */
}
/* 파일 기록 모듈 (old_chat.c) 연동 함수 */
int log_message(const Packet *pkt);
int load_recent_history(Packet *out_packets, int max_count);

#endif /* CHAT_H */
