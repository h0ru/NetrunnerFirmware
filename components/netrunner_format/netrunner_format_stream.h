#pragma once
#include <stdlib.h>
#include <stdbool.h>
#include <toolbox/stream/stream.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NetrunnerStreamValueIgnore,
    NetrunnerStreamValueStr,
    NetrunnerStreamValueHex,
    NetrunnerStreamValueFloat,
    NetrunnerStreamValueInt32,
    NetrunnerStreamValueUint32,
    NetrunnerStreamValueHexUint64,
    NetrunnerStreamValueBool,
} NetrunnerStreamValue;

typedef struct {
    const char* key;
    NetrunnerStreamValue type;
    const void* data;
    size_t data_size;
} NetrunnerStreamWriteData;

/**
 * Writes a key/value pair to the stream.
 * @param stream 
 * @param write_data 
 * @return true 
 * @return false 
 */
bool netrunner_format_stream_write_value_line(Stream* stream, NetrunnerStreamWriteData* write_data);

/**
 * Reads a value by key from a stream.
 * @param stream 
 * @param key 
 * @param type 
 * @param _data 
 * @param data_size 
 * @param strict_mode 
 * @return true 
 * @return false 
 */
bool netrunner_format_stream_read_value_line(
    Stream* stream,
    const char* key,
    NetrunnerStreamValue type,
    void* _data,
    size_t data_size,
    bool strict_mode);

/**
 * Get the count of values by key from a stream.
 * @param stream 
 * @param key 
 * @param count 
 * @param strict_mode 
 * @return true 
 * @return false 
 */
bool netrunner_format_stream_get_value_count(
    Stream* stream,
    const char* key,
    uint32_t* count,
    bool strict_mode);

/**
 * Removes a key and the corresponding value string from the stream and inserts a new key/value pair.
 * @param stream 
 * @param write_data 
 * @param strict_mode 
 * @return true 
 * @return false 
 */
bool netrunner_format_stream_delete_key_and_write(
    Stream* stream,
    NetrunnerStreamWriteData* write_data,
    bool strict_mode);

/**
 * Writes a comment string to the stream.
 * @param stream 
 * @param data 
 * @return true 
 * @return false 
 */
bool netrunner_format_stream_write_comment_cstr(Stream* stream, const char* data);

#ifdef __cplusplus
}
#endif
