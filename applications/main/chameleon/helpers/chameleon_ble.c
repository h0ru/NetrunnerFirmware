#include "chameleon_ble.h"

#include <string.h>
#include <esp_bt.h>
#include <esp_log.h>
#include <host/ble_gap.h>
#include <host/ble_gatt.h>
#include <host/ble_hs.h>
#include <nimble_glue.h>
#include <btshim.h>

#define TAG "ChameleonBle"

/* Nordic UART Service, as the ChameleonUltra exposes it. BLE_UUID128_INIT takes
 * bytes least-significant first. */
static const ble_uuid128_t nus_svc_uuid = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);
static const ble_uuid128_t nus_rx_uuid = BLE_UUID128_INIT( /* write, central->device */
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);
static const ble_uuid128_t nus_tx_uuid = BLE_UUID128_INIT( /* notify, device->central */
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);

#define EVT_SYNCED       (1u << 0)
#define EVT_READY        (1u << 1)
#define EVT_CONN_FAILED  (1u << 2)
#define EVT_RESP_READY   (1u << 3)
#define EVT_DISCONNECTED (1u << 4)
#define EVT_STOP         (1u << 5)
#define EVT_ALL          0x3Fu

#define CONNECT_TIMEOUT_MS 20000u
#define SCAN_DURATION_MS   15000u
#define RESP_TIMEOUT_MS    4000u /* writes (esp. T5577, multi-page) outrun a scan reply */
#define RESP_STATUS_TIMEOUT CHAMELEON_STATUS_TIMEOUT

#define CMD_QUEUE_LEN 4u
#define FRAME_OVERHEAD 10u /* SOF+LRC1+cmd+status+len+LRC2 (9) + LRC3 (1) */

typedef struct {
    uint16_t cmd;
    uint16_t len;
    uint8_t data[64];
} ChameleonCmdReq;

struct ChameleonBle {
    FuriThread* worker;
    FuriMessageQueue* cmd_queue;
    FuriEventFlag* flags;
    FuriMutex* mutex;

    ChameleonBleEventCallback event_cb;
    void* event_ctx;

    volatile bool ready;
    volatile uint16_t conn_handle;

    /* Discovered during connection (host task). */
    uint16_t svc_start;
    uint16_t svc_end;
    uint16_t rx_handle; /* write */
    uint16_t tx_handle; /* notify value handle */
    uint16_t tx_cccd_handle;
    bool connecting_started; /* a connect was issued from the scan callback */

    /* Peer picked during scan. */
    ble_addr_t peer_addr;

    uint8_t own_addr_type;

    /* Notification reassembly (host task writes, worker reads, under mutex). */
    uint8_t rx_acc[CHAMELEON_RESP_MAX + FRAME_OVERHEAD];
    uint16_t rx_acc_len;

    /* Last complete response (under mutex). */
    uint16_t resp_cmd;
    uint16_t resp_status;
    uint8_t resp_data[CHAMELEON_RESP_MAX];
    uint16_t resp_len;
    bool resp_valid;

    char last_error[32];
};

/* ---- framing -------------------------------------------------------------- */

static uint8_t chameleon_lrc(const uint8_t* buf, uint16_t len) {
    uint32_t sum = 0;
    for(uint16_t i = 0; i < len; i++) sum += buf[i];
    return (uint8_t)((0x100 - (sum & 0xFF)) & 0xFF);
}

/* Build a command frame into out (must hold FRAME_OVERHEAD + len). Returns total. */
static uint16_t chameleon_build_frame(
    uint16_t cmd,
    const uint8_t* data,
    uint16_t len,
    uint8_t* out) {
    out[0] = 0x11; /* SOF */
    out[1] = chameleon_lrc(out, 1); /* LRC1 over SOF */
    out[2] = (uint8_t)(cmd >> 8);
    out[3] = (uint8_t)(cmd & 0xFF);
    out[4] = 0; /* status, 0 in requests */
    out[5] = 0;
    out[6] = (uint8_t)(len >> 8);
    out[7] = (uint8_t)(len & 0xFF);
    out[8] = chameleon_lrc(out, 8); /* LRC2 over header */
    if(len && data) memcpy(&out[9], data, len);
    out[9 + len] = chameleon_lrc(out, 9 + len); /* LRC3 over everything before */
    return (uint16_t)(10 + len);
}

/* Try to extract one complete, valid frame from the accumulator. On success,
 * stores it as the last response, drops the consumed bytes, returns true. */
static bool chameleon_try_extract(ChameleonBle* ble) {
    /* Resync to SOF. */
    uint16_t start = 0;
    while(start < ble->rx_acc_len && ble->rx_acc[start] != 0x11) start++;
    if(start) {
        memmove(ble->rx_acc, &ble->rx_acc[start], ble->rx_acc_len - start);
        ble->rx_acc_len -= start;
    }
    if(ble->rx_acc_len < 9) return false; /* need header */
    if(chameleon_lrc(ble->rx_acc, 1) != ble->rx_acc[1]) { /* bad LRC1 */
        memmove(ble->rx_acc, &ble->rx_acc[1], --ble->rx_acc_len);
        return false;
    }
    if(chameleon_lrc(ble->rx_acc, 8) != ble->rx_acc[8]) { /* bad LRC2 */
        memmove(ble->rx_acc, &ble->rx_acc[1], --ble->rx_acc_len);
        return false;
    }
    uint16_t len = ((uint16_t)ble->rx_acc[6] << 8) | ble->rx_acc[7];
    if(len > CHAMELEON_RESP_MAX) { /* implausible; drop SOF and resync */
        memmove(ble->rx_acc, &ble->rx_acc[1], --ble->rx_acc_len);
        return false;
    }
    uint16_t total = (uint16_t)(10 + len);
    if(ble->rx_acc_len < total) return false; /* wait for more */
    if(chameleon_lrc(ble->rx_acc, 9 + len) != ble->rx_acc[9 + len]) { /* bad LRC3 */
        memmove(ble->rx_acc, &ble->rx_acc[1], --ble->rx_acc_len);
        return false;
    }
    ble->resp_cmd = ((uint16_t)ble->rx_acc[2] << 8) | ble->rx_acc[3];
    ble->resp_status = ((uint16_t)ble->rx_acc[4] << 8) | ble->rx_acc[5];
    ble->resp_len = len;
    if(len) memcpy(ble->resp_data, &ble->rx_acc[9], len);
    ble->resp_valid = true;
    /* Drop the consumed frame. */
    if(ble->rx_acc_len > total) {
        memmove(ble->rx_acc, &ble->rx_acc[total], ble->rx_acc_len - total);
    }
    ble->rx_acc_len -= total;
    return true;
}

/* ---- scan / connect / discovery (NimBLE host task) ------------------------ */

static int chameleon_conn_gap_cb(struct ble_gap_event* event, void* arg);

static bool chameleon_adv_matches(const struct ble_gap_disc_desc* d) {
    const uint8_t* buf = d->data;
    uint8_t total = d->length_data;
    uint8_t pos = 0;
    while(pos + 1 < total) {
        uint8_t fld_len = buf[pos];
        if(fld_len == 0 || pos + 1 + fld_len > total) break;
        uint8_t type = buf[pos + 1];
        const uint8_t* val = &buf[pos + 2];
        uint8_t val_len = fld_len - 1;
        if(type == 0x08 || type == 0x09) { /* short / complete local name */
            if(val_len >= 9 && memcmp(val, "Chameleon", 9) == 0) return true;
        } else if(type == 0x06 || type == 0x07) { /* 128-bit service UUIDs */
            for(uint8_t off = 0; off + 16 <= val_len; off += 16) {
                if(memcmp(&val[off], nus_svc_uuid.value, 16) == 0) return true;
            }
        }
        pos += fld_len + 1;
    }
    return false;
}

static int chameleon_scan_gap_cb(struct ble_gap_event* event, void* arg) {
    ChameleonBle* ble = arg;
    switch(event->type) {
    case BLE_GAP_EVENT_DISC:
        if(!ble->connecting_started && chameleon_adv_matches(&event->disc)) {
            ble->peer_addr = event->disc.addr;
            ble->connecting_started = true;
            ble_gap_disc_cancel();
            struct ble_gap_conn_params cp = {
                .scan_itvl = 0x0010,
                .scan_window = 0x0010,
                .itvl_min = BLE_GAP_INITIAL_CONN_ITVL_MIN,
                .itvl_max = BLE_GAP_INITIAL_CONN_ITVL_MAX,
                .latency = 0,
                .supervision_timeout = 0x0100,
                .min_ce_len = 0,
                .max_ce_len = 0,
            };
            int rc = ble_gap_connect(
                ble->own_addr_type, &ble->peer_addr, 10000, &cp, chameleon_conn_gap_cb, ble);
            if(rc != 0) {
                ESP_LOGE(TAG, "ble_gap_connect failed rc=%d", rc);
                strlcpy(ble->last_error, "Connect error", sizeof(ble->last_error));
                furi_event_flag_set(ble->flags, EVT_CONN_FAILED);
            }
        }
        break;
    case BLE_GAP_EVENT_DISC_COMPLETE:
        if(!ble->connecting_started) {
            strlcpy(ble->last_error, "Chameleon not found", sizeof(ble->last_error));
            furi_event_flag_set(ble->flags, EVT_CONN_FAILED);
        }
        break;
    default:
        break;
    }
    return 0;
}

static int chameleon_cccd_write_cb(
    uint16_t conn_handle,
    const struct ble_gatt_error* error,
    struct ble_gatt_attr* attr,
    void* arg) {
    (void)conn_handle;
    (void)attr;
    ChameleonBle* ble = arg;
    if(error->status == 0) {
        ble->ready = true;
        furi_event_flag_set(ble->flags, EVT_READY);
    } else {
        strlcpy(ble->last_error, "Subscribe failed", sizeof(ble->last_error));
        furi_event_flag_set(ble->flags, EVT_CONN_FAILED);
    }
    return 0;
}

static int chameleon_dsc_cb(
    uint16_t conn_handle,
    const struct ble_gatt_error* error,
    uint16_t chr_val_handle,
    const struct ble_gatt_dsc* dsc,
    void* arg) {
    (void)chr_val_handle;
    ChameleonBle* ble = arg;
    if(error->status == 0 && dsc) {
        if(ble_uuid_u16(&dsc->uuid.u) == 0x2902 && !ble->tx_cccd_handle) {
            ble->tx_cccd_handle = dsc->handle;
        }
        return 0;
    }
    if(error->status == BLE_HS_EDONE && ble->tx_cccd_handle) {
        static const uint8_t enable[2] = {0x01, 0x00};
        int rc = ble_gattc_write_flat(
            conn_handle, ble->tx_cccd_handle, enable, sizeof(enable),
            chameleon_cccd_write_cb, ble);
        if(rc != 0) {
            strlcpy(ble->last_error, "Subscribe failed", sizeof(ble->last_error));
            furi_event_flag_set(ble->flags, EVT_CONN_FAILED);
        }
    } else if(error->status != 0) {
        strlcpy(ble->last_error, "No TX descriptor", sizeof(ble->last_error));
        furi_event_flag_set(ble->flags, EVT_CONN_FAILED);
    }
    return 0;
}

static int chameleon_chr_cb(
    uint16_t conn_handle,
    const struct ble_gatt_error* error,
    const struct ble_gatt_chr* chr,
    void* arg) {
    ChameleonBle* ble = arg;
    if(error->status == 0 && chr) {
        if(ble_uuid_cmp(&chr->uuid.u, &nus_rx_uuid.u) == 0) {
            ble->rx_handle = chr->val_handle;
        } else if(ble_uuid_cmp(&chr->uuid.u, &nus_tx_uuid.u) == 0) {
            ble->tx_handle = chr->val_handle;
        }
        return 0;
    }
    if(error->status == BLE_HS_EDONE && ble->rx_handle && ble->tx_handle) {
        int rc = ble_gattc_disc_all_dscs(
            conn_handle, ble->tx_handle, ble->svc_end, chameleon_dsc_cb, ble);
        if(rc != 0) {
            strlcpy(ble->last_error, "Descriptor disc failed", sizeof(ble->last_error));
            furi_event_flag_set(ble->flags, EVT_CONN_FAILED);
        }
    } else if(error->status != 0 || !ble->rx_handle || !ble->tx_handle) {
        strlcpy(ble->last_error, "NUS chars missing", sizeof(ble->last_error));
        furi_event_flag_set(ble->flags, EVT_CONN_FAILED);
    }
    return 0;
}

static int chameleon_svc_cb(
    uint16_t conn_handle,
    const struct ble_gatt_error* error,
    const struct ble_gatt_svc* service,
    void* arg) {
    ChameleonBle* ble = arg;
    if(error->status == 0 && service) {
        ble->svc_start = service->start_handle;
        ble->svc_end = service->end_handle;
        return 0;
    }
    if(error->status == BLE_HS_EDONE && ble->svc_start) {
        int rc = ble_gattc_disc_all_chrs(
            conn_handle, ble->svc_start, ble->svc_end, chameleon_chr_cb, ble);
        if(rc != 0) {
            strlcpy(ble->last_error, "Char disc failed", sizeof(ble->last_error));
            furi_event_flag_set(ble->flags, EVT_CONN_FAILED);
        }
    } else {
        strlcpy(ble->last_error, "No NUS service", sizeof(ble->last_error));
        furi_event_flag_set(ble->flags, EVT_CONN_FAILED);
    }
    return 0;
}

static int chameleon_conn_gap_cb(struct ble_gap_event* event, void* arg) {
    ChameleonBle* ble = arg;
    switch(event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if(event->connect.status == 0) {
            ble->conn_handle = event->connect.conn_handle;
            ble_gattc_exchange_mtu(ble->conn_handle, NULL, NULL);
            int rc = ble_gattc_disc_svc_by_uuid(
                ble->conn_handle, &nus_svc_uuid.u, chameleon_svc_cb, ble);
            if(rc != 0) {
                strlcpy(ble->last_error, "Service disc failed", sizeof(ble->last_error));
                furi_event_flag_set(ble->flags, EVT_CONN_FAILED);
            }
        } else {
            strlcpy(ble->last_error, "Connect error", sizeof(ble->last_error));
            furi_event_flag_set(ble->flags, EVT_CONN_FAILED);
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ble->conn_handle = BLE_HS_CONN_HANDLE_NONE;
        if(ble->ready) {
            ble->ready = false;
            furi_event_flag_set(ble->flags, EVT_DISCONNECTED);
        } else {
            strlcpy(ble->last_error, "Disconnected", sizeof(ble->last_error));
            furi_event_flag_set(ble->flags, EVT_CONN_FAILED);
        }
        break;
    case BLE_GAP_EVENT_NOTIFY_RX:
        if(event->notify_rx.attr_handle == ble->tx_handle) {
            furi_mutex_acquire(ble->mutex, FuriWaitForever);
            uint16_t space = sizeof(ble->rx_acc) - ble->rx_acc_len;
            uint16_t got = 0;
            if(space) {
                int rc = ble_hs_mbuf_to_flat(
                    event->notify_rx.om, &ble->rx_acc[ble->rx_acc_len], space, &got);
                if(rc == 0 && got) {
                    ble->rx_acc_len += got;
                    if(chameleon_try_extract(ble)) {
                        furi_event_flag_set(ble->flags, EVT_RESP_READY);
                    }
                }
            } else {
                ble->rx_acc_len = 0; /* overflow: drop and resync */
            }
            furi_mutex_release(ble->mutex);
        }
        break;
    default:
        break;
    }
    return 0;
}

static void chameleon_on_sync(void* context) {
    ChameleonBle* ble = context;
    ble->own_addr_type = nimble_glue_own_address_type();
    furi_event_flag_set(ble->flags, EVT_SYNCED);
}

/* ---- worker --------------------------------------------------------------- */

static void chameleon_emit(ChameleonBle* ble, ChameleonBleEventType type) {
    if(ble->event_cb) ble->event_cb(type, ble->event_ctx);
}

static int32_t chameleon_worker(void* ctx) {
    ChameleonBle* ble = ctx;

    Bt* bt = furi_record_open(RECORD_BT);
    bt_stop_stack(bt);
    furi_record_close(RECORD_BT);

    bool stack_up = false;
    if(nimble_glue_init("CHAM") == ESP_OK) {
        /* Just-works bonding; the Chameleon pairs without IO. */
        nimble_glue_configure_security(true, false, true, BLE_HS_IO_NO_INPUT_OUTPUT);
        if(nimble_glue_start(chameleon_on_sync, ble) == ESP_OK) stack_up = true;
    }

    bool ok = false;
    if(stack_up) {
        uint32_t f = furi_event_flag_wait(ble->flags, EVT_SYNCED | EVT_STOP, FuriFlagWaitAny, 5000);
        if((f & EVT_SYNCED) && !(f & EVT_STOP)) {
            struct ble_gap_disc_params sp = {
                .passive = 0, .itvl = 0x0050, .window = 0x0030, .filter_duplicates = 0};
            furi_event_flag_clear(ble->flags, EVT_READY | EVT_CONN_FAILED);
            int rc = ble_gap_disc(ble->own_addr_type, SCAN_DURATION_MS, &sp, chameleon_scan_gap_cb, ble);
            if(rc == 0) {
                uint32_t r = furi_event_flag_wait(
                    ble->flags, EVT_READY | EVT_CONN_FAILED | EVT_STOP, FuriFlagWaitAny,
                    CONNECT_TIMEOUT_MS);
                if((r & EVT_READY) && !(r & EVT_STOP)) ok = true;
            } else {
                strlcpy(ble->last_error, "Scan failed", sizeof(ble->last_error));
            }
        }
    } else {
        strlcpy(ble->last_error, "BLE init failed", sizeof(ble->last_error));
    }

    if(!ok) {
        if(ble->last_error[0] == '\0')
            strlcpy(ble->last_error, "Connect timeout", sizeof(ble->last_error));
        chameleon_emit(ble, ChameleonBleEventConnectFailed);
    } else {
        chameleon_emit(ble, ChameleonBleEventConnected);

        /* Command loop. */
        for(;;) {
            uint32_t f = furi_event_flag_get(ble->flags);
            if(f & (EVT_STOP | EVT_DISCONNECTED)) {
                if(f & EVT_DISCONNECTED) chameleon_emit(ble, ChameleonBleEventDisconnected);
                break;
            }
            ChameleonCmdReq req;
            if(furi_message_queue_get(ble->cmd_queue, &req, 200) != FuriStatusOk) continue;

            furi_mutex_acquire(ble->mutex, FuriWaitForever);
            ble->rx_acc_len = 0;
            ble->resp_valid = false;
            furi_mutex_release(ble->mutex);
            furi_event_flag_clear(ble->flags, EVT_RESP_READY);

            uint8_t frame[FRAME_OVERHEAD + sizeof(req.data)];
            uint16_t flen = chameleon_build_frame(req.cmd, req.data, req.len, frame);
            int rc = ble_gattc_write_no_rsp_flat(ble->conn_handle, ble->rx_handle, frame, flen);
            if(rc != 0) {
                ESP_LOGW(TAG, "write cmd %u failed rc=%d", req.cmd, rc);
            }

            uint32_t r = furi_event_flag_wait(
                ble->flags, EVT_RESP_READY | EVT_DISCONNECTED | EVT_STOP, FuriFlagWaitAny,
                RESP_TIMEOUT_MS);
            if(r & EVT_STOP) break;
            if(r & EVT_DISCONNECTED) {
                chameleon_emit(ble, ChameleonBleEventDisconnected);
                break;
            }
            if(!(r & EVT_RESP_READY)) {
                /* Timeout: synthesize a response so the UI can react. */
                furi_mutex_acquire(ble->mutex, FuriWaitForever);
                ble->resp_cmd = req.cmd;
                ble->resp_status = RESP_STATUS_TIMEOUT;
                ble->resp_len = 0;
                ble->resp_valid = true;
                furi_mutex_release(ble->mutex);
            }
            chameleon_emit(ble, ChameleonBleEventResponse);
        }
    }

    /* Teardown. */
    if(ble->conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(ble->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        furi_delay_ms(50);
    }
    if(stack_up) nimble_glue_stop();

    Bt* bt2 = furi_record_open(RECORD_BT);
    bt_start_stack(bt2);
    furi_record_close(RECORD_BT);

    return 0;
}

/* ---- public API ----------------------------------------------------------- */

ChameleonBle* chameleon_ble_alloc(void) {
    ChameleonBle* ble = malloc(sizeof(ChameleonBle));
    memset(ble, 0, sizeof(ChameleonBle));
    ble->conn_handle = BLE_HS_CONN_HANDLE_NONE;
    ble->flags = furi_event_flag_alloc();
    ble->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    ble->cmd_queue = furi_message_queue_alloc(CMD_QUEUE_LEN, sizeof(ChameleonCmdReq));
    ble->worker = furi_thread_alloc_ex("ChameleonBle", 4096, chameleon_worker, ble);
    return ble;
}

void chameleon_ble_set_event_callback(
    ChameleonBle* ble,
    ChameleonBleEventCallback callback,
    void* context) {
    ble->event_cb = callback;
    ble->event_ctx = context;
}

void chameleon_ble_connect(ChameleonBle* ble) {
    furi_thread_start(ble->worker);
}

bool chameleon_ble_is_ready(const ChameleonBle* ble) {
    return ble->ready;
}

bool chameleon_ble_send(ChameleonBle* ble, uint16_t cmd, const uint8_t* data, uint16_t len) {
    if(!ble->ready) return false;
    if(len > sizeof(((ChameleonCmdReq*)0)->data)) return false;
    ChameleonCmdReq req = {.cmd = cmd, .len = len};
    if(len && data) memcpy(req.data, data, len);
    return furi_message_queue_put(ble->cmd_queue, &req, 0) == FuriStatusOk;
}

bool chameleon_ble_take_response(
    ChameleonBle* ble,
    uint16_t* cmd,
    uint16_t* status,
    uint8_t* data_out,
    uint16_t* len_out,
    uint16_t data_cap) {
    bool ok = false;
    furi_mutex_acquire(ble->mutex, FuriWaitForever);
    if(ble->resp_valid) {
        if(cmd) *cmd = ble->resp_cmd;
        if(status) *status = ble->resp_status;
        uint16_t n = ble->resp_len < data_cap ? ble->resp_len : data_cap;
        if(data_out && n) memcpy(data_out, ble->resp_data, n);
        if(len_out) *len_out = n;
        ble->resp_valid = false;
        ok = true;
    }
    furi_mutex_release(ble->mutex);
    return ok;
}

const char* chameleon_ble_last_error(const ChameleonBle* ble) {
    return ble->last_error[0] ? ble->last_error : "Unknown error";
}

const char* chameleon_status_reason(uint16_t status) {
    /* Short (<=20 char) human reason for a device reply status, for failure
     * screens. NULL => unknown; the caller then shows the raw code. Values from
     * RRG chameleon_enum.py Status. */
    switch(status) {
    case CHAMELEON_STATUS_HF_TAG_NO:
        return "No card in field";
    case CHAMELEON_STATUS_LF_TAG_NO:
        return "No tag in field";
    case CHAMELEON_STATUS_MF_ERR_AUTH:
        return "Auth fail (not gen2)";
    case CHAMELEON_STATUS_PAR_ERR:
        return "Bad parameters";
    case CHAMELEON_STATUS_DEVICE_MODE_ERROR:
        return "Not in reader mode";
    case CHAMELEON_STATUS_INVALID_CMD:
        return "Command rejected";
    case CHAMELEON_STATUS_NOT_IMPLEMENTED:
        return "Unsupported (Lite?)";
    case CHAMELEON_STATUS_TIMEOUT:
        return "No response";
    default:
        return NULL;
    }
}

bool chameleon_status_write_transient(uint16_t status) {
    /* Failures that a card-positioning retry can plausibly clear (no tag seen /
     * no reply), unlike auth or mode errors which won't fix themselves. */
    return status == CHAMELEON_STATUS_HF_TAG_NO || status == CHAMELEON_STATUS_LF_TAG_NO ||
           status == CHAMELEON_STATUS_TIMEOUT;
}

bool chameleon_ble_write_t5577_em410x(ChameleonBle* ble, const uint8_t id5[5]) {
    /* Payload: 5-byte id + 4-byte new key + two 4-byte old keys (RRG defaults). */
    static const uint8_t new_key[4] = {0x20, 0x20, 0x66, 0x66};
    static const uint8_t old_keys[8] = {0x51, 0x24, 0x36, 0x48, 0x19, 0x92, 0x04, 0x27};
    uint8_t payload[5 + 4 + 8];
    memcpy(&payload[0], id5, 5);
    memcpy(&payload[5], new_key, 4);
    memcpy(&payload[9], old_keys, 8);
    return chameleon_ble_send(ble, CHAMELEON_CMD_EM410X_WRITE_T55XX, payload, sizeof(payload));
}

bool chameleon_ble_write_mifare_block0(
    ChameleonBle* ble,
    const uint8_t* uid,
    uint8_t uidlen,
    uint8_t sak,
    const uint8_t atqa2[2]) {
    /* Block 0 layout differs by UID length (ATQA keeps the scan's byte order,
     * LSB first; trailing manufacturer bytes are left 0, fine on a magic card):
     *   4-byte: UID(4) | BCC | SAK | ATQA(2) | 8 manufacturer bytes
     *   7-byte: UID(7) |       SAK | ATQA(2) | 6 manufacturer bytes
     * (7-byte cards carry no BCC in block 0; the reader derives the cascade
     *  BCCs from the anticollision.) Other lengths aren't supported. */
    uint8_t block0[16] = {0};
    if(uidlen == 4) {
        memcpy(block0, uid, 4);
        block0[4] = (uint8_t)(uid[0] ^ uid[1] ^ uid[2] ^ uid[3]);
        block0[5] = sak;
        block0[6] = atqa2[0];
        block0[7] = atqa2[1];
    } else if(uidlen == 7) {
        memcpy(block0, uid, 7);
        block0[7] = sak;
        block0[8] = atqa2[0];
        block0[9] = atqa2[1];
    } else {
        return false;
    }
    /* MF1_WRITE_ONE_BLOCK payload: key_type | block | key(6) | data(16). */
    uint8_t payload[1 + 1 + 6 + 16];
    payload[0] = CHAMELEON_MF_KEY_A;
    payload[1] = 0; /* block 0 */
    memset(&payload[2], 0xFF, 6);
    memcpy(&payload[8], block0, 16);
    return chameleon_ble_send(ble, CHAMELEON_CMD_MF1_WRITE_BLOCK, payload, sizeof(payload));
}

const char* chameleon_tag_type_name(uint16_t type) {
    switch(type) {
    case 0:
        return NULL; /* empty */
    case 100:
        return "EM410x";
    case 1000:
        return "MF Mini";
    case 1001:
        return "MF 1K";
    case 1002:
        return "MF 2K";
    case 1003:
        return "MF 4K";
    case 1100:
        return "NTAG213";
    case 1101:
        return "NTAG215";
    case 1102:
        return "NTAG216";
    default:
        /* LF types are < 1000, HF types >= 1000 (RRG TAG_TYPES_LF_END = 999). */
        return (type < 1000) ? "LF" : "HF";
    }
}

void chameleon_ble_free(ChameleonBle* ble) {
    furi_event_flag_set(ble->flags, EVT_STOP);
    furi_thread_join(ble->worker);
    furi_thread_free(ble->worker);
    furi_message_queue_free(ble->cmd_queue);
    furi_mutex_free(ble->mutex);
    furi_event_flag_free(ble->flags);
    free(ble);
}
