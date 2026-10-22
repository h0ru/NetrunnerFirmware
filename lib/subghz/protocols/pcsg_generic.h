#pragma once

#include "../blocks/generic_i.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PCSGBlockGeneric PCSGBlockGeneric;

struct PCSGBlockGeneric {
    const char* protocol_name;
    FuriString* result_ric;
    FuriString* result_msg;
};

/**
 * Serialize data PCSGBlockGeneric.
 * @param instance Pointer to a PCSGBlockGeneric instance
 * @param netrunner_format Pointer to a NetrunnerFormat instance
 * @param preset The modulation on which the signal was received, SubGhzRadioPreset
 * @return true On success
 */
SubGhzProtocolStatus pcsg_block_generic_serialize(
    PCSGBlockGeneric* instance,
    NetrunnerFormat* netrunner_format,
    SubGhzRadioPreset* preset);

/**
 * Deserialize data PCSGBlockGeneric.
 * @param instance Pointer to a PCSGBlockGeneric instance
 * @param netrunner_format Pointer to a NetrunnerFormat instance
 * @return true On success
 */
SubGhzProtocolStatus
    pcsg_block_generic_deserialize(PCSGBlockGeneric* instance, NetrunnerFormat* netrunner_format);

#ifdef __cplusplus
}
#endif
