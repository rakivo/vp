#include <stdlib.h>
#include <stdbool.h>

#include <vector>

#include <raylib.h>

#include <tbb/parallel_for.h>
#include <tbb/blocked_range.h>
#include <tbb/global_control.h>

extern "C" {
  #include <libavutil/opt.h>
  #include <libavutil/avutil.h>
  #include <libavcodec/avcodec.h>
  #include <libswscale/swscale.h>
  #include <libavutil/imgutils.h>
  #include <libavutil/samplefmt.h>
  #include <libavformat/avformat.h>
  #include <libswresample/swresample.h>
}

#define PCM2WAV_IMPLEMENTATION
#include "wav.h"

Texture2D TextureFromFrame(AVFrame *frame, int width, int height)
{
  const Image img = {
    .data = frame->data[0],
    .width = width,
    .height = height,
    .mipmaps = 1,
    .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8
  };

  return LoadTextureFromImage(img);
}

int main(const int argc, const char *argv[])
{
  if (argc < 2) {
    fprintf(stderr, "usage: %s <video.mp4>\n", argv[0]);
    return -1;
  }

  tbb::global_control control(tbb::global_control::max_allowed_parallelism, 4);

  const char *file_path = argv[1];

  AVFormatContext *format_ctx = NULL;
  if (avformat_open_input(&format_ctx, file_path, NULL, NULL) != 0) {
    fprintf(stderr, "could not open video file\n");
    return -1;
  }

  if (avformat_find_stream_info(format_ctx, NULL) < 0) {
    fprintf(stderr, "could not find stream info\n");
    return -1;
  }

  int video_stream_index = -1;
  int audio_stream_index = -1;
  for (unsigned i = 0; i < format_ctx->nb_streams; i++) {
    const auto codec_type = format_ctx->streams[i]->codecpar->codec_type;
    if (codec_type == AVMEDIA_TYPE_VIDEO) {
      video_stream_index = i;
    } else if (codec_type == AVMEDIA_TYPE_AUDIO) {
      audio_stream_index = i;
    }
    if (video_stream_index != -1 && audio_stream_index != -1) break;
  }

  if (video_stream_index == -1 && audio_stream_index == -1) {
    fprintf(stderr, "no video or audio stream found\n");
    return -1;
  }

  AVCodecParameters *codec_par = format_ctx->streams[video_stream_index]->codecpar;
  const AVCodec *decoder = avcodec_find_decoder(codec_par->codec_id);
  if (!decoder) {
    fprintf(stderr, "codec not supported\n");
    return -1;
  }

  AVCodecContext *codec_ctx = avcodec_alloc_context3(decoder);
  avcodec_parameters_to_context(codec_ctx, codec_par);
  avcodec_open2(codec_ctx, decoder, NULL);

  SwsContext *sws_ctx = sws_getContext(codec_ctx->width, codec_ctx->height,
                                       codec_ctx->pix_fmt,
                                       codec_ctx->width, codec_ctx->height,
                                       AV_PIX_FMT_RGB24, SWS_BILINEAR, NULL, NULL, NULL);

  AVCodecParameters *audio_codec_par = format_ctx->streams[audio_stream_index]->codecpar;
  const AVCodec *audio_decoder = avcodec_find_decoder(audio_codec_par->codec_id);
  if (!audio_decoder) {
    fprintf(stderr, "Audio codec not supported\n");
    return -1;
  }

  AVCodecContext *audio_ctx = avcodec_alloc_context3(audio_decoder);
  if (avcodec_parameters_to_context(audio_ctx, audio_codec_par) < 0) {
    fprintf(stderr, "Could not copy codec parameters\n");
    return -1;
  }

  if (avcodec_open2(audio_ctx, audio_decoder, NULL) < 0) {
    fprintf(stderr, "Could not open audio codec\n");
    return -1;
  }

  SwrContext *swr_ctx = swr_alloc();
  if (!swr_ctx) {
    fprintf(stderr, "Could not allocate resample context\n");
    return -1;
  }

  AVChannelLayout in_channel_layout = {};
  AVChannelLayout out_channel_layout = {};
  if (audio_codec_par->ch_layout.nb_channels > 0) {
    av_channel_layout_default(&in_channel_layout,
                              audio_codec_par->ch_layout.nb_channels);

    av_channel_layout_default(&out_channel_layout,
                              audio_codec_par->ch_layout.nb_channels);
  } else {
    av_channel_layout_default(&in_channel_layout, 2);
    av_channel_layout_default(&out_channel_layout, 2);
  }

  av_opt_set_chlayout(swr_ctx, "in_chlayout", &in_channel_layout, 0);
  av_opt_set_chlayout(swr_ctx, "out_chlayout", &out_channel_layout, 0);
  av_opt_set_int(swr_ctx, "in_sample_rate", audio_ctx->sample_rate, 0);
  av_opt_set_int(swr_ctx, "out_sample_rate", audio_ctx->sample_rate, 0);
  av_opt_set_sample_fmt(swr_ctx, "in_sample_fmt", (AVSampleFormat) audio_ctx->sample_fmt, 0);
  av_opt_set_sample_fmt(swr_ctx, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);
  av_opt_set_int(swr_ctx, "filter_size", 32, 0);
  av_opt_set_int(swr_ctx, "phase_shift", 16, 0);
  av_opt_set_int(swr_ctx, "linear_interp", 1, 0);

  if (swr_init(swr_ctx) < 0) {
    fprintf(stderr, "Failed to initialize the resampling context\n");

    char layout_str[256];
    av_channel_layout_describe(&in_channel_layout, layout_str, sizeof(layout_str));
    fprintf(stderr, "Input channel layout: %s\n", layout_str);
    fprintf(stderr, "Number of channels: %d\n", in_channel_layout.nb_channels);

    return -1;
  }

  std::vector<AVFrame *> frames;
  std::vector<int16_t> audio_buffer;

  AVFrame *audio_frame = av_frame_alloc();

  AVPacket packet;
  while (av_read_frame(format_ctx, &packet) >= 0) {
    if (packet.stream_index == video_stream_index) {
      avcodec_send_packet(codec_ctx, &packet);

      while (true) {
        AVFrame *frame = av_frame_alloc();
        if (avcodec_receive_frame(codec_ctx, frame) != 0) {
          av_frame_free(&frame);
          break;
        }
        frames.push_back(frame);
      }
    } else if (packet.stream_index == audio_stream_index) {
      int ret = avcodec_send_packet(audio_ctx, &packet);
      if (ret < 0) {
        fprintf(stderr, "error sending packet for decoding\n");
        break;
      }

      while (ret >= 0) {
        ret = avcodec_receive_frame(audio_ctx, audio_frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) {
          fprintf(stderr, "error during decoding\n");
          return -1;
        }

        uint8_t **converted_samples = NULL;
        int max_dst_nb_samples = av_rescale_rnd(audio_frame->nb_samples,
                                                audio_ctx->sample_rate,
                                                audio_ctx->sample_rate,
                                                AV_ROUND_UP);

        av_samples_alloc_array_and_samples(&converted_samples, NULL,
                                           out_channel_layout.nb_channels,
                                           max_dst_nb_samples, AV_SAMPLE_FMT_S16, 0);

        int converted_samples_count = swr_convert(swr_ctx, converted_samples,
                                                  max_dst_nb_samples,
                                                  (const uint8_t **) audio_frame->data,
                                                  audio_frame->nb_samples);

        if (converted_samples_count > 0) {
          int16_t *pcm_data = (int16_t *) converted_samples[0];
          audio_buffer.insert(audio_buffer.end(), pcm_data,
                              pcm_data
                              + converted_samples_count
                              * out_channel_layout.nb_channels);
        }

        if (converted_samples) {
          av_freep(&converted_samples[0]);
          av_freep(&converted_samples);
        }
      }
    }
    av_packet_unref(&packet);
  }

  std::vector<Texture2D> textures(frames.size());
  std::vector<AVFrame *> rgb_frames(frames.size());

  tbb::parallel_for(
    tbb::blocked_range<size_t>(0, frames.size()),
    [&](const tbb::blocked_range<size_t> &range) {
      for (size_t i = range.begin(); i < range.end(); ++i) {
        AVFrame *frame = frames[i];
        int buf_size = av_image_get_buffer_size(AV_PIX_FMT_RGB24,
                                                 codec_ctx->width,
                                                 codec_ctx->height,
                                                 32);

        uint8_t *buf = (uint8_t *) av_malloc(buf_size);
        AVFrame *rgb_frame = av_frame_alloc();
        av_image_fill_arrays(rgb_frame->data,
                             rgb_frame->linesize,
                             buf,
                             AV_PIX_FMT_RGB24,
                             codec_ctx->width,
                             codec_ctx->height,
                             32);

        sws_scale(sws_ctx,
                  frame->data,
                  frame->linesize,
                  0,
                  codec_ctx->height,
                  rgb_frame->data,
                  rgb_frame->linesize);

        rgb_frames[i] = rgb_frame;
      }
    });

  InitAudioDevice();
  SetWindowState(FLAG_WINDOW_HIDDEN);
  InitWindow(codec_ctx->width, codec_ctx->height, "vp");
  SetTargetFPS(30);

  for (size_t i = 0; i < rgb_frames.size(); ++i) {
    textures[i] = TextureFromFrame(rgb_frames[i], codec_ctx->width, codec_ctx->height);
  }

  tbb::parallel_for(
    tbb::blocked_range<size_t>(0, audio_buffer.size()),
      [&](const tbb::blocked_range<size_t> &range) {
        for (size_t i = range.begin(); i < range.end(); ++i) {
          if (abs(audio_buffer[i]) < 100) audio_buffer[i] = 0;
        }});

  Data_And_Size *wav = Pcm2Wav((const unsigned char *) audio_buffer.data(),
                               audio_buffer.size() * sizeof(int16_t),
                               out_channel_layout.nb_channels,
                               audio_ctx->sample_rate,
                               16);

  if (wav == NULL) {
    fprintf(stderr, "Failed to convert PCM to WAV\n");
    return -1;
  }

  printf("Audio Buffer Size: %zu samples\n", audio_buffer.size());
  printf("Sample Rate: %d\n", audio_ctx->sample_rate);
  printf("Channels: %d\n", out_channel_layout.nb_channels);

  Music music = LoadMusicStreamFromMemory(".wav", wav->data, wav->size);
  PlayMusicStream(music);

  size_t curr_frame = 0;

  ClearWindowState(FLAG_WINDOW_HIDDEN);
  while (!WindowShouldClose()) {
    UpdateMusicStream(music);
    BeginDrawing();
    ClearBackground(BLACK);

    if (!textures.empty()) {
      DrawTexture(textures[curr_frame], 0, 0, WHITE);
      curr_frame = (curr_frame + 1) % textures.size();
    }

    EndDrawing();
  }

  for (auto &texture: textures) {
    UnloadTexture(texture);
  }

  tbb::parallel_for(
    tbb::blocked_range<size_t>(0, rgb_frames.size()),
      [&](const tbb::blocked_range<size_t> &range) {
        for (size_t i = range.begin(); i < range.end(); ++i) {
          av_frame_free(&rgb_frames[i]);
        }});

  CloseWindow();

  swr_free(&swr_ctx);
  av_frame_free(&audio_frame);
  avcodec_free_context(&audio_ctx);
  avformat_close_input(&format_ctx);
  avcodec_free_context(&codec_ctx);
  avformat_close_input(&format_ctx);
  sws_freeContext(sws_ctx);

  return 0;
}
