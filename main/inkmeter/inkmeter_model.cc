#include "inkmeter_model.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>

namespace inkmeter {
namespace {
constexpr double kExactInteger = 9007199254740991.0;
constexpr size_t kMaxModels = 200, kMaxWindows = 64;

bool Count(const cJSON *obj, const char *key, uint64_t &result) {
  const auto *p = Item(obj, key);
  if (!cJSON_IsNumber(p) || !std::isfinite(p->valuedouble) ||
      p->valuedouble < 0 || p->valuedouble > kExactInteger ||
      std::floor(p->valuedouble) != p->valuedouble)
    return false;
  result = static_cast<uint64_t>(p->valuedouble);
  return true;
}
Counts ReadCounts(const cJSON *obj) {
  Counts c;
  c.tokens_known = Count(obj, "input_tokens", c.input) &
                   Count(obj, "output_tokens", c.output) &
                   Count(obj, "cache_creation_tokens", c.write) &
                   Count(obj, "cache_read_tokens", c.read);
  c.requests_known = Count(obj, "requests", c.requests);
  c.cost = Number(obj, "actual_cost", -1);
  c.cost_known = c.cost >= 0;
  if (!c.cost_known)
    c.cost = 0;
  return c;
}
bool Add(Counts &a, const Counts &b) {
  const auto maximum = std::numeric_limits<uint64_t>::max();
  if (b.input > maximum - a.input || b.output > maximum - a.output ||
      b.write > maximum - a.write || b.read > maximum - a.read ||
      b.requests > maximum - a.requests || !std::isfinite(a.cost + b.cost))
    return false;
  a.input += b.input;
  a.output += b.output;
  a.write += b.write;
  a.read += b.read;
  a.requests += b.requests;
  a.cost += b.cost;
  a.tokens_known &= b.tokens_known;
  a.cost_known &= b.cost_known;
  a.requests_known &= b.requests_known;
  uint64_t remaining = maximum;
  for (auto count : {a.input, a.output, a.write, a.read}) {
    if (count > remaining)
      return false;
    remaining -= count;
  }
  return true;
}
bool DateParts(const std::string &s, std::tm &t) {
  if (s.size() != 10 || s[4] != '-' || s[7] != '-')
    return false;
  for (size_t i = 0; i < s.size(); ++i)
    if (i != 4 && i != 7 && (s[i] < '0' || s[i] > '9'))
      return false;
  const int year = std::stoi(s.substr(0, 4)), month = std::stoi(s.substr(5, 2)),
            day = std::stoi(s.substr(8, 2));
  if (year < 1970 || year > 2200 || month < 1 || month > 12 || day < 1 ||
      day > 31)
    return false;
  t = {};
  t.tm_year = year - 1900;
  t.tm_mon = month - 1;
  t.tm_mday = day;
  auto normalized = t;
  return timegm(&normalized) >= 0 && normalized.tm_year == t.tm_year &&
         normalized.tm_mon == t.tm_mon && normalized.tm_mday == t.tm_mday;
}
bool ValidDates(Range range, const std::string &first,
                const std::string &last) {
  std::tm a{}, b{};
  if (!DateParts(first, a) || !DateParts(last, b))
    return false;
  return timegm(&b) - timegm(&a) == (range == Range::Today  ? 0
                                     : range == Range::Week ? 6
                                                            : 29) *
                                        86400;
}
std::string Scalar(const cJSON *p) {
  if (cJSON_IsString(p) && p->valuestring && std::strlen(p->valuestring) <= 128)
    return p->valuestring;
  if (cJSON_IsNumber(p) && std::isfinite(p->valuedouble)) {
    char b[64];
    std::snprintf(b, sizeof(b), "%.12g", p->valuedouble);
    return b;
  }
  return {};
}
bool PutWindow(Account &a, std::string name, const cJSON *used,
               const cJSON *reset) {
  if (name.empty() || name.size() > 160)
    return false;
  Window w;
  w.label = std::move(name);
  if (cJSON_IsNumber(used) && std::isfinite(used->valuedouble) &&
      used->valuedouble >= 0) {
    w.used = used->valuedouble;
    w.known = true;
  }
  w.reset_at = Timestamp(reset);
  if (!w.reset_at && cJSON_IsString(reset))
    w.reset_text = Scalar(reset);
  const bool known = w.known;
  auto old = std::find_if(a.windows.begin(), a.windows.end(),
                          [&](const Window &v) { return v.label == w.label; });
  if (old != a.windows.end())
    *old = std::move(w);
  else if (a.windows.size() < kMaxWindows)
    a.windows.push_back(std::move(w));
  else
    return false;
  if (known)
    a.snapshot = true;
  return known;
}
bool PutBalance(Account &a, const std::string &label, const cJSON *value) {
  std::string text = Scalar(value);
  if (text.empty() || label.size() > 192)
    return false;
  auto old = std::find_if(a.balances.begin(), a.balances.end(),
                          [&](const Balance &b) { return b.label == label; });
  if (old == a.balances.end() && a.balances.size() < 64)
    a.balances.push_back({label, text});
  else if (old != a.balances.end())
    old->value = text;
  else
    return false;
  a.snapshot = true;
  return true;
}
bool PutCredits(Account &a, const cJSON *credits) {
  if (Flag(credits, "unlimited")) {
    Json text(cJSON_CreateString("无限"), cJSON_Delete);
    return PutBalance(a, "Credits", text.get());
  }
  return PutBalance(a, "Credits", Item(credits, "balance"));
}
void ExtraWindow(Account &a, const cJSON *extra, const std::string &prefix,
                 const char *suffix, const char *label) {
  auto key = prefix + suffix;
  if (Item(extra, (key + "_used_percent").c_str()))
    PutWindow(a, label, Item(extra, (key + "_used_percent").c_str()),
              Item(extra, (key + "_reset_at").c_str()));
}
bool RateEnvelope(Account &a, const cJSON *rate, const std::string &prefix) {
  bool known = false;
  for (const char *key : {"primary_window", "secondary_window"}) {
    const auto *w = Item(rate, key);
    if (!cJSON_IsObject(w))
      continue;
    const double duration = Number(w, "limit_window_seconds");
    const auto seconds = duration > 0 && duration <= 315360000 &&
                                 std::floor(duration) == duration
                             ? static_cast<int64_t>(duration)
                             : 0;
    std::string label =
        seconds > 0
            ? seconds % 86400 == 0  ? std::to_string(seconds / 86400) + "d"
              : seconds % 3600 == 0 ? std::to_string(seconds / 3600) + "h"
              : seconds % 60 == 0   ? std::to_string(seconds / 60) + "min"
                                    : std::to_string(seconds) + "s"
            : "窗口";
    known |= PutWindow(a, prefix + label, Item(w, "used_percent"),
                       Item(w, "reset_at"));
  }
  return known;
}
bool StateSnapshot(Account &a, const cJSON *state, bool ollama) {
  if (!cJSON_IsObject(state))
    return false;
  bool known = false;
  const auto *snapshot = Item(state, "snapshot");
  if (QuotaPayloadFailed(state))
    a.error = "站点上次采样失败，保留已有结果";
  const auto *d = Item(snapshot, "data");
  if (!d)
    d = snapshot;
  if (!cJSON_IsObject(d))
    return false;
  const char *const keys[] = {"five_hour", "seven_day", "rolling", "weekly",
                              "monthly"};
  for (unsigned i = ollama ? 0 : 2; i < (ollama ? 2 : 5); ++i) {
    const char *key = keys[i];
    const auto *w = Item(d, key);
    if (!cJSON_IsObject(w))
      continue;
    const std::string status = Text(w, "status", 32);
    const auto *reset = Item(w, ollama ? "reset_at" : "resets_at");
    if ((!reset || cJSON_IsNull(reset)) && ollama)
      reset = Item(w, "reset_text");
    const bool known_status =
        ollama ? status.empty() || status == "ok" : status == "ok";
    known |= PutWindow(
        a, ollama ? (std::string(key) == "five_hour" ? "5h" : "7d") : key,
        known_status ? Item(w, ollama ? "used_percent" : "percent") : nullptr,
        reset);
  }
  known |= PutBalance(a, "余额", Item(d, "balance"));
  if (ollama) {
    const cJSON *row = nullptr;
    cJSON_ArrayForEach(row, Item(d, "models")) known |= PutBalance(
        a,
        Text(row, "model", 160) + " " + Scalar(Item(row, "window")) + " 请求",
        Item(row, "requests"));
  }
  a.observed_at = Timestamp(Item(snapshot, "fetched_at"));
  return known;
}
void LocalQuota(Account &a, const cJSON *obj) {
  for (const char *suffix : {"", "_daily", "_weekly"}) {
    const std::string base = std::string("quota") + suffix;
    const double limit = Number(obj, (base + "_limit").c_str());
    const double used = Number(obj, (base + "_used").c_str(), -1);
    if (limit <= 0 || used < 0)
      continue;
    Json percent(cJSON_CreateNumber(100 * used / limit), cJSON_Delete);
    PutWindow(a,
              std::string("站点") +
                  (suffix[0] ? std::strcmp(suffix, "_daily") == 0 ? "日配额"
                                                                  : "周配额"
                             : "总配额"),
              percent.get(), nullptr);
    a.local = true;
  }
}
} // namespace

Json Parse(const std::string &text) {
  if (text.empty() || text.size() > kMaxJsonBytes)
    return {nullptr, cJSON_Delete};
  unsigned depth = 0;
  bool quoted = false, escaped = false;
  for (size_t i = 0; i < text.size(); ++i) {
    const unsigned char c = text[i];
    if (!c)
      return {nullptr, cJSON_Delete};
    if (quoted) {
      if (escaped) {
        if (c == 'u' && text.compare(i + 1, 4, "0000") == 0)
          return {nullptr, cJSON_Delete};
        escaped = false;
      } else if (c == '\\')
        escaped = true;
      else if (c == '"')
        quoted = false;
    } else if (c == '"')
      quoted = true;
    else if (c == '{' || c == '[') {
      if (++depth > 24)
        return {nullptr, cJSON_Delete};
    } else if (c == '}' || c == ']') {
      if (!depth)
        return {nullptr, cJSON_Delete};
      --depth;
    }
  }
  const char *end = nullptr;
  Json value(
      cJSON_ParseWithLengthOpts(text.c_str(), text.size() + 1, &end, true),
      cJSON_Delete);
  return value;
}
std::string Dump(const cJSON *value) {
  char *s = cJSON_PrintUnformatted(value);
  if (!s)
    return {};
  std::string out(s);
  cJSON_free(s);
  return out;
}
const cJSON *Item(const cJSON *value, const char *key) {
  return value ? cJSON_GetObjectItemCaseSensitive(value, key) : nullptr;
}
const cJSON *Data(const cJSON *root) {
  const auto *code = Item(root, "code");
  if (code && (!cJSON_IsNumber(code) || code->valuedouble != 0))
    return nullptr;
  return Item(root, "data");
}
std::string Text(const cJSON *obj, const char *key, size_t limit) {
  const auto *p = Item(obj, key);
  if (!cJSON_IsString(p) || !p->valuestring ||
      std::strlen(p->valuestring) > limit)
    return {};
  return p->valuestring;
}
std::string UserDisplayName(const cJSON *user, const std::string &previous,
                            int64_t id) {
  const auto trimmed = [](const std::string &value) {
    const auto first = value.find_first_not_of(" \t\r\n\f\v");
    if (first == std::string::npos || value.size() > 160)
      return std::string();
    return value.substr(first, value.find_last_not_of(" \t\r\n\f\v") - first + 1);
  };
  for (const char *key : {"username", "email"}) {
    auto name = trimmed(Text(user, key, 160));
    if (!name.empty())
      return name;
  }
  auto name = trimmed(previous);
  const auto number = std::to_string(id);
  if (name.empty() || name == "#" + number || name == "用户#" + number ||
      name == "用户 #" + number)
    return "用户名未提供";
  return name;
}
double Number(const cJSON *obj, const char *key, double fallback) {
  const auto *p = Item(obj, key);
  return cJSON_IsNumber(p) && std::isfinite(p->valuedouble) ? p->valuedouble
                                                            : fallback;
}
bool Flag(const cJSON *obj, const char *key) {
  return cJSON_IsTrue(Item(obj, key));
}
bool PositiveId(const cJSON *p, int64_t &id) {
  if (!cJSON_IsNumber(p) || !std::isfinite(p->valuedouble) ||
      p->valuedouble < 1 || p->valuedouble > kExactInteger ||
      std::floor(p->valuedouble) != p->valuedouble)
    return false;
  id = static_cast<int64_t>(p->valuedouble);
  return true;
}
std::string Encode(const std::string &s) {
  constexpr char hex[] = "0123456789ABCDEF";
  std::string result;
  for (unsigned char c : s) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || std::strchr("-._~", c))
      result += c;
    else {
      result += '%';
      result += hex[c >> 4];
      result += hex[c & 15];
    }
  }
  return result;
}
bool NormalizeSite(const std::string &input, std::string &output) {
  if (input.size() < 9 || input.size() > 384 ||
      input.compare(0, 8, "https://") != 0)
    return false;
  for (unsigned char c : input)
    if (c <= 32 || c >= 127 || std::strchr("@?#\\\"<>%", c))
      return false;
  const auto end = input.find('/', 8);
  const auto authority =
      input.substr(8, end == std::string::npos ? end : end - 8);
  if (authority.empty() || authority.front() == '.' ||
      authority.find("..") != std::string::npos)
    return false;
  for (char c : authority)
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '-' &&
        c != ':')
      return false;
  const auto colon = authority.find(':');
  if (colon != std::string::npos) {
    if (!colon)
      return false;
    const auto port = authority.substr(colon + 1);
    if (port.empty() || port.size() > 5 ||
        port.find_first_not_of("0123456789") != std::string::npos ||
        std::strtoul(port.c_str(), nullptr, 10) < 1 ||
        std::strtoul(port.c_str(), nullptr, 10) > 65535)
      return false;
  }
  if (input.find("/../") != std::string::npos ||
      input.find("/./") != std::string::npos)
    return false;
  output = input;
  while (output.size() > 8 && output.back() == '/')
    output.pop_back();
  return true;
}
bool ValidSecret(const std::string &value) {
  if (value.empty() || value.size() > 1024)
    return false;
  return std::all_of(value.begin(), value.end(),
                     [](unsigned char c) { return c > 32 && c < 127; });
}
std::string SiteId(const std::string &site) {
  uint64_t hash = 14695981039346656037ull;
  for (unsigned char c : site) {
    hash ^= c;
    hash *= 1099511628211ull;
  }
  char b[20];
  std::snprintf(b, sizeof(b), "%016llx", static_cast<unsigned long long>(hash));
  return b;
}
std::string Compact(uint64_t n) {
  char b[48];
  if (n >= 1000000)
    std::snprintf(b, sizeof(b), "%.2fM", n / 1000000.0);
  else if (n >= 1000)
    std::snprintf(b, sizeof(b), "%.1fK", n / 1000.0);
  else
    std::snprintf(b, sizeof(b), "%llu", static_cast<unsigned long long>(n));
  return b;
}
std::string Money(double n) {
  if (n > 0 && n < .01)
    return "<0.01";
  char b[48];
  std::snprintf(b, sizeof(b), "%.2f", n);
  return b;
}
time_t UtcNow() {
  // shortcut: upstream stores Hong Kong wall time; remove this offset if it switches to UTC.
  return time(nullptr) - kHongKongOffsetSeconds;
}
std::string Clock(time_t timestamp, bool date) {
  if (timestamp <= 0)
    return "—";
  timestamp += kHongKongOffsetSeconds;
  std::tm t{};
  gmtime_r(&timestamp, &t);
  char b[32];
  std::strftime(b, sizeof(b), date ? "%m.%d %H:%M" : "%H:%M", &t);
  return b;
}
time_t Timestamp(const cJSON *v) {
  if (cJSON_IsNumber(v) && std::isfinite(v->valuedouble) &&
      v->valuedouble > 0) {
    const double seconds =
        v->valuedouble > 1e12 ? v->valuedouble / 1000 : v->valuedouble;
    return seconds <= 7258118400.0 ? static_cast<time_t>(seconds) : 0;
  }
  if (!cJSON_IsString(v) || !v->valuestring)
    return 0;
  const std::string s = v->valuestring;
  std::tm t{};
  if (s.size() > 64 || s.size() < 10 || !DateParts(s.substr(0, 10), t))
    return 0;
  if (s.size() == 10)
    return timegm(&t);
  auto two = [&](size_t pos) {
    return pos + 1 < s.size() && s[pos] >= '0' && s[pos] <= '9' &&
                   s[pos + 1] >= '0' && s[pos + 1] <= '9'
               ? (s[pos] - '0') * 10 + s[pos + 1] - '0'
               : -1;
  };
  if (s.size() < 19 || (s[10] != 'T' && s[10] != ' ') || s[13] != ':' ||
      s[16] != ':')
    return 0;
  t.tm_hour = two(11);
  t.tm_min = two(14);
  t.tm_sec = two(17);
  if (t.tm_hour < 0 || t.tm_hour > 23 || t.tm_min < 0 || t.tm_min > 59 ||
      t.tm_sec < 0 || t.tm_sec > 59)
    return 0;
  size_t pos = 19;
  if (pos < s.size() && s[pos] == '.') {
    const auto start = ++pos;
    while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9')
      ++pos;
    if (pos == start)
      return 0;
  }
  int offset = 0;
  if (pos < s.size()) {
    if (s[pos] == 'Z' && pos + 1 == s.size())
      ++pos;
    else {
      if ((s[pos] != '+' && s[pos] != '-') || pos + 6 != s.size() ||
          s[pos + 3] != ':' || two(pos + 1) < 0 || two(pos + 1) > 23 ||
          two(pos + 4) < 0 || two(pos + 4) > 59)
        return 0;
      offset =
          (s[pos] == '+' ? 1 : -1) * (two(pos + 1) * 3600 + two(pos + 4) * 60);
      pos += 6;
    }
  }
  return pos == s.size() ? timegm(&t) - offset : 0;
}
const char *RangeName(Range r) {
  return r == Range::Today ? "today" : r == Range::Week ? "7d" : "30d";
}
int RetryAfterSeconds(const std::string &value, time_t now) {
  constexpr int maximum = std::numeric_limits<int>::max();
  if (!value.empty() &&
      value.find_first_not_of("0123456789") == std::string::npos) {
    int result = 0;
    for (char c : value) {
      if (result > (maximum - (c - '0')) / 10)
        return maximum;
      result = result * 10 + c - '0';
    }
    return result;
  }
  if (now < 1600000000 || value.size() != 29 || value.substr(25) != " GMT" ||
      value[3] != ',' || value[4] != ' ' || value[7] != ' ' ||
      value[11] != ' ' || value[16] != ' ')
    return 0;
  const std::string months = "JanFebMarAprMayJunJulAugSepOctNovDec";
  const auto month = months.find(value.substr(8, 3));
  if (month == std::string::npos || month % 3)
    return 0;
  char iso[32];
  std::snprintf(iso, sizeof(iso), "%.4s-%02u-%.2sT%.8sZ", value.c_str() + 12,
                unsigned(month / 3 + 1), value.c_str() + 5, value.c_str() + 17);
  Json date(cJSON_CreateString(iso), cJSON_Delete);
  const auto deadline = Timestamp(date.get());
  return deadline > now ? int(std::min<int64_t>(maximum, deadline - now)) : 0;
}
int RetryAfterSeconds(const cJSON *value) {
  if (cJSON_IsNumber(value) && std::isfinite(value->valuedouble) &&
      value->valuedouble > 0)
    return int(std::min(double(std::numeric_limits<int>::max()),
                        std::ceil(value->valuedouble)));
  return cJSON_IsString(value) && value->valuestring
             ? RetryAfterSeconds(value->valuestring, 0)
             : 0;
}
bool ParseRange(const std::string &s, Range &r) {
  if (s == "today")
    r = Range::Today;
  else if (s == "7d")
    r = Range::Week;
  else if (s == "30d")
    r = Range::Month;
  else
    return false;
  return true;
}
void DateRange(Range r, time_t now, std::string &first, std::string &last) {
  now += kHongKongOffsetSeconds;
  std::tm t{};
  gmtime_r(&now, &t);
  char b[16];
  std::strftime(b, sizeof(b), "%Y-%m-%d", &t);
  last = b;
  now -= (r == Range::Today ? 0 : r == Range::Week ? 6 : 29) * 86400;
  gmtime_r(&now, &t);
  std::strftime(b, sizeof(b), "%Y-%m-%d", &t);
  first = b;
}
double Counts::HitRate() const {
  const auto denominator = input + write + read;
  return tokens_known && denominator ? 100.0 * read / denominator : -1;
}
bool DecodeUsage(const cJSON *data, int64_t user, Range range,
                 const std::string &first, const std::string &last,
                 Usage &out) {
  if (user <= 0 || !ValidDates(range, first, last) || !cJSON_IsObject(data) ||
      Text(data, "start_date") != first || Text(data, "end_date") != last)
    return false;
  const auto granularity = Text(data, "granularity");
  if (!granularity.empty() &&
      granularity != (range == Range::Today ? "hour" : "day"))
    return false;
  const auto *trend = Item(data, "trend");
  const auto *models = Item(data, "models");
  const int max_buckets = range == Range::Today  ? 24
                          : range == Range::Week ? 7
                                                 : 30;
  // snapshot-v2 omits empty slices; its dated metadata still identifies a valid
  // zero result.
  if ((!trend &&
       (granularity.empty() || !Timestamp(Item(data, "generated_at")))) ||
      (trend && !cJSON_IsArray(trend)) || (models && !cJSON_IsArray(models)) ||
      cJSON_GetArraySize(trend) > max_buckets ||
      cJSON_GetArraySize(models) > int(kMaxModels))
    return false;
  Usage next;
  next.user_id = user;
  next.range = range;
  next.first = first;
  next.last = last;
  const cJSON *row = nullptr;
  cJSON_ArrayForEach(row, trend) {
    Bucket b;
    b.label = Text(row, "date", 48);
    if (b.label.empty())
      b.label = Text(row, "time", 48);
    if (b.label.empty() || b.label.substr(0, 10) < first ||
        b.label.substr(0, 10) > last)
      return false;
    std::tm date{};
    if (!DateParts(b.label.substr(0, 10), date))
      return false;
    b.counts = ReadCounts(row);
    if (!Add(next.totals, b.counts))
      return false;
    next.buckets.push_back(std::move(b));
  }
  std::sort(next.buckets.begin(), next.buckets.end(),
            [](const Bucket &a, const Bucket &b) { return a.label < b.label; });
  for (size_t i = 1; i < next.buckets.size(); ++i)
    if (next.buckets[i - 1].label == next.buckets[i].label)
      return false;
  cJSON_ArrayForEach(row, models) {
    Model m;
    m.id = Text(row, "model", 192);
    if (m.id.empty())
      continue;
    const auto c = ReadCounts(row);
    m.tokens_known = Count(row, "total_tokens", m.tokens);
    if (!Item(row, "total_tokens") && c.tokens_known &&
        c.Total() <= uint64_t(kExactInteger)) {
      m.tokens = c.Total();
      m.tokens_known = true;
    }
    m.cost = c.cost;
    m.cost_known = c.cost_known;
    next.models.push_back(std::move(m));
  }
  next.valid = true;
  out = std::move(next);
  return true;
}
bool UsageCurrent(const Usage &usage, time_t now) {
  if (!usage.valid || now < 1600000000)
    return false;
  std::string first, last;
  DateRange(usage.range, now, first, last);
  return usage.first == first && usage.last == last;
}

bool IsKnownPlatform(const std::string &p) {
  for (const char *k : {"anthropic", "openai", "gemini", "antigravity", "grok",
                        "kimi", "zhipu", "deepseek", "minimax", "opencode_go",
                        "command_code", "cline", "typesafe"})
    if (p == k)
      return true;
  return false;
}
bool DecodeAccount(const cJSON *obj, Account &a) {
  Account next;
  if (!PositiveId(Item(obj, "id"), next.id))
    return false;
  next.name = Text(obj, "name", 160);
  next.platform = Text(obj, "platform", 40);
  next.type = Text(obj, "type", 40);
  if (next.platform.empty() || next.type.empty())
    return false;
  if (next.name.empty())
    next.name = "#" + std::to_string(next.id);
  const auto *extra = Item(obj, "extra");
  next.ollama = Flag(Item(obj, "ollama_cloud_usage"), "eligible");
  next.opencode = Flag(Item(obj, "opencode_go_usage"), "eligible");
  next.manual_supported =
      IsKnownPlatform(next.platform) && next.platform != "typesafe" &&
      next.type != "bedrock" && next.type != "service_account";
  if ((next.platform == "anthropic" || next.platform == "openai" ||
       next.platform == "grok") &&
      next.type != "oauth")
    next.manual_supported = false;
  if (next.platform == "deepseek" && next.type != "apikey")
    next.manual_supported = false;
  if ((next.platform == "kimi" || next.platform == "zhipu" ||
       next.platform == "minimax" || next.platform == "command_code" ||
       next.platform == "cline") &&
      next.type != "apikey")
    next.manual_supported = false;
  if (next.platform == "antigravity" && next.type != "oauth")
    next.manual_supported = false;
  if (next.platform == "opencode_go" && !next.opencode)
    next.manual_supported = false;
  if (next.ollama || next.opencode)
    next.manual_supported = true;
  if (next.ollama)
    StateSnapshot(next, Item(obj, "ollama_cloud_usage"), true);
  else if (next.opencode)
    StateSnapshot(next, Item(obj, "opencode_go_usage"), false);
  else if (next.platform == "openai") {
    ExtraWindow(next, extra, "codex_", "5h", "5h");
    ExtraWindow(next, extra, "codex_", "7d", "7d");
    const auto *credits = Item(extra, "codex_credits_snapshot");
    if (Item(credits, "credits"))
      credits = Item(credits, "credits");
    PutCredits(next, credits);
    PutBalance(
        next, "Reset credits",
        Item(Item(extra, "codex_reset_credit_snapshot"), "available_count"));
    next.observed_at = Timestamp(Item(extra, "codex_usage_updated_at"));
  } else if (next.platform == "grok") {
    DecodeQuota(Item(extra, "grok_usage_snapshot"), next);
    DecodeQuota(Item(extra, "grok_billing_snapshot"), next);
  } else {
    ExtraWindow(next, extra, next.platform + "_", "5h", "5h");
    ExtraWindow(next, extra, next.platform + "_", "weekly", "周窗口");
    ExtraWindow(next, extra, next.platform + "_", "monthly", "月窗口");
    const auto *balances = Item(extra, (next.platform + "_balances").c_str());
    const cJSON *b = nullptr;
    cJSON_ArrayForEach(b, balances)
        PutBalance(next, Text(b, "currency", 16), Item(b, "balance"));
    if (next.balances.empty())
      PutBalance(next,
                 Text(extra, (next.platform + "_balance_currency").c_str(), 16),
                 Item(extra, (next.platform + "_balance").c_str()));
    next.observed_at =
        Timestamp(Item(extra, (next.platform + "_usage_updated_at").c_str()));
    if (!next.observed_at)
      next.observed_at = Timestamp(
          Item(extra, (next.platform + "_balance_updated_at").c_str()));
  }
  const bool provider_snapshot = next.snapshot;
  LocalQuota(next, obj);
  if (next.platform == "gemini" && !next.ollama && !next.opencode)
    next.local = true;
  if (next.platform == "anthropic" && next.type == "setup-token")
    next.local = true;
  next.source = next.local
                    ? provider_snapshot ? "站点快照 + 本地配额" : "站点本地配额"
                    : "站点快照";
  if (!next.snapshot)
    next.note = next.manual_supported || next.platform == "anthropic"
                    ? "待手动同步 / 暂无站点快照"
                    : "未提供供应商额度";
  if (!IsKnownPlatform(next.platform))
    next.note = "暂不支持的平台";
  a = std::move(next);
  return true;
}
bool DecodeQuota(const cJSON *obj, Account &a) {
  if (!cJSON_IsObject(obj))
    return false;
  bool known = false;
  if (QuotaPayloadFailed(obj))
    a.error = "站点上次采样失败，保留已有结果";
  if (a.platform == "grok") {
    known |= DecodeQuota(Item(obj, "billing"), a);
    known |= DecodeQuota(Item(obj, "snapshot"), a);
    const auto *probe_error = Item(obj, "probe_error");
    if (cJSON_IsString(probe_error) && probe_error->valuestring &&
        probe_error->valuestring[0])
      a.error = "部分上游采样失败，保留已有结果";
  }
  const std::string period = Text(obj, "period_type", 32);
  if (Item(obj, "usage_percent"))
    known |= PutWindow(a,
                       period == "weekly"    ? "周窗口"
                       : period == "monthly" ? "月窗口"
                                             : "账单窗口",
                       Item(obj, "usage_percent"), Item(obj, "period_end"));
  if (Item(obj, "used_percent"))
    known |= PutWindow(a, "月窗口", Item(obj, "used_percent"),
                       Item(obj, "billing_period_end"));
  known |= PutBalance(a, "预付余额", Item(obj, "prepaid_balance"));
  if (Item(obj, "grok_billing"))
    known |= DecodeQuota(Item(obj, "grok_billing"), a);
  const cJSON *product = nullptr;
  cJSON_ArrayForEach(product, Item(obj, "product_usage")) known |=
      PutWindow(a, Text(product, "product", 160),
                Item(product, "usage_percent"), Item(obj, "period_end"));
  for (const char *k :
       {"five_hour", "seven_day", "seven_day_sonnet", "seven_day_fable",
        "thirty_day", "gemini_shared_daily", "gemini_pro_daily",
        "gemini_flash_daily", "gemini_shared_minute", "gemini_pro_minute",
        "gemini_flash_minute"}) {
    const auto *w = Item(obj, k);
    if (!cJSON_IsObject(w))
      continue;
    const std::string label =
        std::strcmp(k, "five_hour") == 0          ? "5h"
        : std::strcmp(k, "seven_day") == 0        ? "7d"
        : std::strcmp(k, "seven_day_sonnet") == 0 ? "Sonnet 7d"
        : std::strcmp(k, "seven_day_fable") == 0  ? "Fable 7d"
                                                  : k;
    known |= PutWindow(a, label, Item(w, "utilization"), Item(w, "resets_at"));
  }
  known |= RateEnvelope(a, Item(obj, "rate_limit"), "");
  const cJSON *r = nullptr;
  cJSON_ArrayForEach(r, Item(obj, "additional_rate_limits")) known |=
      RateEnvelope(a, Item(r, "rate_limit"), Text(r, "limit_name", 80) + " ");
  known |= PutCredits(a, Item(obj, "credits"));
  known |= PutBalance(a, "Reset credits",
                      Item(Item(obj, "reset_credits"), "available_count"));
  known |= PutBalance(
      a, "Reset credits",
      Item(Item(obj, "rate_limit_reset_credits"), "available_count"));
  if (Item(obj, "available_count"))
    known |= PutBalance(a, "Reset credits", Item(obj, "available_count"));
  cJSON_ArrayForEach(r, Item(obj, "tiers")) known |= PutWindow(
      a, Text(r, "window", 80), Item(r, "used_percent"), Item(r, "reset_at"));
  cJSON_ArrayForEach(r, Item(obj, "balances")) known |=
      PutBalance(a, Text(r, "currency", 16), Item(r, "balance"));
  if (!Item(obj, "balances"))
    known |= PutBalance(a, Text(obj, "currency", 16), Item(obj, "balance"));
  if (cJSON_IsObject(Item(obj, "balance"))) {
    if (!QuotaPayloadFailed(Item(obj, "balance")))
      known |= DecodeQuota(Item(obj, "balance"), a);
    else
      a.error = "余额未更新，保留已有结果";
  }
  const auto *ag = Item(obj, "antigravity_quota");
  cJSON_ArrayForEach(r, ag) if (r->string) known |=
      PutWindow(a, r->string, Item(r, "utilization"), Item(r, "reset_time"));
  cJSON_ArrayForEach(r, Item(obj, "ai_credits")) known |=
      PutBalance(a, Text(r, "credit_type", 64), Item(r, "amount"));
  for (const char *key : {"requests", "tokens"}) {
    const auto *limit = Item(obj, key);
    if (!limit)
      limit = Item(obj, std::strcmp(key, "requests") == 0 ? "grok_request_quota"
                                                          : "grok_token_quota");
    const double maximum = Number(limit, "limit"),
                 remaining = Number(limit, "remaining", -1);
    if (maximum > 0 && remaining >= 0) {
      Json p(cJSON_CreateNumber(100 * (maximum - remaining) / maximum),
             cJSON_Delete);
      const auto *reset = Item(limit, "reset_unix");
      if (!reset || cJSON_IsNull(reset))
        reset = Item(limit, "reset_at");
      known |= PutWindow(a, key, p.get(), reset);
    }
  }
  if (a.ollama || a.opencode)
    known |= StateSnapshot(a, obj, a.ollama);
  const auto observed = Timestamp(Item(obj, "fetched_at"));
  const auto updated = Timestamp(Item(obj, "updated_at"));
  if (observed || updated)
    a.observed_at = observed ? observed : updated;
  if (Flag(obj, "partial"))
    a.error = "部分窗口未更新，保留已有结果";
  if (a.snapshot)
    a.note.clear();
  return known;
}
bool QuotaPayloadFailed(const cJSON *obj) {
  const auto *success = Item(obj, "success");
  if (!cJSON_IsObject(obj) || (success && !cJSON_IsTrue(success)))
    return true;
  for (const char *key : {"error", "error_code"}) {
    const auto *value = Item(obj, key);
    if (value && !cJSON_IsNull(value) &&
        (!cJSON_IsString(value) ||
         (value->valuestring && value->valuestring[0])))
      return true;
  }
  const auto *snapshot = Item(obj, "snapshot");
  const auto *snapshot_status = Item(snapshot, "status");
  return (snapshot_status &&
          (!cJSON_IsString(snapshot_status) || !snapshot_status->valuestring ||
           std::strcmp(snapshot_status->valuestring, "ok") != 0)) ||
         (Number(obj, "status_code") >= 400 && !Flag(obj, "partial"));
}
std::vector<Request> PassiveRequests(const Account &a) {
  const std::string base = "/api/v1/admin/accounts/" + std::to_string(a.id);
  if (a.ollama)
    return {{"GET", base + "/ollama-cloud-usage"}};
  if (a.opencode)
    return {{"GET", base + "/opencode-go-usage"}};
  if (a.platform == "anthropic" &&
      (a.type == "oauth" || a.type == "setup-token"))
    return {{"GET", base + "/usage?source=passive"}};
  return {};
}
std::vector<Request> ManualRequests(const Account &a) {
  if (!a.manual_supported || !IsKnownPlatform(a.platform))
    return {};
  const std::string id = std::to_string(a.id),
                    base = "/api/v1/admin/accounts/" + id;
  if (a.ollama)
    return {{"POST", base + "/ollama-cloud-usage/refresh"}};
  if (a.opencode)
    return {{"POST", base + "/opencode-go-usage/refresh"}};
  if (a.platform == "opencode_go")
    return {{"POST", base + "/opencode-go-usage/refresh"}};
  if (a.platform == "openai")
    return {{"GET", "/api/v1/admin/openai/accounts/" + id + "/quota"}};
  if (a.platform == "grok")
    return {{"GET", "/api/v1/admin/grok/accounts/" + id + "/quota"}};
  if (a.platform == "anthropic")
    return {{"GET", base + "/usage?source=active&force=false"},
            {"GET", base + "/claude/reset-credits"}};
  if (a.platform == "antigravity" || a.platform == "gemini")
    return {{"GET", base + "/usage?source=active&force=false"}};
  const std::string cn = "/api/v1/admin/cn-providers/accounts/" + id;
  if (a.platform == "deepseek")
    return {{"GET", cn + "/balance"}};
  if (a.platform == "kimi")
    return {{"GET", cn + "/quota"}, {"GET", cn + "/balance"}};
  return {{"GET", cn + "/quota"}};
}

Json EncodeUsage(const Usage &u) {
  Json root(cJSON_CreateObject(), cJSON_Delete);
  auto *r = root.get();
  cJSON_AddNumberToObject(r, "user_id", u.user_id);
  cJSON_AddStringToObject(r, "range", RangeName(u.range));
  cJSON_AddStringToObject(r, "start_date", u.first.c_str());
  cJSON_AddStringToObject(r, "end_date", u.last.c_str());
  cJSON_AddNumberToObject(r, "received_at", u.received_at);
  auto *rows = cJSON_AddArrayToObject(r, "trend");
  for (const auto &b : u.buckets) {
    auto *v = cJSON_CreateObject();
    cJSON_AddItemToArray(rows, v);
    cJSON_AddStringToObject(v, "date", b.label.c_str());
    if (b.counts.tokens_known) {
      cJSON_AddNumberToObject(v, "input_tokens", b.counts.input);
      cJSON_AddNumberToObject(v, "output_tokens", b.counts.output);
      cJSON_AddNumberToObject(v, "cache_creation_tokens", b.counts.write);
      cJSON_AddNumberToObject(v, "cache_read_tokens", b.counts.read);
    }
    if (b.counts.requests_known)
      cJSON_AddNumberToObject(v, "requests", b.counts.requests);
    if (b.counts.cost_known)
      cJSON_AddNumberToObject(v, "actual_cost", b.counts.cost);
  }
  auto *models = cJSON_AddArrayToObject(r, "models");
  for (const auto &m : u.models) {
    auto *v = cJSON_CreateObject();
    cJSON_AddItemToArray(models, v);
    cJSON_AddStringToObject(v, "model", m.id.c_str());
    if (m.tokens_known)
      cJSON_AddNumberToObject(v, "total_tokens", m.tokens);
    if (m.cost_known)
      cJSON_AddNumberToObject(v, "actual_cost", m.cost);
  }
  return root;
}
bool DecodeCachedUsage(const cJSON *obj, Usage &u) {
  int64_t id = 0;
  Range r;
  if (!PositiveId(Item(obj, "user_id"), id) ||
      !ParseRange(Text(obj, "range"), r))
    return false;
  if (!DecodeUsage(obj, id, r, Text(obj, "start_date"), Text(obj, "end_date"),
                   u))
    return false;
  u.received_at = Timestamp(Item(obj, "received_at"));
  return u.received_at > 0;
}
Json EncodeAccount(const Account &a) {
  Json root(cJSON_CreateObject(), cJSON_Delete);
  auto *r = root.get();
  cJSON_AddNumberToObject(r, "id", a.id);
  cJSON_AddStringToObject(r, "name", a.name.c_str());
  cJSON_AddStringToObject(r, "platform", a.platform.c_str());
  cJSON_AddStringToObject(r, "type", a.type.c_str());
  cJSON_AddBoolToObject(r, "snapshot", a.snapshot);
  cJSON_AddBoolToObject(r, "local", a.local);
  cJSON_AddBoolToObject(r, "manual_supported", a.manual_supported);
  cJSON_AddBoolToObject(r, "ollama", a.ollama);
  cJSON_AddBoolToObject(r, "opencode", a.opencode);
  cJSON_AddNumberToObject(r, "observed_at", a.observed_at);
  cJSON_AddNumberToObject(r, "received_at", a.received_at);
  cJSON_AddStringToObject(r, "source", a.source.c_str());
  cJSON_AddStringToObject(r, "note", a.note.c_str());
  auto *windows = cJSON_AddArrayToObject(r, "windows");
  for (const auto &w : a.windows) {
    auto *v = cJSON_CreateObject();
    cJSON_AddItemToArray(windows, v);
    cJSON_AddStringToObject(v, "label", w.label.c_str());
    if (w.known)
      cJSON_AddNumberToObject(v, "used", w.used);
    cJSON_AddNumberToObject(v, "reset_at", w.reset_at);
    cJSON_AddStringToObject(v, "reset_text", w.reset_text.c_str());
  }
  auto *balances = cJSON_AddArrayToObject(r, "balances");
  for (const auto &b : a.balances) {
    auto *v = cJSON_CreateObject();
    cJSON_AddItemToArray(balances, v);
    cJSON_AddStringToObject(v, "label", b.label.c_str());
    cJSON_AddStringToObject(v, "value", b.value.c_str());
  }
  return root;
}
bool DecodeCachedAccount(const cJSON *obj, Account &a) {
  if (!DecodeAccount(obj, a))
    return false;
  a.windows.clear();
  a.balances.clear();
  a.snapshot = false;
  a.local = Flag(obj, "local");
  const bool base_supported = a.manual_supported;
  a.ollama = Flag(obj, "ollama");
  a.opencode = Flag(obj, "opencode");
  a.manual_supported = Flag(obj, "manual_supported") &&
                       IsKnownPlatform(a.platform) &&
                       (base_supported || a.ollama || a.opencode);
  a.observed_at = Timestamp(Item(obj, "observed_at"));
  a.received_at = Timestamp(Item(obj, "received_at"));
  a.source = Text(obj, "source", 64);
  a.note = Text(obj, "note", 256);
  const cJSON *v = nullptr;
  const auto *windows = Item(obj, "windows"), *balances = Item(obj, "balances");
  if (!cJSON_IsArray(windows) || !cJSON_IsArray(balances) ||
      cJSON_GetArraySize(windows) > int(kMaxWindows) ||
      cJSON_GetArraySize(balances) > 64)
    return false;
  cJSON_ArrayForEach(v, windows) {
    const auto *reset = Item(v, "reset_at");
    if (Number(v, "reset_at") <= 0)
      reset = Item(v, "reset_text");
    PutWindow(a, Text(v, "label", 160), Item(v, "used"), reset);
  }
  cJSON_ArrayForEach(v, Item(obj, "balances"))
      PutBalance(a, Text(v, "label", 192), Item(v, "value"));
  return true;
}
} // namespace inkmeter
