/** @file netrunner_format.h Netrunner File Format helper library.
 *
 * Netrunner File Format is a fairly simple format for storing data in a file.
 *
 * Netrunner file structure:
 *
 * ~~~~~~~~~~~~~~~~~~~~~
 * # Commentary
 * Field name: field value
 * ~~~~~~~~~~~~~~~~~~~~~
 *
 * Lines starting with the # character are ignored (considered as comments). The
 * separator between the name of the value and the value itself is the string
 * ": ".
 *
 * Currently supported types:
 *
 * ~~~~~~~~~~~~~~~~~~~~~
 * String: text
 * Int32: 1 2 -3 4
 * Uint32: 1 2 3 4
 * Float: 1.0 1234.654
 * Hex: A4 B3 C2 D1 12 FF
 * ~~~~~~~~~~~~~~~~~~~~~
 *
 * End of line is LF when writing, but CR is supported when reading.
 *
 * The library is designed in such a way that comments and field values are
 * completely ignored when searching for keys, that is, they do not consume
 * memory.
 *
 * File example:
 *
 * ~~~~~~~~~~~~~~~~~~~~~
 * Filetype: Netrunner Test File
 * Version: 1
 * # Just test file
 * String: String value
 * UINT: 1234
 * Hex: 00 01 FF A3
 * ~~~~~~~~~~~~~~~~~~~~~
 *
 * Writing:
 *
 * ~~~~~~~~~~~~~~~~~~~~~
 * NetrunnerFormat* format = netrunner_format_file_alloc(storage);
 *
 * do { const uint32_t version = 1; const char* string_value = "String value";
 * const uint32_t uint32_value = 1234; const uint16_t array_size = 4; const
 * uint8_t* array[array_size] = {0x00, 0x01, 0xFF, 0xA3};
 *
 * if(!netrunner_format_file_open_new(format, EXT_PATH("netrunner_format_test")))
 * break; if(!netrunner_format_write_header_cstr(format, "Netrunner Test File",
 * version)) break; if(!netrunner_format_write_comment_cstr(format,
 * "Just test file")) break; if(!netrunner_format_write_string_cstr(format,
 * "String", string_value)) break; if(!netrunner_format_write_uint32(format,
 * "UINT", &uint32_value, 1)) break; if(!netrunner_format_write_hex(format,
 * "Hex Array", array, array_size)) break;
 *
 * // signal that the file was written successfully } while(0);
 *
 * netrunner_format_free(file);
 * ~~~~~~~~~~~~~~~~~~~~~
 *
 * Reading:
 *
 * ~~~~~~~~~~~~~~~~~~~~~
 * NetrunnerFormat* file = netrunner_format_file_alloc(storage);
 *
 * do { uint32_t version = 1; FuriString* file_type; FuriString* string_value;
 * uint32_t uint32_value = 1; uint16_t array_size = 4; uint8_t*
 * array[array_size] = {0}; file_type = furi_string_alloc(); string_value =
 * furi_string_alloc();
 *
 * if(!netrunner_format_file_open_existing(file, EXT_PATH("netrunner_format_test")))
 * break; if(!netrunner_format_read_header(file, file_type, &version)) break;
 * if(!netrunner_format_read_string(file, "String", string_value)) break;
 * if(!netrunner_format_read_uint32(file, "UINT", &uint32_value, 1)) break;
 * if(!netrunner_format_read_hex(file, "Hex Array", array, array_size)) break;
 *
 * // signal that the file was read successfully } while(0);
 *
 * netrunner_format_free(file);
 * ~~~~~~~~~~~~~~~~~~~~~
 */

#pragma once
#include <stdint.h>
#include <storage/storage.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NetrunnerFormat NetrunnerFormat;

typedef enum {
    NetrunnerFormatOffsetFromCurrent,
    NetrunnerFormatOffsetFromStart,
    NetrunnerFormatOffsetFromEnd,
} NetrunnerFormatOffset;

/** Allocate NetrunnerFormat as string.
 *
 * @return     NetrunnerFormat* pointer to a NetrunnerFormat instance
 */
NetrunnerFormat* netrunner_format_string_alloc(void);

/** Allocate NetrunnerFormat as file.
 *
 * @param      storage  The storage
 *
 * @return     NetrunnerFormat* pointer to a NetrunnerFormat instance
 */
NetrunnerFormat* netrunner_format_file_alloc(Storage* storage);

/** Allocate NetrunnerFormat as file, buffered mode.
 *
 * @param      storage  The storage
 *
 * @return     NetrunnerFormat* pointer to a NetrunnerFormat instance
 */
NetrunnerFormat* netrunner_format_buffered_file_alloc(Storage* storage);

/** Open existing file. Use only if NetrunnerFormat allocated as a file.
 *
 * @param      netrunner_format  Pointer to a NetrunnerFormat instance
 * @param      path            File path
 *
 * @return     True on success
 */
bool netrunner_format_file_open_existing(NetrunnerFormat* netrunner_format, const char* path);

/** Open existing file, buffered mode. Use only if NetrunnerFormat allocated as a
 * buffered file.
 *
 * @param      netrunner_format  Pointer to a NetrunnerFormat instance
 * @param      path            File path
 *
 * @return     True on success
 */
bool netrunner_format_buffered_file_open_existing(NetrunnerFormat* netrunner_format, const char* path);

/** Open existing file for writing and add values to the end of file. Use only if
 * NetrunnerFormat allocated as a file.
 *
 * @param      netrunner_format  Pointer to a NetrunnerFormat instance
 * @param      path            File path
 *
 * @return     True on success
 */
bool netrunner_format_file_open_append(NetrunnerFormat* netrunner_format, const char* path);

/** Open file. Creates a new file, or deletes the contents of the file if it
 * already exists. Use only if NetrunnerFormat allocated as a file.
 *
 * @param      netrunner_format  Pointer to a NetrunnerFormat instance
 * @param      path            File path
 *
 * @return     True on success
 */
bool netrunner_format_file_open_always(NetrunnerFormat* netrunner_format, const char* path);

/** Open file. Creates a new file, or deletes the contents of the file if it
 * already exists, buffered mode. Use only if NetrunnerFormat allocated as a
 * buffered file.
 *
 * @param      netrunner_format  Pointer to a NetrunnerFormat instance
 * @param      path            File path
 *
 * @return     True on success
 */
bool netrunner_format_buffered_file_open_always(NetrunnerFormat* netrunner_format, const char* path);

/** Open file. Creates a new file, fails if file already exists. Use only if
 * NetrunnerFormat allocated as a file.
 *
 * @param      netrunner_format  Pointer to a NetrunnerFormat instance
 * @param      path            File path
 *
 * @return     True on success
 */
bool netrunner_format_file_open_new(NetrunnerFormat* netrunner_format, const char* path);

/** Closes the file, use only if NetrunnerFormat allocated as a file.
 *
 * @param      netrunner_format  The netrunner format
 *
 * @return     true
 * @return     false
 */
bool netrunner_format_file_close(NetrunnerFormat* netrunner_format);

/** Closes the file, use only if NetrunnerFormat allocated as a buffered file.
 *
 * @param      netrunner_format  The netrunner format
 *
 * @return     true
 * @return     false
 */
bool netrunner_format_buffered_file_close(NetrunnerFormat* netrunner_format);

/** Free NetrunnerFormat.
 *
 * @param      netrunner_format  Pointer to a NetrunnerFormat instance
 */
void netrunner_format_free(NetrunnerFormat* netrunner_format);

/** Set NetrunnerFormat mode.
 *
 * @param      netrunner_format  Pointer to a NetrunnerFormat instance
 * @param      strict_mode     True obligates not to skip valid fields. False by
 *                             default.
 */
void netrunner_format_set_strict_mode(NetrunnerFormat* netrunner_format, bool strict_mode);

/** Rewind the RW pointer.
 *
 * @param      netrunner_format  Pointer to a NetrunnerFormat instance
 *
 * @return     True on success
 */
bool netrunner_format_rewind(NetrunnerFormat* netrunner_format);

/** Get the RW pointer position
 * 
 * @param      netrunner_format  Pointer to a NetrunnerFormat instance
 * 
 * @return     RW pointer position
 */
size_t netrunner_format_tell(NetrunnerFormat* netrunner_format);

/** Set the RW pointer position to an arbitrary value
 * 
 * @param      netrunner_format  Pointer to a NetrunnerFormat instance
 * @param      offset          Offset relative to the anchor point
 * @param      anchor          Anchor point (e.g. start of file)
 * 
 * @return     True on success
 */
bool netrunner_format_seek(NetrunnerFormat* netrunner_format, int32_t offset, NetrunnerFormatOffset anchor);

/** Move the RW pointer at the end. Can be useful if you want to add some data
 * after reading.
 *
 * @param      netrunner_format  Pointer to a NetrunnerFormat instance
 *
 * @return     True on success
 */
bool netrunner_format_seek_to_end(NetrunnerFormat* netrunner_format);

/** Check if the key exists.
 *
 * @param      netrunner_format  Pointer to a NetrunnerFormat instance
 * @param      key             Key
 *
 * @return     true key exists
 * @return     false key is not exists
 */
bool netrunner_format_key_exist(NetrunnerFormat* netrunner_format, const char* key);

/** Read the header (file type and version).
 *
 * @param      netrunner_format  Pointer to a NetrunnerFormat instance
 * @param      filetype        File type string
 * @param      version         Version Value
 *
 * @return     True on success
 */
bool netrunner_format_read_header(
    NetrunnerFormat* netrunner_format,
    FuriString* filetype,
    uint32_t* version);

/** Write the header (file type and version).
 *
 * @param      netrunner_format  Pointer to a NetrunnerFormat instance
 * @param      filetype        File type string
 * @param      version         Version Value
 *
 * @return     True on success
 */
bool netrunner_format_write_header(
    NetrunnerFormat* netrunner_format,
    FuriString* filetype,
    const uint32_t version);

/** Write the header (file type and version). Plain C string version.
 *
 * @param      netrunner_format  Pointer to a NetrunnerFormat instance
 * @param      filetype        File type string
 * @param      version         Version Value
 *
 * @return     True on success
 */
bool netrunner_format_write_header_cstr(
    NetrunnerFormat* netrunner_format,
    const char* filetype,
    const uint32_t version);

/** Get the count of values by key
 *
 * @param      netrunner_format  Pointer to a NetrunnerFormat instance
 * @param      key             The key
 * @param      count           The count
 *
 * @return     bool
 */
bool netrunner_format_get_value_count(
    NetrunnerFormat* netrunner_format,
    const char* key,
    uint32_t* count);

/** Read a string by key
 *
 * @param      netrunner_format  Pointer to a NetrunnerFormat instance
 * @param      key             Key
 * @param      data            Value
 *
 * @return     True on success
 */
bool netrunner_format_read_string(NetrunnerFormat* netrunner_format, const char* key, FuriString* data);

/** Write key and string
 *
 * @param      netrunner_format  Pointer to a NetrunnerFormat instance
 * @param      key             Key
 * @param      data            Value
 *
 * @return     True on success
 */
bool netrunner_format_write_string(NetrunnerFormat* netrunner_format, const char* key, FuriString* data);

/** Write key and string. Plain C string version.
 *
 * @param      netrunner_format  Pointer to a NetrunnerFormat instance
 * @param      key             Key
 * @param      data            Value
 *
 * @return     True on success
 */
bool netrunner_format_write_string_cstr(
    NetrunnerFormat* netrunner_format,
    const char* key,
    const char* data);

/** Read array of uint64 in hex format by key
 *
 * @param      netrunner_format  Pointer to a NetrunnerFormat instance
 * @param      key             Key
 * @param      data            Value
 * @param      data_size       Values count
 *
 * @return     True on success
 */
bool netrunner_format_read_hex_uint64(
    NetrunnerFormat* netrunner_format,
    const char* key,
    uint64_t* data,
    const uint16_t data_size);

/** Write key and array of uint64 in hex format
 *
 * @param      netrunner_format  Pointer to a NetrunnerFormat instance
 * @param      key             Key
 * @param      data            Value
 * @param      data_size       Values count
 *
 * @return     True on success
 */
bool netrunner_format_write_hex_uint64(
    NetrunnerFormat* netrunner_format,
    const char* key,
    const uint64_t* data,
    const uint16_t data_size);

/** Read array of uint32 by key
 *
 * @param      netrunner_format  Pointer to a NetrunnerFormat instance
 * @param      key             Key
 * @param      data            Value
 * @param      data_size       Values count
 *
 * @return     True on success
 */
bool netrunner_format_read_uint32(
    NetrunnerFormat* netrunner_format,
    const char* key,
    uint32_t* data,
    const uint16_t data_size);

/** Write key and array of uint32
 *
 * @param      netrunner_format  Pointer to a NetrunnerFormat instance
 * @param      key             Key
 * @param      data            Value
 * @param      data_size       Values count
 *
 * @return     True on success
 */
bool netrunner_format_write_uint32(
    NetrunnerFormat* netrunner_format,
    const char* key,
    const uint32_t* data,
    const uint16_t data_size);

/** Read array of int32 by key
 *
 * @param      netrunner_format  Pointer to a NetrunnerFormat instance
 * @param      key             Key
 * @param      data            Value
 * @param      data_size       Values count
 *
 * @return     True on success
 */
bool netrunner_format_read_int32(
    NetrunnerFormat* netrunner_format,
    const char* key,
    int32_t* data,
    const uint16_t data_size);

/** Write key and array of int32
 *
 * @param      netrunner_format  Pointer to a NetrunnerFormat instance
 * @param      key             Key
 * @param      data            Value
 * @param      data_size       Values count
 *
 * @return     True on success
 */
bool netrunner_format_write_int32(
    NetrunnerFormat* netrunner_format,
    const char* key,
    const int32_t* data,
    const uint16_t data_size);

/** Read array of bool by key
 *
 * @param      netrunner_format  Pointer to a NetrunnerFormat instance
 * @param      key             Key
 * @param      data            Value
 * @param      data_size       Values count
 *
 * @return     True on success
 */
bool netrunner_format_read_bool(
    NetrunnerFormat* netrunner_format,
    const char* key,
    bool* data,
    const uint16_t data_size);

/** Write key and array of bool
 *
 * @param      netrunner_format  Pointer to a NetrunnerFormat instance
 * @param      key             Key
 * @param      data            Value
 * @param      data_size       Values count
 *
 * @return     True on success
 */
bool netrunner_format_write_bool(
    NetrunnerFormat* netrunner_format,
    const char* key,
    const bool* data,
    const uint16_t data_size);

/** Read array of float by key
 *
 * @param      netrunner_format  Pointer to a NetrunnerFormat instance
 * @param      key             Key
 * @param      data            Value
 * @param      data_size       Values count
 *
 * @return     True on success
 */
bool netrunner_format_read_float(
    NetrunnerFormat* netrunner_format,
    const char* key,
    float* data,
    const uint16_t data_size);

/** Write key and array of float
 *
 * @param      netrunner_format  Pointer to a NetrunnerFormat instance
 * @param      key             Key
 * @param      data            Value
 * @param      data_size       Values count
 *
 * @return     True on success
 */
bool netrunner_format_write_float(
    NetrunnerFormat* netrunner_format,
    const char* key,
    const float* data,
    const uint16_t data_size);

/** Read array of hex-formatted bytes by key
 *
 * @param      netrunner_format  Pointer to a NetrunnerFormat instance
 * @param      key             Key
 * @param      data            Value
 * @param      data_size       Values count
 *
 * @return     True on success
 */
bool netrunner_format_read_hex(
    NetrunnerFormat* netrunner_format,
    const char* key,
    uint8_t* data,
    const uint16_t data_size);

/** Write key and array of hex-formatted bytes
 *
 * @param      netrunner_format  Pointer to a NetrunnerFormat instance
 * @param      key             Key
 * @param      data            Value
 * @param      data_size       Values count
 *
 * @return     True on success
 */
bool netrunner_format_write_hex(
    NetrunnerFormat* netrunner_format,
    const char* key,
    const uint8_t* data,
    const uint16_t data_size);

/** Write comment
 *
 * @param      netrunner_format  Pointer to a NetrunnerFormat instance
 * @param      data            Comment text
 *
 * @return     True on success
 */
bool netrunner_format_write_comment(NetrunnerFormat* netrunner_format, FuriString* data);

/** Write comment. Plain C string version.
 *
 * @param      netrunner_format  Pointer to a NetrunnerFormat instance
 * @param      data            Comment text
 *
 * @return     True on success
 */
bool netrunner_format_write_comment_cstr(NetrunnerFormat* netrunner_format, const char* data);

/** Write empty line (Improves readability for human based parsing)
 *
 * @param      netrunner_format  Pointer to a NetrunnerFormat instance
 *
 * @return     True on success
 */
bool netrunner_format_write_empty_line(NetrunnerFormat* netrunner_format);

/** Removes the first matching key and its value. Sets the RW pointer to a
 * position of deleted data.
 *
 * @param      netrunner_format  Pointer to a NetrunnerFormat instance
 * @param      key             Key
 *
 * @return     True on success
 */
bool netrunner_format_delete_key(NetrunnerFormat* netrunner_format, const char* key);

/** Updates the value of the first matching key to a string value. Sets the RW
 * pointer to a position at the end of inserted data.
 *
 * @param      netrunner_format  Pointer to a NetrunnerFormat instance
 * @param      key             Key
 * @param      data            Value
 *
 * @return     True on success
 */
bool netrunner_format_update_string(NetrunnerFormat* netrunner_format, const char* key, FuriString* data);

/** Updates the value of the first matching key to a string value. Plain C
 * version. Sets the RW pointer to a position at the end of inserted data.
 *
 * @param      netrunner_format  Pointer to a NetrunnerFormat instance
 * @param      key             Key
 * @param      data            Value
 *
 * @return     True on success
 */
bool netrunner_format_update_string_cstr(
    NetrunnerFormat* netrunner_format,
    const char* key,
    const char* data);

/** Updates the value of the first matching key to a uint32 array value. Sets the
 * RW pointer to a position at the end of inserted data.
 *
 * @param      netrunner_format  Pointer to a NetrunnerFormat instance
 * @param      key             Key
 * @param      data            Value
 * @param[in]  data_size       The data size
 *
 * @return     True on success
 */
bool netrunner_format_update_uint32(
    NetrunnerFormat* netrunner_format,
    const char* key,
    const uint32_t* data,
    const uint16_t data_size);

/** Updates the value of the first matching key to a int32 array value. Sets the
 * RW pointer to a position at the end of inserted data.
 *
 * @param      netrunner_format  Pointer to a NetrunnerFormat instance
 * @param      key             Key
 * @param      data            Value
 * @param[in]  data_size       The data size
 *
 * @return     True on success
 */
bool netrunner_format_update_int32(
    NetrunnerFormat* netrunner_format,
    const char* key,
    const int32_t* data,
    const uint16_t data_size);

/** Updates the value of the first matching key to a bool array value. Sets the
 * RW pointer to a position at the end of inserted data.
 *
 * @param      netrunner_format  Pointer to a NetrunnerFormat instance
 * @param      key             Key
 * @param      data            Value
 * @param[in]  data_size       The data size
 *
 * @return     True on success
 */
bool netrunner_format_update_bool(
    NetrunnerFormat* netrunner_format,
    const char* key,
    const bool* data,
    const uint16_t data_size);

/** Updates the value of the first matching key to a float array value. Sets the
 * RW pointer to a position at the end of inserted data.
 *
 * @param      netrunner_format  Pointer to a NetrunnerFormat instance
 * @param      key             Key
 * @param      data            Value
 * @param[in]  data_size       The data size
 *
 * @return     True on success
 */
bool netrunner_format_update_float(
    NetrunnerFormat* netrunner_format,
    const char* key,
    const float* data,
    const uint16_t data_size);

/** Updates the value of the first matching key to an array of hex-formatted
 * bytes. Sets the RW pointer to a position at the end of inserted data.
 *
 * @param      netrunner_format  Pointer to a NetrunnerFormat instance
 * @param      key             Key
 * @param      data            Value
 * @param[in]  data_size       The data size
 *
 * @return     True on success
 */
bool netrunner_format_update_hex(
    NetrunnerFormat* netrunner_format,
    const char* key,
    const uint8_t* data,
    const uint16_t data_size);

/** Updates the value of the first matching key to a string value, or adds the
 * key and value if the key did not exist. Sets the RW pointer to a position at
 * the end of inserted data.
 *
 * @param      netrunner_format  Pointer to a NetrunnerFormat instance
 * @param      key             Key
 * @param      data            Value
 *
 * @return     True on success
 */
bool netrunner_format_insert_or_update_string(
    NetrunnerFormat* netrunner_format,
    const char* key,
    FuriString* data);

/** Updates the value of the first matching key to a string value, or adds the
 * key and value if the key did not exist. Plain C version. Sets the RW pointer
 * to a position at the end of inserted data.
 *
 * @param      netrunner_format  Pointer to a NetrunnerFormat instance
 * @param      key             Key
 * @param      data            Value
 *
 * @return     True on success
 */
bool netrunner_format_insert_or_update_string_cstr(
    NetrunnerFormat* netrunner_format,
    const char* key,
    const char* data);

/** Updates the value of the first matching key to a uint32 array value, or adds
 * the key and value if the key did not exist. Sets the RW pointer to a position
 * at the end of inserted data.
 *
 * @param      netrunner_format  Pointer to a NetrunnerFormat instance
 * @param      key             Key
 * @param      data            Value
 * @param[in]  data_size       The data size
 *
 * @return     True on success
 */
bool netrunner_format_insert_or_update_uint32(
    NetrunnerFormat* netrunner_format,
    const char* key,
    const uint32_t* data,
    const uint16_t data_size);

/** Updates the value of the first matching key to a int32 array value, or adds
 * the key and value if the key did not exist. Sets the RW pointer to a position
 * at the end of inserted data.
 *
 * @param      netrunner_format  Pointer to a NetrunnerFormat instance
 * @param      key             Key
 * @param      data            Value
 * @param[in]  data_size       The data size
 *
 * @return     True on success
 */
bool netrunner_format_insert_or_update_int32(
    NetrunnerFormat* netrunner_format,
    const char* key,
    const int32_t* data,
    const uint16_t data_size);

/** Updates the value of the first matching key to a bool array value, or adds
 * the key and value if the key did not exist. Sets the RW pointer to a position
 * at the end of inserted data.
 *
 * @param      netrunner_format  Pointer to a NetrunnerFormat instance
 * @param      key             Key
 * @param      data            Value
 * @param[in]  data_size       The data size
 *
 * @return     True on success
 */
bool netrunner_format_insert_or_update_bool(
    NetrunnerFormat* netrunner_format,
    const char* key,
    const bool* data,
    const uint16_t data_size);

/** Updates the value of the first matching key to a float array value, or adds
 * the key and value if the key did not exist. Sets the RW pointer to a position
 * at the end of inserted data.
 *
 * @param      netrunner_format  Pointer to a NetrunnerFormat instance
 * @param      key             Key
 * @param      data            Value
 * @param[in]  data_size       The data size
 *
 * @return     True on success
 */
bool netrunner_format_insert_or_update_float(
    NetrunnerFormat* netrunner_format,
    const char* key,
    const float* data,
    const uint16_t data_size);

/** Updates the value of the first matching key to an array of hex-formatted
 * bytes, or adds the key and value if the key did not exist. Sets the RW
 * pointer to a position at the end of inserted data.
 *
 * @param      netrunner_format  Pointer to a NetrunnerFormat instance
 * @param      key             Key
 * @param      data            Value
 * @param[in]  data_size       The data size
 *
 * @return     True on success
 */
bool netrunner_format_insert_or_update_hex(
    NetrunnerFormat* netrunner_format,
    const char* key,
    const uint8_t* data,
    const uint16_t data_size);

#ifdef __cplusplus
}
#endif
