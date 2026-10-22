#include "nfc_ndef_builtin.h"

#include <nfc/protocols/mf_ultralight/mf_ultralight.h>
#include <nfc/protocols/mf_classic/mf_classic.h>

/* Defined in the per-protocol translation units (each includes ndef.c once). */
bool nfc_ndef_builtin_parse_ul(const NfcDevice* device, FuriString* parsed_data);
bool nfc_ndef_builtin_parse_mfc(const NfcDevice* device, FuriString* parsed_data);

bool nfc_ndef_builtin_parse(const NfcDevice* device, FuriString* parsed_data) {
    if(device == NULL || parsed_data == NULL) return false;

    const NfcProtocol protocol = nfc_device_get_protocol(device);
    if(protocol == NfcProtocolMfUltralight) {
        return nfc_ndef_builtin_parse_ul(device, parsed_data);
    }
    if(protocol == NfcProtocolMfClassic) {
        return nfc_ndef_builtin_parse_mfc(device, parsed_data);
    }
    return false;
}
