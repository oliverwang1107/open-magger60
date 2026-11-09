#include "km/km_hid.h"

#include <string.h>

#define KM_BOOT_CHUNK_MAX 32u

uint8_t km_hid_checksum(uint8_t len, uint32_t offset, const uint8_t *data)
{
    uint32_t sum = (uint32_t)len + (offset & 0xFFu) + ((offset >> 8) & 0xFFu) + ((offset >> 16) & 0xFFu);
    /* A read request carries a length but no data bytes: pass data == NULL and only
     * len + offsets are summed (the stock protocol's checksum: len + offsets + data). */
    if (data != NULL) {
        for (uint8_t i = 0; i < len; i++) {
            sum += data[i];
        }
    }
    return (uint8_t)(sum & 0xFFu);
}

km_hid_status_t km_hid_build(uint8_t out[KM_HID_REPORT_LEN], uint8_t header, uint8_t cmd,
                             uint8_t xor_key, uint32_t offset, bool wide_offset,
                             const uint8_t *data, uint8_t len)
{
    if (out == NULL || (len != 0 && data == NULL)) {
        return KM_HID_ERR_ARG;
    }
    if (header != KM_HID_HDR_NORMAL && header != KM_HID_HDR_RAW) {
        return KM_HID_ERR_ARG;
    }
    if (len > KM_HID_MAX_DATA) {
        return KM_HID_ERR_LEN;
    }
    if (!wide_offset) {
        offset &= 0xFFFFu;
    } else {
        offset &= 0xFFFFFFu;
    }

    memset(out, 0, KM_HID_REPORT_LEN);
    out[0] = header;
    out[1] = cmd;
    out[2] = (header == KM_HID_HDR_NORMAL) ? xor_key : 0u;
    out[3] = km_hid_checksum(len, offset, data);
    out[4] = len;
    out[5] = (uint8_t)offset;
    out[6] = (uint8_t)(offset >> 8);
    out[7] = (uint8_t)(offset >> 16);
    if (len != 0) {
        memcpy(out + KM_HID_DATA_OFFSET, data, len);
    }

    if (header == KM_HID_HDR_NORMAL && xor_key != 0) {
        for (unsigned i = 3; i <= 7u + len; i++) {
            out[i] ^= xor_key;
        }
    }
    return KM_HID_OK;
}

km_hid_status_t km_hid_build_read(uint8_t out[KM_HID_REPORT_LEN], uint8_t cmd, uint16_t offset, uint8_t len)
{
    /* The stock protocol's reads carry the length in the header but no data bytes. The checksum
     * is len + off0 + off1, i.e. the same formula with an empty payload. */
    km_hid_status_t st = km_hid_build(out, KM_HID_HDR_NORMAL, cmd, 0, offset, false, NULL, 0);
    if (st != KM_HID_OK) {
        return st;
    }
    if (len > KM_HID_MAX_DATA) {
        return KM_HID_ERR_LEN;
    }
    out[4] = len;
    out[3] = km_hid_checksum(len, offset, NULL);
    return KM_HID_OK;
}

km_hid_status_t km_hid_build_write(uint8_t out[KM_HID_REPORT_LEN], uint8_t cmd, uint16_t offset,
                                   const uint8_t *data, uint8_t len)
{
    return km_hid_build(out, KM_HID_HDR_NORMAL, cmd, 0, offset, false, data, len);
}

km_hid_status_t km_hid_build_flash_read(uint8_t out[KM_HID_REPORT_LEN], uint32_t flash_offset, uint8_t len)
{
    if (len > KM_HID_MAX_DATA) {
        return KM_HID_ERR_LEN;
    }
    /* The protocol puts no upper bound on cmd 0xE0, but the flash is 512 KB. */
    if (flash_offset >= KM_HID_FLASH_SIZE || (uint32_t)len > KM_HID_FLASH_SIZE - flash_offset) {
        return KM_HID_ERR_ARG;
    }
    km_hid_status_t st = km_hid_build(out, KM_HID_HDR_NORMAL, KM_CMD_FLASH_READ, 0, flash_offset, true, NULL, 0);
    if (st != KM_HID_OK) {
        return st;
    }
    out[4] = len;
    out[3] = km_hid_checksum(len, flash_offset, NULL);
    return KM_HID_OK;
}

km_hid_status_t km_hid_build_enter_boot(uint8_t out[KM_HID_REPORT_LEN])
{
    static const uint8_t payload[1] = { 0x51 };
    return km_hid_build(out, KM_HID_HDR_RAW, KM_CMD_SET_PROFILE, 0, 0, false, payload, 1);
}

km_hid_status_t km_hid_build_response(uint8_t out[KM_HID_REPORT_LEN], uint8_t header, uint8_t cmd, uint32_t offset,
                                      const uint8_t *data, uint8_t len)
{
    if (out == NULL || (len != 0 && data == NULL) || (header != KM_HID_RESP_OK && header != KM_HID_RESP_CSUM)) {
        return KM_HID_ERR_ARG;
    }
    if (len > KM_HID_MAX_DATA) {
        return KM_HID_ERR_LEN;
    }
    offset &= 0xFFFFFFu;
    memset(out, 0, KM_HID_REPORT_LEN);
    out[0] = header;
    out[1] = cmd;
    out[3] = km_hid_checksum(len, offset, data);
    out[4] = len;
    out[5] = (uint8_t)offset;
    out[6] = (uint8_t)(offset >> 8);
    out[7] = (uint8_t)(offset >> 16);
    if (len != 0) {
        memcpy(out + KM_HID_DATA_OFFSET, data, len);
    }
    return KM_HID_OK;
}

km_hid_status_t km_hid_parse(const uint8_t in[KM_HID_REPORT_LEN], km_hid_req_t *req)
{
    if (in == NULL || req == NULL) {
        return KM_HID_ERR_ARG;
    }
    uint8_t buf[KM_HID_REPORT_LEN];
    memcpy(buf, in, KM_HID_REPORT_LEN);

    const uint8_t header = buf[0];
    if (header != KM_HID_HDR_NORMAL && header != KM_HID_HDR_RAW) {
        return KM_HID_ERR_HEADER;
    }

    uint8_t key = 0;
    uint8_t len = buf[4];
    if (header == KM_HID_HDR_NORMAL && buf[2] != 0) {
        key = buf[2];
        len = (uint8_t)(buf[4] ^ key);
    }
    if (len > KM_HID_MAX_DATA) {
        return KM_HID_ERR_LEN; /* '_' packets carry no length limit in the protocol, but a safe parser must not trust one */
    }
    if (key != 0) {
        for (unsigned i = 3; i <= 7u + len; i++) {
            buf[i] ^= key;
        }
    }

    const uint32_t offset = (uint32_t)buf[5] | ((uint32_t)buf[6] << 8) | ((uint32_t)buf[7] << 16);
    if (header == KM_HID_HDR_NORMAL &&
        km_hid_checksum(len, offset, buf + KM_HID_DATA_OFFSET) != buf[3]) {
        return KM_HID_ERR_CHECKSUM;
    }

    req->header  = header;
    req->cmd     = buf[1];
    req->xor_key = key;
    req->len     = len;
    req->offset  = offset;
    memset(req->data, 0, sizeof req->data);
    memcpy(req->data, buf + KM_HID_DATA_OFFSET, len);
    return KM_HID_OK;
}

km_hid_status_t km_hid_check_response(const uint8_t in[KM_HID_REPORT_LEN], const uint8_t **data)
{
    if (in == NULL) {
        return KM_HID_ERR_ARG;
    }
    if (in[0] == KM_HID_RESP_OK) {
        if (data != NULL) {
            *data = in + KM_HID_DATA_OFFSET;
        }
        return KM_HID_OK;
    }
    return (in[0] == KM_HID_RESP_CSUM) ? KM_HID_ERR_CHECKSUM : KM_HID_ERR_RESPONSE;
}

bool km_cmd_is_read_only(uint8_t cmd)
{
    switch (cmd) {
    case KM_CMD_GET_INFO:
    case KM_CMD_GET_BASE:
    case KM_CMD_GET_PROFILE:
    case KM_CMD_GET_DEF_KEYMAP:
    case KM_CMD_GET_KEYMAP:
    case KM_CMD_GET_COLORS:
    case KM_CMD_GET_MACRO:
    case KM_CMD_GET_TRIGGER:
    case KM_CMD_GET_DKS:
    case KM_CMD_GET_MT:
    case KM_CMD_GET_TGL:
    case KM_CMD_GET_CAL:
    case KM_CMD_LIGHT_READ:
    case KM_CMD_FLASH_READ:
    case KM_CMD_GET_APPDATA:
        return true;
    default:
        return false;
    }
}

km_hid_status_t km_boot_build_erase(uint8_t out[KM_HID_REPORT_LEN], uint16_t vid, uint16_t pid)
{
    if (out == NULL) {
        return KM_HID_ERR_ARG;
    }
    memset(out, 0, KM_HID_REPORT_LEN);
    out[0] = KM_BOOT_ERASE;
    out[1] = 7;
    out[2] = (uint8_t)vid;
    out[3] = (uint8_t)(vid >> 8);
    out[4] = (uint8_t)pid;
    out[5] = (uint8_t)(pid >> 8);
    out[6] = 0;
    return KM_HID_OK;
}

km_hid_status_t km_boot_build_chunk(uint8_t out[KM_HID_REPORT_LEN], km_boot_cmd_t cmd,
                                    uint32_t file_offset, const uint8_t *chunk, uint8_t len)
{
    if (out == NULL || (len != 0 && chunk == NULL) || (cmd != KM_BOOT_WRITE && cmd != KM_BOOT_VERIFY)) {
        return KM_HID_ERR_ARG;
    }
    if (len > KM_BOOT_CHUNK_MAX) {
        return KM_HID_ERR_LEN;
    }
    memset(out, 0, KM_HID_REPORT_LEN);
    out[0] = (uint8_t)cmd;
    out[1] = len;
    out[2] = (uint8_t)file_offset;
    out[3] = (uint8_t)(file_offset >> 8);
    out[4] = (uint8_t)(file_offset >> 16);
    out[5] = (uint8_t)(file_offset >> 24);
    if (len != 0) {
        memcpy(out + 6, chunk, len);
    }
    return KM_HID_OK;
}

km_hid_status_t km_boot_build_finish(uint8_t out[KM_HID_REPORT_LEN], km_boot_cmd_t cmd)
{
    if (out == NULL || (cmd != KM_BOOT_END && cmd != KM_BOOT_DONE)) {
        return KM_HID_ERR_ARG;
    }
    memset(out, 0, KM_HID_REPORT_LEN);
    out[0] = (uint8_t)cmd;
    out[1] = 1;
    return KM_HID_OK;
}
