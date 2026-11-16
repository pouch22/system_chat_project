#include "chat.h"

/* 파일 자물쇠 잠그기 ( 쓰기 잠금: F_WRLCK, 읽기 잠금: F_RDLCK ) */
static int lock_file(int fd, short lock_type) {
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = lock_type;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0; /* 0이면 파일 전체 잠금 */

    while (fcntl(fd, F_SETLKW, &fl) == -1) {
        if (errno == EINTR) continue;
        return -1;
    }
    return 0;
}

/* 파일 자물쇠 풀기 */
static int unlock_file(int fd) {
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_UNLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;

    return fcntl(fd, F_SETLK, &fl);
}

/* 1. 새 채팅 패킷을 파일 맨 뒤에 기록 (open, write, fsync) */
int log_message(const Packet *pkt) {
    int fd = open(HISTORY_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        perror("로그 파일 열기 실패");
        return -1;
    }

    /* 다른 프로세스가 동시에 쓰지 못하도록 쓰기 잠금 */
    lock_file(fd, F_WRLCK);

    ssize_t written = send_all(fd, pkt, PACKET_SIZE);
    
    /* 디스크에 즉시 반영 보장 */
    fsync(fd);

    unlock_file(fd);
    close(fd);

    return (written == (ssize_t)PACKET_SIZE) ? 0 : -1;
}

/* 2. 최근 대화 N개를 파일에서 읽어오기 (open, lseek, read) */
int load_recent_history(Packet *out_packets, int max_count) {
    int fd = open(HISTORY_FILE, O_RDONLY);
    if (fd < 0) {
        /* 아직 생성된 로그 파일이 없으면 0개 반환 */
        return 0;
    }

    lock_file(fd, F_RDLCK);

    /* 파일 전체 크기를 구해서 저장된 총 패킷 개수 계산 */
    off_t file_size = lseek(fd, 0, SEEK_END);
    if (file_size <= 0) {
        unlock_file(fd);
        close(fd);
        return 0;
    }

    int total_packets = (int)(file_size / PACKET_SIZE);
    int to_read = (total_packets > max_count) ? max_count : total_packets;
    int start_index = total_packets - to_read;

    /* 읽어올 시작 위치로 파일 오프셋 이동 */
    lseek(fd, (off_t)start_index * PACKET_SIZE, SEEK_SET);

    int count = 0;
    for (int i = 0; i < to_read; i++) {
        if (recv_all(fd, &out_packets[count], PACKET_SIZE) == (ssize_t)PACKET_SIZE) {
            count++;
        } else {
            break;
        }
    }

    unlock_file(fd);
    close(fd);
    return count;
}