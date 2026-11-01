#pragma once

#include <cJSON.h>
#include <cstdint>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

namespace inkmeter {

constexpr size_t kMaxJsonBytes = 1024 * 1024;
constexpr time_t kHongKongOffsetSeconds = 8 * 3600;

using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
Json Parse(const std::string &text);
std::string Dump(const cJSON *value);
const cJSON *Item(const cJSON *value, const char *key);
const cJSON *Data(const cJSON *envelope);
std::string Text(const cJSON *value, const char *key, size_t limit = 256);
std::string UserDisplayName(const cJSON *user, const std::string &previous = {},
                            int64_t id = 0);
double Number(const cJSON *value, const char *key, double fallback = 0);
bool Flag(const cJSON *value, const char *key);
bool PositiveId(const cJSON *value, int64_t &id);
std::string Encode(const std::string &value);
bool NormalizeSite(const std::string &input, std::string &output);
bool ValidSecret(const std::string &value);
std::string SiteId(const std::string &site);
std::string Compact(uint64_t count);
std::string Money(double value);
time_t UtcNow();
std::string Clock(time_t timestamp, bool date = false);
time_t Timestamp(const cJSON *value);
int RetryAfterSeconds(const std::string &value, time_t now);
int RetryAfterSeconds(const cJSON *value);

enum class Range : uint8_t { Today, Week, Month };
const char *RangeName(Range range);
bool ParseRange(const std::string &name, Range &range);
void DateRange(Range range, time_t now, std::string &first, std::string &last);

struct Counts {
  uint64_t input = 0, output = 0, write = 0, read = 0, requests = 0;
  double cost = 0;
  bool tokens_known = true, cost_known = true, requests_known = true;
  uint64_t Total() const { return input + output + write + read; }
  double HitRate() const;
};
struct Bucket {
  std::string label;
  Counts counts;
};
struct Model {
  std::string id;
  uint64_t tokens = 0;
  double cost = 0;
  bool tokens_known = true, cost_known = true;
};
struct Usage {
  bool valid = false;
  int64_t user_id = 0;
  Range range = Range::Today;
  std::string first, last, etag;
  time_t received_at = 0;
  Counts totals;
  std::vector<Bucket> buckets;
  std::vector<Model> models;
};
bool DecodeUsage(const cJSON *data, int64_t user, Range range,
                 const std::string &first, const std::string &last, Usage &out);
bool UsageCurrent(const Usage &usage, time_t now);

struct Window {
  std::string label;
  double used = 0;
  bool known = false;
  time_t reset_at = 0;
  std::string reset_text;
};
struct Balance {
  std::string label, value;
};
struct Account {
  int64_t id = 0;
  std::string name, platform, type;
  std::vector<Window> windows;
  std::vector<Balance> balances;
  std::string note, error, source;
  time_t observed_at = 0, received_at = 0;
  bool snapshot = false, manual_supported = false, local = false;
  bool ollama = false, opencode = false;
};
bool DecodeAccount(const cJSON *item, Account &out);
bool DecodeQuota(const cJSON *data, Account &inout);
bool QuotaPayloadFailed(const cJSON *data);
struct Request {
  std::string method, path;
};
std::vector<Request> ManualRequests(const Account &account);
std::vector<Request> PassiveRequests(const Account &account);
bool IsKnownPlatform(const std::string &platform);

Json EncodeUsage(const Usage &usage);
bool DecodeCachedUsage(const cJSON *item, Usage &usage);
Json EncodeAccount(const Account &account);
bool DecodeCachedAccount(const cJSON *item, Account &account);

} // namespace inkmeter
