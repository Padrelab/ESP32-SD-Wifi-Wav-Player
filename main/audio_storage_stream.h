#pragma once

#include <stddef.h>
#include <stdio.h>

typedef size_t (*audio_storage_stream_read_fn_t)(void *context, void *buffer, size_t bytes_to_read);
typedef int (*audio_storage_stream_seek_fn_t)(void *context, long offset, int origin);
typedef int (*audio_storage_stream_error_fn_t)(void *context);
typedef int (*audio_storage_stream_close_fn_t)(void *context);

typedef struct {
    audio_storage_stream_read_fn_t read;
    audio_storage_stream_seek_fn_t seek;
    audio_storage_stream_error_fn_t error;
    audio_storage_stream_close_fn_t close;
} audio_storage_stream_ops_t;

typedef struct {
    void *context;
    audio_storage_stream_ops_t ops;
} audio_storage_stream_t;

void audio_storage_stream_init(audio_storage_stream_t *stream, const audio_storage_stream_ops_t *ops, void *context);
void audio_storage_stream_init_file(audio_storage_stream_t *stream, FILE *file);
size_t audio_storage_stream_read(const audio_storage_stream_t *stream, void *buffer, size_t bytes_to_read);
int audio_storage_stream_seek(const audio_storage_stream_t *stream, long offset, int origin);
int audio_storage_stream_error(const audio_storage_stream_t *stream);
int audio_storage_stream_close(audio_storage_stream_t *stream);
