#pragma once
#include <toolbox/stream/stream.h>
#include "netrunner_format.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Returns the underlying stream instance.
 * Use only if you know what you are doing.
 * @param netrunner_format 
 * @return Stream* 
 */
Stream* netrunner_format_get_raw_stream(NetrunnerFormat* netrunner_format);

#ifdef __cplusplus
}
#endif
