#include "inkmeter_clock.h"

#include "application.h"
#include "board.h"
#include "inkmeter_model.h"
#include "pcf8563.h"
#include "power_policy.h"
#include "screen/standby_screen/standby_screen.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <esp_log.h>
#include <esp_network.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <fcntl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/dns.h>
#include <lwip/sockets.h>
#include <lwip/tcpip.h>
#include <mutex>
#include <sys/time.h>
#include <unistd.h>

namespace inkmeter {
namespace {
constexpr char kTag[] = "InkMeterClock";
constexpr const char *kServers[] = {"ntp.aliyun.com", "time.cloudflare.com"};
constexpr int64_t kSecond = 1000000, kNormalInterval = 6 * 3600 * kSecond;
constexpr int64_t kMinUtc = 1704067200; // 2024-01-01
constexpr int64_t kMaxUtc = 4102444800 - kHongKongOffsetSeconds;
std::atomic<bool> queued{false};
std::atomic<int64_t> next_sync{0}, retry_until{0}, next_rtc{0};
std::mutex clock_mu;
unsigned failures = 0, server_index = 0; // clock_mu

// A timed-out query keeps this slot until its final callback; no stack pointer
// or task handle survives in lwIP, and unfinished queries cannot accumulate.
struct DnsSlot {
  std::atomic<bool> busy{false}, done{false};
  ip_addr_t address{};
  const char *host = nullptr;
  int64_t deadline = 0;
  bool resolved = false;
} dns;

bool Allowed() {
  auto &application = Application::GetInstance();
  const auto state = application.GetDeviceState();
  return application.IsBootReady() && state != kDeviceStateUpgrading &&
         state != kDeviceStateFatalError &&
         !StandbyScreen::IsActive() &&
         !Board::GetInstance().IsWifiConfigMode();
}

bool Valid(const std::function<bool()> &still_valid, int64_t deadline) {
  return esp_timer_get_time() < deadline && Allowed() &&
         (!still_valid || still_valid());
}

void DnsFound(const char *, const ip_addr_t *address, void *arg) {
  auto &slot = *static_cast<DnsSlot *>(arg);
  slot.resolved = address && IP_IS_V4(address) && !ip_addr_isany(address);
  if (slot.resolved)
    slot.address = *address;
  slot.done.store(true, std::memory_order_release);
  // This must be the last access: the next query can now reuse the slot.
  slot.busy.store(false, std::memory_order_release);
}

void StartDns(void *arg) {
  auto &slot = *static_cast<DnsSlot *>(arg);
  if (esp_timer_get_time() >= slot.deadline) {
    DnsFound(nullptr, nullptr, arg);
    return;
  }
  ip_addr_t address{};
  const auto result = dns_gethostbyname_addrtype(
      slot.host, &address, DnsFound, arg, LWIP_DNS_ADDRTYPE_IPV4);
  if (result != ERR_INPROGRESS)
    DnsFound(nullptr, result == ERR_OK ? &address : nullptr, arg);
}

bool Resolve(const char *host, uint32_t &address,
             const std::function<bool()> &still_valid, int64_t deadline) {
  bool expected = false;
  if (!dns.busy.compare_exchange_strong(expected, true,
                                        std::memory_order_acq_rel))
    return false;
  dns.done.store(false, std::memory_order_relaxed);
  dns.host = host;
  dns.deadline = std::min(deadline, esp_timer_get_time() + 3 * kSecond);
  if (tcpip_try_callback(StartDns, &dns) != ERR_OK) {
    dns.busy.store(false, std::memory_order_release);
    return false;
  }
  while (!dns.done.load(std::memory_order_acquire)) {
    if (!Valid(still_valid, dns.deadline))
      return false;
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  if (!Valid(still_valid, dns.deadline) || !dns.resolved)
    return false;
  address = ip4_addr_get_u32(ip_2_ip4(&dns.address));
  return true;
}

uint32_t Read32(const uint8_t *p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
         (uint32_t(p[2]) << 8) | p[3];
}

bool DecodeReply(const uint8_t *packet, size_t length, const uint8_t *nonce,
                 timeval &utc) {
  if (length != 48 || (packet[0] & 7) != 4 || (packet[0] >> 6) == 3 ||
      ((packet[0] >> 3) & 7) < 3 || ((packet[0] >> 3) & 7) > 4 ||
      packet[1] == 0 || packet[1] > 15 ||
      std::memcmp(packet + 24, nonce, 8) != 0)
    return false;
  uint64_t seconds = Read32(packet + 40);
  const uint32_t fraction = Read32(packet + 44);
  if (!seconds && !fraction)
    return false;
  // Unfold the 2036 era for the supported 2024..2099 calendar range.
  if (seconds < 2208988800ULL)
    seconds += 1ULL << 32;
  const int64_t unix_seconds = seconds - 2208988800ULL;
  if (unix_seconds < kMinUtc || unix_seconds >= kMaxUtc)
    return false;
  utc.tv_sec = static_cast<time_t>(unix_seconds);
  utc.tv_usec = (uint64_t(fraction) * kSecond) >> 32;
  return true;
}

bool Query(uint32_t address, timeval &utc,
           const std::function<bool()> &still_valid, int64_t deadline) {
  struct Socket {
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ~Socket() {
      if (fd >= 0)
        close(fd);
    }
  } socket;
  if (socket.fd < 0 || socket.fd >= FD_SETSIZE)
    return false;
  const int flags = fcntl(socket.fd, F_GETFL, 0);
  if (flags < 0 || fcntl(socket.fd, F_SETFL, flags | O_NONBLOCK) < 0)
    return false;
  sockaddr_in peer{};
  peer.sin_family = AF_INET;
  peer.sin_port = htons(123);
  peer.sin_addr.s_addr = address;
  if (!Valid(still_valid, deadline) ||
      connect(socket.fd, reinterpret_cast<sockaddr *>(&peer), sizeof(peer)) != 0)
    return false;
  std::array<uint8_t, 48> request{};
  request[0] = 0x23; // NTPv4 client; the echoed nonce correlates this UDP reply.
  esp_fill_random(request.data() + 40, 8);
  if (send(socket.fd, request.data(), request.size(), 0) != int(request.size()))
    return false;
  const int64_t end = std::min(deadline, esp_timer_get_time() + 4 * kSecond);
  while (Valid(still_valid, end)) {
    const int64_t remaining = std::min<int64_t>(100000, end - esp_timer_get_time());
    if (remaining <= 0)
      break;
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(socket.fd, &readable);
    timeval wait{0, static_cast<suseconds_t>(remaining)};
    const int ready = select(socket.fd + 1, &readable, nullptr, nullptr, &wait);
    if (ready < 0 && errno != EINTR)
      return false;
    if (ready <= 0)
      continue;
    std::array<uint8_t, 49> reply{};
    const int length = recv(socket.fd, reply.data(), reply.size(), 0);
    if (length < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
      return false;
    if (length > 0 && DecodeReply(reply.data(), length, request.data() + 40, utc))
      return Valid(still_valid, end);
  }
  return false;
}

bool WriteRtc(const std::function<bool()> &still_valid, int64_t deadline) {
  auto &rtc = Pcf8563::GetInstance();
  std::tm calendar{};
  bool vl_ok = false;
  const bool read = Valid(still_valid, deadline) && rtc.SyncSystemToRtc() &&
                    Valid(still_valid, deadline) &&
                    rtc.GetTime(calendar, &vl_ok);
  const bool calendar_ok =
      read && calendar.tm_year >= 124 && calendar.tm_year <= 199 &&
      calendar.tm_mon >= 0 && calendar.tm_mon < 12 && calendar.tm_mday >= 1 &&
      calendar.tm_mday <= 31 && calendar.tm_hour >= 0 && calendar.tm_hour < 24 &&
      calendar.tm_min >= 0 && calendar.tm_min < 60 && calendar.tm_sec >= 0 &&
      calendar.tm_sec < 60;
  auto normalized = calendar;
  const time_t rtc_time = calendar_ok ? timegm(&normalized) : 0;
  const int64_t difference = int64_t(rtc_time) - time(nullptr);
  const bool ok = calendar_ok && normalized.tm_mday == calendar.tm_mday &&
                  normalized.tm_mon == calendar.tm_mon && vl_ok &&
                  difference >= -2 && difference <= 2;
  next_rtc = ok ? 0 : esp_timer_get_time() + 60 * kSecond;
  ESP_LOGI(kTag, "RTC writeback=%s VL_ok=%d delta=%lld", ok ? "ok" : "retry",
           vl_ok, static_cast<long long>(difference));
  return ok;
}

bool Synchronize(bool wake_network, const std::function<bool()> &still_valid,
                 int64_t deadline) {
  std::unique_lock<std::mutex> lock(clock_mu, std::try_to_lock);
  if (!lock || !Allowed())
    return ClockReady();
  const int64_t now = esp_timer_get_time();
  deadline = deadline ? std::min(deadline, now + 8 * kSecond) : now + 8 * kSecond;
  if (!Valid(still_valid, deadline))
    return ClockReady();
  if (ClockReady() && next_rtc.load() > 0 && now >= next_rtc.load()) {
    PowerNeedHold need(PowerNeed::ForegroundNetwork);
    WriteRtc(still_valid, deadline);
    return true;
  }
  if (now < retry_until.load() || (ClockReady() && now < next_sync.load()))
    return ClockReady();
  auto &board = Board::GetInstance();
  if (!dynamic_cast<EspNetwork *>(board.GetNetwork()))
    return ClockReady();
  if (!wake_network && !board.IsNetworkReady())
    return ClockReady();
  PowerNeedHold need(PowerNeed::ForegroundNetwork);
  bool connected = board.IsNetworkReady();
  while (!connected && wake_network && Valid(still_valid, deadline) &&
         !board.GetNetworkErrorText(nullptr)) {
    const int wait_ms =
        std::min<int64_t>(100, (deadline - esp_timer_get_time()) / 1000);
    if (wait_ms <= 0)
      break;
    const int64_t slice_end = esp_timer_get_time() + wait_ms * 1000LL;
    connected = board.EnsureNetworkReady(wait_ms);
    // Disabled/busy radios can fail immediately; keep retries paced and bounded.
    const int64_t rest =
        std::min(slice_end, deadline) - esp_timer_get_time();
    if (!connected && rest > 0 && Valid(still_valid, deadline))
      vTaskDelay(std::max<TickType_t>(
          1, pdMS_TO_TICKS(static_cast<uint32_t>((rest + 999) / 1000))));
  }
  const char *server = kServers[server_index];
  uint32_t address = 0;
  timeval utc{};
  if (connected && Valid(still_valid, deadline) &&
      Resolve(server, address, still_valid, deadline) &&
      Query(address, utc, still_valid, deadline) &&
      Valid(still_valid, deadline)) {
    timeval local = utc;
    local.tv_sec += kHongKongOffsetSeconds;
    if (settimeofday(&local, nullptr) == 0) {
      failures = 0;
      retry_until = 0;
      next_sync = esp_timer_get_time() + kNormalInterval;
      const bool rtc_ok = WriteRtc(still_valid, deadline);
      std::tm calendar{};
      const time_t local_seconds = local.tv_sec;
      gmtime_r(&local_seconds, &calendar);
      ESP_LOGI(kTag,
               "NTP server=%s utc=%lld local=%04d-%02d-%02d %02d:%02d:%02d rtc=%s",
               server, static_cast<long long>(utc.tv_sec), calendar.tm_year + 1900,
               calendar.tm_mon + 1, calendar.tm_mday, calendar.tm_hour,
               calendar.tm_min, calendar.tm_sec, rtc_ok ? "ok" : "retry");
      return true;
    }
  }
  server_index = (server_index + 1) % 2;
  if (Allowed() && (!still_valid || still_valid())) {
    failures = std::min(failures + 1, 3U);
    constexpr int delays[] = {30, 60, 300};
    retry_until = esp_timer_get_time() + delays[failures - 1] * kSecond;
    ESP_LOGW(kTag, "NTP failed server=%s retry_seconds=%d clock_ready=%d", server,
             delays[failures - 1], ClockReady());
  } else {
    retry_until = esp_timer_get_time() + kSecond;
  }
  return ClockReady();
}
} // namespace

bool ClockReady() {
  const int64_t utc = UtcNow();
  return utc >= kMinUtc && utc < kMaxUtc;
}

int64_t ClockRetryAt() { return ClockReady() ? 0 : retry_until.load(); }

bool EnsureClock(const std::function<bool()> &still_valid, int64_t deadline) {
  return ClockReady() || Synchronize(true, still_valid, deadline);
}

void ServiceClock() {
  const int64_t now = esp_timer_get_time();
  const bool rtc_due =
      ClockReady() && next_rtc.load() > 0 && now >= next_rtc.load();
  const bool sync_due = now >= retry_until.load() &&
                        (!ClockReady() || now >= next_sync.load());
  if ((!rtc_due && !sync_due) || !Allowed() ||
      !Board::GetInstance().IsNetworkReady())
    return;
  bool expected = false;
  if (!queued.compare_exchange_strong(expected, true))
    return;
  if (!Application::GetInstance().Schedule(
          []() {
            struct Done {
              ~Done() { queued = false; }
            } done;
            Synchronize(false, {}, 0);
          },
          Application::TaskDomain::Network)) {
    queued = false;
    retry_until = now + kSecond;
  }
}
} // namespace inkmeter
