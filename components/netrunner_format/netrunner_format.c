#include <string.h>
#include <core/core_defines.h>
#include <core/check.h>
#include <toolbox/stream/stream.h>
#include <toolbox/stream/string_stream.h>
#include <toolbox/stream/file_stream.h>
#include <toolbox/stream/buffered_file_stream.h>
#include "netrunner_format.h"
#include "netrunner_format_i.h"
#include "netrunner_format_stream.h"
#include "netrunner_format_stream_i.h"

// permits direct casting between `NetrunnerFormatOffset` and `StreamOffset`
static_assert((size_t)NetrunnerFormatOffsetFromCurrent == (size_t)StreamOffsetFromCurrent);
static_assert((size_t)NetrunnerFormatOffsetFromStart == (size_t)StreamOffsetFromStart);
static_assert((size_t)NetrunnerFormatOffsetFromEnd == (size_t)StreamOffsetFromEnd);

/********************************** Private **********************************/
struct NetrunnerFormat {
    Stream* stream;
    bool strict_mode;
};

static const char* const netrunner_format_filetype_key = "Filetype";
static const char* const netrunner_format_version_key = "Version";

Stream* netrunner_format_get_raw_stream(NetrunnerFormat* netrunner_format) {
    return netrunner_format->stream;
}

/********************************** Public **********************************/

NetrunnerFormat* netrunner_format_string_alloc(void) {
    NetrunnerFormat* netrunner_format = malloc(sizeof(NetrunnerFormat));
    netrunner_format->stream = string_stream_alloc();
    netrunner_format->strict_mode = false;
    return netrunner_format;
}

NetrunnerFormat* netrunner_format_file_alloc(Storage* storage) {
    NetrunnerFormat* netrunner_format = malloc(sizeof(NetrunnerFormat));
    netrunner_format->stream = file_stream_alloc(storage);
    netrunner_format->strict_mode = false;
    return netrunner_format;
}

NetrunnerFormat* netrunner_format_buffered_file_alloc(Storage* storage) {
    NetrunnerFormat* netrunner_format = malloc(sizeof(NetrunnerFormat));
    netrunner_format->stream = buffered_file_stream_alloc(storage);
    netrunner_format->strict_mode = false;
    return netrunner_format;
}

bool netrunner_format_file_open_existing(NetrunnerFormat* netrunner_format, const char* path) {
    furi_check(netrunner_format);
    return file_stream_open(netrunner_format->stream, path, FSAM_READ_WRITE, FSOM_OPEN_EXISTING);
}

bool netrunner_format_buffered_file_open_existing(NetrunnerFormat* netrunner_format, const char* path) {
    furi_check(netrunner_format);
    return buffered_file_stream_open(
        netrunner_format->stream, path, FSAM_READ_WRITE, FSOM_OPEN_EXISTING);
}

bool netrunner_format_file_open_append(NetrunnerFormat* netrunner_format, const char* path) {
    furi_check(netrunner_format);

    bool result =
        file_stream_open(netrunner_format->stream, path, FSAM_READ_WRITE, FSOM_OPEN_APPEND);

    // Add EOL if it is not there
    if(stream_size(netrunner_format->stream) >= 1) {
        do {
            char last_char;
            result = false;

            if(!stream_seek(netrunner_format->stream, -1, StreamOffsetFromEnd)) break;

            uint16_t bytes_were_read =
                stream_read(netrunner_format->stream, (uint8_t*)&last_char, 1);
            if(bytes_were_read != 1) break;

            if(last_char != netrunner_format_eoln) {
                if(!netrunner_format_stream_write_eol(netrunner_format->stream)) break;
            }

            result = true;
        } while(false);
    } else {
        stream_seek(netrunner_format->stream, 0, StreamOffsetFromEnd);
    }

    return result;
}

bool netrunner_format_file_open_always(NetrunnerFormat* netrunner_format, const char* path) {
    furi_check(netrunner_format);
    return file_stream_open(netrunner_format->stream, path, FSAM_READ_WRITE, FSOM_CREATE_ALWAYS);
}

bool netrunner_format_buffered_file_open_always(NetrunnerFormat* netrunner_format, const char* path) {
    furi_check(netrunner_format);
    return buffered_file_stream_open(
        netrunner_format->stream, path, FSAM_READ_WRITE, FSOM_CREATE_ALWAYS);
}

bool netrunner_format_file_open_new(NetrunnerFormat* netrunner_format, const char* path) {
    furi_check(netrunner_format);
    return file_stream_open(netrunner_format->stream, path, FSAM_READ_WRITE, FSOM_CREATE_NEW);
}

bool netrunner_format_file_close(NetrunnerFormat* netrunner_format) {
    furi_check(netrunner_format);
    return file_stream_close(netrunner_format->stream);
}

bool netrunner_format_buffered_file_close(NetrunnerFormat* netrunner_format) {
    furi_check(netrunner_format);
    return buffered_file_stream_close(netrunner_format->stream);
}

void netrunner_format_free(NetrunnerFormat* netrunner_format) {
    furi_check(netrunner_format);
    stream_free(netrunner_format->stream);
    free(netrunner_format);
}

void netrunner_format_set_strict_mode(NetrunnerFormat* netrunner_format, bool strict_mode) {
    netrunner_format->strict_mode = strict_mode;
}

bool netrunner_format_rewind(NetrunnerFormat* netrunner_format) {
    furi_check(netrunner_format);
    return stream_rewind(netrunner_format->stream);
}

size_t netrunner_format_tell(NetrunnerFormat* netrunner_format) {
    furi_check(netrunner_format);
    return stream_tell(netrunner_format->stream);
}

bool netrunner_format_seek(NetrunnerFormat* netrunner_format, int32_t offset, NetrunnerFormatOffset anchor) {
    furi_check(netrunner_format);
    // direct usage of `anchor` made valid by `static_assert`s at the top of this file
    return stream_seek(netrunner_format->stream, offset, (StreamOffset)anchor);
}

bool netrunner_format_seek_to_end(NetrunnerFormat* netrunner_format) {
    furi_check(netrunner_format);
    return stream_seek(netrunner_format->stream, 0, StreamOffsetFromEnd);
}

bool netrunner_format_key_exist(NetrunnerFormat* netrunner_format, const char* key) {
    size_t pos = stream_tell(netrunner_format->stream);
    stream_seek(netrunner_format->stream, 0, StreamOffsetFromStart);
    bool result = netrunner_format_stream_seek_to_key(netrunner_format->stream, key, false);
    stream_seek(netrunner_format->stream, pos, StreamOffsetFromStart);

    return result;
}

bool netrunner_format_read_header(
    NetrunnerFormat* netrunner_format,
    FuriString* filetype,
    uint32_t* version) {
    furi_check(netrunner_format);
    bool ok =
        netrunner_format_read_string(netrunner_format, netrunner_format_filetype_key, filetype) &&
        netrunner_format_read_uint32(netrunner_format, netrunner_format_version_key, version, 1);
    return ok;
}

bool netrunner_format_write_header(
    NetrunnerFormat* netrunner_format,
    FuriString* filetype,
    const uint32_t version) {
    furi_check(netrunner_format);
    return netrunner_format_write_header_cstr(
        netrunner_format, furi_string_get_cstr(filetype), version);
}

bool netrunner_format_write_header_cstr(
    NetrunnerFormat* netrunner_format,
    const char* filetype,
    const uint32_t version) {
    furi_check(netrunner_format);
    return netrunner_format_write_string_cstr(
               netrunner_format, netrunner_format_filetype_key, filetype) &&
           netrunner_format_write_uint32(netrunner_format, netrunner_format_version_key, &version, 1);
}

bool netrunner_format_get_value_count(
    NetrunnerFormat* netrunner_format,
    const char* key,
    uint32_t* count) {
    furi_check(netrunner_format);
    return netrunner_format_stream_get_value_count(
        netrunner_format->stream, key, count, netrunner_format->strict_mode);
}

bool netrunner_format_read_string(NetrunnerFormat* netrunner_format, const char* key, FuriString* data) {
    furi_check(netrunner_format);
    return netrunner_format_stream_read_value_line(
        netrunner_format->stream, key, NetrunnerStreamValueStr, data, 1, netrunner_format->strict_mode);
}

bool netrunner_format_write_string(NetrunnerFormat* netrunner_format, const char* key, FuriString* data) {
    furi_check(netrunner_format);
    NetrunnerStreamWriteData write_data = {
        .key = key,
        .type = NetrunnerStreamValueStr,
        .data = furi_string_get_cstr(data),
        .data_size = 1,
    };
    bool result = netrunner_format_stream_write_value_line(netrunner_format->stream, &write_data);
    return result;
}

bool netrunner_format_write_string_cstr(
    NetrunnerFormat* netrunner_format,
    const char* key,
    const char* data) {
    furi_check(netrunner_format);
    NetrunnerStreamWriteData write_data = {
        .key = key,
        .type = NetrunnerStreamValueStr,
        .data = data,
        .data_size = 1,
    };
    bool result = netrunner_format_stream_write_value_line(netrunner_format->stream, &write_data);
    return result;
}

bool netrunner_format_read_hex_uint64(
    NetrunnerFormat* netrunner_format,
    const char* key,
    uint64_t* data,
    const uint16_t data_size) {
    furi_check(netrunner_format);
    return netrunner_format_stream_read_value_line(
        netrunner_format->stream,
        key,
        NetrunnerStreamValueHexUint64,
        data,
        data_size,
        netrunner_format->strict_mode);
}

bool netrunner_format_write_hex_uint64(
    NetrunnerFormat* netrunner_format,
    const char* key,
    const uint64_t* data,
    const uint16_t data_size) {
    furi_check(netrunner_format);
    NetrunnerStreamWriteData write_data = {
        .key = key,
        .type = NetrunnerStreamValueHexUint64,
        .data = data,
        .data_size = data_size,
    };
    bool result = netrunner_format_stream_write_value_line(netrunner_format->stream, &write_data);
    return result;
}

bool netrunner_format_read_uint32(
    NetrunnerFormat* netrunner_format,
    const char* key,
    uint32_t* data,
    const uint16_t data_size) {
    furi_check(netrunner_format);
    return netrunner_format_stream_read_value_line(
        netrunner_format->stream,
        key,
        NetrunnerStreamValueUint32,
        data,
        data_size,
        netrunner_format->strict_mode);
}

bool netrunner_format_write_uint32(
    NetrunnerFormat* netrunner_format,
    const char* key,
    const uint32_t* data,
    const uint16_t data_size) {
    furi_check(netrunner_format);
    NetrunnerStreamWriteData write_data = {
        .key = key,
        .type = NetrunnerStreamValueUint32,
        .data = data,
        .data_size = data_size,
    };
    bool result = netrunner_format_stream_write_value_line(netrunner_format->stream, &write_data);
    return result;
}

bool netrunner_format_read_int32(
    NetrunnerFormat* netrunner_format,
    const char* key,
    int32_t* data,
    const uint16_t data_size) {
    return netrunner_format_stream_read_value_line(
        netrunner_format->stream,
        key,
        NetrunnerStreamValueInt32,
        data,
        data_size,
        netrunner_format->strict_mode);
}

bool netrunner_format_write_int32(
    NetrunnerFormat* netrunner_format,
    const char* key,
    const int32_t* data,
    const uint16_t data_size) {
    furi_check(netrunner_format);
    NetrunnerStreamWriteData write_data = {
        .key = key,
        .type = NetrunnerStreamValueInt32,
        .data = data,
        .data_size = data_size,
    };
    bool result = netrunner_format_stream_write_value_line(netrunner_format->stream, &write_data);
    return result;
}

bool netrunner_format_read_bool(
    NetrunnerFormat* netrunner_format,
    const char* key,
    bool* data,
    const uint16_t data_size) {
    return netrunner_format_stream_read_value_line(
        netrunner_format->stream,
        key,
        NetrunnerStreamValueBool,
        data,
        data_size,
        netrunner_format->strict_mode);
}

bool netrunner_format_write_bool(
    NetrunnerFormat* netrunner_format,
    const char* key,
    const bool* data,
    const uint16_t data_size) {
    furi_check(netrunner_format);
    NetrunnerStreamWriteData write_data = {
        .key = key,
        .type = NetrunnerStreamValueBool,
        .data = data,
        .data_size = data_size,
    };
    bool result = netrunner_format_stream_write_value_line(netrunner_format->stream, &write_data);
    return result;
}

bool netrunner_format_read_float(
    NetrunnerFormat* netrunner_format,
    const char* key,
    float* data,
    const uint16_t data_size) {
    return netrunner_format_stream_read_value_line(
        netrunner_format->stream,
        key,
        NetrunnerStreamValueFloat,
        data,
        data_size,
        netrunner_format->strict_mode);
}

bool netrunner_format_write_float(
    NetrunnerFormat* netrunner_format,
    const char* key,
    const float* data,
    const uint16_t data_size) {
    furi_check(netrunner_format);
    NetrunnerStreamWriteData write_data = {
        .key = key,
        .type = NetrunnerStreamValueFloat,
        .data = data,
        .data_size = data_size,
    };
    bool result = netrunner_format_stream_write_value_line(netrunner_format->stream, &write_data);
    return result;
}

bool netrunner_format_read_hex(
    NetrunnerFormat* netrunner_format,
    const char* key,
    uint8_t* data,
    const uint16_t data_size) {
    return netrunner_format_stream_read_value_line(
        netrunner_format->stream,
        key,
        NetrunnerStreamValueHex,
        data,
        data_size,
        netrunner_format->strict_mode);
}

bool netrunner_format_write_hex(
    NetrunnerFormat* netrunner_format,
    const char* key,
    const uint8_t* data,
    const uint16_t data_size) {
    furi_check(netrunner_format);
    NetrunnerStreamWriteData write_data = {
        .key = key,
        .type = NetrunnerStreamValueHex,
        .data = data,
        .data_size = data_size,
    };
    bool result = netrunner_format_stream_write_value_line(netrunner_format->stream, &write_data);
    return result;
}

bool netrunner_format_write_comment(NetrunnerFormat* netrunner_format, FuriString* data) {
    furi_check(netrunner_format);
    return netrunner_format_write_comment_cstr(netrunner_format, furi_string_get_cstr(data));
}

bool netrunner_format_write_comment_cstr(NetrunnerFormat* netrunner_format, const char* data) {
    furi_check(netrunner_format);
    return netrunner_format_stream_write_comment_cstr(netrunner_format->stream, data);
}

bool netrunner_format_write_empty_line(NetrunnerFormat* netrunner_format) {
    furi_check(netrunner_format);
    return netrunner_format_stream_write_eol(netrunner_format->stream);
}

bool netrunner_format_delete_key(NetrunnerFormat* netrunner_format, const char* key) {
    furi_check(netrunner_format);
    NetrunnerStreamWriteData write_data = {
        .key = key,
        .type = NetrunnerStreamValueIgnore,
        .data = NULL,
        .data_size = 0,
    };
    bool result = netrunner_format_stream_delete_key_and_write(
        netrunner_format->stream, &write_data, netrunner_format->strict_mode);
    return result;
}

bool netrunner_format_update_string(NetrunnerFormat* netrunner_format, const char* key, FuriString* data) {
    furi_check(netrunner_format);
    NetrunnerStreamWriteData write_data = {
        .key = key,
        .type = NetrunnerStreamValueStr,
        .data = furi_string_get_cstr(data),
        .data_size = 1,
    };
    bool result = netrunner_format_stream_delete_key_and_write(
        netrunner_format->stream, &write_data, netrunner_format->strict_mode);
    return result;
}

bool netrunner_format_update_string_cstr(
    NetrunnerFormat* netrunner_format,
    const char* key,
    const char* data) {
    furi_check(netrunner_format);
    NetrunnerStreamWriteData write_data = {
        .key = key,
        .type = NetrunnerStreamValueStr,
        .data = data,
        .data_size = 1,
    };
    bool result = netrunner_format_stream_delete_key_and_write(
        netrunner_format->stream, &write_data, netrunner_format->strict_mode);
    return result;
}

bool netrunner_format_update_uint32(
    NetrunnerFormat* netrunner_format,
    const char* key,
    const uint32_t* data,
    const uint16_t data_size) {
    furi_check(netrunner_format);
    NetrunnerStreamWriteData write_data = {
        .key = key,
        .type = NetrunnerStreamValueUint32,
        .data = data,
        .data_size = data_size,
    };
    bool result = netrunner_format_stream_delete_key_and_write(
        netrunner_format->stream, &write_data, netrunner_format->strict_mode);
    return result;
}

bool netrunner_format_update_int32(
    NetrunnerFormat* netrunner_format,
    const char* key,
    const int32_t* data,
    const uint16_t data_size) {
    NetrunnerStreamWriteData write_data = {
        .key = key,
        .type = NetrunnerStreamValueInt32,
        .data = data,
        .data_size = data_size,
    };
    bool result = netrunner_format_stream_delete_key_and_write(
        netrunner_format->stream, &write_data, netrunner_format->strict_mode);
    return result;
}

bool netrunner_format_update_bool(
    NetrunnerFormat* netrunner_format,
    const char* key,
    const bool* data,
    const uint16_t data_size) {
    NetrunnerStreamWriteData write_data = {
        .key = key,
        .type = NetrunnerStreamValueBool,
        .data = data,
        .data_size = data_size,
    };
    bool result = netrunner_format_stream_delete_key_and_write(
        netrunner_format->stream, &write_data, netrunner_format->strict_mode);
    return result;
}

bool netrunner_format_update_float(
    NetrunnerFormat* netrunner_format,
    const char* key,
    const float* data,
    const uint16_t data_size) {
    NetrunnerStreamWriteData write_data = {
        .key = key,
        .type = NetrunnerStreamValueFloat,
        .data = data,
        .data_size = data_size,
    };
    bool result = netrunner_format_stream_delete_key_and_write(
        netrunner_format->stream, &write_data, netrunner_format->strict_mode);
    return result;
}

bool netrunner_format_update_hex(
    NetrunnerFormat* netrunner_format,
    const char* key,
    const uint8_t* data,
    const uint16_t data_size) {
    NetrunnerStreamWriteData write_data = {
        .key = key,
        .type = NetrunnerStreamValueHex,
        .data = data,
        .data_size = data_size,
    };
    bool result = netrunner_format_stream_delete_key_and_write(
        netrunner_format->stream, &write_data, netrunner_format->strict_mode);
    return result;
}

bool netrunner_format_insert_or_update_string(
    NetrunnerFormat* netrunner_format,
    const char* key,
    FuriString* data) {
    bool result = false;

    if(!netrunner_format_key_exist(netrunner_format, key)) {
        netrunner_format_seek_to_end(netrunner_format);
        result = netrunner_format_write_string(netrunner_format, key, data);
    } else {
        result = netrunner_format_update_string(netrunner_format, key, data);
    }

    return result;
}

bool netrunner_format_insert_or_update_string_cstr(
    NetrunnerFormat* netrunner_format,
    const char* key,
    const char* data) {
    bool result = false;

    if(!netrunner_format_key_exist(netrunner_format, key)) {
        netrunner_format_seek_to_end(netrunner_format);
        result = netrunner_format_write_string_cstr(netrunner_format, key, data);
    } else {
        result = netrunner_format_update_string_cstr(netrunner_format, key, data);
    }

    return result;
}

bool netrunner_format_insert_or_update_uint32(
    NetrunnerFormat* netrunner_format,
    const char* key,
    const uint32_t* data,
    const uint16_t data_size) {
    bool result = false;

    if(!netrunner_format_key_exist(netrunner_format, key)) {
        netrunner_format_seek_to_end(netrunner_format);
        result = netrunner_format_write_uint32(netrunner_format, key, data, data_size);
    } else {
        result = netrunner_format_update_uint32(netrunner_format, key, data, data_size);
    }

    return result;
}

bool netrunner_format_insert_or_update_int32(
    NetrunnerFormat* netrunner_format,
    const char* key,
    const int32_t* data,
    const uint16_t data_size) {
    bool result = false;

    if(!netrunner_format_key_exist(netrunner_format, key)) {
        netrunner_format_seek_to_end(netrunner_format);
        result = netrunner_format_write_int32(netrunner_format, key, data, data_size);
    } else {
        result = netrunner_format_update_int32(netrunner_format, key, data, data_size);
    }

    return result;
}

bool netrunner_format_insert_or_update_bool(
    NetrunnerFormat* netrunner_format,
    const char* key,
    const bool* data,
    const uint16_t data_size) {
    bool result = false;

    if(!netrunner_format_key_exist(netrunner_format, key)) {
        netrunner_format_seek_to_end(netrunner_format);
        result = netrunner_format_write_bool(netrunner_format, key, data, data_size);
    } else {
        result = netrunner_format_update_bool(netrunner_format, key, data, data_size);
    }

    return result;
}

bool netrunner_format_insert_or_update_float(
    NetrunnerFormat* netrunner_format,
    const char* key,
    const float* data,
    const uint16_t data_size) {
    bool result = false;

    if(!netrunner_format_key_exist(netrunner_format, key)) {
        netrunner_format_seek_to_end(netrunner_format);
        result = netrunner_format_write_float(netrunner_format, key, data, data_size);
    } else {
        result = netrunner_format_update_float(netrunner_format, key, data, data_size);
    }

    return result;
}

bool netrunner_format_insert_or_update_hex(
    NetrunnerFormat* netrunner_format,
    const char* key,
    const uint8_t* data,
    const uint16_t data_size) {
    bool result = false;

    if(!netrunner_format_key_exist(netrunner_format, key)) {
        netrunner_format_seek_to_end(netrunner_format);
        result = netrunner_format_write_hex(netrunner_format, key, data, data_size);
    } else {
        result = netrunner_format_update_hex(netrunner_format, key, data, data_size);
    }

    return result;
}
