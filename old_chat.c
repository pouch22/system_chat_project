#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/types.h>
#include "chat.h"

// 새 채팅 패킷 1개를 로그 파일 맨 뒤에 저장하는 함수 (성공 시 0, 실패 시 -1 반환)
int log_message(const Packet *pkt)
{
    if (pkt == NULL) // 전달받은 패킷 포인터가 NULL이면 잘못된 인자이므로
        return -1; // -1 반환

    int fd = open(HISTORY_FILE, O_WRONLY | O_CREAT | O_APPEND | O_SYNC, 0644); // chat_history.log를 쓰기 전용, 없으면 생성, 끝에 추가, 디스크 동기화 옵션, 권한 0644로 엶

    if (fd < 0) 
    {
        perror("파일 열기 실패");
        return -1;
    }

    ssize_t written = write(fd, pkt, PACKET_SIZE); // pkt가 가리키는 Packet 구조체 전체 크기를 파일에 씀

    if (written != (ssize_t)PACKET_SIZE) // 요청한 PACKET_SIZE만큼 정확히 쓰이지 않았다면
    {
        perror("write");
        close(fd);
        return -1;
    }

    if (close(fd) < 0) 
    {
        perror("close");
        return -1;
    }

    return 0;
}

// 최근 대화 기록을 대화 max_count개까지 읽어 out_packets 배열에 채우고 읽은 개수를 반환하는 함수
int load_recent_history(Packet *out_packets, int max_count)
{
    if (out_packets == NULL || max_count <= 0) // 결과를 담을 배열 포인터가 NULL이거나 읽어올 개수가 0이하면
        return 0; // 읽을 수 없으므로

    int fd = open(HISTORY_FILE, O_RDONLY); // 로그 파일을 읽기 전용으로 엶

    if (fd < 0) // 파일이 아직 없거나 열지 못하면
        return 0; // 기존 기록이 없는 것이므로

    off_t file_size = lseek(fd, 0, SEEK_END); // 파일 오프셋을 끝으로 0만큼 이동시켜 전체 파일 크기 구함

    if (file_size <= 0) // 파일 크기가 0이하면
    {
        close(fd);
        return 0;
    }

    int total_packets = (int)(file_size / (off_t)PACKET_SIZE); // 전체 바이트 수를 패킷 1개 크기로 나누어 저장된 총 패킷 수 계산

    if (total_packets <= 0) // 패킷이 1개도 없으면
    {
        close(fd);
        return 0; // 기록이 없으므로
    }

    int to_read = (total_packets > max_count) ? max_count : total_packets; // 총 패킷 수와 max_count 중 더 작은 값을 실제로 읽어올 개수로 정함

    int start_index = total_packets - to_read; // 뒤에서부터 to_read개를 가져오기 위해 읽기 시작할 패킷 번호 계산

    if (lseek(fd, (off_t)start_index * (off_t)PACKET_SIZE, SEEK_SET) < 0) // 시작 패킷 위치로 파일 오프셋을 이동
    {
        perror("lseek");
        close(fd);
        return 0;
    }

    int count = 0; // 배열에 성공적으로 저장한 패킷 개수를 세는 변수

    for (int i = 0; i < to_read; i++) // 필요한 개수만큼 반복하며 패킷을 차례대로 읽음
    {
        ssize_t nread = read(fd, &out_packets[count], PACKET_SIZE); // 현재 위치에서 PACKET_SIZE 바이트만큼 읽어 out_packets[count]에 저장

        if (nread != (ssize_t)PACKET_SIZE) // 한 패킷 크기만큼 읽지 못했으면
            break;
        
        count++; // 정상적으로 1개를 읽었으므로 개수 1 증가
    }

    close(fd);

    return count; // 실제로 배열에 담은 패킷 개수 반환
}