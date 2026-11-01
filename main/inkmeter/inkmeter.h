#pragma once

#include "inkmeter_model.h"
#include <atomic>

namespace inkmeter {

struct View {
  bool initialized = false, configured = false, busy = false;
  std::string site, user_name, status, network_status;
  int64_t user_id = 0, syncing_account = 0;
  int usage_minutes = 15;
  size_t selected_accounts = 0;
  Usage usage;
  std::vector<Account> accounts;
  std::vector<std::pair<int64_t, std::string>> users;
  int user_page = 1;
  bool more_users = false;
};

struct ApiResult {
  int status = 200;
  std::string json;
};

void Enter();
void Leave();
void SetStandby(bool standby);
void Tick();
bool IsActive();
uint64_t Revision();
View GetView();
std::string BuildDescription();
void SelectRange(Range range);
void SelectUser(int64_t id);
void BrowseUsers(int page);
void RefreshUsage();
void ReadQuotaSnapshots();
void SyncAccount(int64_t id);
// Called only on the configuration worker, never from LVGL.
ApiResult HandleApi(const std::string &method, const std::string &path,
                    const std::string &query, const std::string &body,
                    uint64_t authorization_epoch = 0);
bool ConfigurationSessionValid(uint64_t epoch);
void CancelConfigurationRequests(uint64_t epoch);

struct PairingInfo {
  bool open = false, paired = false, starting = false;
  std::string url, code, error;
  int remaining_seconds = 0;
};
void OpenConfiguration();
void CloseConfiguration(uint64_t expected_epoch = 0);
PairingInfo GetPairingInfo();

} // namespace inkmeter
