/**
 * @file lfrfid_worker_i.h
 * 
 * lfrfid worker, internal definitions 
 */

#pragma once
#include <furi.h>
#include "lfrfid_worker.h"
#include "lfrfid_raw_worker.h"
#include "protocols/lfrfid_protocols.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void (*const process)(LFRFIDWorker* worker);
} LFRFIDWorkerModeType;

typedef enum {
    LFRFIDWorkerIdle,
    LFRFIDWorkerRead,
    LFRFIDWorkerWrite,
    LFRFIDWorkerWriteAndSetPass,
    LFRFIDWorkerEmulate,
    LFRFIDWorkerReadRaw,
    LFRFIDWorkerEmulateRaw,
} LFRFIDWorkerMode;

struct LFRFIDWorker {
    char* raw_filename;

    LFRFIDWorkerMode mode_index;
    void* mode_storage;

    FuriEventFlag* events;
    FuriThread* thread;

    LFRFIDWorkerReadType read_type;

    LFRFIDWorkerReadCallback read_cb;
    LFRFIDWorkerWriteCallback write_cb;
    LFRFIDWorkerReadRawCallback read_raw_cb;
    LFRFIDWorkerEmulateRawCallback emulate_raw_cb;

    void* cb_ctx;

    /* T5577 "write & set password" key. The LF-RFID app used to live in
     * applications/main/lfrfid and the worker reached into its struct for this;
     * that app was removed (T-Embed has no 125 kHz LF antenna), so the password
     * now lives here and defaults to all-zero. Set via lfrfid_worker_set_pass()
     * before an LFRFIDWorkerWriteAndSetPass if a caller is ever re-added. */
    uint8_t password[4];

    ProtocolDict* protocols;
    LFRFIDProtocol protocol;
};

extern const LFRFIDWorkerModeType lfrfid_worker_modes[];

/**
 * @brief Check for stop flag
 * 
 * @param worker 
 * @return bool 
 */
bool lfrfid_worker_check_for_stop(LFRFIDWorker* worker);

#ifdef __cplusplus
}
#endif
