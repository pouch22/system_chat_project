#include "chat.h"
#include <curl/curl.h>
#include <cjson/cJSON.h>

#define DEFAULT_OLLAMA_URL   "http://127.0.0.1:11434/api/chat"
#define DEFAULT_OLLAMA_MODEL "qwen3:8b"

typedef struct {
    char *data;
    size_t len;
} MemBuf;

typedef struct {
    char *prompt_text;
    int write_fd;
} SummaryJob;

void ai_summary_init(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

void ai_summary_cleanup(void) {
    curl_global_cleanup();
}

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t add = size * nmemb;
    MemBuf *buf = (MemBuf *)userdata;
    char *next = realloc(buf->data, buf->len + add + 1);
    if (!next) return 0;
    buf->data = next;
    memcpy(buf->data + buf->len, ptr, add);
    buf->len += add;
    buf->data[buf->len] = '\0';
    return add;
}

static void write_str_to_pipe(int fd, const char *str) {
    size_t len = strlen(str);
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, str + off, len - off);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            break;
        }
        off += (size_t)n;
    }
}

/* 혹시 <think>...</think> 블록이 섞여 나올 경우 앞부분 제거 */
static const char *skip_think_block(const char *s) {
    const char *end = strstr(s, "</think>");
    if (end) {
        s = end + strlen("</think>");
        while (*s == '\n' || *s == '\r' || *s == ' ') s++;
    }
    return s;
}

static void *summary_worker(void *arg) {
    SummaryJob *job = (SummaryJob *)arg;
    const char *url = getenv("OLLAMA_URL");
    const char *model = getenv("OLLAMA_MODEL");
    if (!url || !url[0]) url = DEFAULT_OLLAMA_URL;
    if (!model || !model[0]) model = DEFAULT_OLLAMA_MODEL;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", model);
    cJSON_AddBoolToObject(root, "think", 0);
    cJSON_AddBoolToObject(root, "stream", 0);

    cJSON *messages = cJSON_AddArrayToObject(root, "messages");
    cJSON *user_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(user_msg, "role", "user");
    cJSON_AddStringToObject(user_msg, "content", job->prompt_text);
    cJSON_AddItemToArray(messages, user_msg);

    char *req_body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!req_body) {
        write_str_to_pipe(job->write_fd, "요청 JSON 생성 실패\n");
        goto done;
    }

    CURL *curl = curl_easy_init();
    MemBuf resp = { calloc(1, 1), 0 };
    if (!curl || !resp.data) {
        free(req_body);
        free(resp.data);
        if (curl) curl_easy_cleanup(curl);
        write_str_to_pipe(job->write_fd, "CURL 초기화 실패\n");
        goto done;
    }

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req_body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    if (res != CURLE_OK) {
        write_str_to_pipe(job->write_fd,
            "AI 서버에 연결할 수 없습니다. (Ollama 실행 여부 및 네트워크를 확인하세요.)\n");
    } else {
        cJSON *json = cJSON_Parse(resp.data);
        cJSON *msg = json ? cJSON_GetObjectItem(json, "message") : NULL;
        cJSON *content = msg ? cJSON_GetObjectItem(msg, "content") : NULL;
        cJSON *err_msg = json ? cJSON_GetObjectItem(json, "error") : NULL;

        if (cJSON_IsString(content) && content->valuestring[0]) {
            const char *clean = skip_think_block(content->valuestring);
            write_str_to_pipe(job->write_fd, clean[0] ? clean : content->valuestring);
        } else if (cJSON_IsString(err_msg)) {
            char err[384];
            snprintf(err, sizeof(err), "Ollama 오류(HTTP %ld): %.280s\n",
                     http_code, err_msg->valuestring);
            write_str_to_pipe(job->write_fd, err);
        } else {
            char err[128];
            snprintf(err, sizeof(err), "Ollama 응답 파싱 실패 (HTTP %ld)\n", http_code);
            write_str_to_pipe(job->write_fd, err);
        }
        if (json) cJSON_Delete(json);
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(req_body);
    free(resp.data);

done:
    close(job->write_fd);
    free(job->prompt_text);
    free(job);
    return NULL;
}

int request_ai_summary_async(const Packet *history, int count, int write_fd) {
    size_t cap = 512 + (size_t)count * (MAX_NAME_LEN + MAX_MSG_LEN + 16);
    char *buf = malloc(cap);
    if (!buf) return -1;

    size_t off = (size_t)snprintf(buf, cap,
        "다음은 리눅스 터미널 채팅방의 최근 대화입니다. "
        "핵심 내용을 한국어로 3줄 이내로 짧게 요약해 주세요. "
        "마크다운 기호(*, # 등)나 인사말은 쓰지 말고 각 줄은 '- '로 시작하세요.\n\n");

    for (int i = 0; i < count && off + 1 < cap; i++) {
        int n = snprintf(buf + off, cap - off, "[%s]: %s\n",
                         history[i].sender, history[i].content);
        if (n > 0) off += (size_t)n;
    }

    SummaryJob *job = malloc(sizeof(SummaryJob));
    if (!job) {
        free(buf);
        return -1;
    }
    job->prompt_text = buf;
    job->write_fd = write_fd;

    pthread_t tid;
    if (pthread_create(&tid, NULL, summary_worker, job) != 0) {
        free(buf);
        free(job);
        return -1;
    }
    pthread_detach(tid);
    return 0;
}