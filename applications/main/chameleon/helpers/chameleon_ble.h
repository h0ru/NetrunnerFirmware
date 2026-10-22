#pragma once

/*
 * BLE central transport + ChameleonUltra wire protocol.
 *
 * The T-Embed takes over the NimBLE host from the bt service (same handover as
 * ble_spam_hal), scans for a ChameleonUltra advertising the Nordic UART Service,
 * connects, subscribes to its TX-notify characteristic and writes commands to
 * its RX characteristic. Commands use the ChameleonUltra data frame:
 *
 *   SOF 0x11 | LRC1(byte0) | cmd(u16 BE) | status(u16 BE) | len(u16 BE)
 *           | LRC2(bytes 0..7) | data[len] | LRC3(all preceding bytes)
 *   LRC = (0x100 - (sum & 0xFF)) & 0xFF
 *
 * Everything UI-facing is delivered as events on the app's thread via the
 * registered callback (the callback itself only posts a view_dispatcher custom
 * event; the heavy lifting runs on an internal worker thread + the NimBLE host
 * task). One command is in flight at a time.
 */

#include <furi.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ChameleonUltra command codes (RRG chameleon_enum.py) used by the MVP. */
#define CHAMELEON_CMD_GET_APP_VERSION   1000u
#define CHAMELEON_CMD_CHANGE_DEVICE_MODE 1001u /* arg: 1=reader, 0=tag/emulator */
#define CHAMELEON_CMD_GET_DEVICE_MODE   1002u
#define CHAMELEON_CMD_SET_ACTIVE_SLOT   1003u /* arg: slot index 0..7 */
#define CHAMELEON_CMD_SET_SLOT_TAG_TYPE 1004u
#define CHAMELEON_CMD_SET_SLOT_ENABLE   1006u
#define CHAMELEON_CMD_GET_SLOT_INFO     1019u
#define CHAMELEON_CMD_GET_ACTIVE_SLOT   1018u
#define CHAMELEON_CMD_GET_BATTERY_INFO  1025u
#define CHAMELEON_CMD_HF14A_SCAN        2000u
#define CHAMELEON_CMD_EM410X_SCAN       3000u /* LF EM410x read */
#define CHAMELEON_CMD_SLOT_DATA_SAVE    1009u /* persist slot config/data to flash */
#define CHAMELEON_CMD_HF14A_SET_ANTICOLL 4001u /* set active HF slot UID/ATQA/SAK */
#define CHAMELEON_CMD_EM410X_SET_EMU_ID 5000u /* set active LF slot EM410x id */
#define CHAMELEON_CMD_EM410X_GET_EMU_ID 5001u /* read active LF slot EM410x id */
#define CHAMELEON_CMD_EM410X_WRITE_T55XX 3001u /* write EM410x id to a real T5577 */
#define CHAMELEON_CMD_HF14A_GET_ANTICOLL 4018u /* read active HF slot UID/ATQA/SAK */
#define CHAMELEON_CMD_MF1_WRITE_BLOCK   2009u /* write one block to a real HF card */

#define CHAMELEON_MF_KEY_A 0x60u

/* TagSpecificType values (RRG chameleon_enum.py); 0 = empty slot. */
#define CHAMELEON_TAG_NONE       0u
#define CHAMELEON_TAG_EM410X     100u
#define CHAMELEON_TAG_MIFARE_1K  1001u
#define CHAMELEON_TAG_NTAG_215   1101u

#define CHAMELEON_SENSE_LF 1u
#define CHAMELEON_SENSE_HF 2u

/* Device reply status codes (RRG chameleon_enum.py Status). Info/action
 * commands succeed with SUCCESS; the scan commands have their own tag-ok codes. */
#define CHAMELEON_STATUS_SUCCESS   0x68u
#define CHAMELEON_STATUS_HF_TAG_OK 0x00u
#define CHAMELEON_STATUS_LF_TAG_OK 0x40u
/* Failure statuses (RRG Status enum) used for human-readable write diagnostics. */
#define CHAMELEON_STATUS_HF_TAG_NO        0x01u /* no ISO14443A card in field */
#define CHAMELEON_STATUS_MF_ERR_AUTH      0x06u /* MIFARE auth failed (key/non-magic) */
#define CHAMELEON_STATUS_LF_TAG_NO        0x41u /* no LF tag found */
#define CHAMELEON_STATUS_PAR_ERR          0x60u
#define CHAMELEON_STATUS_DEVICE_MODE_ERROR 0x66u /* command needs reader mode */
#define CHAMELEON_STATUS_INVALID_CMD      0x67u
#define CHAMELEON_STATUS_NOT_IMPLEMENTED  0x69u
/* Synthesized locally when a command gets no reply within RESP_TIMEOUT_MS; never
 * sent by the device (real statuses are single-byte). */
#define CHAMELEON_STATUS_TIMEOUT   0xFFFFu

#define CHAMELEON_MODE_READER   1u
#define CHAMELEON_MODE_EMULATOR 0u

#define CHAMELEON_RF_FIELD_HF 2u /* TagSenseType.HF */

#define CHAMELEON_SLOT_COUNT 8u

/* Max payload we ever need to read back (slot info is the largest in the MVP). */
#define CHAMELEON_RESP_MAX 300u

typedef enum {
    ChameleonBleEventConnected, /* connected, discovered, subscribed: ready */
    ChameleonBleEventConnectFailed, /* not found / connect / discovery error */
    ChameleonBleEventDisconnected, /* link dropped after being ready */
    ChameleonBleEventResponse, /* a command response is available */
} ChameleonBleEventType;

typedef void (*ChameleonBleEventCallback)(ChameleonBleEventType type, void* context);

typedef struct ChameleonBle ChameleonBle;

ChameleonBle* chameleon_ble_alloc(void);

/* Free: disconnects, stops the NimBLE host and restarts the bt service stack. */
void chameleon_ble_free(ChameleonBle* ble);

void chameleon_ble_set_event_callback(
    ChameleonBle* ble,
    ChameleonBleEventCallback callback,
    void* context);

/* Begin the radio handover + scan + connect sequence. Non-blocking; the result
 * arrives as ChameleonBleEventConnected or ChameleonBleEventConnectFailed. */
void chameleon_ble_connect(ChameleonBle* ble);

bool chameleon_ble_is_ready(const ChameleonBle* ble);

/* Queue a command. Its reply arrives as ChameleonBleEventResponse; read it with
 * chameleon_ble_take_response(). Returns false if not ready or queue is full. */
bool chameleon_ble_send(ChameleonBle* ble, uint16_t cmd, const uint8_t* data, uint16_t len);

/* Copy the most recent response out (thread-safe). Returns false if none. */
bool chameleon_ble_take_response(
    ChameleonBle* ble,
    uint16_t* cmd,
    uint16_t* status,
    uint8_t* data_out,
    uint16_t* len_out,
    uint16_t data_cap);

/* Human-readable reason for the last ConnectFailed, for the error screen. */
const char* chameleon_ble_last_error(const ChameleonBle* ble);

/* Short display name for a TagSpecificType, or NULL for an empty slot (0). */
const char* chameleon_tag_type_name(uint16_t type);

/* Short human-readable reason for a device reply status (NULL if unknown), and
 * whether a write failure is worth retrying with the card held in place. */
const char* chameleon_status_reason(uint16_t status);
bool chameleon_status_write_transient(uint16_t status);

/* Queue an EM410x-to-T5577 write (reader mode) for the given 5-byte id, using
 * the Chameleon's default T5577 keys. Returns false if not ready. */
bool chameleon_ble_write_t5577_em410x(ChameleonBle* ble, const uint8_t id5[5]);

/* Queue a block-0 (UID) write to an external Gen2/CUID magic Mifare Classic
 * card: builds block 0 from the UID (+ computed BCC for 4-byte UIDs) + SAK +
 * ATQA and writes it with key A = FF*6 via MF1_WRITE_ONE_BLOCK. UID must be 4
 * or 7 bytes; returns false for any other length. */
bool chameleon_ble_write_mifare_block0(
    ChameleonBle* ble,
    const uint8_t* uid,
    uint8_t uidlen,
    uint8_t sak,
    const uint8_t atqa2[2]);

#ifdef __cplusplus
}
#endif
