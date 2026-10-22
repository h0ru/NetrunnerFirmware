#pragma once

/* Built-in NDEF parser.
 *
 * The NDEF content parsers (the ones that print "NDEF Format Data / Card: … /
 * M1-R1: URL / <value>") normally ship as external `*_parser.fal` plugins on
 * the SD card under /ext/apps_data/nfc/plugins/. If those files are missing or
 * out of date, the read screen falls back to the plain UID/Tech view and the
 * stored text/URL is not shown.
 *
 * To make the NDEF content show regardless of what is on the SD, the parser for
 * MIFARE Ultralight/NTAG and MIFARE Classic is also compiled straight into the
 * firmware. nfc_ndef_builtin_parse() is used as a fallback when the external
 * plugins do not produce output. */

#include <nfc/nfc_device.h>
#include <furi_string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns true and fills `parsed_data` when the device carries a parsable NDEF
 * message (NTAG/Ultralight or MIFARE Classic); false otherwise. */
bool nfc_ndef_builtin_parse(const NfcDevice* device, FuriString* parsed_data);

#ifdef __cplusplus
}
#endif
