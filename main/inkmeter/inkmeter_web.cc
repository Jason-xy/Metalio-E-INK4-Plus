#include "application.h"
#include "board.h"
#include "inkmeter.h"
#include "power_policy.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_netif.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <mutex>

namespace inkmeter {
namespace {
std::mutex info_mu, control_mu;
PairingInfo info;
httpd_handle_t server = nullptr;
std::atomic<bool> wanted{false}, stop_queued{false};
std::atomic<int64_t> expiry{0};
std::atomic<uint64_t> authorization_epoch{1};
std::string session_token, ip;
std::unique_ptr<PowerNeedHold> server_network;
std::atomic<esp_timer_handle_t> idle_timer{nullptr};
unsigned attempts = 0;
constexpr int64_t kWindowUs = 600000000;
constexpr uint32_t kInternalMemory = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
constexpr char kTag[] = "InkMeterConfig";
// HTTP must not overwrite a Secure cookie left by the earlier HTTPS firmware.
constexpr char kSessionCookie[] = "ink_session_http=";
void LogMemory(const char *phase) {
  ESP_LOGI(kTag, "%s internal free=%u min=%u largest=%u stack_hwm=%u", phase,
           unsigned(heap_caps_get_free_size(kInternalMemory)),
           unsigned(heap_caps_get_minimum_free_size(kInternalMemory)),
           unsigned(heap_caps_get_largest_free_block(kInternalMemory)),
           unsigned(uxTaskGetStackHighWaterMark(nullptr)));
}

bool Equal(const std::string &a, const std::string &b) {
  if (a.size() != b.size())
    return false;
  unsigned char diff = 0;
  for (size_t n = 0; n < a.size(); ++n)
    diff |= a[n] ^ b[n];
  return diff == 0;
}
std::string RandomHex(size_t n) {
  std::vector<uint8_t> bytes(n);
  esp_fill_random(bytes.data(), bytes.size());
  constexpr char hex[] = "0123456789abcdef";
  std::string s;
  for (auto b : bytes) {
    s += hex[b >> 4];
    s += hex[b & 15];
  }
  return s;
}
bool ArmIdle(uint64_t epoch) {
  {
    std::lock_guard<std::mutex> l(info_mu);
    if (!wanted || !info.open || authorization_epoch.load() != epoch ||
        !IsActive() || expiry <= esp_timer_get_time())
      return false;
    expiry = esp_timer_get_time() + kWindowUs;
  }
  if (const auto timer = idle_timer.load()) {
    esp_timer_stop(timer);
    esp_timer_start_once(timer, kWindowUs);
  }
  return true;
}

std::string Header(httpd_req_t *req, const char *name, size_t max = 512) {
  const auto n = httpd_req_get_hdr_value_len(req, name);
  if (!n || n > max)
    return {};
  std::string s(n + 1, '\0');
  if (httpd_req_get_hdr_value_str(req, name, s.data(), s.size()) != ESP_OK)
    return {};
  s.resize(n);
  return s;
}
esp_err_t Send(httpd_req_t *req, int status, const std::string &body) {
  const char *line = status == 200   ? "200 OK"
                     : status == 204 ? "204 No Content"
                     : status == 400 ? "400 Bad Request"
                     : status == 401 ? "401 Unauthorized"
                     : status == 403 ? "403 Forbidden"
                     : status == 404 ? "404 Not Found"
                     : status == 405 ? "405 Method Not Allowed"
                     : status == 409 ? "409 Conflict"
                     : status == 413 ? "413 Payload Too Large"
                     : status == 429 ? "429 Too Many Requests"
                     : status == 502 ? "502 Bad Gateway"
                     : status == 507 ? "507 Insufficient Storage"
                                     : "503 Service Unavailable";
  httpd_resp_set_status(req, line);
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
  return httpd_resp_send(req, body.c_str(), body.size());
}
esp_err_t Failure(httpd_req_t *req, int code, const char *text) {
  auto j = Json(cJSON_CreateObject(), cJSON_Delete);
  cJSON_AddStringToObject(j.get(), "error", text);
  return Send(req, code, Dump(j.get()));
}
bool Authorized(httpd_req_t *req, uint64_t *epoch = nullptr) {
  const auto cookie = Header(req, "Cookie");
  std::string value;
  size_t pos = 0;
  while (pos < cookie.size()) {
    while (pos < cookie.size() && cookie[pos] == ' ')
      ++pos;
    const auto end = cookie.find(';', pos);
    const auto field =
        cookie.substr(pos, end == std::string::npos ? end : end - pos);
    if (field.rfind(kSessionCookie, 0) == 0) {
      if (!value.empty())
        return false;
      value = field.substr(sizeof(kSessionCookie) - 1);
    }
    if (end == std::string::npos)
      break;
    pos = end + 1;
  }
  std::lock_guard<std::mutex> l(info_mu);
  const bool valid = wanted && info.open && expiry > esp_timer_get_time() &&
                     !session_token.empty() && Equal(value, session_token);
  if (valid && epoch)
    *epoch = authorization_epoch.load();
  return valid;
}
std::string QueryValue(const std::string &query, const char *name) {
  char b[512];
  if (httpd_query_key_value(query.c_str(), name, b, sizeof(b)) != ESP_OK)
    return {};
  std::string out;
  for (size_t i = 0; b[i]; ++i) {
    if (b[i] == '+')
      out += ' ';
    else if (b[i] == '%' && b[i + 1] && b[i + 2]) {
      auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9')
          return c - '0';
        if (c >= 'a' && c <= 'f')
          return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
          return c - 'A' + 10;
        return -1;
      };
      int a = hex(b[i + 1]), c = hex(b[i + 2]);
      if (a < 0 || c < 0)
        return {};
      const char value = char((a << 4) | c);
      if (static_cast<unsigned char>(value) < 32)
        return {};
      out += value;
      i += 2;
    } else
      out += b[i];
  }
  return out;
}
esp_err_t Handler(httpd_req_t *req) try {
  const auto request_epoch = authorization_epoch.load();
  if (!wanted || !IsActive() || expiry <= esp_timer_get_time())
    return Failure(req, 403, "配置窗口已关闭，请在设备上重新开启");
  const std::string uri = req->uri, path = uri.substr(0, uri.find('?'));
  const auto origin = Header(req, "Origin");
  if (!origin.empty() && origin != "http://" + ip &&
      origin != "http://" + ip + ":80")
    return Failure(req, 403, "仅允许设备同源访问");
  const auto host = Header(req, "Host");
  if (host != ip && host != ip + ":80")
    return Failure(req, 403, "设备地址不匹配");
  if (req->method != HTTP_GET && origin.empty())
    return Failure(req, 403, "配置写入需要设备同源请求");
  if (req->method == HTTP_GET && path == "/favicon.ico")
    return Send(req, 204, "");
  if (req->method == HTTP_GET && path == "/") {
    extern const unsigned char web_start[] asm("_binary_web_html_start");
    extern const unsigned char web_end[] asm("_binary_web_html_end");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Content-Security-Policy",
                       "default-src 'self'; script-src 'self' 'unsafe-inline'; "
                       "style-src 'self' 'unsafe-inline'; "
                       "connect-src 'self'; base-uri 'none'; frame-ancestors "
                       "'none'; form-action 'self'");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    return httpd_resp_send(req, reinterpret_cast<const char *>(web_start),
                           web_end - web_start - 1);
  }
  if (req->method == HTTP_GET && path == "/api/status") {
    auto j = Json(cJSON_CreateObject(), cJSON_Delete);
    cJSON_AddStringToObject(j.get(), "device_id",
                            Board::GetInstance().GetUuid().c_str());
    cJSON_AddStringToObject(j.get(), "version",
                            esp_app_get_description()->version);
    cJSON_AddStringToObject(j.get(), "build", BuildDescription().c_str());
    cJSON_AddBoolToObject(j.get(), "paired", Authorized(req));
    cJSON_AddNumberToObject(
        j.get(), "remaining_seconds",
        std::max<int64_t>(0, (expiry - esp_timer_get_time()) / 1000000));
    return Send(req, 200, Dump(j.get()));
  }
  std::string body;
  if (req->method != HTTP_GET) {
    if (req->content_len <= 0 || req->content_len > 8192)
      return Failure(req, 413, "配置请求过大或为空");
    const auto content_type = Header(req, "Content-Type");
    if (content_type != "application/json" &&
        content_type.rfind("application/json;", 0) != 0)
      return Failure(req, 400, "需要JSON请求体");
    body.resize(req->content_len);
    size_t offset = 0;
    const int64_t receive_deadline = esp_timer_get_time() + 10000000;
    while (offset < body.size()) {
      if (!wanted || !IsActive() ||
          authorization_epoch.load() != request_epoch ||
          expiry <= esp_timer_get_time() ||
          esp_timer_get_time() >= receive_deadline)
        return Failure(req, 403, "配置窗口已关闭或请求超时");
      const int n =
          httpd_req_recv(req, body.data() + offset, body.size() - offset);
      if (n <= 0)
        return Failure(req, 400, "未能完整接收配置");
      offset += n;
    }
  }
  if (req->method == HTTP_POST && path == "/api/session") {
    auto j = Parse(body);
    const auto code = j ? Text(j.get(), "code", 8) : std::string();
    bool matched = false, locked = false, already = false, window_valid = false;
    std::string token;
    {
      std::lock_guard<std::mutex> l(info_mu);
      window_valid = wanted && info.open &&
                     authorization_epoch.load() == request_epoch &&
                     expiry > esp_timer_get_time() && IsActive();
      already = window_valid && info.paired;
      matched = window_valid && !already && j &&
                info.code.size() == 8 && Equal(code, info.code);
      if (matched) {
        session_token = RandomHex(32);
        token = session_token;
        info.paired = true;
        info.code.clear();
      } else if (window_valid && !already)
        locked = ++attempts >= 5;
    }
    if (!window_valid)
      return Failure(req, 403, "配置窗口已关闭");
    if (already)
      return Failure(req, 409, "已有配置会话，请在设备上重新开启");
    if (!matched) {
      if (locked)
        CloseConfiguration(request_epoch);
      return Failure(req, 401,
                     locked ? "连续5次失败，请在设备重新开启"
                            : "配对码无效");
    }
    if (!ArmIdle(request_epoch))
      return Failure(req, 403, "配置窗口已关闭");
    const std::string cookie =
        std::string(kSessionCookie) + token + "; Path=/; HttpOnly; SameSite=Strict";
    httpd_resp_set_hdr(req, "Set-Cookie", cookie.c_str());
    return Send(req, 200, "{\"ok\":true}");
  }
  uint64_t epoch = 0;
  if (!Authorized(req, &epoch))
    return Failure(req, 401, "请先与设备配对");
  if (epoch != request_epoch || !ArmIdle(epoch))
    return Failure(req, 403, "配置窗口已关闭");
  std::string query;
  if (path == "/api/users" || path == "/api/accounts") {
    const auto qpos = uri.find('?');
    const std::string raw =
                          qpos == std::string::npos ? "" : uri.substr(qpos + 1),
                      id = QueryValue(raw, "id");
    const bool has_id =
        raw.rfind("id=", 0) == 0 || raw.find("&id=") != std::string::npos;
    if (has_id)
      query = "id=" + id;
    else {
      const auto value = QueryValue(raw, "page");
      if (!value.empty() &&
          (value.size() > 6 ||
           value.find_first_not_of("0123456789") != std::string::npos))
        return Failure(req, 400, "页码无效");
      const long page =
          value.empty() ? 1 : std::strtol(value.c_str(), nullptr, 10);
      if (page < 1 || page > 100000)
        return Failure(req, 400, "页码无效");
      const auto search = QueryValue(raw, "search");
      if (search.size() > 128)
        return Failure(req, 400, "搜索条件过长");
      query = "page=" + std::to_string(page) +
              "&page_size=20&search=" + Encode(search);
    }
  }
  const char *method = req->method == HTTP_GET   ? "GET"
                       : req->method == HTTP_PUT ? "PUT"
                                                 : "POST";
  if (heap_caps_get_free_size(kInternalMemory) < 24 * 1024) {
    LogMemory("request-rejected");
    return Failure(req, 503, "内部内存不足，请稍后重试，旧配置仍有效");
  }
  const auto response = HandleApi(method, path, query, body, epoch);
  LogMemory("request-done");
  return Send(req, response.status, response.json);
} catch (...) {
  return Send(req, 503, "{\"error\":\"设备资源不足，请稍后重试\"}");
}
bool StopHttpServer() {
  if (!server)
    return true;
  if (httpd_stop(server) != ESP_OK) {
    // A failed stop retains the live handle, including its task and sockets.
    LogMemory("stop-retry");
    if (const auto timer = idle_timer.load()) {
      esp_timer_stop(timer);
      esp_timer_start_once(timer, 1000000);
    }
    return false;
  }
  server = nullptr;
  LogMemory("stopped");
  return true;
}
void StopServer() {
  std::lock_guard<std::mutex> control(control_mu);
  uint64_t epoch;
  {
    std::lock_guard<std::mutex> l(info_mu);
    if (wanted) {
      stop_queued = false;
      return;
    }
    epoch = authorization_epoch.load();
  }
  if (!StopHttpServer()) {
    stop_queued = false;
    return;
  }
  server_network.reset();
  {
    std::lock_guard<std::mutex> l(info_mu);
    if (epoch == authorization_epoch.load() && !wanted) {
      info.open = false;
      info.starting = false;
      info.paired = false;
      info.code.clear();
    }
    stop_queued = false;
  }
}
void RetryClose(void *) {
  const auto epoch = authorization_epoch.load();
  if (!wanted || expiry <= esp_timer_get_time())
    CloseConfiguration(epoch);
  else if (const auto timer = idle_timer.load())
    esp_timer_start_once(timer,
                         std::max<int64_t>(1, expiry - esp_timer_get_time()));
}
void StartServer(uint64_t epoch) {
  std::lock_guard<std::mutex> control(control_mu);
  const auto valid = [epoch]() {
    return wanted && authorization_epoch.load() == epoch && IsActive() &&
           expiry > esp_timer_get_time();
  };
  auto fail = [epoch](const char *message) {
    std::lock_guard<std::mutex> l(info_mu);
    if (authorization_epoch.load() != epoch)
      return;
    info.starting = false;
    info.error = message;
    wanted = false;
    expiry = 0;
    authorization_epoch++;
  };
  if (!valid()) {
    return;
  }
  if (!StopHttpServer()) {
    fail("配置服务仍在关闭，请稍后重试");
    return;
  }
  server_network.reset();
  auto need = std::make_unique<PowerNeedHold>(PowerNeed::ForegroundNetwork);
  if (!Board::GetInstance().EnsureNetworkReady(12000) || !valid()) {
    fail("请先在原厂设置连接局域网");
    return;
  }
  auto *iface = esp_netif_get_default_netif();
  esp_netif_ip_info_t address{};
  if (!iface || esp_netif_get_ip_info(iface, &address) != ESP_OK ||
      address.ip.addr == 0) {
    fail("未获取到局域网地址");
    return;
  }
  char text[20];
  std::snprintf(text, sizeof(text), IPSTR, IP2STR(&address.ip));
  ip = text;
  if (!valid()) {
    fail("配置已取消");
    return;
  }
  // Keep NVS/flash-capable task stacks internal; TLS and certificate work are gone.
  LogMemory("before-start");
  if (heap_caps_get_free_size(kInternalMemory) < 48 * 1024 ||
      heap_caps_get_largest_free_block(kInternalMemory) < 12 * 1024) {
    fail("内部内存不足，请关闭其他任务后重试");
    return;
  }
  if (!idle_timer.load()) {
    esp_timer_create_args_t timer{};
    timer.callback = RetryClose;
    timer.name = "ink_config";
    esp_timer_handle_t created_timer = nullptr;
    if (esp_timer_create(&timer, &created_timer) != ESP_OK) {
      fail("配置超时计时器不可用");
      return;
    }
    idle_timer = created_timer;
  }
  httpd_config_t conf = HTTPD_DEFAULT_CONFIG();
  conf.stack_size = 8192;
  conf.max_open_sockets = 2;
  conf.backlog_conn = 2;
  conf.lru_purge_enable = true;
  conf.max_uri_handlers = 3;
  conf.uri_match_fn = httpd_uri_match_wildcard;
  conf.recv_wait_timeout = 3;
  conf.send_wait_timeout = 3;
  conf.server_port = 80;
  if (httpd_start(&server, &conf) != ESP_OK) {
    server = nullptr;
    fail("HTTP服务启动失败，请稍后重试");
    return;
  }
  LogMemory("started");
  for (httpd_method_t method : {HTTP_GET, HTTP_POST, HTTP_PUT}) {
    httpd_uri_t route{};
    route.uri = "/*";
    route.method = method;
    route.handler = Handler;
    if (httpd_register_uri_handler(server, &route) != ESP_OK) {
      StopHttpServer();
      fail("HTTP接口注册失败");
      return;
    }
  }
  uint32_t random;
  do {
    random = esp_random();
  } while (random >= 4200000000u);
  char code[9];
  std::snprintf(code, sizeof(code), "%08u", random % 100000000u);
  {
    std::lock_guard<std::mutex> l(info_mu);
    if (valid()) {
      info.code = code;
      info.url = "http://" + ip;
      info.starting = false;
      info.open = true;
      info.paired = false;
      info.error.clear();
      session_token.clear();
      attempts = 0;
    }
  }
  if (!valid()) {
    StopHttpServer();
    fail("配置已取消");
    return;
  }
  server_network = std::move(need);
  ArmIdle(epoch);
}
} // namespace

void OpenConfiguration() {
  if (!IsActive())
    return;
  uint64_t previous_epoch, epoch;
  {
    std::lock_guard<std::mutex> l(info_mu);
    wanted = true;
    expiry = esp_timer_get_time() + kWindowUs;
    info.starting = true;
    previous_epoch = authorization_epoch.fetch_add(1);
    epoch = previous_epoch + 1;
    info.open = false;
    info.paired = false;
    info.error.clear();
    info.code.clear();
    session_token.clear();
  }
  CancelConfigurationRequests(previous_epoch);
  if (!Application::GetInstance().Schedule(
          [epoch]() {
            try {
              StartServer(epoch);
            } catch (...) {
              if (authorization_epoch.load() == epoch) {
                CloseConfiguration(epoch);
                std::lock_guard<std::mutex> l(info_mu);
                if (!wanted && authorization_epoch.load() == epoch + 1)
                  info.error = "HTTP资源不足，请重试";
              }
            }
          },
          Application::TaskDomain::Network)) {
    CloseConfiguration(epoch);
    std::lock_guard<std::mutex> l(info_mu);
    if (!wanted && authorization_epoch.load() == epoch + 1) {
      info.starting = false;
      info.error = "后台任务繁忙，请重试";
    }
  }
}
void CloseConfiguration(uint64_t expected_epoch) {
  uint64_t previous_epoch;
  {
    std::lock_guard<std::mutex> l(info_mu);
    if (expected_epoch && authorization_epoch.load() != expected_epoch)
      return;
    wanted = false;
    expiry = 0;
    info.open = false;
    info.starting = false;
    previous_epoch = authorization_epoch.fetch_add(1);
    info.paired = false;
    info.code.clear();
    session_token.clear();
  }
  CancelConfigurationRequests(previous_epoch);
  bool expected = false;
  if (!stop_queued.compare_exchange_strong(expected, true))
    return;
  if (!Application::GetInstance().Schedule([]() { StopServer(); },
                                           Application::TaskDomain::Cleanup)) {
    stop_queued = false;
    if (const auto timer = idle_timer.load()) {
      esp_timer_stop(timer);
      esp_timer_start_once(timer, 1000000);
    }
  }
}
PairingInfo GetPairingInfo() {
  std::lock_guard<std::mutex> l(info_mu);
  auto result = info;
  result.remaining_seconds =
      std::max<int64_t>(0, (expiry - esp_timer_get_time()) / 1000000);
  return result;
}
bool ConfigurationSessionValid(uint64_t epoch) {
  std::lock_guard<std::mutex> l(info_mu);
  return epoch == authorization_epoch && wanted && info.open && info.paired &&
         expiry > esp_timer_get_time();
}
} // namespace inkmeter
