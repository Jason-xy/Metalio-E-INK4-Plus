#include "inkmeter.h"
#include "inkmeter_clock.h"

#include "SdCardManager.hpp"
#include "application.h"
#include "board.h"
#include "power_policy.h"
#include "sd_access.h"
#include "sd_paths.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <esp_network.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_timer.h>
#include <http.h>
#include <map>
#include <mutex>
#include <nvs.h>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

namespace inkmeter {
namespace {
constexpr size_t kMaxAccounts = 64, kResponseLimit = 768 * 1024,
                 kCacheLimit = kMaxJsonBytes;
constexpr int kCacheSchema = 2;
constexpr char kCachePath[] = SD_APP_ROOT "/inkmeter/cache.json";
struct Config {
  std::string site, key, user_name;
  bool key_configured = false;
  int64_t user_id = 0;
  int minutes = 15;
  std::vector<int64_t> account_ids;
};
std::mutex state_mu, io_mu, config_mu, http_mu;
Config config;
std::array<Usage, 3> usages;
std::vector<Account> accounts;
std::vector<std::pair<int64_t, std::string>> users;
std::atomic<bool> active{false}, standby{false}, busy{false};
std::atomic<uint64_t> revision{1}, generation{1};
std::atomic<int64_t> next_sync{0};
std::atomic<int64_t> usage_retry_until{0}, snapshots_retry_until{0};
std::atomic<int64_t> calendar_day{0};
std::atomic<bool> initialized{false}, want_accounts{false};
std::atomic<int> want_users{0};
bool more_users = false;
int user_page = 1, failures = 0;
Range selected_range = Range::Today;
std::string status;
int64_t syncing_account = 0;
std::shared_ptr<Http> live_http;
uint64_t live_http_epoch = 0, account_generation = 0;
size_t account_cursor = 0;
std::map<int64_t, int64_t> cooldown;
using TaskDomain = Application::TaskDomain;

void Changed() { revision.fetch_add(1); }
bool Foreground() { return active.load() && !standby.load(); }
Config CopyConfig() {
  std::lock_guard<std::mutex> l(state_mu);
  return config;
}
void Status(const std::string &text) {
  std::lock_guard<std::mutex> l(state_mu);
  status = text;
  Changed();
}
Json Object() { return {cJSON_CreateObject(), cJSON_Delete}; }
ApiResult Error(int code, const char *message) {
  auto j = Object();
  cJSON_AddStringToObject(j.get(), "error", message);
  return {code, Dump(j.get())};
}
ApiResult Ok() { return {200, "{\"ok\":true}"}; }
void AddIds(cJSON *root, const std::vector<int64_t> &ids) {
  auto *list = cJSON_AddArrayToObject(root, "account_ids");
  for (auto id : ids)
    cJSON_AddItemToArray(list, cJSON_CreateNumber(id));
}
Json ConfigJson(const Config &c, bool secret) {
  auto j = Object();
  auto *r = j.get();
  cJSON_AddStringToObject(r, "site", c.site.c_str());
  cJSON_AddNumberToObject(r, "user_id", c.user_id);
  cJSON_AddStringToObject(r, "user_name", c.user_name.c_str());
  cJSON_AddNumberToObject(r, "usage_sync_minutes", c.minutes);
  AddIds(r, c.account_ids);
  if (secret)
    cJSON_AddStringToObject(r, "key", c.key.c_str());
  else
    cJSON_AddBoolToObject(r, "key_configured", c.key_configured);
  if (!Item(r, "site") || !Item(r, "user_id") || !Item(r, "user_name") ||
      !Item(r, "usage_sync_minutes") ||
      !Item(r, secret ? "key" : "key_configured") ||
      !cJSON_IsArray(Item(r, "account_ids")) ||
      cJSON_GetArraySize(Item(r, "account_ids")) != int(c.account_ids.size()))
    return {nullptr, cJSON_Delete};
  return j;
}
bool StoreConfig(const Config &c) {
  nvs_handle_t handle = 0;
  if (nvs_open("inkmeter", NVS_READWRITE, &handle) != ESP_OK)
    return false;
  auto j = ConfigJson(c, true);
  std::string bytes = j ? Dump(j.get()) : std::string();
  auto *key = const_cast<cJSON *>(Item(j.get(), "key"));
  if (cJSON_IsString(key) && key->valuestring)
    std::memset(key->valuestring, 0, std::strlen(key->valuestring));
  esp_err_t e = bytes.empty() ? ESP_ERR_NO_MEM
                              : nvs_set_str(handle, "config", bytes.c_str());
  if (e == ESP_OK)
    e = nvs_commit(handle);
  nvs_close(handle);
  std::fill(bytes.begin(), bytes.end(), '\0');
  return e == ESP_OK;
}
std::string ReadSecret(const std::string &expected_site) {
  nvs_handle_t handle = 0;
  if (nvs_open("inkmeter", NVS_READONLY, &handle) != ESP_OK)
    return {};
  size_t length = 0;
  std::string result, bytes;
  if (nvs_get_str(handle, "config", nullptr, &length) == ESP_OK && length > 0 &&
      length < 8192) {
    bytes.resize(length);
    if (nvs_get_str(handle, "config", bytes.data(), &length) == ESP_OK) {
      bytes.resize(length - 1);
      auto root = Parse(bytes);
      if (root) {
        std::string stored_site;
        if (NormalizeSite(Text(root.get(), "site", 384), stored_site) &&
            stored_site == expected_site)
          result = Text(root.get(), "key", 1024);
        auto *p = const_cast<cJSON *>(Item(root.get(), "key"));
        if (cJSON_IsString(p) && p->valuestring)
          std::memset(p->valuestring, 0, std::strlen(p->valuestring));
      }
    }
  }
  nvs_close(handle);
  std::fill(bytes.begin(), bytes.end(), '\0');
  return result;
}
void LoadConfig() {
  nvs_handle_t handle = 0;
  Config c;
  if (nvs_open("inkmeter", NVS_READONLY, &handle) == ESP_OK) {
    size_t len = 0;
    if (nvs_get_str(handle, "config", nullptr, &len) == ESP_OK && len > 0 &&
        len < 8192) {
      std::string bytes(len, '\0');
      if (nvs_get_str(handle, "config", bytes.data(), &len) == ESP_OK) {
        bytes.resize(len ? len - 1 : 0);
        auto j = Parse(bytes);
        if (j) {
          const double interval = Number(j.get(), "usage_sync_minutes", 15);
          if (interval >= 1 && interval <= 1440 &&
              std::floor(interval) == interval)
            c.minutes = int(interval);
        }
        std::string site;
        if (j && NormalizeSite(Text(j.get(), "site", 384), site) &&
            ValidSecret(Text(j.get(), "key", 1024))) {
          c.site = site;
          c.key = Text(j.get(), "key", 1024);
          c.key_configured = true;
          PositiveId(Item(j.get(), "user_id"), c.user_id);
          if (c.user_id > 0)
            c.user_name =
                UserDisplayName(nullptr, Text(j.get(), "user_name", 160), c.user_id);
          const cJSON *id = nullptr;
          cJSON_ArrayForEach(id, Item(j.get(), "account_ids")) {
            int64_t n;
            if (c.account_ids.size() < kMaxAccounts && PositiveId(id, n) &&
                std::find(c.account_ids.begin(), c.account_ids.end(), n) ==
                    c.account_ids.end())
              c.account_ids.push_back(n);
          }
        }
        auto *key = const_cast<cJSON *>(Item(j.get(), "key"));
        if (cJSON_IsString(key) && key->valuestring)
          std::memset(key->valuestring, 0, std::strlen(key->valuestring));
      }
      std::fill(bytes.begin(), bytes.end(), '\0');
    }
    nvs_close(handle);
  }
  std::fill(c.key.begin(), c.key.end(), '\0');
  c.key.clear();
  std::lock_guard<std::mutex> l(state_mu);
  config = std::move(c);
  initialized = true;
  Changed();
}
void SaveCache() try {
  if (!SdCardManager::GetInstance().IsMounted())
    return;
  auto root = Object();
  std::array<Usage, 3> snapshots;
  std::vector<Account> values;
  Config c;
  {
    std::lock_guard<std::mutex> l(state_mu);
    snapshots = usages;
    values = accounts;
    c = config;
  }
  cJSON_AddNumberToObject(root.get(), "schema", kCacheSchema);
  cJSON_AddStringToObject(root.get(), "site_id", SiteId(c.site).c_str());
  cJSON_AddStringToObject(root.get(), "timezone", "Asia/Hong_Kong");
  auto *u = cJSON_AddArrayToObject(root.get(), "usage");
  for (const auto &v : snapshots)
    if (v.valid && v.user_id == c.user_id)
      cJSON_AddItemToArray(u, EncodeUsage(v).release());
  auto *a = cJSON_AddArrayToObject(root.get(), "accounts");
  for (const auto &v : values)
    cJSON_AddItemToArray(a, EncodeAccount(v).release());
  const std::string bytes = Dump(root.get());
  if (bytes.empty() || bytes.size() > kCacheLimit)
    return;
  sd_access::Transaction txn;
  if (!txn || !SdEnsureDir(SD_APP_ROOT "/inkmeter"))
    return;
  const std::string tmp = std::string(kCachePath) + ".tmp",
                    previous = std::string(kCachePath) + ".previous";
  FILE *f = fopen(tmp.c_str(), "wb");
  if (!f)
    return;
  bool ok = fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
  if (fflush(f) != 0)
    ok = false;
  if (ok && fsync(fileno(f)) != 0)
    ok = false;
  if (fclose(f) != 0)
    ok = false;
  if (!ok)
    return;
  struct stat st{};
  const bool existed = stat(kCachePath, &st) == 0;
  if (existed) {
    unlink(previous.c_str());
    if (rename(kCachePath, previous.c_str()) != 0)
      return;
  }
  if (rename(tmp.c_str(), kCachePath) != 0 && existed)
    rename(previous.c_str(), kCachePath);
} catch (...) {
  // SD is optional; a cache allocation/write failure cannot undo committed
  // configuration.
}
void LoadCache() {
  sd_access::Transaction txn;
  if (!txn)
    return;
  const auto c = CopyConfig();
  for (const auto &path :
       {std::string(kCachePath), std::string(kCachePath) + ".tmp",
        std::string(kCachePath) + ".previous"}) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
      continue;
    fseek(f, 0, SEEK_END);
    const auto len = ftell(f);
    rewind(f);
    if (len <= 0 || len > long(kCacheLimit)) {
      fclose(f);
      continue;
    }
    std::string bytes(size_t(len), '\0');
    const bool ok = fread(bytes.data(), 1, bytes.size(), f) == bytes.size();
    fclose(f);
    if (!ok)
      continue;
    auto root = Parse(bytes);
    if (!root || Number(root.get(), "schema") != kCacheSchema ||
        Text(root.get(), "site_id") != SiteId(c.site) ||
        Text(root.get(), "timezone") != "Asia/Hong_Kong")
      continue;
    std::array<Usage, 3> next;
    std::vector<Account> qs;
    const cJSON *row = nullptr;
    cJSON_ArrayForEach(row, Item(root.get(), "usage")) {
      Usage u;
      if (DecodeCachedUsage(row, u) && u.user_id == c.user_id)
        next[size_t(u.range)] = std::move(u);
    }
    cJSON_ArrayForEach(row, Item(root.get(), "accounts")) {
      Account a;
      if (qs.size() < kMaxAccounts && DecodeCachedAccount(row, a) &&
          std::find(c.account_ids.begin(), c.account_ids.end(), a.id) !=
              c.account_ids.end())
        qs.push_back(std::move(a));
    }
    std::lock_guard<std::mutex> l(state_mu);
    usages = std::move(next);
    accounts = std::move(qs);
    Changed();
    break;
  }
}
struct Response {
  int status = 0, retry = 0;
  std::string body, etag;
};
Response HttpRequest(const Config &c, const Request &request,
                     const std::string &etag = {}, uint64_t token = 0,
                     uint64_t authorization = 0, int64_t deadline = 0) try {
  Response result;
  const int64_t started_at = esp_timer_get_time();
  if (!token)
    token = generation.load();
  deadline = deadline ? std::min(deadline, esp_timer_get_time() + 15000000)
                      : esp_timer_get_time() + 15000000;
  const auto valid = [&]() {
    return Foreground() && token == generation.load() &&
           esp_timer_get_time() < deadline &&
           (!authorization || ConfigurationSessionValid(authorization));
  };
  if (!valid() || c.site.empty())
    return result;
  PowerNeedHold need(PowerNeed::ForegroundNetwork);
  const int wait_ms = std::min<int64_t>(
      12000, std::max<int64_t>(1, (deadline - esp_timer_get_time()) / 1000));
  if (!Board::GetInstance().EnsureNetworkReady(wait_ms) || !valid())
    return result;
  auto *network =
      dynamic_cast<EspNetwork *>(Board::GetInstance().GetNetwork());
  if (!network) {
    result.status = -2;
    return result;
  }
  if (!EnsureClock(valid, deadline)) {
    result.status = -3;
    return result;
  }
  std::string secret = c.key.empty() ? ReadSecret(c.site) : c.key;
  if (!ValidSecret(secret)) {
    result.status = 401;
    return result;
  }
  auto client = std::shared_ptr<Http>(network->CreateHttp(0).release());
  if (!client)
    return result;
  {
    std::lock_guard<std::mutex> l(http_mu);
    live_http = client;
    live_http_epoch = authorization;
  }
  client->SetTimeout(std::min<int64_t>(
      12000, std::max<int64_t>(1, (deadline - esp_timer_get_time()) / 1000)));
  client->SetRequestDeadline(deadline);
  if (valid())
    client->SetHeader("x-api-key", secret);
  std::fill(secret.begin(), secret.end(), '\0');
  client->SetHeader("Accept", "application/json");
  if (!etag.empty())
    client->SetHeader("If-None-Match", etag);
  if (request.method == "POST")
    client->SetContent(std::string());
  if (valid() && client->Open(request.method, c.site + request.path)) {
    result.status = client->GetStatusCode();
    result.etag = client->GetResponseHeader("ETag");
    if (result.etag.size() > 256 ||
        std::any_of(result.etag.begin(), result.etag.end(),
                    [](unsigned char c) { return c < 32 || c >= 127; }))
      result.etag.clear();
    const auto retry = client->GetResponseHeader("Retry-After");
    result.retry = RetryAfterSeconds(retry, UtcNow());
    if (client->GetBodyLength() > kResponseLimit)
      result.status = 413;
    else if ((result.status >= 200 && result.status < 300) ||
             result.status == 429) {
      char chunk[1024];
      int n;
      while ((n = client->Read(chunk, sizeof(chunk))) > 0) {
        if (!valid() || result.body.size() + n > kResponseLimit) {
          result.status = valid() ? 413 : 0;
          result.body.clear();
          break;
        }
        result.body.append(chunk, n);
      }
      if (n < 0) {
        result.status = 0;
        result.body.clear();
      }
    }
  }
  const int transport_error = client->GetLastError();
  client->Close();
  ESP_LOGI("InkMeterFetch",
           "done status=%d bytes=%u error=%d elapsed_ms=%lld stack_hwm=%u internal=%u",
           result.status, unsigned(result.body.size()), transport_error,
           static_cast<long long>((esp_timer_get_time() - started_at) / 1000),
           unsigned(uxTaskGetStackHighWaterMark(nullptr)),
           unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
  if (!valid()) {
    result.status = 0;
    result.body.clear();
  }
  if (result.status == 429) {
    auto p = Parse(result.body);
    const auto *meta = p ? Item(p.get(), "metadata") : nullptr;
    if (!meta && p)
      meta = Item(p.get(), "details");
    if (!meta && p)
      meta = Item(Item(p.get(), "data"), "metadata");
    result.retry = std::max(
        result.retry, RetryAfterSeconds(Item(meta, "retry_after_seconds")));
    result.body.clear();
  }
  {
    std::lock_guard<std::mutex> l(http_mu);
    if (live_http == client) {
      live_http.reset();
      live_http_epoch = 0;
    }
  }
  return result;
} catch (...) {
  std::shared_ptr<Http> client;
  {
    std::lock_guard<std::mutex> l(http_mu);
    client = std::move(live_http);
    live_http_epoch = 0;
  }
  if (client)
    client->Close();
  return {};
}
const char *HttpError(int code) {
  if (code == -2)
    return "当前网络不支持证书验证，请使用 Wi-Fi";
  if (code == -3)
    return "设备尚未完成对时，请稍后重试";
  if (code == 401)
    return "站点凭据无效，请重新配置";
  if (code == 403)
    return "站点权限不足";
  if (code == 404)
    return "站点接口不兼容";
  if (code == 429)
    return "站点限流，请稍后重试";
  if (code == 413)
    return "响应超过设备容量，请缩小查询";
  return "网络或站点请求失败，保留缓存";
}
bool Success(const Response &r) { return r.status >= 200 && r.status < 300; }
std::string SnapshotPath(const Config &c, Range range, const std::string &first,
                         const std::string &last) {
  return "/api/v1/admin/dashboard/snapshot-v2?user_id=" +
         std::to_string(c.user_id) + "&start_date=" + first +
         "&end_date=" + last + "&timezone=Asia%2FHong_Kong&granularity=" +
         (range == Range::Today ? "hour" : "day") +
         "&include_stats=false&include_trend=true&include_model_stats=true&"
         "include_group_stats=false&"
         "include_users_trend=false";
}
enum class FetchResult { Ok, Failed, Deferred };
FetchResult FetchUsage() {
  const uint64_t token = generation.load();
  std::unique_lock<std::mutex> io(io_mu, std::try_to_lock);
  if (!io) {
    next_sync = esp_timer_get_time() + 1000000;
    return FetchResult::Deferred;
  }
  if (!Foreground() || token != generation.load())
    return FetchResult::Failed;
  if (esp_timer_get_time() < usage_retry_until) {
    Status("站点仍在限流冷却，请稍后重试");
    return FetchResult::Failed;
  }
  const auto c = CopyConfig();
  Range range;
  std::string etag;
  {
    std::lock_guard<std::mutex> l(state_mu);
    range = selected_range;
    etag = usages[size_t(range)].etag;
  }
  if (c.site.empty() || !c.key_configured || c.user_id <= 0) {
    Status("请在设备设置中开启手机配置，选择监控用户");
    return FetchResult::Failed;
  }
  const int64_t deadline = esp_timer_get_time() + 20000000;
  const auto valid = [&]() {
    return Foreground() && token == generation.load() &&
           esp_timer_get_time() < deadline;
  };
  if (!EnsureClock(valid, deadline)) {
    next_sync = std::max(esp_timer_get_time() + int64_t(5000000), ClockRetryAt());
    Status("设备尚未对时，请先连接网络");
    return FetchResult::Deferred;
  }
  const auto now = UtcNow();
  {
    const auto user_response = HttpRequest(
        c, {"GET", "/api/v1/admin/users/" + std::to_string(c.user_id)}, {},
        token, 0, deadline);
    if (token != generation.load() || !Foreground())
      return FetchResult::Failed;
    auto user = Success(user_response) ? Parse(user_response.body)
                                       : Json(nullptr, cJSON_Delete);
    const auto *user_data = user ? Data(user.get()) : nullptr;
    int64_t returned_user = 0;
    const auto *deleted_at = Item(user_data, "deleted_at");
    if (!user_data || !PositiveId(Item(user_data, "id"), returned_user) ||
        returned_user != c.user_id ||
        (deleted_at && !cJSON_IsNull(deleted_at))) {
      if (user_response.status == 429)
        usage_retry_until =
            esp_timer_get_time() +
            int64_t(std::max(60, user_response.retry)) * 1000000;
      Status(user_response.status == 404 ||
                     (deleted_at && !cJSON_IsNull(deleted_at))
                 ? "监控用户已删除或不可访问，请重新选择"
             : Success(user_response) ? "监控用户接口数据不兼容"
                                      : HttpError(user_response.status));
      return FetchResult::Failed;
    }
    const auto name = UserDisplayName(user_data, c.user_name, c.user_id);
    if (name != c.user_name) {
      auto updated = c;
      updated.user_name = name;
      updated.key = ReadSecret(c.site);
      if (ValidSecret(updated.key))
        StoreConfig(updated);
      std::fill(updated.key.begin(), updated.key.end(), '\0');
      std::lock_guard<std::mutex> l(state_mu);
      if (token != generation.load() || !Foreground())
        return FetchResult::Failed;
      config.user_name = name;
      Changed();
    }
  }
  std::string first, last;
  DateRange(range, now, first, last);
  {
    std::lock_guard<std::mutex> l(state_mu);
    const auto &old = usages[size_t(range)];
    if (old.first != first || old.last != last || old.user_id != c.user_id)
      etag.clear();
  }
  const auto r = HttpRequest(c, {"GET", SnapshotPath(c, range, first, last)},
                             etag, token, 0, deadline);
  if (token != generation.load() || !Foreground())
    return FetchResult::Failed;
  if (r.status == 304) {
    std::lock_guard<std::mutex> l(state_mu);
    if (token != generation.load() || !Foreground())
      return FetchResult::Failed;
    auto &u = usages[size_t(range)];
    if (!u.valid || u.user_id != c.user_id || u.first != first ||
        u.last != last || etag.empty()) {
      status = "站点返回无效缓存状态";
      Changed();
      return FetchResult::Failed;
    }
    u.received_at = UtcNow();
    status.clear();
    failures = 0;
    next_sync = selected_range == range
                    ? esp_timer_get_time() + int64_t(c.minutes) * 60000000
                    : 0;
    Changed();
    return FetchResult::Ok;
  }
  auto root = Success(r) ? Parse(r.body) : Json(nullptr, cJSON_Delete);
  Usage next;
  if (!root ||
      !DecodeUsage(Data(root.get()), c.user_id, range, first, last, next)) {
    if (r.status == 429)
      usage_retry_until =
          esp_timer_get_time() + int64_t(std::max(60, r.retry)) * 1000000;
    Status(Success(r) ? "统计接口数据不兼容" : HttpError(r.status));
    return FetchResult::Failed;
  }
  next.received_at = UtcNow();
  next.etag = r.etag;
  ESP_LOGI("InkMeterFetch", "usage accepted buckets=%u models=%u internal=%u",
           unsigned(next.buckets.size()), unsigned(next.models.size()),
           unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
  {
    std::lock_guard<std::mutex> l(state_mu);
    if (token != generation.load())
      return FetchResult::Failed;
    usages[size_t(range)] = std::move(next);
    status.clear();
    failures = 0;
    next_sync = selected_range == range
                    ? esp_timer_get_time() + int64_t(c.minutes) * 60000000
                    : 0;
    Changed();
  }
  SaveCache();
  return FetchResult::Ok;
}
void FetchAccounts() {
  const auto token = generation.load();
  std::unique_lock<std::mutex> io(io_mu, std::try_to_lock);
  if (!io)
    return;
  if (!Foreground() || token != generation.load())
    return;
  const auto c = CopyConfig();
  if (c.site.empty() || !c.key_configured) {
    want_accounts = false;
    return;
  }
  int64_t id = 0;
  Account a;
  {
    std::lock_guard<std::mutex> l(state_mu);
    if (account_generation != token || account_cursor >= c.account_ids.size()) {
      account_generation = token;
      account_cursor = 0;
    }
    if (c.account_ids.empty()) {
      want_accounts = false;
      return;
    }
    id = c.account_ids[account_cursor];
    auto old = std::find_if(accounts.begin(), accounts.end(),
                            [&](const Account &v) { return v.id == id; });
    if (old != accounts.end())
      a = *old;
  }
  // Yield the shared Network worker after one account, even for a 64-account
  // selection.
  const int64_t deadline = esp_timer_get_time() + 20000000;
  const auto response =
      HttpRequest(c, {"GET", "/api/v1/admin/accounts/" + std::to_string(id)},
                  {}, token, 0, deadline);
  if (response.status == 429)
    snapshots_retry_until =
        esp_timer_get_time() + int64_t(std::max(60, response.retry)) * 1000000;
  auto root =
      Success(response) ? Parse(response.body) : Json(nullptr, cJSON_Delete);
  Account current;
  if (root && DecodeAccount(Data(root.get()), current) && current.id == id) {
    if (!current.snapshot && a.snapshot && current.platform == a.platform &&
        current.type == a.type && current.ollama == a.ollama &&
        current.opencode == a.opencode) {
      current.windows = a.windows;
      current.balances = a.balances;
      current.snapshot = true;
      current.source = a.source;
      current.observed_at = a.observed_at;
      current.received_at = a.received_at;
      current.note = "保留上次采样";
    }
    const bool dto_snapshot =
        current.snapshot && current.note != "保留上次采样";
    a = std::move(current);
    if (dto_snapshot)
      a.received_at = UtcNow();
    for (const auto &req : PassiveRequests(a)) {
      if (!Foreground() || token != generation.load())
        return;
      const auto r = HttpRequest(c, req, {}, token, 0, deadline);
      if (r.status == 429)
        snapshots_retry_until =
            esp_timer_get_time() + int64_t(std::max(60, r.retry)) * 1000000;
      auto p = Success(r) ? Parse(r.body) : Json(nullptr, cJSON_Delete);
      const auto *data = p ? Data(p.get()) : nullptr;
      if (data && !QuotaPayloadFailed(data)) {
        Account candidate = a;
        if (DecodeQuota(data, candidate)) {
          a = std::move(candidate);
          a.received_at = UtcNow();
        }
      } else
        a.error =
            Success(r) ? "站点快照未更新，保留已有结果" : HttpError(r.status);
    }
  } else {
    a.id = id;
    if (a.name.empty())
      a.name = "#" + std::to_string(id);
    a.error = response.status == 404 ? "账户已删除或不可访问"
              : Success(response)    ? "账户接口数据不兼容"
                                     : HttpError(response.status);
    if (response.status == 404) {
      a.manual_supported = false;
      a.note = "请在手机重新选择账户";
    }
  }
  {
    std::lock_guard<std::mutex> l(state_mu);
    if (token != generation.load() || !Foreground())
      return;
    auto old = std::find_if(accounts.begin(), accounts.end(),
                            [&](const Account &v) { return v.id == id; });
    if (old != accounts.end())
      *old = std::move(a);
    else
      accounts.push_back(std::move(a));
    want_accounts = ++account_cursor < c.account_ids.size();
    Changed();
  }
  SaveCache();
}
void Queue(std::function<void()> work) {
  if (!Foreground())
    return;
  bool expected = false;
  if (!busy.compare_exchange_strong(expected, true))
    return;
  Changed();
  const auto ticket = generation.load();
  if (!Application::GetInstance().Schedule(
          [work = std::move(work), ticket]() {
            try {
              if (Foreground() && generation.load() == ticket)
                work();
            } catch (...) {
              if (Foreground() && generation.load() == ticket) {
                want_accounts = false;
                Status("设备资源不足，请重试");
              }
            }
            {
              std::lock_guard<std::mutex> l(state_mu);
              syncing_account = 0;
            }
            busy = false;
            Changed();
          },
          TaskDomain::Network)) {
    busy = false;
    Status("后台任务繁忙，请重试");
  }
}
ApiResult Browse(const Config &c, bool account, const std::string &query,
                 uint64_t token = 0, uint64_t authorization = 0,
                 int64_t deadline = 0) {
  const bool id_query = query.rfind("id=", 0) == 0;
  std::string endpoint =
      account ? "/api/v1/admin/accounts" : "/api/v1/admin/users";
  if (id_query) {
    const auto value = query.substr(3);
    if (value.empty() || value.size() > 16 ||
        value.find_first_not_of("0123456789") != std::string::npos ||
        std::strtoll(value.c_str(), nullptr, 10) < 1 ||
        std::strtoll(value.c_str(), nullptr, 10) > 9007199254740991LL)
      return Error(400, "请输入完整的正整数ID");
    endpoint += "/" + value;
  } else
    endpoint += "?" + query;
  const auto response =
      HttpRequest(c, {"GET", endpoint}, {}, token, authorization, deadline);
  if (!Success(response))
    return Error(response.status == 429 ? 429 : 502,
                 HttpError(response.status));
  auto root = Parse(response.body);
  const auto *data = root ? Data(root.get()) : nullptr;
  if (!data)
    return Error(502, "列表接口数据不兼容");
  if (id_query) {
    int64_t returned_id = 0;
    if (!PositiveId(Item(data, "id"), returned_id) ||
        returned_id != std::strtoll(query.substr(3).c_str(), nullptr, 10))
      return Error(502, "列表返回的对象ID不匹配");
  }
  auto out = Object();
  auto *items = cJSON_AddArrayToObject(out.get(), "items");
  const auto *rows = id_query ? nullptr : Item(data, "items");
  const auto append = [&](const cJSON *obj) {
    int64_t id;
    if (!PositiveId(Item(obj, "id"), id))
      return;
    auto *item = cJSON_CreateObject();
    cJSON_AddNumberToObject(item, "id", id);
    std::string name =
        account ? Text(obj, "name", 160)
                : UserDisplayName(obj, id == c.user_id ? c.user_name : "", id);
    if (account && name.empty())
      name = "#" + std::to_string(id);
    cJSON_AddStringToObject(item, "name", name.c_str());
    if (account) {
      cJSON_AddStringToObject(item, "platform",
                              Text(obj, "platform", 40).c_str());
      cJSON_AddStringToObject(item, "type", Text(obj, "type", 40).c_str());
      cJSON_AddStringToObject(item, "status", Text(obj, "status", 40).c_str());
    }
    cJSON_AddItemToArray(items, item);
  };
  if (id_query)
    append(data);
  else {
    if (!cJSON_IsArray(rows) || cJSON_GetArraySize(rows) > 100)
      return Error(502, "列表接口数据不兼容");
    const cJSON *row = nullptr;
    cJSON_ArrayForEach(row, rows) append(row);
  }
  cJSON_AddNumberToObject(out.get(), "total",
                          id_query ? cJSON_GetArraySize(items)
                                   : Number(data, "total"));
  cJSON_AddNumberToObject(out.get(), "page",
                          id_query ? 1 : Number(data, "page", 1));
  return {200, Dump(out.get())};
}
bool Commit(Config next) {
  next.user_name = next.user_id > 0
                       ? UserDisplayName(nullptr, next.user_name, next.user_id)
                       : "";
  if (!StoreConfig(next))
    return false;
  next.key_configured = !next.key.empty();
  std::fill(next.key.begin(), next.key.end(), '\0');
  next.key.clear();
  {
    std::lock_guard<std::mutex> l(state_mu);
    const bool site_changed = config.site != next.site,
               user_changed = config.user_id != next.user_id;
    const bool accounts_changed = config.account_ids != next.account_ids;
    config = std::move(next);
    generation++;
    if (site_changed || user_changed)
      usages = {};
    if (site_changed) {
      accounts.clear();
      users.clear();
      cooldown.clear();
      usage_retry_until = 0;
      snapshots_retry_until = 0;
    } else
      accounts.erase(
          std::remove_if(accounts.begin(), accounts.end(),
                         [&](const Account &a) {
                           return std::find(config.account_ids.begin(),
                                            config.account_ids.end(),
                                            a.id) == config.account_ids.end();
                         }),
          accounts.end());
    status.clear();
    if (accounts_changed || site_changed)
      want_accounts = true;
    for (auto it = cooldown.begin(); it != cooldown.end();)
      if (std::find(config.account_ids.begin(), config.account_ids.end(),
                    it->first) == config.account_ids.end())
        it = cooldown.erase(it);
      else
        ++it;
    next_sync = 0;
    Changed();
  }
  SaveCache();
  return true;
}
} // namespace

void Enter() {
  active = true;
  standby = false;
  next_sync = 0;
  const auto now = UtcNow();
  calendar_day =
      now > 1600000000 ? (now + kHongKongOffsetSeconds) / 86400 : 0;
  Queue([]() {
    if (!initialized) {
      LoadConfig();
      LoadCache();
    }
    FetchUsage();
  });
}
void Leave() {
  active = false;
  standby = false;
  generation++;
  std::shared_ptr<Http> h;
  {
    std::lock_guard<std::mutex> l(http_mu);
    h = live_http;
  }
  if (h)
    h->Cancel();
  CloseConfiguration();
}
void SetStandby(bool value) {
  standby = value;
  if (value) {
    generation++;
    std::shared_ptr<Http> h;
    {
      std::lock_guard<std::mutex> l(http_mu);
      h = live_http;
    }
    if (h)
      h->Cancel();
    CloseConfiguration();
  } else if (active)
    next_sync = 0;
  Changed();
}
bool IsActive() { return Foreground(); }
void CancelConfigurationRequests(uint64_t epoch) {
  std::shared_ptr<Http> client;
  {
    std::lock_guard<std::mutex> l(http_mu);
    if (live_http_epoch == epoch)
      client = live_http;
  }
  if (client)
    client->Cancel();
}
uint64_t Revision() { return revision.load(); }
std::string BuildDescription() {
  char elf_hash[9];
  esp_app_get_elf_sha256(elf_hash, sizeof(elf_hash));
  return std::string(esp_app_get_description()->version) + " · InkMeter " + elf_hash;
}
View GetView() {
  View v;
  auto &board = Board::GetInstance();
  v.network_status = board.IsWifiConfigMode() ? "请在原厂设置完成 Wi-Fi 配网"
                     : board.IsNetworkReady() ? "网络已连接"
                                              : "请在原厂设置连接 Wi-Fi";
  std::lock_guard<std::mutex> l(state_mu);
  v.initialized = initialized;
  v.configured = !config.site.empty() && config.key_configured;
  v.site = config.site;
  v.user_id = config.user_id;
  v.user_name = config.user_name;
  v.usage_minutes = config.minutes;
  v.selected_accounts = config.account_ids.size();
  v.usage = usages[size_t(selected_range)];
  v.usage.range = selected_range;
  v.accounts = accounts;
  v.users = users;
  v.user_page = user_page;
  v.more_users = more_users;
  v.status = status;
  const auto now = UtcNow();
  if (v.usage.valid && now > 1600000000 && !UsageCurrent(v.usage, now)) {
    v.status = "旧日期缓存 " + v.usage.first +
               (v.usage.first == v.usage.last ? "" : " 至 " + v.usage.last) +
               "（UTC+8）" + (status.empty() ? "，等待同步" : " · " + status);
  } else if (v.usage.valid &&
             now - v.usage.received_at > int64_t(config.minutes) * 120) {
    v.status =
        "数据已过期，保留上次成功快照" + (status.empty() ? "" : " · " + status);
  }
  v.busy = busy.load();
  v.syncing_account = syncing_account;
  return v;
}
void Tick() {
  if (!Foreground() || busy)
    return;
  if (!initialized) {
    Queue([]() {
      LoadConfig();
      LoadCache();
      FetchUsage();
    });
    return;
  }
  if (want_users > 0) {
    BrowseUsers(want_users.load());
    return;
  }
  const auto now = esp_timer_get_time();
  const auto wall_time = UtcNow();
  if (wall_time > 1600000000) {
    const int64_t day = (wall_time + kHongKongOffsetSeconds) / 86400;
    if (calendar_day.exchange(day) != day) {
      next_sync = 0;
      Changed();
    }
  } else {
    calendar_day = 0;
  }
  if (now < next_sync) {
    if (want_accounts && now >= snapshots_retry_until)
      ReadQuotaSnapshots();
    return;
  }
  const auto c = CopyConfig();
  if (c.site.empty() || c.user_id <= 0) {
    next_sync = now + 60000000;
    if (want_accounts && now >= snapshots_retry_until)
      ReadQuotaSnapshots();
    return;
  }
  next_sync = now + int64_t(c.minutes) * 60000000;
  Queue([]() {
    const auto token = generation.load();
    const auto result = FetchUsage();
    if (result == FetchResult::Failed && token == generation.load() &&
        Foreground()) {
      const auto c = CopyConfig();
      std::lock_guard<std::mutex> l(state_mu);
      failures = std::min(3, failures + 1);
      next_sync = std::max(usage_retry_until.load(),
                           esp_timer_get_time() +
                               int64_t(c.minutes) * 60000000 * (1 << failures));
    }
  });
}
void SelectRange(Range r) {
  {
    std::lock_guard<std::mutex> l(state_mu);
    selected_range = r;
    Changed();
  }
  RefreshUsage();
}
void RefreshUsage() {
  if (esp_timer_get_time() < usage_retry_until) {
    next_sync = usage_retry_until.load();
    Status("站点仍在限流冷却，请稍后重试");
    return;
  }
  next_sync = 0;
  Queue([]() { FetchUsage(); });
}
void ReadQuotaSnapshots() {
  want_accounts = true;
  if (esp_timer_get_time() < snapshots_retry_until) {
    Status("站点仍在限流冷却，请稍后刷新额度");
    return;
  }
  Queue([]() { FetchAccounts(); });
}
void BrowseUsers(int page) {
  if (page < 1 || page > 100000)
    return;
  want_users = page;
  Queue([page]() {
    const auto token = generation.load();
    std::unique_lock<std::mutex> io(io_mu, std::try_to_lock);
    if (!io)
      return;
    int expected = page;
    want_users.compare_exchange_strong(expected, 0);
    if (!Foreground() || token != generation.load())
      return;
    const auto c = CopyConfig();
    auto r = Browse(c, false, "page=" + std::to_string(page) + "&page_size=20",
                    token);
    if (!Foreground() || token != generation.load())
      return;
    auto p = Parse(r.json);
    if (r.status != 200 || !p) {
      Status("读取用户列表失败");
      return;
    }
    std::vector<std::pair<int64_t, std::string>> list;
    const cJSON *row = nullptr;
    cJSON_ArrayForEach(row, Item(p.get(), "items")) {
      int64_t id;
      if (PositiveId(Item(row, "id"), id))
        list.emplace_back(id, Text(row, "name", 160));
    }
    std::lock_guard<std::mutex> l(state_mu);
    users = std::move(list);
    user_page = page;
    more_users = Number(p.get(), "total") > page * 20;
    Changed();
  });
}
void SelectUser(int64_t id) {
  Queue([id]() {
    auto b = Object();
    cJSON_AddNumberToObject(b.get(), "user_id", id);
    const auto r = HandleApi("PUT", "/api/config/user", "", Dump(b.get()));
    if (r.status != 200)
      Status("监控用户保存失败，保留旧配置");
    else
      FetchUsage();
  });
}
void SyncAccount(int64_t id) {
  Queue([id]() {
    const auto token = generation.load();
    std::unique_lock<std::mutex> io(io_mu, std::try_to_lock);
    if (!io) {
      Status("手机配置正在处理，请稍后同步此账户");
      return;
    }
    if (!Foreground() || token != generation.load())
      return;
    Config c = CopyConfig();
    Account a;
    {
      std::lock_guard<std::mutex> l(state_mu);
      auto it = std::find_if(accounts.begin(), accounts.end(),
                             [&](const Account &v) { return v.id == id; });
      if (it == accounts.end())
        return;
      const int64_t now = esp_timer_get_time();
      if (cooldown[id] > now) {
        status = "此账户仍在冷却，请稍后重试";
        Changed();
        return;
      }
      a = *it;
      a.error.clear();
      syncing_account = id;
      cooldown[id] = now + 60000000;
      Changed();
    }
    const auto plan = ManualRequests(a);
    bool success = false;
    std::string error;
    const int64_t deadline = esp_timer_get_time() + 30000000;
    for (const auto &request : plan) {
      if (!Foreground() || token != generation.load())
        break;
      const auto r = HttpRequest(c, request, {}, token, 0, deadline);
      auto root = Success(r) ? Parse(r.body) : Json(nullptr, cJSON_Delete);
      const auto *data = root ? Data(root.get()) : nullptr;
      if (data && !QuotaPayloadFailed(data)) {
        Account candidate = a;
        if (DecodeQuota(data, candidate)) {
          a = std::move(candidate);
          success = true;
        } else
          error = "接口未提供可用额度，保留上次结果";
      } else {
        error = Success(r) ? "上游采样失败，保留上次结果" : HttpError(r.status);
      }
      const int retry = std::max(
          {r.retry, RetryAfterSeconds(Item(data, "retry_after_seconds")),
           RetryAfterSeconds(Item(data, "grok_retry_after_seconds")),
           RetryAfterSeconds(
               Item(Item(data, "snapshot"), "retry_after_seconds"))});
      if (r.status == 429 || retry > 0) {
        std::lock_guard<std::mutex> l(state_mu);
        if (token == generation.load())
          cooldown[id] = std::max(cooldown[id], esp_timer_get_time() +
                                                    int64_t(std::max(60, retry)) * 1000000);
        break;
      }
    }
    if (plan.empty())
      error = "此账号类型未提供上游同步接口";
    if (success) {
      a.source = "上次手动同步";
      a.received_at = UtcNow();
      if (!error.empty())
        a.error = error;
    } else
      a.error = error.empty() ? "同步失败，保留上次结果" : error;
    {
      std::lock_guard<std::mutex> l(state_mu);
      syncing_account = 0;
      if (token == generation.load() && Foreground()) {
        auto it = std::find_if(accounts.begin(), accounts.end(),
                               [&](const Account &v) { return v.id == id; });
        if (it != accounts.end())
          *it = std::move(a);
      }
      Changed();
    }
    if (success && token == generation.load() && Foreground())
      SaveCache();
  });
}

ApiResult HandleApi(const std::string &method, const std::string &path,
                    const std::string &query, const std::string &body,
                    uint64_t authorization_epoch) {
  const auto token = generation.load();
  const auto authorized = [&]() {
    return Foreground() && token == generation.load() &&
           (!authorization_epoch ||
            ConfigurationSessionValid(authorization_epoch));
  };
  if (!Foreground())
    return Error(503, "应用已暂停");
  if (!authorized())
    return Error(403, "配置会话已关闭，未应用草稿");
  if (method == "GET" && path == "/api/config") {
    const auto c = CopyConfig();
    auto j = ConfigJson(c, false);
    if (!j)
      return Error(503, "设备资源不足，请重试");
    return {200, Dump(j.get())};
  }
  if (method == "GET" && (path == "/api/users" || path == "/api/accounts")) {
    std::lock_guard<std::mutex> io(io_mu);
    if (!authorized())
      return Error(403, "配置会话已关闭");
    const auto result = Browse(CopyConfig(), path == "/api/accounts", query,
                               token, authorization_epoch);
    return authorized() ? result : Error(403, "配置会话已关闭");
  }
  if (method != "POST" && method != "PUT")
    return Error(405, "不支持的操作");
  auto root = Parse(body);
  if (!root || !cJSON_IsObject(root.get()))
    return Error(400, "请求格式不正确");
  std::unique_lock<std::mutex> writer(config_mu, std::defer_lock);
  if (authorization_epoch)
    writer.lock();
  else if (!writer.try_lock())
    return Error(503, "手机配置正在处理，请稍后重试");
  if (!authorized())
    return Error(403, "配置会话或当前配置已变化，未应用草稿");
  Config next = CopyConfig();
  if (next.key_configured) {
    next.key = ReadSecret(next.site);
    if (!ValidSecret(next.key))
      return Error(507, "读取已保存凭据失败，旧配置仍有效");
  }
  const int64_t deadline = esp_timer_get_time() + 60000000;
  if (path == "/api/site/test" || path == "/api/config/site") {
    std::string site;
    if (!NormalizeSite(Text(root.get(), "site", 384), site))
      return Error(400, "请输入不含凭据和查询参数的HTTPS站点地址");
    const auto key = Text(root.get(), "key", 1024);
    const auto *submitted_key = Item(root.get(), "key");
    if (submitted_key &&
        (!cJSON_IsString(submitted_key) || !submitted_key->valuestring ||
         std::strlen(submitted_key->valuestring) > 1024))
      return Error(400, "Key格式不正确或过长");
    if (site != next.site && key.empty())
      return Error(400, "更换站点需要新的Key");
    const bool key_changed = !key.empty() && key != next.key;
    if (!key.empty())
      next.key = key;
    if (!ValidSecret(next.key))
      return Error(400, "Key为空或包含无效字符");
    const bool changed = next.site != site;
    next.site = site;
    if (!EnsureClock(authorized, deadline))
      return Error(503, "设备尚未完成对时，请稍后测试");
    const auto now = UtcNow();
    {
      std::lock_guard<std::mutex> io(io_mu);
      for (const char *endpoint :
           {"/api/v1/admin/users?page=1&page_size=1",
            "/api/v1/admin/accounts?page=1&page_size=1"}) {
        if (!authorized())
          return Error(403, "配置会话已关闭");
        const auto r = HttpRequest(next, {"GET", endpoint}, {}, token,
                                   authorization_epoch, deadline);
        auto p = Success(r) ? Parse(r.body) : Json(nullptr, cJSON_Delete);
        if (!p || !cJSON_IsArray(Item(Data(p.get()), "items"))) {
          ESP_LOGW("InkMeterFetch", "list probe http=%d bytes=%u json=%d data=%d",
                   r.status, unsigned(r.body.size()), bool(p), p && Data(p.get()));
          return Error(502, Success(r) ? "站点列表接口数据不兼容" : HttpError(r.status));
        }
      }
      auto probe = next;
      if (probe.user_id <= 0)
        probe.user_id = 1;
      std::string first, last;
      DateRange(Range::Today, now, first, last);
      if (!authorized())
        return Error(403, "配置会话已关闭，未保存");
      const auto r = HttpRequest(
          probe, {"GET", SnapshotPath(probe, Range::Today, first, last)}, {},
          token, authorization_epoch, deadline);
      auto p = Success(r) ? Parse(r.body) : Json(nullptr, cJSON_Delete);
      Usage sample;
      if (!p || !DecodeUsage(Data(p.get()), probe.user_id, Range::Today, first,
                             last, sample))
        return Error(502, "统计快照接口不兼容或请求失败，未保存");
      if (!changed && key_changed && path == "/api/config/site") {
        if (next.user_id > 0) {
          const auto user =
              Browse(next, false, "id=" + std::to_string(next.user_id), token,
                     authorization_epoch, deadline);
          if (user.status != 200)
            return Error(400, "新Key无法访问已选用户，旧配置仍有效");
        }
        for (auto id : next.account_ids) {
          if (!authorized() || esp_timer_get_time() >= deadline)
            return Error(503, "验证已取消或超时，旧配置仍有效");
          const auto account = Browse(next, true, "id=" + std::to_string(id),
                                      token, authorization_epoch, deadline);
          if (account.status != 200)
            return Error(400, "新Key无法访问已选账户，旧配置仍有效");
        }
      }
    }
    if (!authorized())
      return Error(403, "配置会话已关闭，未保存");
    if (path == "/api/site/test")
      return {200,
              "{\"ok\":true,\"message\":"
              "\"TLS、管理员列表和统计快照接口通过；统计分桶需站点部署为UTC+"
              "8。额度未触发上游采样。\"}"};
    if (changed) {
      next.user_id = 0;
      next.user_name.clear();
      next.account_ids.clear();
    }
  } else if (path == "/api/config/user") {
    int64_t id;
    if (!PositiveId(Item(root.get(), "user_id"), id))
      return Error(400, "请选择有效用户");
    std::lock_guard<std::mutex> io(io_mu);
    if (!authorized())
      return Error(403, "配置会话已关闭，未保存用户");
    const auto r = Browse(next, false, "id=" + std::to_string(id), token,
                          authorization_epoch, deadline);
    auto p = Parse(r.json);
    const auto *row =
        p ? cJSON_GetArrayItem(Item(p.get(), "items"), 0) : nullptr;
    if (r.status != 200 || !row)
      return Error(400, "用户不存在或不可访问");
    next.user_id = id;
    next.user_name = UserDisplayName(nullptr, Text(row, "name", 160), id);
  } else if (path == "/api/config/accounts") {
    const auto *ids = Item(root.get(), "account_ids");
    if (!cJSON_IsArray(ids) || cJSON_GetArraySize(ids) > int(kMaxAccounts))
      return Error(400, "最多选择64个额度账户");
    std::set<int64_t> unique;
    const cJSON *p = nullptr;
    cJSON_ArrayForEach(p, ids) {
      int64_t id;
      if (!PositiveId(p, id) || !unique.insert(id).second)
        return Error(400, "账户ID无效或重复");
    }
    next.account_ids.assign(unique.begin(), unique.end());
    std::lock_guard<std::mutex> io(io_mu);
    for (auto id : next.account_ids) {
      if (!authorized() || esp_timer_get_time() >= deadline)
        return Error(503, "验证已取消或超时，未保存账户选择");
      const auto r = HttpRequest(
          next, {"GET", "/api/v1/admin/accounts/" + std::to_string(id)}, {},
          token, authorization_epoch, deadline);
      auto p = Success(r) ? Parse(r.body) : Json(nullptr, cJSON_Delete);
      Account a;
      if (!p || !DecodeAccount(Data(p.get()), a) || a.id != id)
        return Error(400, "账户不存在或不可访问，未保存");
    }
  } else if (path == "/api/config/display") {
    const double n = Number(root.get(), "usage_sync_minutes", -1);
    if (n < 1 || n > 1440 || std::floor(n) != n)
      return Error(400, "请输入1至1440的整数分钟");
    next.minutes = int(n);
  } else
    return Error(404, "接口不存在");
  // Keep NVS credentials and their runtime site/generation together across
  // replacement.
  std::lock_guard<std::mutex> io(io_mu);
  if (!authorized())
    return Error(503, "应用已暂停，未保存草稿");
  if (!Commit(std::move(next)))
    return Error(507, "设备存储不足或保存失败，旧配置仍有效");
  return Ok();
}
} // namespace inkmeter
