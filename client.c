#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#include "chat.h"
#include <langinfo.h>
#include <limits.h>
#include <locale.h>
#include <ncursesw/ncurses.h>
#include <wchar.h>
#include <wctype.h>

#define MAX_HISTORY 200
#define HISTORY_CHARS (MAX_MSG_LEN + MAX_NAME_LEN + 96)
#define INPUT_HEIGHT 4
#define MIN_ROWS 8
#define MIN_COLS 24
#define EDIT_HISTORY_LIMIT 100
#define CTRL_KEY(ch) ((ch) & 0x1f)
#define CURSOR_BLINK_MS 500

typedef struct {
    wchar_t text[MAX_MSG_LEN];
    size_t len, bytes, cursor, anchor;
} EditState;

typedef struct {
    EditState states[EDIT_HISTORY_LIMIT];
    size_t count;
} EditStack;

/* curses와 공유 상태는 ui_lock으로 보호한다. 소켓 I/O 중에는 잠금을 놓는다. */
static pthread_mutex_t ui_lock = PTHREAD_MUTEX_INITIALIZER;
static WINDOW *chat_win, *input_win;
static wchar_t history[MAX_HISTORY][HISTORY_CHARS];
static int history_head, history_count, scroll_offset;
static wchar_t input[MAX_MSG_LEN];
static size_t input_len, input_bytes;
static size_t input_cursor, selection_anchor, input_view_start;
static EditStack undo_stack, redo_stack;
static int running = 1;
static char status_text[160];
static char nickname[MAX_NAME_LEN];
static int sock_fd = -1;
static volatile sig_atomic_t stop_requested;
static struct timespec cursor_blink_started;
static int cursor_blink_active, cursor_visible = 1;

static void handle_stop(int sig) {
    (void)sig;
    stop_requested = 1;
}

static size_t selection_start(void) {
    return input_cursor < selection_anchor ? input_cursor : selection_anchor;
}

static size_t selection_end(void) {
    return input_cursor > selection_anchor ? input_cursor : selection_anchor;
}

/* 터미널의 기본 깜빡임 설정과 무관하게 커서 표시를 0.5초마다 전환한다.
 * 입력/이동 직후에는 즉시 표시하고, 선택 중에는 커서를 숨긴다.
 * 호출자는 ui_lock을 소유해야 한다. 수신에 의한 재렌더링은 주기를 유지한다. */
static void update_cursor_blink_locked(int reset) {
    if (!input_win || input_cursor != selection_anchor) {
        cursor_blink_active = 0;
        curs_set(0);
        return;
    }
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        curs_set(1);
        return;
    }
    if (reset || !cursor_blink_active) {
        cursor_blink_active = 1;
        cursor_visible = 1;
        cursor_blink_started = now;
    } else {
        long long elapsed_ms = (long long)(now.tv_sec - cursor_blink_started.tv_sec) * 1000
                             + (now.tv_nsec - cursor_blink_started.tv_nsec) / 1000000;
        if (elapsed_ms >= CURSOR_BLINK_MS) {
            cursor_visible = !cursor_visible;
            cursor_blink_started = now;
        }
    }
    curs_set(cursor_visible);
}

static void save_edit(EditStack *stack) {
    if (stack->count == EDIT_HISTORY_LIMIT) {
        memmove(stack->states, stack->states + 1,
                (EDIT_HISTORY_LIMIT - 1) * sizeof(EditState));
        --stack->count;
    }
    EditState *state = &stack->states[stack->count++];
    wmemcpy(state->text, input, input_len + 1);
    state->len = input_len;
    state->bytes = input_bytes;
    state->cursor = input_cursor;
    state->anchor = selection_anchor;
}

static void restore_edit_locked(EditStack *from, EditStack *to) {
    if (!from->count) {
        snprintf(status_text, sizeof(status_text), "더 이상 되돌릴 편집이 없습니다.");
        return;
    }
    save_edit(to);
    const EditState *state = &from->states[--from->count];
    wmemcpy(input, state->text, state->len + 1);
    input_len = state->len;
    input_bytes = state->bytes;
    input_cursor = state->cursor;
    selection_anchor = state->anchor;
    input_view_start = 0;
}

/* 선택 영역(또는 삭제할 범위)을 문자 하나/빈 문자열로 교체하는 단일 편집. */
static void replace_range_locked(size_t first, size_t last, wchar_t ch) {
    wchar_t replacement[MAX_MSG_LEN];
    size_t inserted = ch ? 1 : 0;
    size_t length = input_len - (last - first) + inserted;
    if (length >= MAX_MSG_LEN) {
        snprintf(status_text, sizeof(status_text), "메시지는 UTF-8 기준 최대 %d바이트입니다.", MAX_MSG_LEN - 1);
        return;
    }
    wmemcpy(replacement, input, first);
    if (ch) replacement[first] = ch;
    wmemcpy(replacement + first + inserted, input + last, input_len - last + 1);
    size_t bytes = wcstombs(NULL, replacement, 0);
    if (bytes == (size_t)-1 || bytes >= MAX_MSG_LEN) {
        snprintf(status_text, sizeof(status_text), "메시지는 UTF-8 기준 최대 %d바이트입니다.", MAX_MSG_LEN - 1);
        return;
    }
    save_edit(&undo_stack);
    redo_stack.count = 0;
    wmemcpy(input, replacement, length + 1);
    input_len = length;
    input_bytes = bytes;
    input_cursor = selection_anchor = first + inserted;
}

/* UTF-8 선택 내용을 OSC 52로 터미널 클립보드에 전달한다.
 * 서버의 X11/Wayland 대신 사용자가 보고 있는 터미널이 복사를 담당한다.
 * 지원/허용 여부는 터미널 설정에 달려 있어 성공 여부를 단정하지 않는다. */
static void copy_selection_locked(void) {
    size_t first = selection_start(), last = selection_end();
    if (first == last) {
        snprintf(status_text, sizeof(status_text), "복사할 내용을 먼저 선택하세요.");
        return;
    }
    wchar_t selected[MAX_MSG_LEN];
    wmemcpy(selected, input + first, last - first);
    selected[last - first] = L'\0';
    unsigned char utf8[MAX_MSG_LEN];
    size_t length = wcstombs((char *)utf8, selected, sizeof(utf8));
    if (length == (size_t)-1) {
        snprintf(status_text, sizeof(status_text), "복사할 문자의 인코딩 변환 실패");
        return;
    }
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char encoded[4 * ((MAX_MSG_LEN + 2) / 3) + 1];
    size_t used = 0;
    for (size_t i = 0; i < length; i += 3) {
        unsigned int word = (unsigned int)utf8[i] << 16;
        if (i + 1 < length) word |= (unsigned int)utf8[i + 1] << 8;
        if (i + 2 < length) word |= utf8[i + 2];
        encoded[used++] = alphabet[(word >> 18) & 63];
        encoded[used++] = alphabet[(word >> 12) & 63];
        encoded[used++] = i + 1 < length ? alphabet[(word >> 6) & 63] : '=';
        encoded[used++] = i + 2 < length ? alphabet[word & 63] : '=';
    }
    encoded[used] = '\0';
    if (fprintf(stdout, "\033]52;c;%s\a", encoded) < 0 || fflush(stdout) == EOF)
        snprintf(status_text, sizeof(status_text), "클립보드 복사 요청 실패");
    else
        snprintf(status_text, sizeof(status_text), "복사 요청 전송 (OSC 52 지원 터미널 필요)");
}

/* terminfo에 없는 경우도 흔한 xterm Shift+방향키 조합을 명시적으로 등록. */
static void configure_editor_keys(void) {
    define_key("\033[1;2D", KEY_SLEFT);
    define_key("\033[1;2C", KEY_SRIGHT);
    define_key("\033[1;2A", KEY_SR);
    define_key("\033[1;2B", KEY_SF);
    define_key("\033[1;2H", KEY_SHOME);
    define_key("\033[1;2F", KEY_SEND);
}

/* 잘못된 수신 바이트는 대체 문자로, 개행/제어 문자는 공백으로 표시한다. */
static void decode_text(wchar_t *out, size_t capacity, const char *text) {
    mbstate_t state = {0};
    size_t used = 0;
    while (*text && used + 1 < capacity) {
        wchar_t ch;
        size_t n = mbrtowc(&ch, text, MB_CUR_MAX, &state);
        if (n == (size_t)-1 || n == (size_t)-2) {
            memset(&state, 0, sizeof(state));
            ch = L'\ufffd';
            n = 1;
        }
        if (n == 0) break;
        out[used++] = iswprint(ch) ? ch : L' ';
        text += n;
    }
    out[used] = L'\0';
}

/* 한글의 표시 폭(보통 2칸)을 고려해 한 화면 행의 끝을 구한다. */
static size_t wrap_end(const wchar_t *text, size_t start, int width) {
    int columns = 0;
    size_t end = start;
    while (text[end]) {
        int cells = wcwidth(text[end]);
        if (cells < 0) cells = 1;
        if (columns + cells > width) break;
        columns += cells;
        ++end;
    }
    return end;
}

static int message_rows(const wchar_t *text, int width) {
    int count = 0;
    size_t pos = 0;
    do {
        pos = wrap_end(text, pos, width);
        ++count;
    } while (text[pos]);
    return count;
}

static int input_columns(size_t first, size_t last) {
    int columns = 0;
    for (size_t i = first; i < last; ++i) {
        int cells = wcwidth(input[i]);
        columns += cells < 0 ? 1 : cells;
    }
    return columns;
}

/* 호출자는 ui_lock 소유. 입력창을 마지막에 갱신하여 입력 커서를 복원한다. */
static void render_locked(void) {
    if (!chat_win || !input_win) {
        update_cursor_blink_locked(0);
        erase();
        mvaddnstr(0, 0, "Resize terminal: at least 24 x 8", COLS > 1 ? COLS - 1 : 0);
        wnoutrefresh(stdscr);
        doupdate();
        return;
    }
    int height = getmaxy(chat_win);
    int width = getmaxx(chat_win) - 2;
    int visible = height - 2;
    int total = 0;
    for (int i = 0; i < history_count; ++i)
        total += message_rows(history[(history_head + i) % MAX_HISTORY], width);
    int limit = total > visible ? total - visible : 0;
    if (scroll_offset > limit) scroll_offset = limit;
    if (scroll_offset < 0) scroll_offset = 0;
    int first = total - scroll_offset - visible;
    if (first < 0) first = 0;

    werase(chat_win);
    box(chat_win, 0, 0);
    mvwaddnstr(chat_win, 0, 2, " Chat | Up/Down PgUp/PgDn ", width - 2);
    int row = 0, drawn = 0;
    for (int i = 0; i < history_count && drawn < visible; ++i) {
        const wchar_t *text = history[(history_head + i) % MAX_HISTORY];
        size_t start = 0;
        do {
            size_t end = wrap_end(text, start, width);
            if (row >= first && drawn < visible) {
                wmove(chat_win, ++drawn, 1);
                waddnwstr(chat_win, text + start, (int)(end - start));
            }
            ++row;
            start = end;
        } while (text[start] && drawn < visible);
    }
    char footer[80];
    snprintf(footer, sizeof(footer), " %d messages | %d/%d rows up ",
             history_count, scroll_offset, limit);
    mvwaddnstr(chat_win, height - 1, 2, footer, width - 2);
    wnoutrefresh(chat_win);

    werase(input_win);
    box(input_win, 0, 0);
    mvwaddnstr(input_win, 0, 2, " Input | /w name text | /quit ", width - 2);
    mvwaddstr(input_win, 1, 1, "> ");
    /* 편집 커서가 보이도록 가로 스크롤하고 선택 영역은 반전 표시한다. */
    int available = width - 3;
    if (input_view_start > input_cursor) input_view_start = input_cursor;
    while (input_columns(input_view_start, input_cursor) > available)
        ++input_view_start;
    int columns = input_columns(input_view_start, input_cursor);
    size_t first_selected = selection_start(), last_selected = selection_end();
    int drawn_columns = 0;
    for (size_t i = input_view_start; i < input_len; ++i) {
        int cells = wcwidth(input[i]);
        if (cells < 0) cells = 1;
        if (drawn_columns + cells > available) break;
        if (i >= first_selected && i < last_selected) wattron(input_win, A_REVERSE);
        waddnwstr(input_win, input + i, 1);
        wattroff(input_win, A_REVERSE);
        drawn_columns += cells;
    }
    if (status_text[0]) {
        wchar_t status[160];
        decode_text(status, 160, status_text);
        size_t end = wrap_end(status, 0, width);
        mvwaddnwstr(input_win, 2, 1, status, (int)end);
    } else {
        char hint[100];
        snprintf(hint, sizeof(hint), "%zu/%d bytes | ^A all ^C copy ^Z undo ^Y redo",
                 input_bytes, MAX_MSG_LEN - 1);
        mvwaddnstr(input_win, 2, 1, hint, width);
    }
    wmove(input_win, 1, 3 + columns);
    update_cursor_blink_locked(0);
    wnoutrefresh(input_win);
    doupdate();
}

static void resize_locked(void) {
    if (chat_win) delwin(chat_win);
    if (input_win) delwin(input_win);
    chat_win = input_win = NULL;
    erase();
    wnoutrefresh(stdscr);
    if (LINES >= MIN_ROWS && COLS >= MIN_COLS) {
        chat_win = newwin(LINES - INPUT_HEIGHT, COLS, 0, 0);
        input_win = newwin(INPUT_HEIGHT, COLS, LINES - INPUT_HEIGHT, 0);
        if (!chat_win || !input_win) {
            if (chat_win) delwin(chat_win);
            if (input_win) delwin(input_win);
            chat_win = input_win = NULL;
        }
    }
    /* stdscr에서 키를 읽으므로 창을 다시 만들어도 입력 대상은 안정적이다. */
    render_locked();
}

static void add_message_locked(const Packet *packet) {
    char formatted[HISTORY_CHARS];
    char clock_text[16] = "00:00";
    struct tm local;
    if (localtime_r(&packet->timestamp, &local))
        strftime(clock_text, sizeof(clock_text), "%H:%M", &local);
    if (packet->type == MSG_TYPE_NOTICE)
        snprintf(formatted, sizeof(formatted), "=== [%s] %s ===", clock_text, packet->content);
    else
        snprintf(formatted, sizeof(formatted), "[%s] %s[%s]: %s", clock_text,
                 packet->type == MSG_TYPE_WHISPER ? "(귓속말) " : "",
                 packet->sender, packet->content);
    int index = (history_head + history_count) % MAX_HISTORY;
    if (history_count == MAX_HISTORY)
        history_head = (history_head + 1) % MAX_HISTORY;
    else
        ++history_count;
    decode_text(history[index], HISTORY_CHARS, formatted);
    /* 과거 대화를 읽는 중에는 수신된 메시지의 행 수만큼 보정한다. */
    if (scroll_offset > 0 && chat_win)
        scroll_offset += message_rows(history[index], getmaxx(chat_win) - 2);
    render_locked();
}

static void *receive_worker(void *arg) {
    (void)arg;
    for (;;) {
        Packet packet;
        ssize_t n = recv_all(sock_fd, &packet, PACKET_SIZE);
        pthread_mutex_lock(&ui_lock);
        if (!running) {
            pthread_mutex_unlock(&ui_lock);
            break;
        }
        if (n != (ssize_t)PACKET_SIZE) {
            snprintf(status_text, sizeof(status_text), "서버 연결 종료. 아무 키나 누르면 종료합니다.");
            running = 0;
            render_locked();
            pthread_mutex_unlock(&ui_lock);
            break;
        }
        packet.sender[MAX_NAME_LEN - 1] = '\0';
        packet.content[MAX_MSG_LEN - 1] = '\0';
        add_message_locked(&packet);
        pthread_mutex_unlock(&ui_lock);
    }
    return NULL;
}

/* 입력 시 바이트 한도를 검사하므로 wide -> UTF-8 변환 시 잘리지 않는다. */
static int prepare_message_locked(Packet *packet) {
    char text[MAX_MSG_LEN];
    if (wcstombs(text, input, sizeof(text)) == (size_t)-1) {
        snprintf(status_text, sizeof(status_text), "문자 인코딩 변환 실패");
        return 0;
    }
    memset(packet, 0, sizeof(*packet));
    packet->type = MSG_TYPE_CHAT;
    packet->timestamp = time(NULL);
    snprintf(packet->sender, sizeof(packet->sender), "%s", nickname);
    if (strcmp(text, "/quit") == 0) {
        packet->type = MSG_TYPE_LEAVE;
        return 1;
    }
    if (strcmp(text, "/w") == 0 || strncmp(text, "/w ", 3) == 0) {
        char *target = text + 2;
        while (*target == ' ') ++target;
        char *body = strchr(target, ' ');
        if (!body || body == target || (size_t)(body - target) >= MAX_NAME_LEN) {
            snprintf(status_text, sizeof(status_text), "사용법: /w 상대닉네임 내용 (닉네임 최대 31바이트)");
            return 0;
        }
        *body++ = '\0';
        while (*body == ' ') ++body;
        if (!*body) {
            snprintf(status_text, sizeof(status_text), "귓속말 내용을 입력하세요.");
            return 0;
        }
        packet->type = MSG_TYPE_WHISPER;
        memcpy(packet->receiver, target, strlen(target) + 1);
        snprintf(packet->content, sizeof(packet->content), "%s", body);
    } else {
        snprintf(packet->content, sizeof(packet->content), "%s", text);
    }
    return 1;
}

static void process_key_locked(int result, wint_t key) {
    if (result == OK) {
        switch (key) {
        case CTRL_KEY('a'):
            selection_anchor = 0;
            input_cursor = input_len;
            return;
        case CTRL_KEY('c'): copy_selection_locked(); return;
        case CTRL_KEY('z'): restore_edit_locked(&undo_stack, &redo_stack); return;
        case CTRL_KEY('y'): restore_edit_locked(&redo_stack, &undo_stack); return;
        case 27: selection_anchor = input_cursor; return;
        }
    }
    if (result == KEY_CODE_YES) {
        int page = chat_win ? getmaxy(chat_win) - 2 : 1;
        switch (key) {
        case KEY_RESIZE: resize_locked(); return;
        case KEY_UP: ++scroll_offset; return;
        case KEY_DOWN: --scroll_offset; return;
        case KEY_PPAGE: scroll_offset += page; return;
        case KEY_NPAGE: scroll_offset -= page; return;
        case KEY_SLEFT:
            if (input_cursor > 0) --input_cursor;
            return;
        case KEY_SRIGHT:
            if (input_cursor < input_len) ++input_cursor;
            return;
        case KEY_SR: case KEY_SHOME: input_cursor = 0; return;
        case KEY_SF: case KEY_SEND: input_cursor = input_len; return;
        case KEY_LEFT:
            if (selection_start() != selection_end()) input_cursor = selection_start();
            else if (input_cursor > 0) --input_cursor;
            selection_anchor = input_cursor;
            return;
        case KEY_RIGHT:
            if (selection_start() != selection_end()) input_cursor = selection_end();
            else if (input_cursor < input_len) ++input_cursor;
            selection_anchor = input_cursor;
            return;
        case KEY_HOME: input_cursor = selection_anchor = 0; return;
        case KEY_END: input_cursor = selection_anchor = input_len; return;
        case KEY_DC: {
            size_t first = selection_start(), last = selection_end();
            if (first == last && last < input_len) ++last;
            if (first != last) replace_range_locked(first, last, L'\0');
            return;
        }
        case KEY_BACKSPACE: break; /* 문자 8/127과 같은 삭제 경로로 처리 */
        default: return;
        }
    }
    if ((result == KEY_CODE_YES && key == KEY_BACKSPACE) || key == 8 || key == 127) {
        size_t first = selection_start(), last = selection_end();
        if (first == last && first > 0) --first;
        if (first != last) replace_range_locked(first, last, L'\0');
    } else if (iswprint(key)) {
        replace_range_locked(selection_start(), selection_end(), (wchar_t)key);
    }
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "사용법: %s <닉네임>\n", argv[0]);
        return EXIT_FAILURE;
    }
    if (!setlocale(LC_ALL, "") ||
        (strcmp(nl_langinfo(CODESET), "UTF-8") != 0 &&
         strcmp(nl_langinfo(CODESET), "UTF8") != 0)) {
        fprintf(stderr, "UTF-8 로케일이 필요합니다. 예: LANG=C.UTF-8 %s <닉네임>\n", argv[0]);
        return EXIT_FAILURE;
    }
    wchar_t wide_name[MAX_NAME_LEN];
    if (!argv[1][0] || strlen(argv[1]) >= MAX_NAME_LEN ||
        mbstowcs(wide_name, argv[1], MAX_NAME_LEN) == (size_t)-1) {
        fprintf(stderr, "닉네임은 올바른 UTF-8로 1~%d바이트여야 합니다.\n", MAX_NAME_LEN - 1);
        return EXIT_FAILURE;
    }
    for (size_t i = 0; wide_name[i]; ++i) {
        if (!iswprint(wide_name[i]) || iswspace(wide_name[i])) {
            fprintf(stderr, "닉네임에 공백이나 제어 문자를 사용할 수 없습니다.\n");
            return EXIT_FAILURE;
        }
    }
    snprintf(nickname, sizeof(nickname), "%s", argv[1]);
    signal(SIGPIPE, SIG_IGN);
    struct sigaction action = {0};
    action.sa_handler = handle_stop;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);

    sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("소켓 생성 실패");
        return EXIT_FAILURE;
    }
    struct sockaddr_un address = {0};
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", SOCKET_PATH);
    if (connect(sock_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("서버 연결 실패 (먼저 ./server 실행)");
        close(sock_fd);
        return EXIT_FAILURE;
    }
    if (!initscr()) {
        close(sock_fd);
        return EXIT_FAILURE;
    }
    raw(); /* Ctrl+C / Ctrl+Z를 SIGINT / SIGTSTP 대신 편집 키로 받는다. */
    noecho();
    keypad(stdscr, TRUE);
    configure_editor_keys();
    wtimeout(stdscr, 50);
    set_escdelay(25);
    curs_set(1);
    pthread_mutex_lock(&ui_lock);
    resize_locked();
    pthread_mutex_unlock(&ui_lock);
    pthread_t receiver;
    int error = pthread_create(&receiver, NULL, receive_worker, NULL);
    if (error) {
        if (chat_win) delwin(chat_win);
        if (input_win) delwin(input_win);
        endwin();
        close(sock_fd);
        fprintf(stderr, "수신 스레드 생성 실패: %s\n", strerror(error));
        return EXIT_FAILURE;
    }
    Packet join = {0};
    join.type = MSG_TYPE_JOIN;
    join.timestamp = time(NULL);
    snprintf(join.sender, sizeof(join.sender), "%s", nickname);
    int exit_status = EXIT_SUCCESS;
    if (send_all(sock_fd, &join, PACKET_SIZE) != (ssize_t)PACKET_SIZE) {
        pthread_mutex_lock(&ui_lock);
        snprintf(status_text, sizeof(status_text), "입장 전송 실패. 아무 키나 누르면 종료합니다.");
        running = 0;
        render_locked();
        pthread_mutex_unlock(&ui_lock);
        exit_status = EXIT_FAILURE;
    }
    while (!stop_requested) {
        Packet outgoing;
        int send_packet = 0;
        pthread_mutex_lock(&ui_lock);
        wint_t key;
        int result = wget_wch(stdscr, &key); /* 입력도 curses 함수이므로 잠금 필수 */
        if (!running) {
            pthread_mutex_unlock(&ui_lock);
            if (result != ERR) break;
            struct timespec pause = {0, 1000000};
            nanosleep(&pause, NULL);
            continue;
        }
        if (result != ERR) {
            status_text[0] = '\0';
            if ((result == OK && (key == L'\n' || key == L'\r')) ||
                (result == KEY_CODE_YES && key == KEY_ENTER)) {
                if (input_len > 0) send_packet = prepare_message_locked(&outgoing);
            } else {
                process_key_locked(result, key);
            }
            update_cursor_blink_locked(1);
            render_locked();
        } else {
            update_cursor_blink_locked(0);
        }
        pthread_mutex_unlock(&ui_lock);
        if (send_packet) {
            ssize_t sent = send_all(sock_fd, &outgoing, PACKET_SIZE);
            pthread_mutex_lock(&ui_lock);
            if (sent != (ssize_t)PACKET_SIZE) {
                snprintf(status_text, sizeof(status_text), "메시지 전송 실패. 아무 키나 누르면 종료합니다.");
                running = 0;
                exit_status = EXIT_FAILURE;
            } else if (outgoing.type != MSG_TYPE_LEAVE) {
                add_message_locked(&outgoing);
                input_len = input_bytes = 0;
                input_cursor = selection_anchor = input_view_start = 0;
                /* 이미 전송한 메시지는 실행 취소로 복원하지 않는다. */
                undo_stack.count = redo_stack.count = 0;
                input[0] = L'\0';
            }
            render_locked();
            pthread_mutex_unlock(&ui_lock);
            if (outgoing.type == MSG_TYPE_LEAVE) break;
        }
        /* 다음 입력 대기 전에 수신 스레드가 잠금을 얻을 기회를 준다. */
        struct timespec pause = {0, 1000000};
        nanosleep(&pause, NULL);
    }
    pthread_mutex_lock(&ui_lock);
    running = 0;
    pthread_mutex_unlock(&ui_lock);
    shutdown(sock_fd, SHUT_RDWR); /* recv_all을 깨운 다음 join한다. */
    pthread_join(receiver, NULL);
    close(sock_fd);
    if (chat_win) delwin(chat_win);
    if (input_win) delwin(input_win);
    endwin();
    return exit_status;
}
