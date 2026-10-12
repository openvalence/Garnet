#include "wifi.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"
#include "lwip/sockets.h"
#include "nvs.h"

namespace garnet::wifi {
namespace {

constexpr char TAG[] = "wifi";
constexpr char NVS_NS[] = "garnet";
constexpr int FAIL_LIMIT = 5;
constexpr int RETRY_CAP_S = 10;
constexpr int MAX_SCAN = 16;
constexpr uint8_t AP_IP[4] = {192, 168, 4, 1};

// ssid_/pass_ are written only by load/set_credentials/forget; readers take no lock.
char ssid_[33], pass_[65];
char portal_ssid_[16], portal_pass_[9];
char scan_[MAX_SCAN][33];
wifi_ap_record_t recs_[MAX_SCAN];

std::atomic<State> state_{State::Off};
std::atomic<uint32_t> ip_{0}, bcast_{0};
std::atomic<bool> started_{false}, expect_disc_{false};
std::atomic<bool> dns_want_{false}, dns_running_{false};
std::atomic<int> scan_n_{0}, fails_{0};
esp_netif_t* ap_if_;
esp_timer_handle_t retry_t_, apply_t_;
httpd_handle_t httpd_;

void apply_sta();

void arm(esp_timer_handle_t t, uint64_t us) {
  esp_timer_stop(t);
  esp_timer_start_once(t, us);
}

// ---- captive DNS: every A query answers 192.168.4.1 ----
void dns_task(void*) {
  do {
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(53);
    timeval tv{0, 500000};
    if (s >= 0) setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    bool ok = s >= 0 && bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0;
    uint8_t b[256];
    while (ok && dns_want_) {
      sockaddr_in from;
      socklen_t fl = sizeof from;
      int n = recvfrom(s, b, sizeof b - 16, 0, reinterpret_cast<sockaddr*>(&from), &fl);
      if (n < 17 || (b[2] & 0x80) || b[4] || b[5] != 1) continue;
      int i = 12;
      while (i < n && b[i]) i += b[i] + 1;
      i += 5;  // root label + QTYPE + QCLASS
      if (i > n) continue;
      bool a_rec = b[i - 4] == 0 && b[i - 3] == 1;
      b[2] = 0x81; b[3] = 0x80;
      b[6] = 0; b[7] = a_rec ? 1 : 0;
      b[8] = b[9] = b[10] = b[11] = 0;
      if (a_rec) {
        const uint8_t ans[] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4,
                               AP_IP[0], AP_IP[1], AP_IP[2], AP_IP[3]};
        memcpy(b + i, ans, sizeof ans);
        i += sizeof ans;
      }
      sendto(s, b, i, 0, reinterpret_cast<sockaddr*>(&from), fl);
    }
    if (s >= 0) close(s);
    if (!ok && dns_want_) vTaskDelay(pdMS_TO_TICKS(500));
    dns_running_ = false;
  } while (dns_want_ && !dns_running_.exchange(true));
  vTaskDelete(nullptr);
}

void dns_start() {
  dns_want_ = true;
  if (!dns_running_.exchange(true)) xTaskCreate(dns_task, "dns", 3072, nullptr, 3, nullptr);
}

// ---- portal HTTP ----
void esc(const char* in, char* out) {
  for (; *in; ++in) {
    switch (*in) {
      case '&': out += sprintf(out, "&amp;"); break;
      case '<': out += sprintf(out, "&lt;"); break;
      case '>': out += sprintf(out, "&gt;"); break;
      case '"': out += sprintf(out, "&quot;"); break;
      case '\'': out += sprintf(out, "&#39;"); break;
      default: *out++ = *in;
    }
  }
  *out = 0;
}

void url_decode(char* s) {
  char* o = s;
  for (; *s; ++s) {
    unsigned v;
    if (*s == '+') *o++ = ' ';
    else if (*s == '%' && s[1] && s[2] && sscanf(s + 1, "%2x", &v) == 1) { *o++ = char(v); s += 2; }
    else *o++ = *s;
  }
  *o = 0;
}

esp_err_t page(httpd_req_t* r, const char* body) {
  httpd_resp_set_type(r, "text/html");
  return httpd_resp_sendstr(r, body);
}

esp_err_t get_root(httpd_req_t* r) {
  char e[200];
  httpd_resp_set_type(r, "text/html");
  httpd_resp_sendstr_chunk(r, "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
                              "<title>Garnet</title><h2>Garnet Wi-Fi</h2><form method=post action=/save>"
                              "<p>Network<br><input name=ssid list=n maxlength=32 required><datalist id=n>");
  for (int i = 0, n = scan_n_; i < n; ++i) {
    esc(scan_[i], e);
    httpd_resp_sendstr_chunk(r, "<option value=\"");
    httpd_resp_sendstr_chunk(r, e);
    httpd_resp_sendstr_chunk(r, "\">");
  }
  httpd_resp_sendstr_chunk(r, "</datalist><p>Password<br><input name=pass type=password maxlength=63>"
                              "<p><button>Join</button></form>");
  return httpd_resp_sendstr_chunk(r, nullptr);
}

esp_err_t post_save(httpd_req_t* r) {
  char body[400], rs[97], rp[193];
  size_t len = r->content_len;
  if (len == 0 || len >= sizeof body) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "bad size");
  for (size_t got = 0; got < len;) {
    int n = httpd_req_recv(r, body + got, len - got);
    if (n <= 0) return ESP_FAIL;
    got += n;
  }
  body[len] = 0;
  rs[0] = rp[0] = 0;
  httpd_query_key_value(body, "ssid", rs, sizeof rs);
  httpd_query_key_value(body, "pass", rp, sizeof rp);
  url_decode(rs);
  url_decode(rp);
  if (!set_credentials(rs, rp)) return page(r, "Invalid network or password. <a href=/>Back</a>");
  char e[200], out[320];
  esc(rs, e);
  snprintf(out, sizeof out, "Saved. Garnet is joining %s.", e);
  return page(r, out);
}

esp_err_t get_any(httpd_req_t* r) {
  httpd_resp_set_status(r, "302 Found");
  httpd_resp_set_hdr(r, "Location", "http://192.168.4.1/");
  return httpd_resp_send(r, nullptr, 0);
}

// Order matters: the wildcard must stay last.
const httpd_uri_t kUris[] = {{"/", HTTP_GET, get_root, nullptr},
                             {"/save", HTTP_POST, post_save, nullptr},
                             {"/*", HTTP_GET, get_any, nullptr}};

void http_start() {
  if (httpd_) return;
  httpd_config_t c = HTTPD_DEFAULT_CONFIG();
  c.stack_size = 6144;
  c.max_uri_handlers = 3;
  c.lru_purge_enable = true;
  c.uri_match_fn = httpd_uri_match_wildcard;
  if (httpd_start(&httpd_, &c) != ESP_OK) { httpd_ = nullptr; ESP_LOGW(TAG, "httpd start failed"); return; }
  for (const auto& h : kUris) httpd_register_uri_handler(httpd_, &h);
}

// ---- state machine ----
void retry_cb(void*) {
  State s = state_;
  if (s == State::Connecting || s == State::Failed) esp_wifi_connect();
}

void apply_cb(void*) { apply_sta(); }

void on_scan_done() {
  uint16_t n = MAX_SCAN;
  if (esp_wifi_scan_get_ap_records(&n, recs_) != ESP_OK) return;
  int k = 0;
  for (int i = 0; i < n; ++i) {
    const char* s = reinterpret_cast<const char*>(recs_[i].ssid);
    bool dup = !*s;
    for (int j = 0; j < k && !dup; ++j) dup = !strcmp(scan_[j], s);
    if (!dup) strcpy(scan_[k++], s);
  }
  scan_n_ = k;
}

void on_disconnect() {
  State s = state_;
  if (s == State::Portal || s == State::Off) return;
  ip_ = bcast_ = 0;
  if (expect_disc_.exchange(false)) { esp_wifi_connect(); return; }
  int f = ++fails_;
  state_ = f >= FAIL_LIMIT ? State::Failed : State::Connecting;
  arm(retry_t_, uint64_t(std::min(1 << std::min(f - 1, 4), RETRY_CAP_S)) * 1000000);
}

void on_event(void*, esp_event_base_t base, int32_t id, void* data) {
  if (base == WIFI_EVENT) {
    if (id == WIFI_EVENT_STA_START && state_ == State::Connecting) esp_wifi_connect();
    else if (id == WIFI_EVENT_STA_DISCONNECTED) on_disconnect();
    else if (id == WIFI_EVENT_SCAN_DONE && state_ == State::Portal) on_scan_done();
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    auto* e = static_cast<ip_event_got_ip_t*>(data);
    fails_ = 0;
    esp_timer_stop(retry_t_);
    bcast_ = e->ip_info.ip.addr | ~e->ip_info.netmask.addr;
    ip_ = e->ip_info.ip.addr;
    state_ = State::Connected;
    ESP_LOGI(TAG, "connected to %s", ssid_);
  }
}

void begin_portal() {
  esp_timer_stop(retry_t_);
  esp_timer_stop(apply_t_);
  ip_ = bcast_ = 0;
  scan_n_ = 0;
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
  snprintf(portal_ssid_, sizeof portal_ssid_, "Garnet-%02X%02X", mac[4], mac[5]);
  snprintf(portal_pass_, sizeof portal_pass_, "%08lu", static_cast<unsigned long>(esp_random() % 100000000u));

  // The DHCP server must offer the AP address as DNS or phones never ask the responder.
  esp_netif_dns_info_t dns{};
  dns.ip.type = IPADDR_TYPE_V4;
  IP4_ADDR(&dns.ip.u_addr.ip4, AP_IP[0], AP_IP[1], AP_IP[2], AP_IP[3]);
  uint8_t offer = 1;
  esp_netif_dhcps_stop(ap_if_);
  esp_netif_dhcps_option(ap_if_, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer, sizeof offer);
  esp_netif_set_dns_info(ap_if_, ESP_NETIF_DNS_MAIN, &dns);

  wifi_config_t ap{};
  memcpy(ap.ap.ssid, portal_ssid_, strlen(portal_ssid_));
  ap.ap.ssid_len = strlen(portal_ssid_);
  memcpy(ap.ap.password, portal_pass_, strlen(portal_pass_));
  ap.ap.channel = 1;
  ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
  ap.ap.max_connection = 2;
  state_ = State::Portal;
  expect_disc_ = false;
  if (started_) esp_wifi_disconnect();
  esp_wifi_set_mode(WIFI_MODE_APSTA);
  esp_wifi_set_config(WIFI_IF_AP, &ap);
  if (!started_.exchange(true)) esp_wifi_start();
  esp_wifi_scan_start(nullptr, false);
  http_start();
  dns_start();
  ESP_LOGI(TAG, "portal %s up", portal_ssid_);
}

void apply_sta() {
  dns_want_ = false;
  if (httpd_) { httpd_stop(httpd_); httpd_ = nullptr; }
  esp_timer_stop(retry_t_);
  fails_ = 0;
  wifi_config_t sta{};
  memcpy(sta.sta.ssid, ssid_, strlen(ssid_));
  memcpy(sta.sta.password, pass_, strlen(pass_));
  sta.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
  sta.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
  sta.sta.pmf_cfg.capable = true;
  bool was_connected = state_ == State::Connected;
  state_ = State::Connecting;
  esp_wifi_set_mode(WIFI_MODE_STA);
  esp_wifi_set_config(WIFI_IF_STA, &sta);
  esp_wifi_set_ps(WIFI_PS_NONE);
  if (!started_.exchange(true)) { esp_wifi_start(); return; }  // STA_START connects
  if (was_connected) { expect_disc_ = true; esp_wifi_disconnect(); }
  else esp_wifi_connect();
  ESP_LOGI(TAG, "joining %s", ssid_);
}

bool load() {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
  size_t n = sizeof ssid_;
  bool ok = nvs_get_str(h, "wifi_ssid", ssid_, &n) == ESP_OK && ssid_[0];
  n = sizeof pass_;
  if (nvs_get_str(h, "wifi_pass", pass_, &n) != ESP_OK) pass_[0] = 0;
  nvs_close(h);
  if (!ok) ssid_[0] = 0;
  return ok;
}

}  // namespace

void start() {
  esp_netif_init();
  esp_netif_create_default_wifi_sta();
  ap_if_ = esp_netif_create_default_wifi_ap();
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  esp_wifi_init(&cfg);
  esp_wifi_set_storage(WIFI_STORAGE_RAM);
  esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, nullptr);
  esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, nullptr);
  const esp_timer_create_args_t ra = {retry_cb, nullptr, ESP_TIMER_TASK, "wifi_retry", false};
  const esp_timer_create_args_t aa = {apply_cb, nullptr, ESP_TIMER_TASK, "wifi_apply", false};
  esp_timer_create(&ra, &retry_t_);
  esp_timer_create(&aa, &apply_t_);
  if (load()) apply_sta();
  else begin_portal();
}

State state() { return state_; }
const char* sta_ssid() { return ssid_; }
uint32_t ipv4() { return ip_; }
uint32_t broadcast_ipv4() { return bcast_; }
const char* portal_ssid() { return portal_ssid_; }
const char* portal_pass() { return portal_pass_; }

bool set_credentials(const char* ssid, const char* pass) {
  size_t sl = ssid ? strlen(ssid) : 0, pl = pass ? strlen(pass) : 0;
  if (sl == 0 || sl > 32 || pl > 63 || (pl > 0 && pl < 8)) return false;
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
  bool ok = nvs_set_str(h, "wifi_ssid", ssid) == ESP_OK && nvs_set_str(h, "wifi_pass", pass ? pass : "") == ESP_OK &&
            nvs_commit(h) == ESP_OK;
  nvs_close(h);
  if (!ok) return false;
  strcpy(ssid_, ssid);
  strcpy(pass_, pass ? pass : "");
  if (state_ == State::Portal) arm(apply_t_, 1000000);
  else apply_sta();
  return true;
}

void forget() {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
    nvs_erase_key(h, "wifi_ssid");
    nvs_erase_key(h, "wifi_pass");
    nvs_commit(h);
    nvs_close(h);
  }
  ssid_[0] = pass_[0] = 0;
  if (state_ != State::Portal) begin_portal();
}

}  // namespace garnet::wifi
