#include "npc_brain.h"
#include "npc_brain_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <set>
#include <thread>
#include <utility>

#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "config.h"
#include "debug.h"
#include "game_config.h"

namespace fallout {

namespace {

    using Clock = std::chrono::steady_clock;

    constexpr unsigned int kPersonaRefreshMs = 60000;
    constexpr unsigned int kPersonaTimeoutMs = 1500;
    // The engine waits a bit longer than the service, which answers with a
    // fallback line by its own timeout.
    constexpr unsigned int kTimeoutMarginMs = 3000;
    constexpr size_t kMaxResponseBytes = 256 * 1024;

    // ---- JSON -------------------------------------------------------------

    using namespace brainJson;

    // ---- HTTP -------------------------------------------------------------

    struct HttpEndpoint {
        std::string host;
        std::string port;
        std::string basePath;
    };

    bool parseEndpoint(const std::string& url, HttpEndpoint* endpoint)
    {
        const char* prefix = "http://";
        if (url.compare(0, strlen(prefix), prefix) != 0) {
            return false;
        }
        std::string rest = url.substr(strlen(prefix));
        size_t slash = rest.find('/');
        std::string hostPort = slash == std::string::npos ? rest : rest.substr(0, slash);
        endpoint->basePath = slash == std::string::npos ? "" : rest.substr(slash);
        while (!endpoint->basePath.empty() && endpoint->basePath.back() == '/') {
            endpoint->basePath.pop_back();
        }

        if (!hostPort.empty() && hostPort[0] == '[') {
            size_t close = hostPort.find(']');
            if (close == std::string::npos) {
                return false;
            }
            endpoint->host = hostPort.substr(1, close - 1);
            endpoint->port = close + 1 < hostPort.size() && hostPort[close + 1] == ':' ? hostPort.substr(close + 2) : "80";
        } else {
            size_t colon = hostPort.rfind(':');
            endpoint->host = colon == std::string::npos ? hostPort : hostPort.substr(0, colon);
            endpoint->port = colon == std::string::npos ? "80" : hostPort.substr(colon + 1);
        }
        return !endpoint->host.empty() && !endpoint->port.empty();
    }

    long msLeft(Clock::time_point deadline)
    {
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        return left > 0 ? static_cast<long>(left) : 0;
    }

    bool decodeChunked(const std::string& body, std::string* out)
    {
        size_t pos = 0;
        for (;;) {
            size_t lineEnd = body.find("\r\n", pos);
            if (lineEnd == std::string::npos) {
                return false;
            }
            unsigned long size = strtoul(body.c_str() + pos, nullptr, 16);
            pos = lineEnd + 2;
            if (size == 0) {
                return true;
            }
            if (pos + size > body.size()) {
                return false;
            }
            out->append(body, pos, size);
            pos += size + 2;
        }
    }

    // Returns the HTTP status, or -1 (no connection, timeout, bad answer).
    int httpRequest(const HttpEndpoint& endpoint, const char* method, const std::string& path, const std::string& token, const std::string& body, unsigned int timeoutMs, std::string* responseBody, std::string* error)
    {
#ifdef _WIN32
        *error = "not supported on this platform";
        return -1;
#else
        Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);

        struct addrinfo hints;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        struct addrinfo* addresses = nullptr;
        if (getaddrinfo(endpoint.host.c_str(), endpoint.port.c_str(), &hints, &addresses) != 0 || addresses == nullptr) {
            *error = "cannot resolve " + endpoint.host;
            return -1;
        }

        int fd = -1;
        for (struct addrinfo* address = addresses; address != nullptr && fd == -1; address = address->ai_next) {
            int candidate = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
            if (candidate == -1) {
                continue;
            }
            fcntl(candidate, F_SETFL, fcntl(candidate, F_GETFL, 0) | O_NONBLOCK);
            int rc = connect(candidate, address->ai_addr, address->ai_addrlen);
            if (rc != 0 && errno == EINPROGRESS) {
                struct pollfd pfd = { candidate, POLLOUT, 0 };
                if (poll(&pfd, 1, static_cast<int>(msLeft(deadline))) == 1) {
                    int socketError = 0;
                    socklen_t length = sizeof(socketError);
                    getsockopt(candidate, SOL_SOCKET, SO_ERROR, &socketError, &length);
                    rc = socketError == 0 ? 0 : -1;
                }
            }
            if (rc == 0) {
                fd = candidate;
            } else {
                close(candidate);
            }
        }
        freeaddrinfo(addresses);

        if (fd == -1) {
            *error = "cannot connect to " + endpoint.host + ":" + endpoint.port;
            return -1;
        }

        std::string request = std::string(method) + " " + endpoint.basePath + path + " HTTP/1.1\r\n";
        request += "Host: " + endpoint.host + "\r\n";
        request += "Connection: close\r\n";
        request += "Accept: application/json\r\n";
        if (!token.empty()) {
            request += "Authorization: Bearer " + token + "\r\n";
        }
        if (!body.empty()) {
            request += "Content-Type: application/json\r\n";
            request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        }
        request += "\r\n";
        request += body;

#ifdef MSG_NOSIGNAL
        const int sendFlags = MSG_NOSIGNAL;
#else
        const int sendFlags = 0;
#endif

        size_t sent = 0;
        while (sent < request.size()) {
            struct pollfd pfd = { fd, POLLOUT, 0 };
            if (poll(&pfd, 1, static_cast<int>(msLeft(deadline))) != 1) {
                close(fd);
                *error = "timeout sending";
                return -1;
            }
            ssize_t n = send(fd, request.data() + sent, request.size() - sent, sendFlags);
            if (n <= 0) {
                if (n == -1 && (errno == EAGAIN || errno == EINTR)) {
                    continue;
                }
                close(fd);
                *error = "send failed";
                return -1;
            }
            sent += static_cast<size_t>(n);
        }

        std::string raw;
        char buffer[4096];
        for (;;) {
            struct pollfd pfd = { fd, POLLIN, 0 };
            int ready = poll(&pfd, 1, static_cast<int>(msLeft(deadline)));
            if (ready != 1) {
                close(fd);
                *error = "timeout waiting for the answer";
                return -1;
            }
            ssize_t n = recv(fd, buffer, sizeof(buffer), 0);
            if (n == 0) {
                break;
            }
            if (n < 0) {
                if (errno == EAGAIN || errno == EINTR) {
                    continue;
                }
                close(fd);
                *error = "receive failed";
                return -1;
            }
            raw.append(buffer, static_cast<size_t>(n));
            if (raw.size() > kMaxResponseBytes) {
                close(fd);
                *error = "answer too big";
                return -1;
            }
        }
        close(fd);

        size_t headerEnd = raw.find("\r\n\r\n");
        if (raw.compare(0, 5, "HTTP/") != 0 || headerEnd == std::string::npos) {
            *error = "not an HTTP answer";
            return -1;
        }

        size_t space = raw.find(' ');
        int status = space == std::string::npos ? -1 : atoi(raw.c_str() + space + 1);

        std::string headers = raw.substr(0, headerEnd);
        for (char& ch : headers) {
            if (ch >= 'A' && ch <= 'Z') {
                ch = static_cast<char>(ch - 'A' + 'a');
            }
        }

        std::string payload = raw.substr(headerEnd + 4);
        if (headers.find("transfer-encoding: chunked") != std::string::npos) {
            std::string decoded;
            if (!decodeChunked(payload, &decoded)) {
                *error = "bad chunked answer";
                return -1;
            }
            payload.swap(decoded);
        } else {
            size_t lengthHeader = headers.find("content-length:");
            if (lengthHeader != std::string::npos) {
                size_t length = strtoul(headers.c_str() + lengthHeader + 15, nullptr, 10);
                if (payload.size() < length) {
                    *error = "truncated answer";
                    return -1;
                }
                payload.resize(length);
            }
        }

        *responseBody = std::move(payload);
        return status;
#endif
    }

    // ---- State ------------------------------------------------------------

    struct BrainConfig {
        bool enabled = false;
        HttpEndpoint endpoint;
        std::string token;
        unsigned int timeoutMs = 25000;
    };

    struct BrainState {
        std::mutex mutex;
        BrainConfig config;
        std::set<int> pids;
        bool allDialogueNpcs = false;
        bool fetching = false;
        bool fetchedOnce = false;
        Clock::time_point lastFetch;
    };

    // Never freed: detached threads hold it while they run, also at exit.
    BrainState* gBrain = nullptr;

    void logLine(const char* format, const std::string& text)
    {
        debugPrint(format, text.c_str());
    }

    void fetchPersonas(BrainState* brain, BrainConfig config)
    {
        std::string body;
        std::string error;
        int status = httpRequest(config.endpoint, "GET", "/v1/npc/personas", config.token, "", kPersonaTimeoutMs, &body, &error);

        std::set<int> pids;
        bool allDialogueNpcs = false;
        bool ok = false;
        if (status == 200) {
            JsonValue root;
            JsonParser parser(body);
            if (parser.parse(&root)) {
                const JsonValue* all = root.get("all_dialogue_npcs");
                allDialogueNpcs = all != nullptr && all->type == JsonValue::Type::Bool && all->boolean;
                const JsonValue* list = root.get("pids");
                if (list != nullptr && list->type == JsonValue::Type::Array) {
                    ok = true;
                    for (const JsonValue& value : list->array) {
                        if (value.type == JsonValue::Type::Number) {
                            pids.insert(static_cast<int>(value.number));
                        }
                    }
                }
            }
            if (!ok) {
                error = "bad persona list";
            }
        } else if (status != -1) {
            error = "HTTP " + std::to_string(status);
        }

        std::lock_guard<std::mutex> lock(brain->mutex);
        // A failed fetch turns free talk off until the service answers again.
        brain->pids = ok ? std::move(pids) : std::set<int>();
        brain->allDialogueNpcs = ok && allDialogueNpcs;
        brain->fetching = false;
        brain->fetchedOnce = true;
        brain->lastFetch = Clock::now();
        if (!ok) {
            logLine("\nNPC brain: offline (%s)\n", error);
        }
    }

    void requestPersonaRefresh(BrainState* brain, bool force)
    {
        BrainConfig config;
        {
            std::lock_guard<std::mutex> lock(brain->mutex);
            if (!brain->config.enabled || brain->fetching) {
                return;
            }
            auto age = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - brain->lastFetch).count();
            if (!force && brain->fetchedOnce && age < kPersonaRefreshMs) {
                return;
            }
            brain->fetching = true;
            config = brain->config;
        }
        std::thread(fetchPersonas, brain, config).detach();
    }

    class TalkCall : public NpcBrainCall {
    public:
        NpcBrainStatus status() override
        {
            return status_.load();
        }

        NpcBrainResponse response() override
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return response_;
        }

        void finish(NpcBrainStatus status, NpcBrainResponse response)
        {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                response_ = std::move(response);
            }
            status_.store(status);
        }

    private:
        std::atomic<NpcBrainStatus> status_ { NpcBrainStatus::Pending };
        std::mutex mutex_;
        NpcBrainResponse response_;
    };

    void runTalk(std::shared_ptr<TalkCall> call, BrainConfig config, std::string body, std::vector<std::string> allowedIntents)
    {
        std::string responseBody;
        NpcBrainResponse response;
        int status = httpRequest(config.endpoint, "POST", "/v1/npc/talk", config.token, body, config.timeoutMs + kTimeoutMarginMs, &responseBody, &response.error);
        if (status == 200 && npcBrainParseResponse(responseBody, allowedIntents, &response)) {
            call->finish(NpcBrainStatus::Done, std::move(response));
            return;
        }

        if (status != -1) {
            response.error = "HTTP " + std::to_string(status) + (status == 200 ? " (bad answer)" : "");
        }
        logLine("\nNPC brain: talk failed (%s)\n", response.error);
        call->finish(NpcBrainStatus::Failed, std::move(response));
    }

} // namespace

void npcBrainInit()
{
    if (gBrain == nullptr) {
        gBrain = new BrainState();
    }

    BrainConfig config;
    configGetBool(&gGameConfig, "ai", "enabled", &config.enabled, false);

    char* server = nullptr;
    if (configGetString(&gGameConfig, "ai", "server", &server, "http://127.0.0.1:8820") && server != nullptr) {
        if (!parseEndpoint(server, &config.endpoint)) {
            logLine("\nNPC brain: bad server address %s (only http://host:port)\n", server);
            config.enabled = false;
        }
    }

    char* token = nullptr;
    if (configGetString(&gGameConfig, "ai", "token", &token, "") && token != nullptr) {
        config.token = token;
    }

    int timeoutMs = 25000;
    configGetInt(&gGameConfig, "ai", "timeout_ms", &timeoutMs, 25000);
    config.timeoutMs = static_cast<unsigned int>(timeoutMs < 1000 ? 1000 : (timeoutMs > 120000 ? 120000 : timeoutMs));

    {
        std::lock_guard<std::mutex> lock(gBrain->mutex);
        gBrain->config = config;
        gBrain->pids.clear();
        gBrain->fetchedOnce = false;
    }

    if (config.enabled) {
        logLine("\nNPC brain: %s\n", "enabled");
        requestPersonaRefresh(gBrain, true);
    }
}

void npcBrainExit()
{
    if (gBrain == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(gBrain->mutex);
    gBrain->config.enabled = false;
    gBrain->pids.clear();
}

bool npcBrainOffersTalk(int pid)
{
    if (gBrain == nullptr) {
        return false;
    }

    requestPersonaRefresh(gBrain, false);

    std::lock_guard<std::mutex> lock(gBrain->mutex);
    return gBrain->config.enabled && (gBrain->pids.count(pid) != 0
        || (gBrain->allDialogueNpcs && pid > 0x01000000 && pid < 0x02000000));
}

unsigned int npcBrainTimeoutMs()
{
    if (gBrain == nullptr) {
        return 25000 + kTimeoutMarginMs;
    }

    std::lock_guard<std::mutex> lock(gBrain->mutex);
    return gBrain->config.timeoutMs + kTimeoutMarginMs;
}

std::shared_ptr<NpcBrainCall> npcBrainTalk(const NpcBrainRequest& request)
{
    auto call = std::make_shared<TalkCall>();

    BrainConfig config;
    if (gBrain != nullptr) {
        std::lock_guard<std::mutex> lock(gBrain->mutex);
        config = gBrain->config;
    }

    if (!config.enabled) {
        NpcBrainResponse response;
        response.error = "disabled";
        call->finish(NpcBrainStatus::Failed, response);
        return call;
    }

    std::thread(runTalk, call, config, npcBrainRequestToJson(request), request.allowedIntents).detach();
    return call;
}

} // namespace fallout
