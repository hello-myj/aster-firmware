/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>
#include <stddef.h>
typedef struct {
    /* Read at most size bytes; negative on timeout/error. */
    int (*read)(void *context,uint8_t *data,size_t size);
    void (*reply)(void *context,const char *line);
    void *context;
} aster_transfer_io_t;
/* Caller validates/maps the destination and serializes access. */
int aster_transfer_receive(const char *path,uint32_t length,uint32_t crc,uint32_t offset,const aster_transfer_io_t *io);
uint32_t aster_transfer_crc32(uint32_t crc,const uint8_t *data,size_t size);
int aster_transfer_package_path(const char *remote,char *out,size_t capacity);
int aster_idf_transfer_register(void);
