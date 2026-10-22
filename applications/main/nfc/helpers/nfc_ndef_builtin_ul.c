/* Built-in NDEF parser for UL. Reuses the plugin parser source
 * (plugins/supported_cards/ndef.c) compiled into the firmware with
 * NDEF_PROTO = UL.
 *
 * ndef.c is a loadable .fal plugin: its FAP entry/descriptor are unused here,
 * and a few top-level helpers (ndef_parse_record/message/tlv) plus the plugin
 * entry are NON-static, so they are renamed per translation unit to avoid
 * colliding with the other protocol's unit. Unused-symbol warnings from the
 * plugin boilerplate are suppressed for this unit so they don't trip -Werror.
 * The suppression is intentionally left active to end-of-file. */

#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-const-variable"

#define NDEF_PROTO 1

#define ndef_plugin_ep         ndef_plugin_ep_ul_builtin
#define ndef_plugin            ndef_plugin_ul_builtin
#define ndef_plugin_descriptor ndef_plugin_descriptor_ul_builtin
#define ndef_parse_record      ndef_parse_record_ul_builtin
#define ndef_parse_message     ndef_parse_message_ul_builtin
#define ndef_parse_tlv         ndef_parse_tlv_ul_builtin

#include "../plugins/supported_cards/ndef.c"

bool nfc_ndef_builtin_parse_ul(const NfcDevice* device, FuriString* parsed_data) {
    return ndef_ul_parse(device, parsed_data);
}
