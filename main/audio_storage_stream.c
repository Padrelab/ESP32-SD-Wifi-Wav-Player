#include "audio_storage_stream.h"

#include <string.h>

static size_t audio_storage_stream_file_read(void *context, void *buffer, size_t bytes_to_read)
{
    return fread(buffer, 1, bytes_to_read, context);
}

static int audio_storage_stream_file_seek(void *context, long offset, int origin)
{
    return fseek(context, offset, origin);
}

static int audio_storage_stream_file_error(void *context)
{
    return ferror(context);
}

static int audio_storage_stream_file_close(void *context)
{
    return fclose(context);
}

void audio_storage_stream_init(audio_storage_stream_t *stream, const audio_storage_stream_ops_t *ops, void *context)
{
    if (stream == NULL) {
        return;
    }

    memset(stream, 0, sizeof(*stream));
    if (ops != NULL) {
        stream->ops = *ops;
    }
    stream->context = context;
}

void audio_storage_stream_init_file(audio_storage_stream_t *stream, FILE *file)
{
    static const audio_storage_stream_ops_t k_file_ops = {
        .read = audio_storage_stream_file_read,
        .seek = audio_storage_stream_file_seek,
        .error = audio_storage_stream_file_error,
        .close = audio_storage_stream_file_close,
    };

    audio_storage_stream_init(stream, &k_file_ops, file);
}

size_t audio_storage_stream_read(const audio_storage_stream_t *stream, void *buffer, size_t bytes_to_read)
{
    if (stream == NULL || stream->ops.read == NULL || buffer == NULL) {
        return 0;
    }

    return stream->ops.read(stream->context, buffer, bytes_to_read);
}

int audio_storage_stream_seek(const audio_storage_stream_t *stream, long offset, int origin)
{
    if (stream == NULL || stream->ops.seek == NULL) {
        return -1;
    }

    return stream->ops.seek(stream->context, offset, origin);
}

int audio_storage_stream_error(const audio_storage_stream_t *stream)
{
    if (stream == NULL || stream->ops.error == NULL) {
        return 1;
    }

    return stream->ops.error(stream->context);
}

int audio_storage_stream_close(audio_storage_stream_t *stream)
{
    int result = 0;

    if (stream == NULL) {
        return EOF;
    }

    if (stream->ops.close != NULL) {
        result = stream->ops.close(stream->context);
    }

    stream->context = NULL;
    memset(&stream->ops, 0, sizeof(stream->ops));
    return result;
}
