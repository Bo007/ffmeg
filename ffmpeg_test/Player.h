#pragma once
extern "C"
{
#include <libavcodec/avcodec.h>
}
#include <SDL.h>

struct SwrContext;

// compatibility with newer API
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(55,28,1)
#define av_frame_alloc avcodec_alloc_frame
#define av_frame_free avcodec_free_frame
#endif

const int SDL_AUDIO_BUFFER_SIZE = 1024;
const int MAX_AUDIO_FRAME_SIZE = 192000;

const int MAX_AUDIOQ_SIZE = (5 * 16 * 1024);
const int MAX_VIDEOQ_SIZE = (5 * 256 * 1024);

const double AV_SYNC_THRESHOLD = 10.0;
const double AV_NOSYNC_THRESHOLD = 0.01;

const int SAMPLE_CORRECTION_PERCENT_MAX = 10;
const int AUDIO_DIFF_AVG_NB = 20;

const int FF_REFRESH_EVENT = (SDL_USEREVENT);
const int FF_QUIT_EVENT = (SDL_USEREVENT + 1);

const int VIDEO_PICTURE_QUEUE_SIZE = 1;

typedef struct PacketQueue PacketQueue;

typedef struct VideoPicture VideoPicture;

typedef struct VideoState VideoState;

enum
{
	AV_SYNC_AUDIO_MASTER,
	AV_SYNC_VIDEO_MASTER,
	AV_SYNC_EXTERNAL_MASTER,
};

const int DEFAULT_AV_SYNC_TYPE = AV_SYNC_VIDEO_MASTER;

/* Since we only have one decoding thread, the Big Struct
can be global in case we need it. */


SDL_Rect scaleKeepAspectRatio(const int& sourceWidth, const int& sourceHeight,
	const int& distWidth, const int& distHeight);

void packet_queue_init(PacketQueue *q);
int packet_queue_put(PacketQueue *q, AVPacket *pkt);
static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block);
static void packet_queue_flush(PacketQueue *q);

double get_audio_clock(VideoState *is);
double get_video_clock(VideoState *is);
double get_master_clock(VideoState *is);

int synchronize_audio(VideoState *is, short *samples,
	int samples_size, double pts);
int audio_decode_frame(VideoState *is, uint8_t *audio_buf, int buf_size, double *pts_ptr);
void audio_callback(void *userdata, Uint8 *stream, int len);

static Uint32 sdl_refresh_timer_cb(Uint32 interval, void *opaque)
{
	SDL_Event event;
	event.type = FF_REFRESH_EVENT;
	event.user.data1 = opaque;
	SDL_PushEvent(&event);
	return 0; /* 0 means stop timer */
}
static void schedule_refresh(VideoState *is, int delay)
{
	SDL_AddTimer(delay, sdl_refresh_timer_cb, is);
}

void video_display(VideoState *is);
void video_refresh_timer(void *userdata);
void alloc_picture(void *userdata);
int queue_picture(VideoState *is, AVFrame *pFrame);

int eventLoop( char* fileName );

void stream_seek(VideoState *is, int64_t pos, int rel);
int decode_thread(void *arg);
int stream_component_open(VideoState *is, int stream_index);
int video_thread(void *arg);
double synchronize_video(VideoState *is, AVFrame *src_frame, double pts);

bool globalInit(char* fileName);