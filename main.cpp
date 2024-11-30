#include <stdlib.h>
#include <stdbool.h>

#include <vector>
#include <thread>

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
  #include <libavfilter/avfilter.h>
  #include <libavformat/avformat.h>
  #include <libavfilter/buffersrc.h>
  #include <libavfilter/buffersink.h>
  #include <libswresample/swresample.h>
}

#define PCM2WAV_IMPLEMENTATION
#include "wav.h"

#define creturn(ret_) { cleanup(); return ret_; }

static AVFormatContext *format_ctx = NULL;
static AVCodecContext *codec_ctx = NULL;
static AVCodecContext *audio_ctx = NULL;
static SwsContext *sws_ctx = NULL;
static SwrContext *swr_ctx = NULL;
static AVFilterContext *buffersrc_ctx = NULL;
static AVFilterContext *buffersink_ctx = NULL;
static AVFilterGraph *filter_graph = NULL;
static AVDictionary *options = NULL;
static AVFrame *audio_frame = NULL;
static AVFrame *filtered_frame = NULL;
static Texture2D *textures = NULL;
static size_t frames_count = 0;

static void cleanup(void)
{
  for (size_t i = 0; i < frames_count; ++i) {
    UnloadTexture(textures[i]);
  }
  if (audio_frame) av_frame_free(&audio_frame);
  if (filtered_frame) av_frame_free(&filtered_frame);
  if (audio_ctx) avcodec_free_context(&audio_ctx);
  if (codec_ctx) avcodec_free_context(&codec_ctx);
  if (sws_ctx) sws_freeContext(sws_ctx);
  if (swr_ctx) swr_free(&swr_ctx);
  if (buffersrc_ctx) avfilter_free(buffersrc_ctx);
  if (buffersink_ctx) avfilter_free(buffersink_ctx);
  if (filter_graph) avfilter_graph_free(&filter_graph);
  if (format_ctx) avformat_close_input(&format_ctx);
  if (options) av_dict_free(&options);
}

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
    creturn(-1);
  }

  const unsigned int threads_count = std::thread::hardware_concurrency();
  tbb::global_control control(tbb::global_control::max_allowed_parallelism,
                              threads_count / 2);

  const char *file_path = argv[1];

  format_ctx = NULL;
  if (avformat_open_input(&format_ctx, file_path, NULL, NULL) != 0) {
    fprintf(stderr, "could not open video file\n");
    creturn(-1);
  }

  if (avformat_find_stream_info(format_ctx, NULL) < 0) {
    fprintf(stderr, "could not find stream info\n");
    creturn(-1);
  }

  int video_stream_index = -1;
  int audio_stream_index = -1;
  for (size_t i = 0; i < format_ctx->nb_streams; i++) {
    const auto codec_type = format_ctx->streams[i]->codecpar->codec_type;
    if (codec_type == AVMEDIA_TYPE_VIDEO) {
      video_stream_index = (int) i;
    } else if (codec_type == AVMEDIA_TYPE_AUDIO) {
      audio_stream_index = (int) i;
    }
    if (video_stream_index != -1 && audio_stream_index != -1) break;
  }

  if (video_stream_index == -1 && audio_stream_index == -1) {
    fprintf(stderr, "no video or audio stream found\n");
    creturn(-1);
  }

  AVCodecParameters *codec_par = format_ctx->streams[video_stream_index]->codecpar;
  const AVCodec *decoder = avcodec_find_decoder(codec_par->codec_id);
  if (!decoder) {
    fprintf(stderr, "codec not supported\n");
    creturn(-1);
  }

  codec_ctx = avcodec_alloc_context3(decoder);
  avcodec_parameters_to_context(codec_ctx, codec_par);
  avcodec_open2(codec_ctx, decoder, NULL);

  sws_ctx = sws_getContext(codec_ctx->width, codec_ctx->height,
                           codec_ctx->pix_fmt,
                           codec_ctx->width, codec_ctx->height,
                           AV_PIX_FMT_RGB24, SWS_BILINEAR,
                           NULL, NULL, NULL);

  AVCodecParameters *audio_codec_par = format_ctx->streams[audio_stream_index]->codecpar;
  const AVCodec *audio_decoder = avcodec_find_decoder(audio_codec_par->codec_id);
  if (!audio_decoder) {
    fprintf(stderr, "Audio codec not supported\n");
    creturn(-1);
  }

  AVCodecContext *audio_ctx = avcodec_alloc_context3(audio_decoder);
  if (avcodec_parameters_to_context(audio_ctx, audio_codec_par) < 0) {
    fprintf(stderr, "could not copy codec parameters\n");
    creturn(-1);
  }

  if (avcodec_open2(audio_ctx, audio_decoder, NULL) < 0) {
    fprintf(stderr, "could not open audio codec\n");
    creturn(-1);
  }

  filter_graph = avfilter_graph_alloc();
  if (!filter_graph) {
    fprintf(stderr, "could not create filter graph\n");
    creturn(-1);
  }

  const AVFilter *buffersrc = avfilter_get_by_name("abuffer");
  char args[512];
  snprintf(args, sizeof(args),
           "sample_rate=%d:sample_fmt=%s:time_base=1/%d:channel_layout=stereo",
           audio_ctx->sample_rate,
           av_get_sample_fmt_name(audio_ctx->sample_fmt),
           audio_ctx->sample_rate);

  if (avfilter_graph_create_filter(&buffersrc_ctx,
                                   buffersrc,
                                   "in",
                                   args,
                                   NULL,
                                   filter_graph) < 0)
  {
    fprintf(stderr, "could not create audio buffer source\n");
    creturn(-1);
  }

  const AVFilter *buffersink = avfilter_get_by_name("abuffersink");
  if (avfilter_graph_create_filter(&buffersink_ctx,
                                   buffersink,
                                   "out",
                                   NULL,
                                   NULL,
                                   filter_graph) < 0)
  {
    fprintf(stderr, "could not create audio buffer sink\n");
    creturn(-1);
  }

  AVFilterContext *noise_filter_ctx = NULL;
  const AVFilter *noise_filter = avfilter_get_by_name("anlmdn");

  options = NULL;
  av_dict_set(&options, "s", "5", 0);

  if (avfilter_graph_create_filter(&noise_filter_ctx, noise_filter,
                                   "noise_reduction",
                                   NULL,
                                   options,
                                   filter_graph) < 0)
  {
    fprintf(stderr, "could not create noise reduction filter\n");
    creturn(-1);
  }

  if (avfilter_link(buffersrc_ctx, 0, noise_filter_ctx, 0)  < 0 ||
      avfilter_link(noise_filter_ctx, 0, buffersink_ctx, 0) < 0)
  {
    fprintf(stderr, "could not link audio filters\n");
    creturn(-1);
  }

  if (avfilter_graph_config(filter_graph, NULL) < 0) {
    fprintf(stderr, "could not configure filter graph\n");
    creturn(-1);
  }

  swr_ctx = swr_alloc();
  if (!swr_ctx) {
    fprintf(stderr, "could not allocate resample context\n");
    creturn(-1);
  }

  AVChannelLayout in_channel_layout = {};
  AVChannelLayout out_channel_layout = {};
  if (audio_codec_par->ch_layout.nb_channels > 0) {
    av_channel_layout_default(&in_channel_layout, audio_codec_par->ch_layout.nb_channels);
    av_channel_layout_default(&out_channel_layout, audio_codec_par->ch_layout.nb_channels);
  } else {
    av_channel_layout_default(&in_channel_layout, 2);
    av_channel_layout_default(&out_channel_layout, 2);
  }

  av_opt_set_int(swr_ctx, "filter_size", 32, 0);
  av_opt_set_int(swr_ctx, "phase_shift", 16, 0);
  av_opt_set_int(swr_ctx, "linear_interp", 1, 0);
  av_opt_set_chlayout(swr_ctx, "in_chlayout", &in_channel_layout, 0);
  av_opt_set_chlayout(swr_ctx, "out_chlayout", &out_channel_layout, 0);
  av_opt_set_int(swr_ctx, "in_sample_rate", audio_ctx->sample_rate, 0);
  av_opt_set_int(swr_ctx, "out_sample_rate", audio_ctx->sample_rate, 0);
  av_opt_set_sample_fmt(swr_ctx, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);
  av_opt_set_sample_fmt(swr_ctx, "in_sample_fmt", (AVSampleFormat) audio_ctx->sample_fmt, 0);

  if (swr_init(swr_ctx) < 0) {
    fprintf(stderr, "failed to initialize the resampling context\n");

    char layout_str[256];
    av_channel_layout_describe(&in_channel_layout, layout_str, sizeof(layout_str));
    fprintf(stderr, "input channel layout: %s\n", layout_str);
    fprintf(stderr, "number of channels: %d\n", in_channel_layout.nb_channels);

    creturn(-1);
  }

  std::vector<AVFrame *> frames = {};
  std::vector<int16_t> audio_buffer = {};

  audio_frame = av_frame_alloc();
  filtered_frame = av_frame_alloc();

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
          av_packet_unref(&packet);
          creturn(-1);
        }

        if (av_buffersrc_add_frame(buffersrc_ctx, audio_frame) < 0) {
          fprintf(stderr, "error sending frame to filter\n");
          av_packet_unref(&packet);
          creturn(-1);
        }

        while (av_buffersink_get_frame(buffersink_ctx, filtered_frame) >= 0) {
          uint8_t **converted_samples = NULL;
          int max_dst_nb_samples = av_rescale_rnd(filtered_frame->nb_samples,
                                                  audio_ctx->sample_rate,
                                                  audio_ctx->sample_rate,
                                                  AV_ROUND_UP);

          av_samples_alloc_array_and_samples(&converted_samples,
                                             NULL,
                                             out_channel_layout.nb_channels,
                                             max_dst_nb_samples,
                                             AV_SAMPLE_FMT_S16,
                                             0);

          int converted_samples_count = swr_convert(swr_ctx,
                                                    converted_samples,
                                                    max_dst_nb_samples,
                                                    (const uint8_t **) filtered_frame->data,
                                                    filtered_frame->nb_samples);

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

          av_frame_unref(filtered_frame);
        }
      }
    }
    av_packet_unref(&packet);
  }

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
        av_frame_free(&frame);
      }
    });

  SetWindowState(FLAG_WINDOW_RESIZABLE | FLAG_WINDOW_MAXIMIZED);
  InitWindow(800, 600, "vp");
  InitAudioDevice();
  SetTargetFPS(30);

  textures = (Texture2D *) malloc(sizeof(Texture2D) * rgb_frames.size());
  for (size_t i = 0; i < rgb_frames.size(); ++i) {
    textures[i] = TextureFromFrame(rgb_frames[i], codec_ctx->width, codec_ctx->height);
  }

  Data_And_Size *wav = Pcm2Wav((const unsigned char *) audio_buffer.data(),
                               audio_buffer.size() * sizeof(int16_t),
                               out_channel_layout.nb_channels,
                               audio_ctx->sample_rate,
                               16);

  if (wav == NULL) {
    fprintf(stderr, "failed to convert PCM to WAV\n");
    creturn(-1);
  }

  printf("Audio Buffer Size: %zu samples\n", audio_buffer.size());
  printf("Sample Rate: %d\n", audio_ctx->sample_rate);
  printf("Channels: %d\n", out_channel_layout.nb_channels);

  Music music = LoadMusicStreamFromMemory(".wav", wav->data, wav->size);
  PlayMusicStream(music);

  size_t curr_frame = 0;
  frames_count = rgb_frames.size();
  bool frames_exist = frames_count != 0;

  while (!WindowShouldClose()) {
    UpdateMusicStream(music);
    BeginDrawing();
    {
      ClearBackground(BLACK);

      if (frames_exist) {
        DrawTexture(textures[curr_frame], 0, 0, WHITE);
        curr_frame = (curr_frame + 1) % frames_count;
      }
    }
    EndDrawing();
  }

  cleanup();
  CloseWindow();

  return 0;
}
