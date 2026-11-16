#include "chat.h"
#include <locale.h>
#include <wchar.h>
#include <ncursesw/ncurses.h>

#define MAX_HISTORY 200

static pthread_mutex_t ui_lock = PTHREAD_MUTEX_INITIALIZER;

static WINDOW *chat_win;
static WINDOW *input_win;
static int chat_h, chat_w;

static char history[MAX_HISTORY][MAX_MSG_LEN + MAX_NAME_LEN + 64];
static int history_count = 0;
static int scroll_offset = 0;

static int sock_fd = -1;
static char my_nickname[MAX_NAME_LEN];
static volatile int is_running = 1;

/* 상단 대화창 다시 그리기 */
static void redraw_chat_window(void) {
    werase(chat_win);

    int visible_lines = chat_h;
    int end_idx = history_count - scroll_offset;
    if (end_idx < 0) end_idx = 0;

    int start_idx = end_idx - visible_lines;
    if (start_idx < 0) start_idx = 0;

    for (int i = start_idx; i < end_idx; i++) {
        int actual_idx = i % MAX_HISTORY;
        wprintw(chat_win, "%s\n", history[actual_idx]);
    }

    if (scroll_offset > 0) {
        mvwprintw(chat_win, 0, chat_w - 25, "[과거 대화 (%d줄 위)]", scroll_offset);
    }

    wrefresh(chat_win);
}

/* 대화 내역 버퍼에 한 줄 추가 및 UI 갱신 */
static void add_message_to_ui(const char *sender, const char *msg, MessageType type, time_t ts) {
    pthread_mutex_lock(&ui_lock);

    struct tm *tm_info = localtime(&ts);
    char time_str[16] = "00:00";
    if (tm_info != NULL) {
        strftime(time_str, sizeof(time_str), "%H:%M", tm_info);
    }

    int idx = history_count % MAX_HISTORY;
    if (type == MSG_TYPE_NOTICE) {
        snprintf(history[idx], sizeof(history[idx]), "=== [%s] %s ===", time_str, msg);
    } else if (type == MSG_TYPE_WHISPER) {
        snprintf(history[idx], sizeof(history[idx]), "[%s] (귓속말) [%s]: %s", time_str, sender, msg);
    } else {
        snprintf(history[idx], sizeof(history[idx]), "[%s] [%s]: %s", time_str, sender, msg);
    }
    history_count++;

    scroll_offset = 0;
    redraw_chat_window();

    pthread_mutex_unlock(&ui_lock);
}

/* 하단 입력창 다시 그리기 */
static void render_input_window(const wchar_t *wbuf) {
    pthread_mutex_lock(&ui_lock);
    werase(input_win);
    box(input_win, 0, 0);
    mvwprintw(input_win, 0, 2, "[ 입력 (↑/↓: 스크롤 | /w 이름 할말: 귓속말 | /quit: 종료) ]");
    mvwaddwstr(input_win, 1, 2, wbuf);
    wrefresh(input_win);
    pthread_mutex_unlock(&ui_lock);
}

/* 백그라운드 수신 전용 스레드 */
static void *receive_worker(void *arg) {
    (void)arg;
    Packet pkt;

    while (is_running) {
        ssize_t bytes = recv_all(sock_fd, &pkt, PACKET_SIZE);
        if (bytes <= 0) {
            if (is_running) {
                add_message_to_ui("시스템", "서버와 연결이 끊어졌습니다. 아무 키나 누르면 종료됩니다.", MSG_TYPE_NOTICE, time(NULL));
                is_running = 0;
            }
            break;
        }

        add_message_to_ui(pkt.sender, pkt.content, pkt.type, pkt.timestamp);
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "사용법: %s <닉네임>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    strncpy(my_nickname, argv[1], MAX_NAME_LEN - 1);
    my_nickname[MAX_NAME_LEN - 1] = '\0';

    setlocale(LC_ALL, "");
    signal(SIGPIPE, SIG_IGN);

    /* 1. 서버 도메인 소켓 연결 */
    sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("소켓 생성 실패");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("서버 연결 실패 (먼저 ./server 를 실행하세요)");
        close(sock_fd);
        exit(EXIT_FAILURE);
    }

    /* 2. TUI 초기화 */
    initscr();
    cbreak();
    noecho();

    int parent_y, parent_x;
    getmaxyx(stdscr, parent_y, parent_x);

    int input_h = 4;
    chat_h = parent_y - input_h;
    chat_w = parent_x;

    chat_win = newwin(chat_h, chat_w, 0, 0);
    input_win = newwin(input_h, chat_w, chat_h, 0);
    keypad(input_win, TRUE);

    /* 3. 수신 스레드 먼저 가동 (입장 직후 날아오는 이전 대화 기록을 받기 위함) */
    pthread_t recv_thread;
    if (pthread_create(&recv_thread, NULL, receive_worker, NULL) != 0) {
        endwin();
        perror("스레드 생성 실패");
        close(sock_fd);
        exit(EXIT_FAILURE);
    }

    /* 4. 입장(JOIN) 패킷 전송 */
    Packet join_pkt;
    memset(&join_pkt, 0, PACKET_SIZE);
    join_pkt.type = MSG_TYPE_JOIN;
    strncpy(join_pkt.sender, my_nickname, MAX_NAME_LEN - 1);
    join_pkt.timestamp = time(NULL);
    send_all(sock_fd, &join_pkt, PACKET_SIZE);

    wchar_t wbuf[MAX_MSG_LEN];
    int wlen = 0;
    wbuf[0] = L'\0';

    render_input_window(wbuf);

    /* 5. 메인 키보드 입력 루프 */
    while (is_running) {
        wint_t ch;
        int ret = wget_wch(input_win, &ch);

        if (!is_running) break;

        /* 방향키 및 페이지 스크롤 처리 */
        if (ret == KEY_CODE_YES) {
            pthread_mutex_lock(&ui_lock);
            if (ch == KEY_UP) {
                if (scroll_offset + 1 < history_count) scroll_offset += 1;
                redraw_chat_window();
            } else if (ch == KEY_DOWN) {
                if (scroll_offset - 1 >= 0) scroll_offset -= 1;
                redraw_chat_window();
            } else if (ch == KEY_PPAGE) {
                if (scroll_offset + 5 < history_count) scroll_offset += 5;
                redraw_chat_window();
            } else if (ch == KEY_NPAGE) {
                if (scroll_offset - 5 >= 0) scroll_offset -= 5;
                else scroll_offset = 0;
                redraw_chat_window();
            }
            pthread_mutex_unlock(&ui_lock);
            render_input_window(wbuf);
            continue;
        }

        /* 엔터 입력 시 전송 */
        if (ch == L'\n' || ch == L'\r') {
            if (wlen > 0) {
                char utf8_msg[MAX_MSG_LEN];
                memset(utf8_msg, 0, sizeof(utf8_msg));
                wcstombs(utf8_msg, wbuf, sizeof(utf8_msg) - 1);

                if (strcmp(utf8_msg, "/quit") == 0) {
                    is_running = 0;
                    Packet leave_pkt;
                    memset(&leave_pkt, 0, PACKET_SIZE);
                    leave_pkt.type = MSG_TYPE_LEAVE;
                    strncpy(leave_pkt.sender, my_nickname, MAX_NAME_LEN - 1);
                    send_all(sock_fd, &leave_pkt, PACKET_SIZE);
                    break;
                }

                /* 귓속말 명령어 파싱 (/w 대상이름 할말) */
                if (strncmp(utf8_msg, "/w ", 3) == 0) {
                    char target[MAX_NAME_LEN] = {0};
                    char whisper_body[MAX_MSG_LEN] = {0};
                    if (sscanf(utf8_msg + 3, "%31s %[^\n]", target, whisper_body) == 2) {
                        Packet w_pkt;
                        memset(&w_pkt, 0, PACKET_SIZE);
                        w_pkt.type = MSG_TYPE_WHISPER;
                        strncpy(w_pkt.sender, my_nickname, MAX_NAME_LEN - 1);
                        strncpy(w_pkt.receiver, target, MAX_NAME_LEN - 1);
                        strncpy(w_pkt.content, whisper_body, MAX_MSG_LEN - 1);
                        w_pkt.timestamp = time(NULL);

                        send_all(sock_fd, &w_pkt, PACKET_SIZE);
                        add_message_to_ui(my_nickname, whisper_body, MSG_TYPE_WHISPER, w_pkt.timestamp);
                    } else {
                        add_message_to_ui("시스템", "사용법: /w 상대방닉네임 메시지내용", MSG_TYPE_NOTICE, time(NULL));
                    }
                } else {
                    /* 일반 전체 채팅 전송 */
                    Packet send_pkt;
                    memset(&send_pkt, 0, PACKET_SIZE);
                    send_pkt.type = MSG_TYPE_CHAT;
                    strncpy(send_pkt.sender, my_nickname, MAX_NAME_LEN - 1);
                    strncpy(send_pkt.content, utf8_msg, MAX_MSG_LEN - 1);
                    send_pkt.timestamp = time(NULL);

                    send_all(sock_fd, &send_pkt, PACKET_SIZE);
                    add_message_to_ui(my_nickname, utf8_msg, MSG_TYPE_CHAT, send_pkt.timestamp);
                }

                wlen = 0;
                wbuf[0] = L'\0';
            }
        } else if (ch == 127 || ch == 8 || ch == KEY_BACKSPACE) {
            if (wlen > 0) {
                wlen--;
                wbuf[wlen] = L'\0';
            }
        } else {
            if (wlen < MAX_MSG_LEN - 1) {
                wbuf[wlen++] = (wchar_t)ch;
                wbuf[wlen] = L'\0';
            }
        }

        render_input_window(wbuf);
    }

    /* 6. 종료 정리 */
    is_running = 0;
    close(sock_fd);
    pthread_cancel(recv_thread);
    pthread_join(recv_thread, NULL);

    delwin(chat_win);
    delwin(input_win);
    endwin();

    return 0;
}