#include "m_buf.h"
#include "m_events.h"
//
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_log.h"
#include <string.h>

#define BT_MAX_META_LEN 64

typedef enum {
  BT_GAP_IDLE = 0,
  BT_GAP_DISCOVERABLE,
  BT_GAP_CONNECTABLE,
  BT_GAP_CONNECTED
} bt_gap_state_t;

typedef enum {
  BT_A2DP_DISCONNECTED = 0,
  BT_A2DP_CONNECTED,
  BT_A2DP_STREAMING
} bt_a2dp_state_t;

typedef struct {
  char title[BT_MAX_META_LEN];
  char artist[BT_MAX_META_LEN];
  char album[BT_MAX_META_LEN];
} bt_metadata_t;

typedef struct {
  char name[64];
  int sample_rate;
  uint8_t volume;
  bt_gap_state_t gap_state;
  bt_a2dp_state_t a2dp_state;
  esp_bd_addr_t peer_bda;
  bt_metadata_t metadata;
  esp_avrc_playback_stat_t playback_stat;
} bt_cfg_t;

static ring_buf_handle_t m_ring_buf = NULL;
static bt_cfg_t m_bt;
static const char *TAG = "BT";
static const char *BT_DEF_SPEAKER_NAME = "M_SPEAKER";

void m_bt_set_local_volume(uint8_t vol_percent) {
  if (vol_percent > 100) {
    vol_percent = 100;
  }

  uint8_t bt_vol = (vol_percent * 127) / 100;
  m_bt.volume = bt_vol;

  esp_avrc_rn_param_t rn;
  rn.volume = m_bt.volume;
  esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_VOLUME_CHANGE, ESP_AVRC_RN_RSP_CHANGED,
                          &rn);
}

void m_bt_set_speaker_name(const char *name) {

  if (name == NULL || strlen(name) == 0) {
    ESP_LOGW(TAG, "pusta nazwa, pomijam zmiane");
    return;
  }

  strncpy(m_bt.name, name, sizeof(m_bt.name) - 1);
  m_bt.name[sizeof(m_bt.name) - 1] = '\0';

  esp_err_t err = esp_bt_gap_set_device_name(m_bt.name);
  if (err == ESP_OK) {
    ESP_LOGI(TAG, "Zmieniono nazwe Bluetooth na: %s", m_bt.name);

    // 3. (Zalecane) Zapisz nową nazwę do NVS, aby przetrwała restart ESP32
    // m_nvs_save_bt_name(m_bt.name);
  } else {
    ESP_LOGE(TAG, "Blad podczas zmiany nazwy: %d", err);
  }
}

static void m_gap_cb(esp_bt_gap_cb_event_t event,
                     esp_bt_gap_cb_param_t *param) {
  switch (event) {

  case ESP_BT_GAP_KEY_NOTIF_EVT:
    ESP_LOGI(TAG, "passkey: %06d", param->key_notif.passkey);
    break;

  case ESP_BT_GAP_AUTH_CMPL_EVT:
    if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
      ESP_LOGI(TAG, "Autoryzacja OK. Urzadzenie: %s",
               param->auth_cmpl.device_name);
      // Możesz tu zapisać adres MAC do NVS, aby wiedzieć z kim się połączono
    } else {
      ESP_LOGE(TAG, "Autoryzacja FAIL, status:%d", param->auth_cmpl.stat);
    }
    break;

  case ESP_BT_GAP_MODE_CHG_EVT:
    // Informacja o aktualnym trybie skanowania
    ESP_LOGI(TAG, "GAP Mode Change: %d", param->mode_chg.mode);
    break;

  case ESP_BT_GAP_PIN_REQ_EVT:
    // Obsługa starszych urządzeń BT bez SSP (tzw. Legacy Pairing)
    ESP_LOGI(TAG, "PIN request");
    esp_bt_pin_code_t pin_code;
    pin_code[0] = '0';
    pin_code[1] = '0';
    pin_code[2] = '0';
    pin_code[3] = '0';
    esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin_code);
    break;

#if 0 // Zakomentowane, ponieważ dla ESP_BT_IO_CAP_NONE to zdarzenie nie wystąpi
    case ESP_BT_GAP_CFM_REQ_EVT:
      ESP_LOGI(TAG, "Żądanie potwierdzenia passkey, ignorujemy dla głośnika");
      esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
      break;
#endif

  default:
    ESP_LOGI(TAG, "GAP event: %d", event);
    break;
  }
}

static void m_a2d_data_cb(const uint8_t *data, uint32_t len) {

  if (m_ring_buf) {
    ring_buf_write(m_ring_buf, data, len);
  }
}

static void m_a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param) {
  switch (event) {

  case ESP_A2D_CONNECTION_STATE_EVT:
    if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {

      m_bt.a2dp_state = BT_A2DP_CONNECTED;
      m_bt.gap_state = BT_GAP_CONNECTED;
      esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);

      esp_event_post(M_EVENTS, EVENT_BT_CONNECTED, NULL, 0, portMAX_DELAY);

    } else if (param->conn_stat.state ==
               ESP_A2D_CONNECTION_STATE_DISCONNECTED) {

      m_bt.a2dp_state = BT_A2DP_DISCONNECTED;
      m_bt.gap_state = BT_GAP_DISCOVERABLE;
      esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);

      esp_event_post(M_EVENTS, EVENT_BT_DISCONNECTED, NULL, 0, portMAX_DELAY);
    }
    break;

  case ESP_A2D_AUDIO_CFG_EVT: {
    uint8_t *cie = (uint8_t *)&param->audio_cfg.mcc.cie;

    ESP_LOGI(TAG, "SBC CIE %02x %02x %02x %02x", cie[0], cie[1], cie[2],
             cie[3]);

    uint8_t sf = cie[0]; // sampling freq to górny nibble wg spec

    // wartości wg spec A2DP, nie wg IDF
    if (sf & 0x10) { // 0x10 = 48k
      m_bt.sample_rate = 48000;
    } else if (sf & 0x20) { // 0x20 = 44.1k
      m_bt.sample_rate = 44100;
    } else if (sf & 0x40) { // 0x40 = 32k
      m_bt.sample_rate = 32000;
    } else if (sf & 0x80) { // 0x80 = 16k
      m_bt.sample_rate = 16000;
    } else {
      m_bt.sample_rate = 44100;
    }

    ESP_LOGI(TAG, "A2DP AUDIO CFG - Sample rate: %d Hz (sf=0x%02x)",
             m_bt.sample_rate, sf);
    esp_event_post(M_EVENTS, EVENT_SAMPLE_RATE, &m_bt.sample_rate, sizeof(int),
                   portMAX_DELAY);
    esp_event_post(M_EVENTS, EVENT_SAMPLE_RATE, &m_bt.sample_rate, sizeof(int),
                   portMAX_DELAY);
    break;
  }

  case ESP_A2D_AUDIO_STATE_EVT:
    if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED) {
      ESP_LOGI(TAG, "A2DP Audio (Started)");
      m_bt.a2dp_state = BT_A2DP_STREAMING;
    } else {
      ESP_LOGI(TAG, "A2DP Audio (Pause/Stop/Suspend)");
      m_bt.a2dp_state = BT_A2DP_CONNECTED;
    }
    break;

  default:
    break;
  }
}

static void m_avrc_cb(esp_avrc_ct_cb_event_t event,
                      esp_avrc_ct_cb_param_t *param) {

  switch (event) {

  case ESP_AVRC_CT_CONNECTION_STATE_EVT:
    ESP_LOGI(TAG, "AVRC CT conn %d", param->conn_stat.connected);
    if (param->conn_stat.connected) {
      esp_avrc_ct_send_register_notification_cmd(1, ESP_AVRC_RN_TRACK_CHANGE,
                                                 0);
      esp_avrc_ct_send_register_notification_cmd(
          2, ESP_AVRC_RN_PLAY_STATUS_CHANGE, 0);
    }
    break;

  case ESP_AVRC_CT_CHANGE_NOTIFY_EVT:
    ESP_LOGI(TAG, "AVRC notify id %d", param->change_ntf.event_id);

    if (param->change_ntf.event_id == ESP_AVRC_RN_TRACK_CHANGE) {

      memset(&m_bt.metadata, 0, sizeof(bt_metadata_t));
      esp_event_post(M_EVENTS, EVENT_AUDIO_FLUSH, NULL, 0, portMAX_DELAY);

      uint32_t attr_mask = ESP_AVRC_MD_ATTR_TITLE | ESP_AVRC_MD_ATTR_ARTIST |
                           ESP_AVRC_MD_ATTR_ALBUM;
      esp_avrc_ct_send_metadata_cmd(0, attr_mask);
      esp_avrc_ct_send_register_notification_cmd(1, ESP_AVRC_RN_TRACK_CHANGE,
                                                 0);
    } else if (param->change_ntf.event_id == ESP_AVRC_RN_PLAY_STATUS_CHANGE) {

      ESP_LOGI(TAG, "AVRC Play Status Changed: %d",
               param->change_ntf.event_parameter.playback);
      m_bt.playback_stat =
          (esp_avrc_playback_stat_t)param->change_ntf.event_parameter.playback;
      if (m_bt.playback_stat == ESP_AVRC_PLAYBACK_PLAYING) {

        esp_event_post(M_EVENTS, EVENT_AUDIO_PLAY, NULL, 0, portMAX_DELAY);
      } else {

        esp_event_post(M_EVENTS, EVENT_AUDIO_PAUSE, NULL, 0, portMAX_DELAY);
      }

      esp_avrc_ct_send_register_notification_cmd(
          2, ESP_AVRC_RN_PLAY_STATUS_CHANGE, 0);
    }
    break;

  case ESP_AVRC_CT_METADATA_RSP_EVT: {
    int copy_len = param->meta_rsp.attr_length;
    if (copy_len >= BT_MAX_META_LEN) {
      copy_len = BT_MAX_META_LEN - 1; // Zostawienie miejsca na '\0'
    }

    switch (param->meta_rsp.attr_id) {
    case ESP_AVRC_MD_ATTR_TITLE:
      strncpy(m_bt.metadata.title, (char *)param->meta_rsp.attr_text, copy_len);
      m_bt.metadata.title[copy_len] = '\0';
      break;

    case ESP_AVRC_MD_ATTR_ARTIST:
      strncpy(m_bt.metadata.artist, (char *)param->meta_rsp.attr_text,
              copy_len);
      m_bt.metadata.artist[copy_len] = '\0';
      break;

    case ESP_AVRC_MD_ATTR_ALBUM:
      strncpy(m_bt.metadata.album, (char *)param->meta_rsp.attr_text, copy_len);
      m_bt.metadata.album[copy_len] = '\0';
      break;
    }

    ESP_LOGI(TAG, "Zaktualizowano metadane. Tytul: %s, Artysta: %s",
             m_bt.metadata.title, m_bt.metadata.artist);

    esp_event_post(M_EVENTS, EVENT_NEW_METADATA, &m_bt.metadata,
                   sizeof(bt_metadata_t), portMAX_DELAY);
    break;
  }

  case ESP_AVRC_CT_PASSTHROUGH_RSP_EVT:
    ESP_LOGI(TAG, "AVRC Passthrough response: key_code %d",
             param->psth_rsp.key_code);
    break;

  default:
    ESP_LOGD(TAG, "AVRC evt %d unhandled", event);
    break;
  }
}

static void m_avrc_tg_cb(esp_avrc_tg_cb_event_t event,
                         esp_avrc_tg_cb_param_t *param) {
  switch (event) {

  case ESP_AVRC_TG_CONNECTION_STATE_EVT:
    ESP_LOGI(TAG, "AVRC_TG conn %d", param->conn_stat.connected);
    break;

  case ESP_AVRC_TG_REGISTER_NOTIFICATION_EVT:
    ESP_LOGI(TAG, "TG reg %d", param->reg_ntf.event_id);
    if (param->reg_ntf.event_id == ESP_AVRC_RN_VOLUME_CHANGE) {
      esp_avrc_rn_param_t rn;
      rn.volume = m_bt.volume;
      esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_VOLUME_CHANGE,
                              ESP_AVRC_RN_RSP_INTERIM, &rn);
    }
    break;

  case ESP_AVRC_TG_SET_ABSOLUTE_VOLUME_CMD_EVT: {
    ESP_LOGI(TAG, "TG vol %d", param->set_abs_vol.volume);

    m_bt.volume = param->set_abs_vol.volume;
    uint8_t sys_vol = (uint8_t)(m_bt.volume * 100 / 127);
    esp_event_post(M_EVENTS, EVENT_VOLUME_CHANGE, &sys_vol, sizeof(sys_vol),
                   portMAX_DELAY);

    esp_avrc_rn_param_t rn2;
    rn2.volume = m_bt.volume;
    esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_VOLUME_CHANGE, ESP_AVRC_RN_RSP_CHANGED,
                            &rn2);
    break;
  }

  default:
    break;
  }
}

void m_bt_init(ring_buf_handle_t buf) {

  m_ring_buf = buf;

  ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));

  esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
  ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));

  esp_bluedroid_config_t bl_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_bluedroid_init_with_cfg(&bl_cfg));
  ESP_ERROR_CHECK(esp_bluedroid_enable());

  m_bt_set_speaker_name(BT_DEF_SPEAKER_NAME);

  ESP_ERROR_CHECK(esp_bt_gap_register_callback(m_gap_cb));

  // CoD
  esp_bt_cod_t cod = {0};
  cod.major = 0x04;
  cod.minor = 0x05;
  cod.service = 0x120;
  esp_bt_gap_set_cod(cod, ESP_BT_SET_COD_ALL);

  ESP_ERROR_CHECK(esp_avrc_ct_init());
  ESP_ERROR_CHECK(esp_avrc_ct_register_callback(m_avrc_cb));

  ESP_ERROR_CHECK(esp_avrc_tg_init());
  ESP_ERROR_CHECK(esp_avrc_tg_register_callback(m_avrc_tg_cb));

  esp_avrc_psth_bit_mask_t allowed_mask = {0};
  esp_avrc_tg_get_psth_cmd_filter(ESP_AVRC_PSTH_FILTER_ALLOWED_CMD,
                                  &allowed_mask);
  esp_avrc_tg_set_psth_cmd_filter(ESP_AVRC_PSTH_FILTER_SUPPORTED_CMD,
                                  &allowed_mask);

  esp_avrc_rn_evt_cap_mask_t evt = {0};
  esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &evt,
                                     ESP_AVRC_RN_VOLUME_CHANGE);
  esp_avrc_tg_set_rn_evt_cap(&evt);

  ESP_ERROR_CHECK(esp_a2d_sink_init());
  ESP_ERROR_CHECK(esp_a2d_register_callback(&m_a2d_cb));
  ESP_ERROR_CHECK(esp_a2d_sink_register_data_callback(&m_a2d_data_cb));

  // esp_bt_io_cap_t io_cap = ESP_BT_IO_CAP_IO;
  esp_bt_io_cap_t io_cap = ESP_BT_IO_CAP_NONE;
  ESP_ERROR_CHECK(esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &io_cap,
                                                sizeof(io_cap)));
  ESP_ERROR_CHECK(esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE,
                                           ESP_BT_GENERAL_DISCOVERABLE));
}
