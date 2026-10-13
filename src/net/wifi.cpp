#include "wifi.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>

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
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "valence/net/saved_networks.hpp"

namespace garnet::wifi {
namespace {

namespace vn = valence::net;
static_assert(vn::SavedNetworks::kCapacity == size_t(kMaxNetworks));

constexpr char TAG[] = "wifi";
constexpr char NVS_NS[] = "garnet";
constexpr char NVS_KEY[] = "nets";
constexpr int FAIL_LIMIT = 5;
constexpr int RETRY_CAP_S = 10;
constexpr int MAX_SCAN = 16;
constexpr int MAX_HEARD = 32;
constexpr int8_t FLOOR_DBM = -85;
constexpr uint64_t TRY_US = 12000000;   // one saved network, scan or connect
constexpr uint64_t SETTLE_US = 300000;  // after a disconnect, before the next step
constexpr uint8_t AP_IP[4] = {192, 168, 4, 1};

// Where a join sequence stands. Settle and Advance wait out the disconnect
// event of a call this code made; Backoff waits out the retry timer.
enum class Phase : uint8_t { Idle, Scanning, Trying, Settle, Advance, Backoff };

// One mutex (mu_) guards everything in this block: the UI task and the console
// call the list API, the event loop and the timer task run the join.
SemaphoreHandle_t mu_;
vn::SavedNetworks list_, bak_;
uint8_t blob_[vn::SavedNetworks::kBlobBytes];
uint8_t live_id_, cur_id_;
uint8_t order_[kMaxNetworks];  // ids, in the order to try
int8_t order_rssi_[kMaxNetworks];
int order_n_, pos_;
Phase phase_ = Phase::Idle;
wifi_ap_record_t heard_[MAX_HEARD];
vn::Sighting sight_[MAX_HEARD];
uint8_t order_idx_[kMaxNetworks];
int8_t rssi_idx_[kMaxNetworks];

// Portal state: written under mu_ or before the portal serves; the HTTP task reads it unlocked.
char portal_ssid_[16], portal_pass_[9];
char scan_[MAX_SCAN][33];
wifi_ap_record_t recs_[MAX_SCAN];

std::atomic<State> state_{State::Off};
std::atomic<uint32_t> ip_{0}, bcast_{0};
std::atomic<bool> started_{false};
std::atomic<bool> dns_want_{false}, dns_running_{false};
std::atomic<int> scan_n_{0}, fails_{0};
esp_netif_t* ap_if_;
esp_timer_handle_t retry_t_, apply_t_, try_t_;
httpd_handle_t httpd_;

struct Lock {
  Lock() { xSemaphoreTake(mu_, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(mu_); }
};

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
                              "<title>Add network</title><h2>Add a network to up to 4 saved</h2><form method=post action=/save>"
                              "<p>Network<br><input name=ssid list=n maxlength=32 required><datalist id=n>");
  for (int i = 0, n = scan_n_; i < n; ++i) {
    esc(scan_[i], e);
    httpd_resp_sendstr_chunk(r, "<option value=\"");
    httpd_resp_sendstr_chunk(r, e);
    httpd_resp_sendstr_chunk(r, "\">");
  }
  httpd_resp_sendstr_chunk(r, "</datalist><p>Password<br><input name=pass type=password maxlength=63>"
                              "<p><button>Save</button></form>");
  return httpd_resp_sendstr_chunk(r, nullptr);
}

esp_err_t post_save(httpd_req_t* r) {
  char body[400], rs[97], rp[193], e[200], out[320];
  size_t len = r->content_len;
  if (len == 0 || len >= sizeof body) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "bad size");
  for (size_t got = 0; got < len;) {
    int n = httpd_req_recv(r, body + got, len - got);
    if (n <= 0) { vn::wipeBytes(body, sizeof body); return ESP_FAIL; }
    got += n;
  }
  body[len] = 0;
  rs[0] = rp[0] = 0;
  httpd_query_key_value(body, "ssid", rs, sizeof rs);
  httpd_query_key_value(body, "pass", rp, sizeof rp);
  url_decode(rs);
  url_decode(rp);
  const bool ok = add(rs, rp);
  esc(rs, e);
  vn::wipeBytes(body, sizeof body);
  vn::wipeBytes(rs, sizeof rs);
  vn::wipeBytes(rp, sizeof rp);
  if (!ok) return page(r, "Not saved: invalid network or password, or 4 networks are already saved. <a href=/>Back</a>");
  snprintf(out, sizeof out, "Saved. Garnet is joining the saved networks (%s added).", e);
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

// ---- persistence (mu_ held) ----
esp_err_t write_nets(nvs_handle_t h) {
  if (list_.count() == 0) {
    const esp_err_t e = nvs_erase_key(h, NVS_KEY);
    return e == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : e;
  }
  list_.encode(std::span<uint8_t>(blob_));
  const esp_err_t e = nvs_set_blob(h, NVS_KEY, blob_, sizeof blob_);
  vn::wipeBytes(blob_, sizeof blob_);
  return e;
}

bool persist_locked() {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
  const bool ok = write_nets(h) == ESP_OK && nvs_commit(h) == ESP_OK;
  nvs_close(h);
  return ok;
}

// The pre-RFC-116 keys wifi_ssid/wifi_pass become slot 1 once.
void load_locked() {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
  size_t n = sizeof blob_;
  const esp_err_t e = nvs_get_blob(h, NVS_KEY, blob_, &n);
  if (e == ESP_OK) {
    if (!list_.decode(std::span<const uint8_t>(blob_, n))) ESP_LOGW(TAG, "saved list unreadable, starting empty");
    vn::wipeBytes(blob_, sizeof blob_);
  } else if (e == ESP_ERR_NVS_NOT_FOUND) {
    char s[33], p[65];
    size_t sn = sizeof s, pn = sizeof p;
    if (nvs_get_str(h, "wifi_ssid", s, &sn) == ESP_OK && s[0]) {
      if (nvs_get_str(h, "wifi_pass", p, &pn) != ESP_OK) p[0] = 0;
      if (list_.save(std::nullopt, s, p).ok && write_nets(h) == ESP_OK && nvs_commit(h) == ESP_OK) {
        nvs_erase_key(h, "wifi_ssid");
        nvs_erase_key(h, "wifi_pass");
        nvs_commit(h);
        ESP_LOGI(TAG, "moved the stored network to slot 1");
      } else {
        list_.clear();
      }
    }
    vn::wipeBytes(s, sizeof s);
    vn::wipeBytes(p, sizeof p);
  } else {
    ESP_LOGW(TAG, "saved list unreadable (nvs %d), starting empty", int(e));
  }
  nvs_close(h);
}

// ---- join sequence (mu_ held) ----
void begin_portal_locked();
void advance_locked();

int slot_of(uint8_t id) {
  const auto i = list_.findId(id);
  return i ? int(*i) : -1;
}

void fail_all_locked() {
  const int f = ++fails_;
  state_ = f >= FAIL_LIMIT ? State::Failed : State::Connecting;
  phase_ = Phase::Backoff;
  arm(retry_t_, uint64_t(std::min(1 << std::min(f - 1, 4), RETRY_CAP_S)) * 1000000);
}

void try_entry_locked(uint8_t id) {
  const auto at = list_.findId(id);
  if (!at) { advance_locked(); return; }
  wifi_config_t sta{};
  const std::string_view s = list_.ssid(*at), p = list_.passphrase(*at);
  memcpy(sta.sta.ssid, s.data(), s.size());
  memcpy(sta.sta.password, p.data(), p.size());
  // Multi-AP site, one SSID: scan every channel, take the strongest BSSID,
  // ignore ones too weak to hold, and let 802.11k/v steer later (as Nucleus
  // 0.1.48 does).
  sta.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
  sta.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
  sta.sta.threshold.rssi = FLOOR_DBM;
  sta.sta.rm_enabled = 1;
  sta.sta.btm_enabled = 1;
  sta.sta.pmf_cfg.capable = true;
  esp_wifi_set_config(WIFI_IF_STA, &sta);
  vn::wipeBytes(&sta, sizeof sta);
  cur_id_ = id;
  phase_ = Phase::Trying;
  const int8_t r = order_rssi_[pos_];
  if (r == vn::kRssiUnseen) ESP_LOGI(TAG, "trying slot %d of %u, not heard", int(*at) + 1, unsigned(list_.count()));
  else ESP_LOGI(TAG, "trying slot %d of %u, scan %d dBm", int(*at) + 1, unsigned(list_.count()), int(r));
  if (list_.count() > 1) arm(try_t_, TRY_US);
  if (esp_wifi_connect() != ESP_OK) advance_locked();
}

void advance_locked() {
  esp_timer_stop(try_t_);
  if (++pos_ < order_n_) try_entry_locked(order_[pos_]);
  else fail_all_locked();
}

// Scan results (or none) to an order, then the first attempt.
void order_locked(size_t heard) {
  const size_t n = vn::joinOrder(list_, std::span<const vn::Sighting>(sight_, heard),
                                 std::span<uint8_t, kMaxNetworks>(order_idx_),
                                 std::span<int8_t, kMaxNetworks>(rssi_idx_), FLOOR_DBM);
  for (size_t k = 0; k < n; ++k) {
    order_[k] = list_.id(order_idx_[k]);
    order_rssi_[k] = rssi_idx_[order_idx_[k]];
  }
  order_n_ = int(n);
  pos_ = -1;
  advance_locked();
}

void scan_done_locked() {
  uint16_t n = MAX_HEARD;
  if (esp_wifi_scan_get_ap_records(&n, heard_) != ESP_OK) n = 0;
  for (int i = 0; i < n; ++i) {
    const char* name = reinterpret_cast<const char*>(heard_[i].ssid);
    sight_[i] = {std::string_view(name, strnlen(name, 32)), int8_t(heard_[i].rssi)};
  }
  order_locked(n);
}

// One saved network: connect directly. Two or more: scan first, then the join rule.
void kick_locked() {
  esp_timer_stop(try_t_);
  const size_t n = list_.count();
  if (n == 0) { begin_portal_locked(); return; }
  if (n == 1) {
    order_[0] = list_.id(0);
    order_rssi_[0] = vn::kRssiUnseen;
    order_n_ = 1;
    pos_ = 0;
    try_entry_locked(order_[0]);
    return;
  }
  phase_ = Phase::Scanning;
  const wifi_scan_config_t cfg{};  // every channel, active, no hidden
  if (esp_wifi_scan_start(&cfg, false) != ESP_OK) { order_locked(0); return; }
  arm(try_t_, TRY_US);
}

void apply_locked() {
  esp_timer_stop(retry_t_);
  esp_timer_stop(try_t_);
  fails_ = 0;
  if (list_.count() == 0) { begin_portal_locked(); return; }
  state_ = State::Connecting;
  ip_ = bcast_ = 0;
  live_id_ = 0;
  esp_wifi_set_mode(WIFI_MODE_STA);
  esp_wifi_set_ps(WIFI_PS_NONE);
  if (!started_.exchange(true)) {  // STA_START kicks
    phase_ = Phase::Idle;
    esp_wifi_start();
    return;
  }
  phase_ = Phase::Settle;
  esp_wifi_disconnect();
  arm(retry_t_, SETTLE_US);
}

// ---- timers and events ----
void retry_cb(void*) {
  Lock l;
  if (state_ != State::Connecting && state_ != State::Failed) return;
  if (phase_ == Phase::Advance) advance_locked();
  else if (phase_ == Phase::Settle || phase_ == Phase::Backoff) kick_locked();
}

void try_cb(void*) {
  Lock l;
  if (phase_ == Phase::Scanning) {
    esp_wifi_scan_stop();
    order_locked(0);
  } else if (phase_ == Phase::Trying) {
    phase_ = Phase::Advance;
    esp_wifi_disconnect();
    arm(retry_t_, SETTLE_US);
  }
}

// The portal's HTTP task may be inside add(), which takes mu_: stop it before locking.
void apply_cb(void*) {
  httpd_handle_t h;
  {
    Lock l;
    dns_want_ = false;
    h = httpd_;
    httpd_ = nullptr;
  }
  if (h) httpd_stop(h);
  Lock l;
  apply_locked();
}

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
  Lock l;
  const State s = state_;
  if (s == State::Portal || s == State::Off) return;
  ip_ = bcast_ = 0;
  live_id_ = 0;
  if (phase_ == Phase::Trying) {
    advance_locked();
  } else if (phase_ == Phase::Idle) {  // a drop
    state_ = State::Connecting;
    if (list_.count() < 2) fail_all_locked();
    else kick_locked();
  }
}

void on_event(void*, esp_event_base_t base, int32_t id, void* data) {
  if (base == WIFI_EVENT) {
    if (id == WIFI_EVENT_STA_DISCONNECTED) {
      on_disconnect();
    } else if (id == WIFI_EVENT_STA_START) {
      Lock l;
      if (state_ == State::Connecting) kick_locked();
    } else if (id == WIFI_EVENT_SCAN_DONE) {
      Lock l;
      if (phase_ == Phase::Scanning) {
        esp_timer_stop(try_t_);
        scan_done_locked();
      } else if (state_ == State::Portal) {
        on_scan_done();
      }
    }
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    Lock l;
    auto* e = static_cast<ip_event_got_ip_t*>(data);
    fails_ = 0;
    esp_timer_stop(retry_t_);
    esp_timer_stop(try_t_);
    phase_ = Phase::Idle;
    live_id_ = cur_id_;
    bcast_ = e->ip_info.ip.addr | ~e->ip_info.netmask.addr;
    ip_ = e->ip_info.ip.addr;
    state_ = State::Connected;
    wifi_ap_record_t ap{};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
      ESP_LOGI(TAG, "joined slot %d of %u: bssid ..%02x:%02x, channel %u, rssi %d dBm", slot_of(live_id_) + 1,
               unsigned(list_.count()), ap.bssid[4], ap.bssid[5], unsigned(ap.primary), int(ap.rssi));
    }
  }
}

void begin_portal_locked() {
  esp_timer_stop(retry_t_);
  esp_timer_stop(apply_t_);
  esp_timer_stop(try_t_);
  phase_ = Phase::Idle;
  live_id_ = 0;
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
  if (started_) esp_wifi_disconnect();
  esp_wifi_set_mode(WIFI_MODE_APSTA);
  esp_wifi_set_config(WIFI_IF_AP, &ap);
  if (!started_.exchange(true)) esp_wifi_start();
  esp_wifi_scan_start(nullptr, false);
  http_start();
  dns_start();
  ESP_LOGI(TAG, "portal %s up", portal_ssid_);
}

}  // namespace

void start() {
  mu_ = xSemaphoreCreateMutex();
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
  const esp_timer_create_args_t ta = {try_cb, nullptr, ESP_TIMER_TASK, "wifi_try", false};
  esp_timer_create(&ra, &retry_t_);
  esp_timer_create(&aa, &apply_t_);
  esp_timer_create(&ta, &try_t_);
  Lock l;
  load_locked();
  if (list_.count()) apply_locked();
  else begin_portal_locked();
}

State state() { return state_; }
uint32_t ipv4() { return ip_; }
uint32_t broadcast_ipv4() { return bcast_; }
const char* portal_ssid() { return portal_ssid_; }
const char* portal_pass() { return portal_pass_; }

int count() {
  Lock l;
  return int(list_.count());
}

int live() {
  Lock l;
  return state_ == State::Connected ? slot_of(live_id_) : -1;
}

bool ssid(int i, char* out, size_t n) {
  Lock l;
  if (i < 0 || size_t(i) >= list_.count()) return false;
  const std::string_view s = list_.ssid(size_t(i));
  if (s.size() + 1 > n) return false;
  memcpy(out, s.data(), s.size());
  out[s.size()] = 0;
  return true;
}

bool add(const char* ssid, const char* pass) {
  if (!ssid) return false;
  Lock l;
  bak_ = list_;
  const auto old = list_.saveTarget(std::nullopt, ssid);
  const uint8_t old_id = old ? list_.id(*old) : 0;
  const auto r = list_.save(std::nullopt, ssid, pass ? pass : "");
  const bool ok = r.ok && persist_locked();
  if (!ok) list_ = bak_;
  else if (old_id && old_id == live_id_) live_id_ = list_.id(r.saved.slot);
  bak_.clear();
  if (!ok) return false;
  if (state_ == State::Portal) arm(apply_t_, 1000000);
  else if (state_ != State::Connected) arm(apply_t_, 100000);
  return true;
}

bool remove(int i) {
  Lock l;
  if (i < 0 || size_t(i) >= list_.count()) return false;
  const uint8_t id = list_.id(size_t(i));
  bak_ = list_;
  const bool ok = list_.remove(size_t(i)) && persist_locked();
  if (!ok) list_ = bak_;
  bak_.clear();
  if (!ok) return false;
  if (state_ == State::Portal) return true;
  if (list_.count() == 0) begin_portal_locked();
  else if (id == live_id_ || state_ != State::Connected) arm(apply_t_, 100000);
  return true;
}

bool move(int from, int to) {
  Lock l;
  if (from < 0 || to < 0) return false;
  bak_ = list_;
  const bool ok = list_.move(size_t(from), size_t(to)) && persist_locked();
  if (!ok) list_ = bak_;
  bak_.clear();
  return ok;
}

void forget_all() {
  Lock l;
  list_.clear();
  persist_locked();
  live_id_ = 0;
  if (state_ != State::Portal) begin_portal_locked();
}

void open_portal() {
  Lock l;
  if (state_ != State::Portal) begin_portal_locked();
}

void cancel_portal() {
  Lock l;
  if (state_ == State::Portal && list_.count()) arm(apply_t_, 100000);
}

}  // namespace garnet::wifi
