#pragma clang diagnostic ignored "-Wdeprecated-declarations"

#include "selfdrive/loggerd/raw_logger.h"

#include <fcntl.h>
#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>

#define __STDC_CONSTANT_MACROS

#include "libyuv.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
}

#include "common/swaglog.h"
#include "common/util.h"

RawLogger::RawLogger(const char* filename, CameraType type, int in_width, int in_height, int fps,
                     int bitrate, bool h265, int out_width, int out_height, bool write)
  : in_width_(in_width), in_height_(in_height), filename(filename), fps(fps) {

  // TODO: respect write arg

  codec = avcodec_find_encoder(AV_CODEC_ID_FFVHUFF);
  // codec = avcodec_find_encoder(AV_CODEC_ID_FFV1);
  assert(codec);

  codec_ctx = avcodec_alloc_context3(codec);
  assert(codec_ctx);
  codec_ctx->width = out_width;
  codec_ctx->height = out_height;
  codec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;

  // codec_ctx->thread_count = 2;

  // ffv1enc doesn't respect AV_PICTURE_TYPE_I. make every frame a key frame for now.
  // codec_ctx->gop_size = 0;

  codec_ctx->time_base = (AVRational){ 1, fps };

  int err = avcodec_open2(codec_ctx, codec, NULL);
  assert(err >= 0);

  frame = av_frame_alloc();
  assert(frame);
  frame->format = codec_ctx->pix_fmt;
  frame->width = out_width;
  frame->height = out_height;
  frame->linesize[0] = out_width;
  frame->linesize[1] = out_width/2;
  frame->linesize[2] = out_width/2;

  if (in_width != out_width || in_height != out_height) {
    downscale_buf.resize(out_width * out_height * 3 / 2);
  }
}

RawLogger::~RawLogger() {
  av_frame_free(&frame);
  avcodec_close(codec_ctx);
  av_free(codec_ctx);
}

void RawLogger::encoder_open(const char* path) {
  vid_path = util::string_format("%s/%s", path, filename);

  // create camera lock file
  lock_path = util::string_format("%s/%s.lock", path, filename);

  LOG("open %s\n", lock_path.c_str());

  int lock_fd = HANDLE_EINTR(open(lock_path.c_str(), O_RDWR | O_CREAT, 0664));
  assert(lock_fd >= 0);
  close(lock_fd);

  format_ctx = NULL;
  avformat_alloc_output_context2(&format_ctx, NULL, "matroska", vid_path.c_str());
  assert(format_ctx);

  stream = avformat_new_stream(format_ctx, codec);
  // AVStream *stream = avformat_new_stream(format_ctx, NULL);
  assert(stream);
  stream->id = 0;
  stream->time_base = (AVRational){ 1, fps };
  // codec_ctx->time_base = stream->time_base;

  int err = avcodec_parameters_from_context(stream->codecpar, codec_ctx);
  assert(err >= 0);

  err = avio_open(&format_ctx->pb, vid_path.c_str(), AVIO_FLAG_WRITE);
  assert(err >= 0);

  err = avformat_write_header(format_ctx, NULL);
  assert(err >= 0);

  is_open = true;
  counter = 0;
}

void RawLogger::encoder_close() {
  if (!is_open) return;

  int err = av_write_trailer(format_ctx);
  assert(err == 0);

  err = avio_closep(&format_ctx->pb);
  assert(err == 0);

  avformat_free_context(format_ctx);
  format_ctx = NULL;

  unlink(lock_path.c_str());
  is_open = false;
}

int RawLogger::encode_frame(const uint8_t *y_ptr, const uint8_t *u_ptr, const uint8_t *v_ptr,
                            int in_width, int in_height, uint64_t ts) {
  assert(in_width == this->in_width_);
  assert(in_height == this->in_height_);
  AVPacket pkt;
  av_init_packet(&pkt);
  pkt.data = NULL;
  pkt.size = 0;

  if (downscale_buf.size() > 0) {
    uint8_t *out_y = downscale_buf.data();
    uint8_t *out_u = out_y + codec_ctx->width * codec_ctx->height;
    uint8_t *out_v = out_u + (codec_ctx->width / 2) * (codec_ctx->height / 2);
    libyuv::I420Scale(y_ptr, in_width,
                      u_ptr, in_width/2,
                      v_ptr, in_width/2,
                      in_width, in_height,
                      out_y, codec_ctx->width,
                      out_u, codec_ctx->width/2,
                      out_v, codec_ctx->width/2,
                      codec_ctx->width, codec_ctx->height,
                      libyuv::kFilterNone);
    frame->data[0] = out_y;
    frame->data[1] = out_u;
    frame->data[2] = out_v;
  } else {
    frame->data[0] = (uint8_t*)y_ptr;
    frame->data[1] = (uint8_t*)u_ptr;
    frame->data[2] = (uint8_t*)v_ptr;
  }
  frame->pts = counter;

  int ret = counter;

  int err = avcodec_send_frame(codec_ctx, frame);
  if (ret < 0) {
    LOGE("avcode_send_frame error %d", err);
    ret = -1;
  }

  while (ret >= 0){
    err = avcodec_receive_packet(codec_ctx, &pkt);
    if (err == AVERROR_EOF) {
      break;
    } else if (err == AVERROR(EAGAIN)) {
      // Encoder might need a few frames on startup to get started. Keep going
      ret = 0;
      break;
    } else if (err < 0) {
      LOGE("avcodec_receive_packet error %d", err);
      ret = -1;
      break;
    }

    av_packet_rescale_ts(&pkt, codec_ctx->time_base, stream->time_base);
    pkt.stream_index = 0;

    err = av_interleaved_write_frame(format_ctx, &pkt);
    if (err < 0) {
      LOGE("av_interleaved_write_frame %d", err);
      ret = -1;
    } else {
      counter++;
    }
  }

  av_packet_unref(&pkt);
  return ret;
}
