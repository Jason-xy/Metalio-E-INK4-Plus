#include "esp_ssl.h"
#include <esp_log.h>
#include <esp_crt_bundle.h>
#include <cerrno>
#include <cstring>
#include <unistd.h>
#include <algorithm>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <esp_timer.h>

static const char *TAG = "EspSsl";

EspSsl::EspSsl() {
    event_group_ = xEventGroupCreate();
}

EspSsl::~EspSsl() {
    Disconnect();

    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
        event_group_ = nullptr;
    }
}

bool EspSsl::Connect(const std::string& host, int port) {
    if (event_group_ == nullptr) {
        last_error_ = ENOMEM;
        ESP_LOGE(TAG, "Failed to allocate TLS receive event group");
        return false;
    }
    if (tls_client_ != nullptr) {
        ESP_LOGE(TAG, "tls client has been initialized");
        return false;
    }

    tls_client_ = esp_tls_init();
    if (tls_client_ == nullptr) {
        last_error_ = ENOMEM;
        ESP_LOGE(TAG, "Failed to initialize TLS");
        return false;
    }

    esp_tls_cfg_t cfg = {};
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    if (request_deadline_us_) {
        const int64_t remaining_ms = (request_deadline_us_ - esp_timer_get_time()) / 1000;
        if (remaining_ms <= 0) {
            last_error_ = ETIMEDOUT;
            esp_tls_conn_destroy(tls_client_);
            tls_client_ = nullptr;
            return false;
        }
        cfg.timeout_ms = int(remaining_ms);
    }

    int ret = esp_tls_conn_new_sync(host.c_str(), host.length(), port, &cfg, tls_client_);
    if (ret != 1) {
        esp_tls_error_handle_t last_error;
        if (esp_tls_get_error_handle(tls_client_, &last_error) == ESP_OK) {
            int error_code, error_flags;
            esp_err_t err = esp_tls_get_and_clear_last_error(last_error, &error_code, &error_flags);
            last_error_ = err;
        } else {
            last_error_ = -1;
            ESP_LOGE(TAG, "Failed to get error handle");
        }
        esp_tls_conn_destroy(tls_client_);
        tls_client_ = nullptr;
        return false;
    }

    connected_ = true;

    xEventGroupClearBits(event_group_, ESP_SSL_EVENT_RECEIVE_TASK_EXIT);
    const auto task_result = xTaskCreate([](void* arg) {
        EspSsl* ssl = (EspSsl*)arg;
        ssl->ReceiveTask();
        xEventGroupSetBits(ssl->event_group_, ESP_SSL_EVENT_RECEIVE_TASK_EXIT);
        vTaskDelete(NULL);
    }, "ssl_receive", 4096, this, 1, &receive_task_handle_);
    if (task_result != pdPASS) {
        connected_ = false;
        receive_task_handle_ = nullptr;
        last_error_ = ENOMEM;
        ESP_LOGE(TAG, "Failed to create TLS receive task");
        // No task exists to signal exit; release TLS without waiting for one.
        esp_tls_conn_destroy(tls_client_);
        tls_client_ = nullptr;
        return false;
    }
    return true;
}

void EspSsl::Disconnect() {
    connected_ = false;
    
    // Wake the reader without releasing the descriptor owned by esp_tls.
    if (tls_client_ != nullptr) {
        int sockfd;
        ESP_ERROR_CHECK(esp_tls_get_conn_sockfd(tls_client_, &sockfd));
        if (sockfd >= 0) {
            shutdown(sockfd, SHUT_RDWR);
        }
    
        auto bits = xEventGroupWaitBits(event_group_, ESP_SSL_EVENT_RECEIVE_TASK_EXIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(10000));
        if (!(bits & ESP_SSL_EVENT_RECEIVE_TASK_EXIT)) {
            ESP_LOGE(TAG, "Failed to wait for receive task exit");
        }

        esp_tls_conn_destroy(tls_client_);
        tls_client_ = nullptr;
    }
}

/* CONFIG_MBEDTLS_SSL_RENEGOTIATION should be disabled in sdkconfig.
 * Otherwise, invalid memory access may be triggered.
 */
int EspSsl::Send(const std::string& data) {
    if (!connected_) {
        ESP_LOGE(TAG, "Not connected");
        return -1;
    }

    size_t total_sent = 0;
    size_t data_size = data.size();
    const char* data_ptr = data.data();
    
    while (total_sent < data_size) {
        if (request_deadline_us_ && esp_timer_get_time() >= request_deadline_us_) {
            last_error_ = ETIMEDOUT;
            return -1;
        }
        const size_t write_size = data_size - total_sent;
        int ret = esp_tls_conn_write(tls_client_, data_ptr + total_sent, write_size);

        if (ret == ESP_TLS_ERR_SSL_WANT_WRITE) {
            continue;
        }

        if (ret <= 0) {
            ESP_LOGE(TAG, "SSL send failed: ret=%d, errno=%d", ret, errno);
            return ret;
        }
        
        total_sent += ret;
    }
    
    return total_sent;
}

void EspSsl::ReceiveTask() {
    std::string data;
    while (connected_) {
        data.resize(1500);
        int ret = esp_tls_conn_read(tls_client_, data.data(), data.size());

        if (ret == ESP_TLS_ERR_SSL_WANT_READ) {
            continue;
        }

        if (ret <= 0) {
            if (ret < 0) {
                ESP_LOGE(TAG, "SSL receive failed: %d", ret);
            }
            connected_ = false;
            // 接收失败或连接断开时调用断连回调
            if (disconnect_callback_) {
                disconnect_callback_();
            }
            break;
        }
        
        if (stream_callback_) {
            data.resize(ret);
            stream_callback_(data);
        }
    }
}

int EspSsl::GetLastError() {
    return last_error_;
}
