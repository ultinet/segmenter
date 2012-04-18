/*
 * Copyright (c) 2009 Chase Douglas
 *               2011-2012 Ultinet.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif /* CONFIG_H */

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <getopt.h>
#ifdef HAVE_LIBGEN_H
#include <libgen.h>
#endif /* HAVE_LIBGEN_H */

#include "libavformat/avformat.h"

#ifdef HAVE_AV_MEDIA_TYPE
#define CODEC_TYPE_AUDIO AVMEDIA_TYPE_AUDIO
#define CODEC_TYPE_VIDEO AVMEDIA_TYPE_VIDEO
#endif /* HAVE_AV_MEDIA_TYPE */

#ifdef HAVE_AV_PKT_FLAG_KEY
#define PKT_FLAG_KEY AV_PKT_FLAG_KEY
#endif /* HAVE_AV_PKT_FLAG_KEY */

#ifndef HAVE_BASENAME
static char *basename(char *path)
{
    char *p = path + strlen(path);
    for (;;) {
        if (p == path) {
            break;
        }
        --p;
        if (*p == '/') {
            ++p;
            break;
        }
    }
    return p;
}
#endif

static AVStream *add_output_stream(AVFormatContext *output_format_context, AVStream *input_stream) {
    AVCodecContext *input_codec_context;
    AVCodecContext *output_codec_context;
    AVStream *output_stream;

#ifdef HAVE_AVFORMAT_NEW_STREAM
    output_stream = avformat_new_stream(output_format_context, 0);
#else
    output_stream = av_new_stream(output_format_context, 0);
#endif /* HAVE_AV_NEW_PROGRAM */
    if (!output_stream) {
        av_log(output_format_context, AV_LOG_ERROR, "Could not allocate stream\n");
        return NULL;
    }

    input_codec_context = input_stream->codec;
    output_codec_context = output_stream->codec;

    output_codec_context->codec_id = input_codec_context->codec_id;
    output_codec_context->codec_type = input_codec_context->codec_type;
    output_codec_context->codec_tag = input_codec_context->codec_tag;
    output_codec_context->bit_rate = input_codec_context->bit_rate;

    if (input_codec_context->extradata) {
        output_codec_context->extradata = malloc(input_codec_context->extradata_size);
        if (!output_codec_context->extradata_size) {
            av_log(output_format_context, AV_LOG_ERROR, "Could not allocate memory for extradata\n");
            return NULL;
        }
        memmove(output_codec_context->extradata, input_codec_context->extradata, input_codec_context->extradata_size);
        output_codec_context->extradata_size = input_codec_context->extradata_size;
    }

    if(av_q2d(input_codec_context->time_base) * input_codec_context->ticks_per_frame > av_q2d(input_stream->time_base) && av_q2d(input_stream->time_base) < 1.0/1000) {
        output_codec_context->time_base = input_codec_context->time_base;
        output_codec_context->time_base.num *= input_codec_context->ticks_per_frame;
    }
    else {
        output_codec_context->time_base = input_stream->time_base;
    }

    switch (input_codec_context->codec_type) {
        case CODEC_TYPE_AUDIO:
            output_codec_context->channel_layout = input_codec_context->channel_layout;
            output_codec_context->sample_rate = input_codec_context->sample_rate;
            output_codec_context->channels = input_codec_context->channels;
            output_codec_context->frame_size = input_codec_context->frame_size;
            if ((input_codec_context->block_align == 1 && input_codec_context->codec_id == CODEC_ID_MP3) || input_codec_context->codec_id == CODEC_ID_AC3) {
                output_codec_context->block_align = 0;
            }
            else {
                output_codec_context->block_align = input_codec_context->block_align;
            }
            break;
        case CODEC_TYPE_VIDEO:
            output_codec_context->pix_fmt = input_codec_context->pix_fmt;
            output_codec_context->width = input_codec_context->width;
            output_codec_context->height = input_codec_context->height;
            output_codec_context->has_b_frames = input_codec_context->has_b_frames;

            if (output_format_context->oformat->flags & AVFMT_GLOBALHEADER) {
                output_codec_context->flags |= CODEC_FLAG_GLOBAL_HEADER;
            }
            break;
    default:
        break;
    }

    return output_stream;
}

static AVInputFormat *guess_input_format_from_filename(const char *filename)
{
    AVInputFormat *ifmt = NULL;
    while ((ifmt = av_iformat_next(ifmt))) {
        if (!ifmt->extensions)
            continue;
        if (av_match_ext(filename, ifmt->extensions)) {
            return ifmt;
        }
    }
    return NULL;
}

typedef struct IndexFileWriter {
    const char *index_file;
    char *tmp_file;
    FILE *fp;
    unsigned int segment_duration;
    const char *output_prefix;
    const char *output_ext;
    size_t output_prefix_sz;
    const char *http_prefix;
    unsigned int sequence_num;
    char *current_ts_file;
} IndexFileWriter;

static int index_file_writer_finalize(IndexFileWriter *writer) {
    if (writer->fp) {
        if (fprintf(writer->fp, "#EXT-X-ENDLIST\n") < 0) {
            av_log(NULL, AV_LOG_ERROR, "Could not write last file and endlist tag to m3u8 index file\n");
            return 1;
        }
        fclose(writer->fp);
        writer->fp = NULL;
        rename(writer->tmp_file, writer->index_file);
        if (writer->tmp_file)
            free(writer->tmp_file);
        writer->tmp_file = NULL; 
    }
    return 0;
}

static void index_file_writer_free(IndexFileWriter *writer) {
    if (writer->fp)
        fclose(writer->fp);
    if (writer->tmp_file) {
        remove(writer->tmp_file);
        free(writer->tmp_file);
    }
    if (writer->current_ts_file) {
        free(writer->current_ts_file);
    }
}

static int index_file_writer_init(IndexFileWriter *writer, const char *index_file, unsigned int segment_duration, const char *output_prefix, const char *output_ext, const char *http_prefix, unsigned int first_sequence_num) {
    char *tmp_file;

    {
        const char *dot;
        size_t dot_index;
        size_t index_file_sz = strlen(index_file);

        tmp_file = malloc(index_file_sz + 2);
        if (!tmp_file) {
            av_log(NULL, AV_LOG_ERROR, "Could not allocate space for temporary index filename\n");
            return 1;
        }
        dot = strrchr(index_file, '/');
        dot = dot ? dot + 1: index_file;
        dot_index = dot - index_file;
        memmove(tmp_file, index, dot_index);
        tmp_file[dot_index] = '.';
        memmove(tmp_file + dot_index + 1, index + dot_index, index_file_sz - dot_index);
    }

    writer->index_file = index_file;
    writer->tmp_file = tmp_file;
    writer->fp = NULL;
    writer->segment_duration = segment_duration;
    writer->output_prefix = output_prefix;
    writer->output_prefix_sz = strlen(output_prefix);
    writer->output_ext = output_ext;
    writer->http_prefix = http_prefix;
    writer->sequence_num = first_sequence_num;
    writer->current_ts_file = NULL;
    return 0;
}

static int index_file_writer_populate_current_ts_file(IndexFileWriter *writer)
{
    if (!writer->current_ts_file) {
        char *buf = calloc(writer->output_prefix_sz + 32, sizeof(char));
        if (!buf) {
            av_log(NULL, AV_LOG_ERROR, "Could not allocate space for ts filename\n");
            return 1;
        }
        writer->current_ts_file = buf;
    }
    snprintf(writer->current_ts_file, writer->output_prefix_sz + 32, "%s-%u.%s", writer->output_prefix, writer->sequence_num, writer->output_ext);
    return 0;
}

static int index_file_writer_begin(IndexFileWriter *writer)
{
    writer->fp = fopen(writer->tmp_file, "w");
    if (!writer->fp) {
        av_log(NULL, AV_LOG_ERROR, "Could not open temporary m3u8 index file (%s), no index file will be created\n", writer->tmp_file);
        return 1;
    }
    if (fprintf(writer->fp, "#EXTM3U\n") < 0)
        goto err;

    if (fprintf(writer->fp, "#EXT-X-TARGETDURATION:%u\n", writer->segment_duration) < 0)
        goto err;

    if (writer->sequence_num != 1) {
        if (fprintf(writer->fp, "#EXT-X-MEDIA-SEQUENCE:%u\n", writer->sequence_num) < 0)
            goto err;
    }

    return index_file_writer_populate_current_ts_file(writer);
err:
    av_log(NULL, AV_LOG_ERROR, "Could not write to m3u8 index file, will not continue writing to index file\n");
    return 1;
}

static int index_file_writer_write_index(IndexFileWriter *writer, unsigned int duration)
{
    if (fprintf(writer->fp, "#EXTINF:%u,\n%s%s\n", duration, writer->http_prefix, writer->current_ts_file) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Could not write to m3u8 index file, will not continue writing to index file\n");
        return 1;
    }
    writer->sequence_num++;
    return index_file_writer_populate_current_ts_file(writer);
}

int main(int argc, char **argv)
{
    const char *input;
    const char *input_ext = NULL;
    char *output_prefix = NULL;
    const char *input_format_str = NULL;
    const char *output_format_str = "mpegts";
    char output_ext[1024] = "ts";
    double segment_duration;
    char *segment_duration_check;
    const char *index;
    const char *http_prefix;
    long max_tsfiles = 0;
    char *max_tsfiles_check;
    AVInputFormat *input_format = NULL;
    AVOutputFormat *output_format = NULL;
    AVFormatContext *ic = NULL;
    AVFormatContext *oc = NULL;
    AVStream *video_st = NULL;
    AVStream *audio_st = NULL;
    int video_index, audio_index;
    IndexFileWriter writer;
    double frame_time = 0., video_frame_time = 0., audio_frame_time = 0., last_frame_time = 0.;
    int ret;
    int i;
    int err = 0;
    const char *progname = argv[0];

    {
        int optch;
        while ((optch = getopt(argc, argv, "e:f:p:")) != -1) {
            switch (optch) {
            case 'e':
                /* format */
                input_format_str = optarg;
                break;
            case 'f':
                /* format */
                output_format_str = optarg;
                break;
            case 'p':
                /* prefix */
                if (output_prefix)
                    free(output_prefix);
                output_prefix = strdup(optarg);
                break;
            }
        }
    }
    argc -= optind;
    argv += optind;

    if (argc < 4 || argc > 5) {
        av_log(NULL, AV_LOG_ERROR, "Usage: %s [-e input_format] [-f output_format] [-p output_prefix] <input MPEG-TS / MP3 file> <segment duration in seconds> <output m3u8 index file> <http prefix> [<segment window size>]\n", progname);
        if (output_prefix)
            free(output_prefix);
        return 1;
    }

    av_register_all();

    input = argv[0];
    if (!strcmp(input, "-")) {
        input = "pipe:";
        if (!output_prefix) {
            av_log(NULL, AV_LOG_ERROR, "Please specify output prefix\n");
            err = 1;
            goto out;
        }
    } else {
        const char *input_basename = basename((char *)input);
        const char *p = strchr(input_basename, '.');
        if (!p) {
            if (!output_prefix)
                output_prefix = strdup(input_basename);
        } else {
            input_ext = p + 1;
            if (!output_prefix) {
                size_t len = p - input_basename;
                output_prefix = malloc(len + 1);
                memmove(output_prefix, input_basename, len);
                output_prefix[len] = '\0';
            }
        }
    }

    segment_duration = strtod(argv[1], &segment_duration_check);
    if (segment_duration_check == argv[1] || segment_duration == HUGE_VAL || segment_duration == -HUGE_VAL) {
        av_log(NULL, AV_LOG_ERROR, "Segment duration time (%s) invalid\n", argv[1]);
        return 1;
    }
    index = argv[2];
    http_prefix=argv[3];
    if (argc == 5) {
        max_tsfiles = strtol(argv[4], &max_tsfiles_check, 10);
        if (max_tsfiles_check == argv[4] || max_tsfiles < 0 || max_tsfiles >= INT_MAX) {
            av_log(NULL, AV_LOG_ERROR, "Maximum number of ts files (%s) invalid\n", argv[4]);
            return 1;
        }
    }

    if (index_file_writer_init(&writer, index, segment_duration, output_prefix, output_ext, http_prefix, 1)) {
        return 1;
    }

    if (input_format_str) {
        input_format = av_find_input_format(input_format_str);
        if (!input_format) {
            av_log(NULL, AV_LOG_ERROR, "Specified input file format is not supported.\n");
            err = 1;
            goto out;
        }
    } else {
        input_format = guess_input_format_from_filename(input);
        if (!input_format) {
            av_log(NULL, AV_LOG_INFO, "Could not determine input file format. MPEG-TS assumed\n");
            input_format = av_find_input_format("mpegts");
            if (!input_format) {
                av_log(NULL, AV_LOG_ERROR, "Could not find MPEG-TS demuxer\n");
                err = 1;
                goto out;
            }
        }
    }

#ifdef HAVE_AVFORMAT_OPEN_INPUT
    ret = avformat_open_input(&ic, input, input_format, NULL);
#else
    ret = av_open_input_file(&ic, input, input_format, 0, NULL);
#endif /* HAVE_AVFORMAT_OPEN_INPUT */
    if (ret != 0) {
        char buf[1024];
#ifdef HAVE_AV_STRERROR
        av_strerror(ret, buf, sizeof(buf));
#else
        snprintf(buf, sizeof(buf), "%d", ret);
#endif /* HAVE_AV_STRERROR */

        av_log(NULL, AV_LOG_ERROR, "Could not open input file, make sure it is %s file: %s\n", input_format->long_name, buf);
        err = 1;
        goto out;
    }

#ifdef HAVE_AVFORMAT_FIND_STREAM_INFO
    if (avformat_find_stream_info(ic, NULL) < 0)
#else
    if (av_find_stream_info(ic) < 0)
#endif /* HAVE_AVFORMAT_FIND_STREAM_INFO */
    {
        av_log(NULL, AV_LOG_ERROR, "Could not read stream information\n");
        err = 1;
        goto out;
    }

    output_format = av_guess_format(output_format_str, NULL, NULL);
    if (!output_format) {
        av_log(NULL, AV_LOG_ERROR, "Could not find MPEG-TS muxer\n");
        err = 1;
        goto out;
    }
    if (output_format->extensions) {
        const char *extensions = output_format->extensions, *p;
        const char *end = extensions + strlen(extensions);
        const char *sep;
        for (p = extensions; p; p = sep == end ? NULL: sep + 1) {
            sep = strchr(p, ',');
            if (!sep)
                sep = end;
            assert(sep - p > 0);
            /* Use the same extension as the input file if possible */
            if (input_ext && (strncmp(p, input_ext, sep - p) == 0 || p == extensions)) {
                strncpy(output_ext, p, sep - p);
                output_ext[sep - p] = '\0';
            }
        }
    }

    oc = avformat_alloc_context();
    if (!oc) {
        av_log(NULL, AV_LOG_ERROR, "Could not allocated output context\n");
        err = 1;
        goto out;
    }
    oc->oformat = output_format;

    video_index = -1;
    audio_index = -1;

    for (i = 0; i < ic->nb_streams && (video_index < 0 || audio_index < 0); i++) {
        switch (ic->streams[i]->codec->codec_type) {
            case CODEC_TYPE_VIDEO:
                video_index = i;
                ic->streams[i]->discard = AVDISCARD_NONE;
                if (!(video_st = add_output_stream(oc, ic->streams[i]))) {
                    err = 1;
                    goto out;
                }
                break;
            case CODEC_TYPE_AUDIO:
                audio_index = i;
                ic->streams[i]->discard = AVDISCARD_NONE;
                if (!(audio_st = add_output_stream(oc, ic->streams[i]))) {
                    err = 1;
                    goto out;
                }
                break;
            default:
                ic->streams[i]->discard = AVDISCARD_ALL;
                break;
        }
    }

#if 0
    if (av_set_parameters(oc, NULL) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Invalid output format parameters\n");
        err = 1;
        goto out;
    }
#endif

    av_dump_format(oc, 0, output_prefix, 1);

	if (video_st) {
        AVCodec *codec = avcodec_find_decoder(video_st->codec->codec_id);
        if (!codec) {
            av_log(NULL, AV_LOG_ERROR, "Could not find video decoder, key frames will not be honored\n");
        }

#ifdef HAVE_AVCODEC_OPEN2
        if (avcodec_open2(video_st->codec, codec, NULL) < 0)
#else
        if (avcodec_open(video_st->codec, codec) < 0)
#endif
        {
            av_log(NULL, AV_LOG_ERROR, "Could not open video decoder, key frames will not be honored\n");
        }
    }

    if (audio_st) {
        AVCodec *codec = avcodec_find_decoder(audio_st->codec->codec_id);
        if (!codec) {
            av_log(NULL, AV_LOG_ERROR, "Could not find video decoder, key frames will not be honored\n");
        }

#ifdef HAVE_AVCODEC_OPEN2
        if (avcodec_open2(audio_st->codec, codec, NULL) < 0)
#else
        if (avcodec_open(audio_st->codec, codec) < 0)
#endif
        {
            av_log(NULL, AV_LOG_ERROR, "Could not open video decoder, key frames will not be honored\n");
        }
    }

    if (index_file_writer_begin(&writer)) {
        err = 1;
        goto out;
    }

#ifdef HAVE_AVIO_OPEN
    if (avio_open(&oc->pb, writer.current_ts_file, URL_WRONLY) < 0)
#else
    if (url_fopen(&oc->pb, writer.current_ts_file, URL_WRONLY) < 0)
#endif
    {
        av_log(NULL, AV_LOG_ERROR, "Could not open '%s'\n", writer.current_ts_file);
        err = 1;
        goto out;
    }

#ifdef HAVE_AVFORMAT_WRITE_HEADER
    if (avformat_write_header(oc, NULL))
#else
    if (av_write_header(oc))
#endif
    {
        av_log(NULL, AV_LOG_ERROR, "Could not write mpegts header to first output file\n");
        err = 1;
        goto out;
    }

    {
        AVPacket packet;

        for (;;) {
            ret = av_read_frame(ic, &packet);
            if (ret == AVERROR(EAGAIN))
                continue;

            if (ret == AVERROR_EOF)
                break;
            else if (ret < 0) {
                char buf[1024];
                av_strerror(ret, buf, sizeof(buf));
                av_log(NULL, AV_LOG_WARNING, "Warning: %s (reached EOF?)\n", buf);
                break;
            }

            if (av_dup_packet(&packet) < 0) {
                av_log(NULL, AV_LOG_ERROR, "Could not duplicate packet\n");
                av_free_packet(&packet);
                break;
            }

            if (packet.stream_index == video_index) {
                video_frame_time = (double)packet.pts * video_st->codec->time_base.num / video_st->codec->time_base.den;
                packet.pts = packet.pts * video_st->codec->time_base.num * video_st->time_base.den / video_st->codec->time_base.den * video_st->time_base.num;
                packet.dts = packet.dts * video_st->codec->time_base.num * video_st->time_base.den / video_st->codec->time_base.den * video_st->time_base.num;
            } else if (packet.stream_index == audio_index) {
                audio_frame_time = (double)packet.pts * audio_st->codec->time_base.num / audio_st->codec->time_base.den;
                packet.pts = packet.pts * audio_st->codec->time_base.num * audio_st->time_base.den / audio_st->codec->time_base.den * audio_st->time_base.num;
                packet.dts = packet.dts * audio_st->codec->time_base.num * audio_st->time_base.den / audio_st->codec->time_base.den * audio_st->time_base.num;
            }

            av_log(NULL, AV_LOG_INFO, "video frame time=%f, audio frame time=%f\n", video_frame_time, audio_frame_time);
            if (video_st) {
                if (audio_st) {
                    frame_time = video_frame_time < audio_frame_time ? video_frame_time: audio_frame_time;
                } else {
                    frame_time = video_frame_time;
                }
            } else {
                assert(audio_st != NULL);
                frame_time = audio_frame_time;
            }

            if ((packet.flags & PKT_FLAG_KEY) && frame_time - last_frame_time >= segment_duration) {
                av_log(NULL, AV_LOG_DEBUG, "Flushing\n");
#ifdef HAVE_AVIO_FLUSH
                avio_flush(oc->pb);
#else
                put_flush_packet(oc->pb);
#endif
#ifdef HAVE_AVIO_CLOSE
                avio_close(oc->pb);
#else
                url_fclose(oc->pb);
#endif

                index_file_writer_write_index(&writer, segment_duration);

#ifdef HAVE_AVIO_OPEN
                if (avio_open(&oc->pb, writer.current_ts_file, URL_WRONLY) < 0)
#else
                if (url_fopen(&oc->pb, writer.current_ts_file, URL_WRONLY) < 0)
#endif
                {
                    av_log(NULL, AV_LOG_ERROR, "Could not open '%s'\n", writer.current_ts_file);
                    av_free_packet(&packet);
                    break;
                }

                last_frame_time = frame_time;
            }

            ret = av_interleaved_write_frame(oc, &packet);
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR, "Warning: Could not write frame of stream\n");
            }
            else if (ret > 0) {
                av_log(NULL, AV_LOG_ERROR, "End of stream requested\n");
                av_free_packet(&packet);
                break;
            }

            av_free_packet(&packet);
        }
    }

    av_write_trailer(oc);

    if (ic->duration != AV_NOPTS_VALUE)
        index_file_writer_write_index(&writer, ((double)ic->duration / AV_TIME_BASE) - last_frame_time);
    else
        index_file_writer_write_index(&writer, segment_duration);

    index_file_writer_finalize(&writer);

out:
    if (output_prefix)
        free(output_prefix);

    if (video_st)
        avcodec_close(video_st->codec);

    if (audio_st)
        avcodec_close(audio_st->codec);

    if (ic)
#ifdef HAVE_AVFORMAT_CLOSE_INPUT
        avformat_close_input(&ic);
#else
        av_close_input_file(ic);
#endif

    if (oc) {
        for(i = 0; i < oc->nb_streams; i++) {
            av_freep(&oc->streams[i]->codec);
            av_freep(&oc->streams[i]);
        }

        if (oc->pb) {
#ifdef HAVE_AVIO_CLOSE
            avio_close(oc->pb);
#else
            url_fclose(oc->pb);
#endif
        }
        av_free(oc);
    }

    index_file_writer_free(&writer);

    return 0;
}

// vim:sw=4:ts=4:ai:expandtab
