/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * id.c -- UUIDv7 generation (RFC 9562)
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>

#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif

#include "id.h"

/* Read random bytes from the OS */
static int random_bytes(uint8_t *buf, size_t len)
{
#if defined(__linux__) && defined(SYS_getrandom)
    #include <sys/syscall.h>
    long ret = syscall(SYS_getrandom, buf, len, 0);
    if (ret == (long)len) return 0;
#endif
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, len);
    close(fd);
    return (n == (ssize_t)len) ? 0 : -1;
}

mnemon_err_t mnemon_uuid_generate(mnemon_uuid_t *out)
{
    struct timespec ts;
    uint64_t ms;

    if (!out) return MNEMON_ERR_INVALID_INPUT;

    clock_gettime(CLOCK_REALTIME, &ts);
    ms = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;

    /* Fill with random bytes first */
    if (random_bytes(out->bytes, 16) != 0)
        return MNEMON_ERR_IO;

    /* Bytes 0-5: 48-bit timestamp (big-endian) */
    out->bytes[0] = (uint8_t)(ms >> 40);
    out->bytes[1] = (uint8_t)(ms >> 32);
    out->bytes[2] = (uint8_t)(ms >> 24);
    out->bytes[3] = (uint8_t)(ms >> 16);
    out->bytes[4] = (uint8_t)(ms >> 8);
    out->bytes[5] = (uint8_t)(ms);

    /* Byte 6: version 7 (0111xxxx) */
    out->bytes[6] = (out->bytes[6] & 0x0F) | 0x70;

    /* Byte 8: variant 10 (10xxxxxx) */
    out->bytes[8] = (out->bytes[8] & 0x3F) | 0x80;

    return MNEMON_OK;
}

static const char hex_chars[] = "0123456789abcdef";

mnemon_err_t mnemon_uuid_to_string(const mnemon_uuid_t *id,
                                   char *buf, size_t len)
{
    static const int groups[] = {4, 2, 2, 2, 6};
    int pos = 0, g, bi = 0;

    if (!id || !buf || len < 37)
        return MNEMON_ERR_INVALID_INPUT;

    for (g = 0; g < 5; g++) {
        int i;
        if (g > 0) buf[pos++] = '-';
        for (i = 0; i < groups[g]; i++) {
            buf[pos++] = hex_chars[(id->bytes[bi] >> 4) & 0x0F];
            buf[pos++] = hex_chars[id->bytes[bi] & 0x0F];
            bi++;
        }
    }
    buf[pos] = '\0';
    return MNEMON_OK;
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

mnemon_err_t mnemon_uuid_from_string(const char *str, mnemon_uuid_t *out)
{
    int bi = 0, i;

    if (!str || !out) return MNEMON_ERR_INVALID_INPUT;
    if (strlen(str) != 36) return MNEMON_ERR_INVALID_INPUT;

    /* Validate dash positions: 8-4-4-4-12 */
    if (str[8] != '-' || str[13] != '-' || str[18] != '-' || str[23] != '-')
        return MNEMON_ERR_INVALID_INPUT;

    memset(out, 0, sizeof(*out));
    for (i = 0; i < 36; i++) {
        if (str[i] == '-') continue;
        int hi = hex_val(str[i]);
        int lo = hex_val(str[++i]);
        if (hi < 0 || lo < 0) return MNEMON_ERR_INVALID_INPUT;
        out->bytes[bi++] = (uint8_t)((hi << 4) | lo);
    }

    return MNEMON_OK;
}

int mnemon_uuid_compare(const mnemon_uuid_t *a, const mnemon_uuid_t *b)
{
    if (!a || !b) return 0;
    return memcmp(a->bytes, b->bytes, 16);
}

bool mnemon_uuid_is_zero(const mnemon_uuid_t *id)
{
    static const uint8_t zero[16] = {0};
    if (!id) return true;
    return memcmp(id->bytes, zero, 16) == 0;
}
