#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#define MAX_QUEUE_SIZE (15 * 1024 * 1024)

AVPacket *flush_pkt = NULL;
TTF_Font *global_font = NULL;
TTF_Font *large_font = NULL; // Для стартового экрана

char next_filepath[2048] = {0};
char current_filepath[2048] = {0};
bool app_quit = false;
bool is_fullscreen = false;
double target_start_time = 0.0;
int target_audio_stream = -1;

typedef struct PacketList {
  AVPacket *pkt;
  struct PacketList *next;
} PacketList;
typedef struct PacketQueue {
  PacketList *first_pkt, *last_pkt;
  int size;
  SDL_Mutex *mutex;
  SDL_Condition *cond;
} PacketQueue;

typedef struct VideoState {
  AVFormatContext *ic;
  int video_stream_idx;
  AVCodecContext *video_ctx;
  PacketQueue videoq;
  AVFrame *current_frame;
  SDL_Mutex *frame_mutex;
  bool frame_ready;
  int audio_stream_idx;
  AVCodecContext *audio_ctx;
  PacketQueue audioq;
  struct SwrContext *swr_ctx;
  SDL_AudioStream *audio_stream;
  int audio_streams[16];
  SDL_Texture *audio_track_textures[16];
  int audio_track_w[16];
  int audio_track_h[16];
  int nb_audio_streams;
  double audio_clock;
  int audio_bytes_per_sec;
  int seek_req;
  int64_t seek_pos;
  int seek_flags;
  bool quit;
  bool paused;
  SDL_Thread *parse_thread, *video_thread, *audio_thread;
} VideoState;

VideoState *global_is = NULL;

void file_dialog_callback(void *userdata, const char *const *filelist,
                          int filter) {
  if (filelist && filelist[0]) {
    strncpy(next_filepath, filelist[0], sizeof(next_filepath) - 1);
    target_start_time = 0.0;
    target_audio_stream = -1;
    if (global_is)
      global_is->quit = true;
  }
}

// --- Улучшенная Геометрия UI ---
void draw_triangle(SDL_Renderer *renderer, float x, float y, float size,
                   bool right, SDL_FColor color) {
  SDL_Vertex v[3];
  for (int i = 0; i < 3; i++)
    v[i].color = color;
  if (right) {
    v[0].position = (SDL_FPoint){x - size / 2, y - size / 2};
    v[1].position = (SDL_FPoint){x - size / 2, y + size / 2};
    v[2].position = (SDL_FPoint){x + size / 2, y};
  } else {
    v[0].position = (SDL_FPoint){x + size / 2, y - size / 2};
    v[1].position = (SDL_FPoint){x + size / 2, y + size / 2};
    v[2].position = (SDL_FPoint){x - size / 2, y};
  }
  int indices[3] = {0, 1, 2};
  SDL_RenderGeometry(renderer, NULL, v, 3, indices, 3);
}

void draw_circle(SDL_Renderer *renderer, float cx, float cy, float r,
                 SDL_FColor color) {
  const int segs = 20;
  SDL_Vertex v[21];
  v[0].position = (SDL_FPoint){cx, cy};
  v[0].color = color;
  for (int i = 0; i < segs; i++) {
    float angle = (i / (float)segs) * 2.0f * 3.14159f;
    v[i + 1].position =
        (SDL_FPoint){cx + cosf(angle) * r, cy + sinf(angle) * r};
    v[i + 1].color = color;
  }
  int indices[60];
  for (int i = 0; i < segs; i++) {
    indices[i * 3] = 0;
    indices[i * 3 + 1] = i + 1;
    indices[i * 3 + 2] = (i + 1 == segs) ? 1 : i + 2;
  }
  SDL_RenderGeometry(renderer, NULL, v, segs + 1, indices, segs * 3);
}

void draw_rect_outline(SDL_Renderer *renderer, float x, float y, float w,
                       float h, float thick, SDL_FColor color) {
  SDL_SetRenderDrawColor(renderer, (Uint8)(color.r * 255),
                         (Uint8)(color.g * 255), (Uint8)(color.b * 255),
                         (Uint8)(color.a * 255));
  SDL_FRect r[4] = {{x, y, w, thick},
                    {x, y + h - thick, w, thick},
                    {x, y, thick, h},
                    {x + w - thick, y, thick, h}};
  for (int i = 0; i < 4; i++)
    SDL_RenderFillRect(renderer, &r[i]);
}

void draw_audio_icon(SDL_Renderer *renderer, float x, float y,
                     SDL_FColor color) {
  SDL_SetRenderDrawColor(renderer, (Uint8)(color.r * 255),
                         (Uint8)(color.g * 255), (Uint8)(color.b * 255),
                         (Uint8)(color.a * 255));
  SDL_FRect b[3] = {
      {x - 7, y - 4, 3, 12}, {x - 1, y - 8, 3, 16}, {x + 5, y - 2, 3, 10}};
  for (int i = 0; i < 3; i++)
    SDL_RenderFillRect(renderer, &b[i]);
}

void draw_gradient_panel(SDL_Renderer *renderer, float w, float h, float alpha,
                         bool top) {
  SDL_Vertex v[4];
  for (int i = 0; i < 4; i++)
    v[i].color = (SDL_FColor){0.0f, 0.0f, 0.0f, 0.0f};
  float panel_h = 100.0f;
  if (top) {
    v[0].position = (SDL_FPoint){0, 0};
    v[0].color.a = 0.8f * alpha;
    v[1].position = (SDL_FPoint){w, 0};
    v[1].color.a = 0.8f * alpha;
    v[2].position = (SDL_FPoint){w, panel_h};
    v[3].position = (SDL_FPoint){0, panel_h};
  } else {
    v[0].position = (SDL_FPoint){0, h - panel_h};
    v[1].position = (SDL_FPoint){w, h - panel_h};
    v[2].position = (SDL_FPoint){w, h};
    v[2].color.a = 0.95f * alpha;
    v[3].position = (SDL_FPoint){0, h};
    v[3].color.a = 0.95f * alpha;
  }
  int indices[6] = {0, 1, 2, 0, 2, 3};
  SDL_RenderGeometry(renderer, NULL, v, 4, indices, 6);
}

void draw_folder_icon(SDL_Renderer *renderer, float x, float y, float scale,
                      SDL_FColor color) {
  SDL_SetRenderDrawColor(renderer, (Uint8)(color.r * 255),
                         (Uint8)(color.g * 255), (Uint8)(color.b * 255),
                         (Uint8)(color.a * 255));
  SDL_FRect back = {x - 15 * scale, y - 10 * scale, 30 * scale, 20 * scale};
  SDL_RenderFillRect(renderer, &back);
  SDL_FRect tab = {x - 15 * scale, y - 14 * scale, 12 * scale, 4 * scale};
  SDL_RenderFillRect(renderer, &tab);
  SDL_SetRenderDrawColor(renderer, (Uint8)(color.r * 200),
                         (Uint8)(color.g * 200), (Uint8)(color.b * 200),
                         (Uint8)(color.a * 255));
  SDL_FRect front = {x - 17 * scale, y - 6 * scale, 34 * scale, 16 * scale};
  SDL_RenderFillRect(renderer, &front);
}

// Рендер текста (кэшированный или разовый)
SDL_Texture *render_text(SDL_Renderer *renderer, TTF_Font *font,
                         const char *text, SDL_Color color, int *w, int *h) {
  if (!font)
    return NULL;
  SDL_Surface *surf = TTF_RenderText_Blended(font, text, 0, color);
  if (!surf)
    return NULL;
  SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer, surf);
  if (w)
    *w = surf->w;
  if (h)
    *h = surf->h;
  SDL_DestroySurface(surf);
  return tex;
}

double get_audio_clock(VideoState *is) {
  if (is->audio_bytes_per_sec == 0)
    return 0;
  return is->audio_clock - ((double)SDL_GetAudioStreamQueued(is->audio_stream) /
                            is->audio_bytes_per_sec);
}

void do_seek(VideoState *is, double target_time) {
  if (target_time < 0)
    target_time = 0;
  double duration = is->ic->duration / (double)AV_TIME_BASE;
  if (target_time > duration)
    target_time = duration;
  is->seek_pos = (int64_t)(target_time * AV_TIME_BASE);
  is->seek_flags = target_time < get_audio_clock(is) ? AVSEEK_FLAG_BACKWARD : 0;
  is->seek_req = 1;
}

double get_chapter_time(VideoState *is, int dir) {
  double current_time = get_audio_clock(is);
  if (is->ic->nb_chapters <= 0)
    return current_time + (dir * 60.0);
  int cur_idx = -1;
  for (unsigned int i = 0; i < is->ic->nb_chapters; i++) {
    double start =
        is->ic->chapters[i]->start * av_q2d(is->ic->chapters[i]->time_base);
    double end =
        is->ic->chapters[i]->end * av_q2d(is->ic->chapters[i]->time_base);
    if (current_time >= start && current_time < end) {
      cur_idx = i;
      break;
    }
  }
  if (dir > 0) {
    if (cur_idx >= 0 && cur_idx < is->ic->nb_chapters - 1)
      return is->ic->chapters[cur_idx + 1]->start *
             av_q2d(is->ic->chapters[cur_idx + 1]->time_base);
    return is->ic->duration / (double)AV_TIME_BASE;
  } else {
    if (cur_idx >= 0) {
      double start = is->ic->chapters[cur_idx]->start *
                     av_q2d(is->ic->chapters[cur_idx]->time_base);
      if (current_time - start > 3.0)
        return start;
      if (cur_idx > 0)
        return is->ic->chapters[cur_idx - 1]->start *
               av_q2d(is->ic->chapters[cur_idx - 1]->time_base);
    }
    return 0;
  }
}

void packet_queue_init(PacketQueue *q) {
  memset(q, 0, sizeof(PacketQueue));
  q->mutex = SDL_CreateMutex();
  q->cond = SDL_CreateCondition();
}
void packet_queue_flush(PacketQueue *q) {
  PacketList *pkt, *pkt1;
  SDL_LockMutex(q->mutex);
  for (pkt = q->first_pkt; pkt != NULL; pkt = pkt1) {
    pkt1 = pkt->next;
    av_packet_free(&pkt->pkt);
    av_freep(&pkt);
  }
  q->last_pkt = NULL;
  q->first_pkt = NULL;
  q->size = 0;
  SDL_UnlockMutex(q->mutex);
}
int packet_queue_put(PacketQueue *q, AVPacket *pkt) {
  PacketList *pkt1 = av_malloc(sizeof(PacketList));
  if (!pkt1)
    return -1;
  pkt1->pkt = av_packet_alloc();
  if (pkt == flush_pkt)
    pkt1->pkt->data = flush_pkt->data;
  else
    av_packet_move_ref(pkt1->pkt, pkt);
  pkt1->next = NULL;
  SDL_LockMutex(q->mutex);
  if (!q->last_pkt)
    q->first_pkt = pkt1;
  else
    q->last_pkt->next = pkt1;
  q->last_pkt = pkt1;
  q->size += pkt1->pkt->size;
  SDL_SignalCondition(q->cond);
  SDL_UnlockMutex(q->mutex);
  return 0;
}
int packet_queue_get(PacketQueue *q, AVPacket *pkt, VideoState *is) {
  PacketList *pkt1;
  int ret;
  SDL_LockMutex(q->mutex);
  for (;;) {
    if (is->quit) {
      ret = -1;
      break;
    }
    pkt1 = q->first_pkt;
    if (pkt1) {
      q->first_pkt = pkt1->next;
      if (!q->first_pkt)
        q->last_pkt = NULL;
      q->size -= pkt1->pkt->size;
      av_packet_move_ref(pkt, pkt1->pkt);
      av_packet_free(&pkt1->pkt);
      av_free(pkt1);
      ret = 1;
      break;
    } else
      SDL_WaitCondition(q->cond, q->mutex);
  }
  SDL_UnlockMutex(q->mutex);
  return ret;
}

int audio_thread(void *arg) {
  VideoState *is = (VideoState *)arg;
  AVPacket *pkt = av_packet_alloc();
  AVFrame *frame = av_frame_alloc();
  uint8_t **res = NULL;
  int linesize;
  av_samples_alloc_array_and_samples(&res, &linesize, 2, 192000,
                                     AV_SAMPLE_FMT_FLT, 0);
  AVRational tb = is->ic->streams[is->audio_stream_idx]->time_base;
  while (!is->quit) {
    if (is->paused) {
      SDL_Delay(10);
      continue;
    }
    if (packet_queue_get(&is->audioq, pkt, is) < 0)
      break;
    if (pkt->data == flush_pkt->data) {
      avcodec_flush_buffers(is->audio_ctx);
      SDL_ClearAudioStream(is->audio_stream);
      continue;
    }
    avcodec_send_packet(is->audio_ctx, pkt);
    while (avcodec_receive_frame(is->audio_ctx, frame) == 0) {
      if (frame->pts != AV_NOPTS_VALUE)
        is->audio_clock = frame->pts * av_q2d(tb);
      int out = swr_convert(is->swr_ctx, res, frame->nb_samples,
                            (const uint8_t **)frame->data, frame->nb_samples);
      int size = av_samples_get_buffer_size(NULL, 2, out, AV_SAMPLE_FMT_FLT, 1);
      while (SDL_GetAudioStreamQueued(is->audio_stream) >
                 is->audio_bytes_per_sec &&
             !is->quit)
        SDL_Delay(10);
      SDL_PutAudioStreamData(is->audio_stream, res[0], size);
    }
    av_packet_unref(pkt);
  }
  av_freep(&res[0]);
  av_freep(&res);
  av_frame_free(&frame);
  av_packet_free(&pkt);
  return 0;
}

int video_thread(void *arg) {
  VideoState *is = (VideoState *)arg;
  AVPacket *pkt = av_packet_alloc();
  AVFrame *frame = av_frame_alloc();
  AVRational tb = is->ic->streams[is->video_stream_idx]->time_base;
  while (!is->quit) {
    if (is->paused) {
      SDL_Delay(10);
      continue;
    }
    if (packet_queue_get(&is->videoq, pkt, is) < 0)
      break;
    if (pkt->data == flush_pkt->data) {
      avcodec_flush_buffers(is->video_ctx);
      continue;
    }
    avcodec_send_packet(is->video_ctx, pkt);
    while (avcodec_receive_frame(is->video_ctx, frame) == 0) {
      double pts = (frame->pts != AV_NOPTS_VALUE) ? frame->pts * av_q2d(tb) : 0;
      double delay = pts - get_audio_clock(is);
      if (delay > 0.01 && delay < 3.0)
        SDL_Delay((Uint32)(delay * 1000));
      SDL_LockMutex(is->frame_mutex);
      av_frame_unref(is->current_frame);
      av_frame_ref(is->current_frame, frame);
      is->frame_ready = true;
      SDL_UnlockMutex(is->frame_mutex);
    }
    av_packet_unref(pkt);
  }
  av_frame_free(&frame);
  av_packet_free(&pkt);
  return 0;
}

int parse_thread(void *arg) {
  VideoState *is = (VideoState *)arg;
  AVPacket *pkt = av_packet_alloc();
  while (!is->quit) {
    if (is->paused) {
      SDL_Delay(10);
      continue;
    }
    if (is->seek_req) {
      avformat_seek_file(is->ic, -1, INT64_MIN, is->seek_pos, INT64_MAX,
                         is->seek_flags);
      packet_queue_flush(&is->audioq);
      packet_queue_put(&is->audioq, flush_pkt);
      packet_queue_flush(&is->videoq);
      packet_queue_put(&is->videoq, flush_pkt);
      is->seek_req = 0;
    }
    if (is->videoq.size > MAX_QUEUE_SIZE || is->audioq.size > MAX_QUEUE_SIZE) {
      SDL_Delay(10);
      continue;
    }
    if (av_read_frame(is->ic, pkt) < 0) {
      SDL_Delay(10);
      continue;
    }
    if (pkt->stream_index == is->video_stream_idx)
      packet_queue_put(&is->videoq, pkt);
    else if (pkt->stream_index == is->audio_stream_idx)
      packet_queue_put(&is->audioq, pkt);
    else
      av_packet_unref(pkt);
  }
  av_packet_free(&pkt);
  return 0;
}

int main(int argc, char *argv[]) {
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO))
    return -1;

  if (TTF_Init() < 0) {
    printf("TTF_Init Error: %s\n", SDL_GetError());
  }
  global_font = TTF_OpenFont("font.ttf", 16);
  large_font = TTF_OpenFont("font.ttf", 24);

  flush_pkt = av_packet_alloc();
  flush_pkt->data = (uint8_t *)"FLUSH";
  SDL_Window *window =
      SDL_CreateWindow("C Media Player", 1280, 720, SDL_WINDOW_RESIZABLE);
  SDL_Renderer *renderer = SDL_CreateRenderer(window, NULL);

  // Отрисовка приветственного текста один раз
  SDL_Texture *welcome_tex = NULL;
  int ww = 0, wh = 0;
  if (large_font)
    welcome_tex = render_text(renderer, large_font,
                              "Перетащите видео сюда или кликните для выбора",
                              (SDL_Color){200, 200, 200, 255}, &ww, &wh);

  if (argc > 1)
    strncpy(next_filepath, argv[1], sizeof(next_filepath) - 1);

  while (!app_quit) {
    // --- ЭКРАН ПРИВЕТСТВИЯ ---
    if (next_filepath[0] == '\0') {
      SDL_Event event;
      while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT)
          app_quit = true;
        if (event.type == SDL_EVENT_DROP_FILE) {
          strncpy(next_filepath, event.drop.data, sizeof(next_filepath) - 1);
          target_start_time = 0.0;
          target_audio_stream = -1;
        }
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
            event.button.button == SDL_BUTTON_LEFT) {
          SDL_ShowOpenFileDialog(file_dialog_callback, NULL, window, NULL, 0,
                                 NULL, false);
        }
      }
      int iw, ih;
      SDL_GetWindowSize(window, &iw, &ih);
      SDL_SetRenderDrawColor(renderer, 25, 25, 25, 255);
      SDL_RenderClear(renderer);
      draw_folder_icon(renderer, iw / 2.0f, ih / 2.0f - 40, 2.0f,
                       (SDL_FColor){0.6f, 0.6f, 0.6f, 1.0f});
      if (welcome_tex) {
        SDL_FRect dst = {iw / 2.0f - ww / 2.0f, ih / 2.0f + 20, ww, wh};
        SDL_RenderTexture(renderer, welcome_tex, NULL, &dst);
      }
      SDL_RenderPresent(renderer);
      SDL_Delay(16);
      continue;
    }

    strncpy(current_filepath, next_filepath, sizeof(current_filepath) - 1);
    next_filepath[0] = '\0';

    VideoState *is = av_mallocz(sizeof(VideoState));
    global_is = is;
    if (avformat_open_input(&is->ic, current_filepath, NULL, NULL) < 0) {
      av_free(is);
      continue;
    }
    avformat_find_stream_info(is->ic, NULL);

    is->video_stream_idx = -1;
    is->audio_stream_idx = -1;
    is->nb_audio_streams = 0;

    for (unsigned int i = 0; i < is->ic->nb_streams; i++) {
      if (is->ic->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
          is->video_stream_idx < 0)
        is->video_stream_idx = i;
      if (is->ic->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
        if (is->nb_audio_streams < 16) {
          is->audio_streams[is->nb_audio_streams] = i;
          AVDictionaryEntry *lang =
              av_dict_get(is->ic->streams[i]->metadata, "language", NULL, 0);
          AVDictionaryEntry *title =
              av_dict_get(is->ic->streams[i]->metadata, "title", NULL, 0);
          char track_name[256];
          snprintf(track_name, sizeof(track_name), "[%s] %s",
                   lang ? lang->value : "und",
                   title ? title->value : "Audio Track");
          if (global_font)
            is->audio_track_textures[is->nb_audio_streams] =
                render_text(renderer, global_font, track_name,
                            (SDL_Color){255, 255, 255, 255},
                            &is->audio_track_w[is->nb_audio_streams],
                            &is->audio_track_h[is->nb_audio_streams]);
          is->nb_audio_streams++;
        }
        if (target_audio_stream == i)
          is->audio_stream_idx = i;
        else if (is->audio_stream_idx < 0 && target_audio_stream == -1)
          is->audio_stream_idx = i;
      }
    }

    AVCodecParameters *vpar = is->ic->streams[is->video_stream_idx]->codecpar;
    const AVCodec *vcodec = avcodec_find_decoder(vpar->codec_id);
    is->video_ctx = avcodec_alloc_context3(vcodec);
    avcodec_parameters_to_context(is->video_ctx, vpar);
    avcodec_open2(is->video_ctx, vcodec, NULL);

    AVCodecParameters *apar = is->ic->streams[is->audio_stream_idx]->codecpar;
    const AVCodec *acodec = avcodec_find_decoder(apar->codec_id);
    is->audio_ctx = avcodec_alloc_context3(acodec);
    avcodec_parameters_to_context(is->audio_ctx, apar);
    avcodec_open2(is->audio_ctx, acodec, NULL);

    SDL_AudioSpec aspec = {SDL_AUDIO_F32, 2, is->audio_ctx->sample_rate};
    is->audio_stream = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &aspec, NULL, NULL);
    SDL_ResumeAudioDevice(SDL_GetAudioStreamDevice(is->audio_stream));
    is->audio_bytes_per_sec = aspec.freq * aspec.channels * sizeof(float);

    AVChannelLayout out_ch;
    av_channel_layout_default(&out_ch, 2);
    swr_alloc_set_opts2(&is->swr_ctx, &out_ch, AV_SAMPLE_FMT_FLT, aspec.freq,
                        &is->audio_ctx->ch_layout, is->audio_ctx->sample_fmt,
                        is->audio_ctx->sample_rate, 0, NULL);
    swr_init(is->swr_ctx);

    SDL_Texture *texture = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING,
        is->video_ctx->width, is->video_ctx->height);

    packet_queue_init(&is->videoq);
    packet_queue_init(&is->audioq);
    is->current_frame = av_frame_alloc();
    is->frame_mutex = SDL_CreateMutex();

    if (target_start_time > 0.0) {
      is->seek_pos = (int64_t)(target_start_time * AV_TIME_BASE);
      is->seek_flags = AVSEEK_FLAG_BACKWARD;
      is->seek_req = 1;
      target_start_time = 0.0;
    }

    is->parse_thread = SDL_CreateThread(parse_thread, "Parse", is);
    is->video_thread = SDL_CreateThread(video_thread, "Video", is);
    is->audio_thread = SDL_CreateThread(audio_thread, "Audio", is);

    SDL_Event event;
    double last_mouse_time = (double)av_gettime_relative() / 1000000.0;
    float ui_alpha = 1.0f;
    bool show_audio_menu = false;

    // Переменные для таймера
    int last_rendered_sec = -1;
    SDL_Texture *time_texture = NULL;
    int time_w = 0, time_h = 0;

    // --- ЦИКЛ ВОСПРОИЗВЕДЕНИЯ ---
    while (!is->quit && !app_quit) {
      float win_w, win_h;
      int iw, ih;
      SDL_GetWindowSize(window, &iw, &ih);
      win_w = iw;
      win_h = ih;

      float panel_h = 60.0f;
      float btn_y = win_h - panel_h / 2.0f;
      float cx = win_w / 2.0f;
      float mx, my;
      SDL_GetMouseState(&mx, &my);

      float menu_w = 200, item_h = 32;
      for (int i = 0; i < is->nb_audio_streams; i++) {
        if (is->audio_track_w[i] + 40 > menu_w)
          menu_w = is->audio_track_w[i] + 40;
      }
      float menu_h = is->nb_audio_streams * item_h;
      float menu_x = win_w - 90 - menu_w / 2;
      if (menu_x + menu_w > win_w)
        menu_x = win_w - menu_w - 10;
      float menu_y = win_h - panel_h - menu_h - 10;

      while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT) {
          app_quit = true;
          break;
        }
        if (event.type == SDL_EVENT_MOUSE_MOTION)
          last_mouse_time = (double)av_gettime_relative() / 1000000.0;
        if (event.type == SDL_EVENT_DROP_FILE) {
          strncpy(next_filepath, event.drop.data, sizeof(next_filepath) - 1);
          target_start_time = 0.0;
          target_audio_stream = -1;
          is->quit = true;
        }

        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
            event.button.button == SDL_BUTTON_LEFT) {
          bool ui_clicked = false;

          if (show_audio_menu) {
            if (mx >= menu_x && mx <= menu_x + menu_w && my >= menu_y &&
                my <= menu_y + menu_h) {
              int track_idx = (my - menu_y) / item_h;
              if (track_idx >= 0 && track_idx < is->nb_audio_streams) {
                target_audio_stream = is->audio_streams[track_idx];
                target_start_time = get_audio_clock(is);
                strncpy(next_filepath, current_filepath,
                        sizeof(next_filepath) - 1);
                is->quit = true;
              }
              ui_clicked = true;
            }
            show_audio_menu = false;
          }

          if (!ui_clicked && ui_alpha > 0.1f) {
            if (my < 50 && mx < 60) {
              SDL_ShowOpenFileDialog(file_dialog_callback, NULL, window, NULL,
                                     0, NULL, false);
              ui_clicked = true;
            } else if (my > win_h - panel_h - 10 && my < win_h - panel_h + 10) {
              do_seek(is,
                      (mx / win_w) * (is->ic->duration / (double)AV_TIME_BASE));
              ui_clicked = true;
            } else if (my > win_h - panel_h) {
              if (mx > cx - 20 && mx < cx + 20) {
                is->paused = !is->paused;
                if (is->paused)
                  SDL_PauseAudioDevice(
                      SDL_GetAudioStreamDevice(is->audio_stream));
                else
                  SDL_ResumeAudioDevice(
                      SDL_GetAudioStreamDevice(is->audio_stream));
              } else if (mx > cx - 70 && mx < cx - 30)
                do_seek(is, get_audio_clock(is) - 5.0);
              else if (mx > cx + 30 && mx < cx + 70)
                do_seek(is, get_audio_clock(is) + 5.0);
              else if (mx > cx - 130 && mx < cx - 90)
                do_seek(is, get_chapter_time(is, -1));
              else if (mx > cx + 90 && mx < cx + 130)
                do_seek(is, get_chapter_time(is, 1));
              else if (mx > win_w - 110 && mx < win_w - 70 &&
                       is->nb_audio_streams > 1)
                show_audio_menu = true;
              else if (mx > win_w - 50 && mx < win_w - 10) {
                is_fullscreen = !is_fullscreen;
                SDL_SetWindowFullscreen(window, is_fullscreen);
              }
              ui_clicked = true;
            }
          }

          if (!ui_clicked && event.button.clicks >= 2) {
            is_fullscreen = !is_fullscreen;
            SDL_SetWindowFullscreen(window, is_fullscreen);
          }
        }

        if (event.type == SDL_EVENT_KEY_DOWN) {
          last_mouse_time = (double)av_gettime_relative() / 1000000.0;
          if (event.key.key == SDLK_ESCAPE) {
            if (is_fullscreen) {
              is_fullscreen = false;
              SDL_SetWindowFullscreen(window, false);
            } else {
              app_quit = true;
              is->quit = true;
            }
          }
          if (event.key.key == SDLK_SPACE) {
            is->paused = !is->paused;
            if (is->paused)
              SDL_PauseAudioDevice(SDL_GetAudioStreamDevice(is->audio_stream));
            else
              SDL_ResumeAudioDevice(SDL_GetAudioStreamDevice(is->audio_stream));
          }
          if (event.key.key == SDLK_LEFT)
            do_seek(is, get_audio_clock(is) - 5.0);
          if (event.key.key == SDLK_RIGHT)
            do_seek(is, get_audio_clock(is) + 5.0);
          if (event.key.key == SDLK_DOWN)
            do_seek(is, get_chapter_time(is, -1));
          if (event.key.key == SDLK_UP)
            do_seek(is, get_chapter_time(is, 1));
          if (event.key.key == SDLK_F || event.key.key == SDLK_F11) {
            is_fullscreen = !is_fullscreen;
            SDL_SetWindowFullscreen(window, is_fullscreen);
          }
        }
      }

      double current_sys_time = (double)av_gettime_relative() / 1000000.0;
      if (!is->paused && !show_audio_menu &&
          (current_sys_time - last_mouse_time > 2.0)) {
        ui_alpha -= 0.05f;
        if (ui_alpha < 0.0f) {
          ui_alpha = 0.0f;
          SDL_HideCursor();
        }
      } else {
        ui_alpha += 0.1f;
        if (ui_alpha > 1.0f) {
          ui_alpha = 1.0f;
          SDL_ShowCursor();
        }
      }

      SDL_LockMutex(is->frame_mutex);
      if (is->frame_ready && is->current_frame->data[0]) {
        SDL_UpdateYUVTexture(
            texture, NULL, is->current_frame->data[0],
            is->current_frame->linesize[0], is->current_frame->data[1],
            is->current_frame->linesize[1], is->current_frame->data[2],
            is->current_frame->linesize[2]);
        is->frame_ready = false;
      }
      SDL_UnlockMutex(is->frame_mutex);
      SDL_RenderClear(renderer);
      SDL_RenderTexture(renderer, texture, NULL, NULL);

      // --- РЕНДЕР ИНТЕРФЕЙСА ---
      if (ui_alpha > 0.01f) {
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

        // Верхний градиент и папка
        draw_gradient_panel(renderer, win_w, win_h, ui_alpha, true);
        SDL_FColor c_norm = {0.7f, 0.7f, 0.7f, ui_alpha};
        SDL_FColor c_hover = {1.0f, 1.0f, 1.0f, ui_alpha};
        draw_folder_icon(renderer, 30, 25, 1.0f,
                         (mx < 60 && my < 50) ? c_hover : c_norm);

        // Нижний градиент
        draw_gradient_panel(renderer, win_w, win_h, ui_alpha, false);

        // Обновление и рендер времени
        double dur = is->ic->duration / (double)AV_TIME_BASE;
        float progress = dur > 0 ? (float)(get_audio_clock(is) / dur) : 0;
        if (progress > 1.0f)
          progress = 1.0f;
        int current_sec = (int)get_audio_clock(is);
        if (current_sec != last_rendered_sec && global_font) {
          if (time_texture)
            SDL_DestroyTexture(time_texture);
          int dur_sec = (int)dur;
          char time_str[32];
          snprintf(time_str, sizeof(time_str), "%02d:%02d / %02d:%02d",
                   current_sec / 60, current_sec % 60, dur_sec / 60,
                   dur_sec % 60);
          time_texture =
              render_text(renderer, global_font, time_str,
                          (SDL_Color){220, 220, 220, 255}, &time_w, &time_h);
          last_rendered_sec = current_sec;
        }

        // Отрисовка времени слева
        if (time_texture) {
          SDL_SetTextureAlphaMod(time_texture, (Uint8)(255 * ui_alpha));
          SDL_FRect t_rect = {20, btn_y - time_h / 2.0f, time_w, time_h};
          SDL_RenderTexture(renderer, time_texture, NULL, &t_rect);
        }

        // Прогресс бар (сразу над кнопками)
        bool hover_bar = (!show_audio_menu && my > win_h - panel_h - 10 &&
                          my < win_h - panel_h + 10);
        float bar_h = hover_bar ? 6.0f : 3.0f;
        float bar_y = win_h - panel_h - (bar_h / 2.0f);
        SDL_SetRenderDrawColor(renderer, 100, 100, 100,
                               (Uint8)(100 * ui_alpha));
        SDL_FRect bg_rect = {0, bar_y, win_w, bar_h};
        SDL_RenderFillRect(renderer, &bg_rect);
        SDL_SetRenderDrawColor(renderer, 230, 30, 30, (Uint8)(255 * ui_alpha));
        SDL_FRect prog_rect = {0, bar_y, win_w * progress, bar_h};
        SDL_RenderFillRect(renderer, &prog_rect);
        if (hover_bar)
          draw_circle(renderer, win_w * progress, win_h - panel_h, 7.0f,
                      (SDL_FColor){0.9f, 0.1f, 0.1f, ui_alpha});

        // Кнопки по центру
        SDL_FColor c_prev = (mx > cx - 130 && mx < cx - 90 &&
                             my > win_h - panel_h && !show_audio_menu)
                                ? c_hover
                                : c_norm;
        SDL_FColor c_rw = (mx > cx - 70 && mx < cx - 30 &&
                           my > win_h - panel_h && !show_audio_menu)
                              ? c_hover
                              : c_norm;
        SDL_FColor c_play = (mx > cx - 20 && mx < cx + 20 &&
                             my > win_h - panel_h && !show_audio_menu)
                                ? c_hover
                                : c_norm;
        SDL_FColor c_fw = (mx > cx + 30 && mx < cx + 70 &&
                           my > win_h - panel_h && !show_audio_menu)
                              ? c_hover
                              : c_norm;
        SDL_FColor c_next = (mx > cx + 90 && mx < cx + 130 &&
                             my > win_h - panel_h && !show_audio_menu)
                                ? c_hover
                                : c_norm;

        SDL_SetRenderDrawColor(renderer, (Uint8)(c_prev.r * 255),
                               (Uint8)(c_prev.g * 255), (Uint8)(c_prev.b * 255),
                               (Uint8)(c_prev.a * 255));
        SDL_FRect bar_prev = {cx - 120, btn_y - 8, 4, 16};
        SDL_RenderFillRect(renderer, &bar_prev);
        draw_triangle(renderer, cx - 110, btn_y, 16, false, c_prev);
        draw_triangle(renderer, cx - 100, btn_y, 16, false, c_prev);
        draw_triangle(renderer, cx - 55, btn_y, 20, false, c_rw);
        draw_triangle(renderer, cx - 45, btn_y, 20, false, c_rw);

        SDL_SetRenderDrawColor(renderer, (Uint8)(c_play.r * 255),
                               (Uint8)(c_play.g * 255), (Uint8)(c_play.b * 255),
                               (Uint8)(c_play.a * 255));
        if (is->paused)
          draw_triangle(renderer, cx + 2, btn_y, 22, true, c_play);
        else {
          SDL_FRect bar1 = {cx - 8, btn_y - 10, 5, 20};
          SDL_FRect bar2 = {cx + 3, btn_y - 10, 5, 20};
          SDL_RenderFillRect(renderer, &bar1);
          SDL_RenderFillRect(renderer, &bar2);
        }

        draw_triangle(renderer, cx + 45, btn_y, 20, true, c_fw);
        draw_triangle(renderer, cx + 55, btn_y, 20, true, c_fw);
        SDL_SetRenderDrawColor(renderer, (Uint8)(c_next.r * 255),
                               (Uint8)(c_next.g * 255), (Uint8)(c_next.b * 255),
                               (Uint8)(c_next.a * 255));
        draw_triangle(renderer, cx + 100, btn_y, 16, true, c_next);
        draw_triangle(renderer, cx + 110, btn_y, 16, true, c_next);
        SDL_FRect bar_next = {cx + 116, btn_y - 8, 4, 16};
        SDL_RenderFillRect(renderer, &bar_next);

        // Кнопки справа
        SDL_FColor c_aud = (mx > win_w - 110 && mx < win_w - 70 &&
                            my > win_h - panel_h && !show_audio_menu)
                               ? c_hover
                               : (show_audio_menu ? c_hover : c_norm);
        SDL_FColor c_fs = (mx > win_w - 50 && mx < win_w - 10 &&
                           my > win_h - panel_h && !show_audio_menu)
                              ? c_hover
                              : c_norm;

        if (is->nb_audio_streams > 1)
          draw_audio_icon(renderer, win_w - 90, btn_y, c_aud);
        draw_rect_outline(renderer, win_w - 40, btn_y - 10, 24, 20, 3, c_fs);

        // Меню аудио
        if (show_audio_menu) {
          SDL_SetRenderDrawColor(renderer, 25, 25, 25, 245);
          SDL_FRect bg = {menu_x, menu_y, menu_w, menu_h};
          SDL_RenderFillRect(renderer, &bg);
          draw_rect_outline(renderer, menu_x, menu_y, menu_w, menu_h, 1,
                            (SDL_FColor){0.3f, 0.3f, 0.3f, 1.0f});

          for (int i = 0; i < is->nb_audio_streams; i++) {
            float item_y = menu_y + i * item_h;
            if (mx >= menu_x && mx <= menu_x + menu_w && my >= item_y &&
                my < item_y + item_h) {
              SDL_SetRenderDrawColor(renderer, 60, 60, 60, 255);
              SDL_FRect h_rect = {menu_x, item_y, menu_w, item_h};
              SDL_RenderFillRect(renderer, &h_rect);
            }
            if (is->audio_track_textures[i]) {
              SDL_SetTextureColorMod(
                  is->audio_track_textures[i],
                  is->audio_streams[i] == is->audio_stream_idx ? 250 : 200,
                  is->audio_streams[i] == is->audio_stream_idx ? 50 : 200,
                  is->audio_streams[i] == is->audio_stream_idx ? 50 : 200);
              SDL_FRect dst = {menu_x + 15,
                               item_y + (item_h - is->audio_track_h[i]) / 2,
                               is->audio_track_w[i], is->audio_track_h[i]};
              SDL_RenderTexture(renderer, is->audio_track_textures[i], NULL,
                                &dst);
            }
          }
        }
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
      }
      SDL_RenderPresent(renderer);
      SDL_Delay(5);
    }

    is->quit = true;
    SDL_SignalCondition(is->videoq.cond);
    SDL_SignalCondition(is->audioq.cond);
    SDL_WaitThread(is->parse_thread, NULL);
    SDL_WaitThread(is->video_thread, NULL);
    SDL_WaitThread(is->audio_thread, NULL);

    for (int i = 0; i < is->nb_audio_streams; i++)
      if (is->audio_track_textures[i])
        SDL_DestroyTexture(is->audio_track_textures[i]);
    if (time_texture)
      SDL_DestroyTexture(time_texture);

    swr_free(&is->swr_ctx);
    SDL_DestroyAudioStream(is->audio_stream);
    av_frame_free(&is->current_frame);
    SDL_DestroyMutex(is->frame_mutex);
    SDL_DestroyTexture(texture);
    avcodec_free_context(&is->video_ctx);
    avcodec_free_context(&is->audio_ctx);
    avformat_close_input(&is->ic);
    av_free(is);
    global_is = NULL;
  }

  av_packet_free(&flush_pkt);
  if (global_font)
    TTF_CloseFont(global_font);
  if (large_font)
    TTF_CloseFont(large_font);
  if (welcome_tex)
    SDL_DestroyTexture(welcome_tex);
  TTF_Quit();
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
