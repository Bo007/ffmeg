#include "Player.h"

extern "C"
{
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/time.h>
#include <libswresample\swresample.h>
}

#include <thread>
#include <mutex>
#include <condition_variable>

#include <iostream>

SDL_Window		*screen;
SDL_Renderer    *renderer;
std::mutex      *screen_mutex;
VideoState		*global_video_state;
AVPacket		flush_pkt;
SDL_AudioSpec	spec;

struct PacketQueue
{
	AVPacketList *first_pkt, *last_pkt;
	int nb_packets;
	int size;
	std::mutex *mutex;
	std::condition_variable *cond;
};

struct VideoPicture
{
	SDL_Texture *bmp;
	int width, height; /* source height & width */
	int allocated;
	double pts;
};

struct VideoState
{

	AVFormatContext *pFormatCtx;
	int             videoStream, audioStream;

	int             av_sync_type;
	double          external_clock; /* external clock base */
	int64_t         external_clock_time;
	int             seek_req;
	int             seek_flags;
	int64_t         seek_pos;

	double          audio_clock;
	AVStream        *audio_st;
	AVCodecContext  *audio_ctx;
	PacketQueue     audioq;
	uint8_t         audio_buf[(MAX_AUDIO_FRAME_SIZE * 3) / 2];
	unsigned int    audio_buf_size;
	unsigned int    audio_buf_index;
	AVFrame         audio_frame;
	AVPacket        audio_pkt;
	int             audio_pkt_size;
	int             audio_hw_buf_size;
	double          audio_diff_cum; /* used for AV difference average computation */
	double          audio_diff_avg_coef;
	double          audio_diff_threshold;
	int             audio_diff_avg_count;
	double          frame_timer;
	double          frame_last_pts;
	double          frame_last_delay;
	double          video_clock; ///<pts of last decoded frame / predicted pts of next decoded frame
	double          video_current_pts; ///<current displayed pts (different from video_clock if frame fifos are used)
	int64_t         video_current_pts_time;  ///<time (av_gettime) at which we updated video_current_pts - used to have running video pts
	AVStream        *video_st;
	AVCodecContext  *video_ctx;
	PacketQueue     videoq;
	struct SwsContext *sws_ctx;

	VideoPicture    pictq[VIDEO_PICTURE_QUEUE_SIZE];
	int             pictq_size, pictq_rindex, pictq_windex;
	std::mutex* pictq_mutex;
	std::condition_variable* pictq_cond;

	std::thread      *parse_tid;
	std::thread      *video_tid;

	char            filename[1024];
	int             quit;
};

SDL_Rect scaleKeepAspectRatio(const int& sourceWidth, const int& sourceHeight,
	const int& distWidth, const int& distHeight)
{

	double widthAspect = sourceWidth;
	widthAspect /= distWidth;

	double heightAspect = sourceHeight;
	heightAspect /= distHeight;

	double sourceAspectRatio = widthAspect > heightAspect ? widthAspect : heightAspect;

	int xCenterAspect = (distWidth - sourceWidth / sourceAspectRatio) / 2;
	int yCenterAspect = (distHeight - sourceHeight / sourceAspectRatio) / 2;

	return{ xCenterAspect, yCenterAspect,
		static_cast<int>(sourceWidth / sourceAspectRatio),
		static_cast<int>(sourceHeight / sourceAspectRatio) };

}

void packet_queue_init(PacketQueue *q)
{
	memset(q, 0, sizeof(PacketQueue));
	q->mutex = new std::mutex();
	q->cond = new std::condition_variable();
}
int packet_queue_put(PacketQueue *q, AVPacket *pkt)
{
	AVPacketList *pkt1;
	if (pkt != &flush_pkt && av_dup_packet(pkt) < 0) {
		return -1;
	}
	pkt1 = static_cast<AVPacketList*>(av_malloc(sizeof(AVPacketList)));
	if (!pkt1)
		return -1;
	pkt1->pkt = *pkt;
	pkt1->next = NULL;

	q->mutex->lock();
	
	if (!q->last_pkt)
		q->first_pkt = pkt1;
	else
		q->last_pkt->next = pkt1;
	q->last_pkt = pkt1;
	q->nb_packets++;
	q->size += pkt1->pkt.size;
	q->cond->notify_one();

	q->mutex->unlock();
	return 0;
}
static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block)
{
	AVPacketList *pkt1;
	int ret;

	q->mutex->lock();

	for (;;)
	{
		if (global_video_state->quit)
		{
			ret = -1;
			break;
		}

		pkt1 = q->first_pkt;
		if (pkt1) {
			q->first_pkt = pkt1->next;
			if (!q->first_pkt)
				q->last_pkt = NULL;
			q->nb_packets--;
			q->size -= pkt1->pkt.size;
			*pkt = pkt1->pkt;
			av_free(pkt1);
			ret = 1;
			break;
		}
		else if (!block) {
			ret = 0;
			break;
		}
		else
		{
			q->cond->wait(std::unique_lock<std::mutex>(*q->mutex, std::defer_lock));
		}
	}
	q->mutex->unlock();
	return ret;
}
static void packet_queue_flush(PacketQueue *q)
{
	AVPacketList *pkt, *pkt1;

	q->mutex->lock();
	for (pkt = q->first_pkt; pkt != NULL; pkt = pkt1) {
		pkt1 = pkt->next;
		av_free_packet(&pkt->pkt);
		av_freep(&pkt);
	}
	q->last_pkt = NULL;
	q->first_pkt = NULL;
	q->nb_packets = 0;
	q->size = 0;
	q->mutex->unlock();
}

double get_audio_clock(VideoState *is)
{
	double pts = is->audio_clock; /* maintained in the audio thread */
	int hw_buf_size = is->audio_buf_size - is->audio_buf_index;
	int bytes_per_sec = 0;
	int n = spec.channels;
	if (is->audio_st)
	{
		bytes_per_sec = spec.freq; // is->audio_ctx->sample_rate * n;
	}
	if (bytes_per_sec)
		pts -= (double)hw_buf_size / bytes_per_sec;
	return pts;
}
double get_video_clock(VideoState *is)
{
	double delta = (av_gettime() - is->video_current_pts_time) / 1000000.0;
	return is->video_current_pts + delta;
}
double get_master_clock(VideoState *is)
{
	if (is->av_sync_type == AV_SYNC_VIDEO_MASTER)
		return get_video_clock(is);
	else if (is->av_sync_type == AV_SYNC_AUDIO_MASTER)
		return get_audio_clock(is);
	else
		return av_gettime() / 1000000.0;
}


/* Add or subtract samples to get a better sync, return new
audio buffer size */
int synchronize_audio(VideoState *is, short *samples,
	int samples_size, double pts)
{
	if (is->av_sync_type != AV_SYNC_AUDIO_MASTER)
	{

		double diff, avg_diff;
		int wanted_size, min_size, max_size /*, nb_samples */;

		double ref_clock = get_master_clock(is);
		diff = get_audio_clock(is) - ref_clock;
		//std::cout << diff << " - " <<
		//get_master_clock(is) << std::endl;

		if (diff < AV_NOSYNC_THRESHOLD)
		{
			// accumulate the diffs
			is->audio_diff_cum = diff + is->audio_diff_avg_coef
				* is->audio_diff_cum;
			if (is->audio_diff_avg_count < AUDIO_DIFF_AVG_NB)
				is->audio_diff_avg_count++;
			else
			{
				avg_diff = is->audio_diff_cum * (1.0 - is->audio_diff_avg_coef);
				if (fabs(avg_diff) >= is->audio_diff_threshold)
				{
					int n = spec.channels;
					wanted_size = samples_size + ((int)(diff * spec.freq) * n);
					min_size = samples_size * ((100 - SAMPLE_CORRECTION_PERCENT_MAX) / 100);
					max_size = samples_size * ((100 + SAMPLE_CORRECTION_PERCENT_MAX) / 100);
					if (wanted_size < min_size)
						wanted_size = min_size;
					else if (wanted_size > max_size)
						wanted_size = max_size;
					if (wanted_size < samples_size)
					{
						/* remove samples */
						samples_size = wanted_size;
					}
					else if (wanted_size > samples_size)
					{
						uint8_t *samples_end, *q;
						int nb;

						/* add samples by copying final sample*/
						nb = (samples_size - wanted_size);
						samples_end = (uint8_t *)samples + samples_size - n;
						q = samples_end + n;
						while (nb > 0)
						{
							memcpy(q, samples_end, n);
							q += n;
							nb -= n;
						}
						samples_size = wanted_size;
					}
				}
			}
		}
		else
		{
			/* difference is TOO big; reset diff stuff */
			is->audio_diff_avg_count = 0;
			is->audio_diff_cum = 0;
		}
	}
	return samples_size;
}

int audio_decode_frame(VideoState *is, uint8_t *audio_buf, int buf_size, double *pts_ptr)
{
	AVPacket *pkt = &is->audio_pkt;
	for (;;)
	{
		while (is->audio_pkt_size > 0)
		{
			int got_frame = 0;
			int len1 = avcodec_decode_audio4(is->audio_ctx, &is->audio_frame, &got_frame, pkt);
			if (len1 < 0)
			{
				/* if error, skip frame */
				is->audio_pkt_size = 0;
				break;
			}
			int data_size = 0;
			if (got_frame)
			{
				data_size = av_samples_get_buffer_size(
					NULL,
					is->audio_ctx->channels,
					is->audio_frame.nb_samples,
					is->audio_ctx->sample_fmt,
					1);

				static SDL_AudioCVT cvt;
				static int err = SDL_BuildAudioCVT(&cvt,
					AUDIO_F32, is->audio_ctx->channels, is->audio_ctx->sample_rate,
					spec.format, spec.channels * 2, spec.freq);

				if (data_size * cvt.len_mult > buf_size )
					exit(1);

				SDL_assert(cvt.needed); // obviously, this one is always needed.
				cvt.len = data_size;  // 1024 stereo float32 sample frames.
				cvt.buf = audio_buf;
				memcpy(audio_buf, is->audio_frame.data[0], data_size);
				
				SDL_ConvertAudio(&cvt);
				data_size = spec.size;
			}
			is->audio_pkt_size -= len1;

			if (data_size <= 0)/* No data yet, get more frames */
				continue;

			double pts = is->audio_clock;
			*pts_ptr = pts;
			int n = spec.channels;
			is->audio_clock += (double)data_size /
				(double)(n * spec.freq);
			is->audio_clock /= spec.freq;
			is->audio_clock *= is->audio_frame.sample_rate;
			/* We have data, return it and come back for more later */
			return data_size;
		}
		if (pkt->data)
			av_free_packet(pkt);

		if (is->quit)
			return -1;
		
		/* next packet */
		if (packet_queue_get(&is->audioq, pkt, 1) < 0)
			return -1;
		
		if (pkt->data == flush_pkt.data)
		{
			avcodec_flush_buffers(is->audio_ctx);
			continue;
		}
		is->audio_pkt_size = pkt->size;
		/* if update, update the audio clock w/pts */
		if (pkt->pts != AV_NOPTS_VALUE)
			is->audio_clock = av_q2d(is->audio_st->time_base)*pkt->pts;
	}
}

void audio_callback(void *userdata, Uint8 *stream, int len)
{
	VideoState *is = (VideoState *)userdata;
	while (len > 0)
	{
		if (is->audio_buf_index >= is->audio_buf_size)
		{
			/* We have already sent all our data; get more */
			double pts;
			int audio_size = audio_decode_frame(is, is->audio_buf, sizeof(is->audio_buf), &pts);
			if (audio_size < 0)
			{
				/* If error, output silence */
				is->audio_buf_size = spec.samples;
				memset(is->audio_buf, 0, is->audio_buf_size);
			}
			else
			{
				audio_size = synchronize_audio(is, (short *)is->audio_buf,
					audio_size, pts);
				is->audio_buf_size = audio_size;
			}
			is->audio_buf_index = 0;
		}
		int len1 = is->audio_buf_size - is->audio_buf_index;
		if (len1 > len)
			len1 = len;
 
		auto inBuff = is->audio_buf + is->audio_buf_index;

		//static SDL_AudioCVT cvt;
		//static int err = SDL_BuildAudioCVT(&cvt,
		//	AUDIO_F32, is->audio_ctx->channels, is->audio_ctx->sample_rate,
		//	spec.format, spec.channels * 2, spec.freq );
		//if (err < 0)
		//	std::cout << "SDL_BuildAudioCVT error" << std::endl;
		//SDL_assert(cvt.needed); // obviously, this one is always needed.
		//cvt.len = len1;  // 1024 stereo float32 sample frames.
		//cvt.buf = inBuff;

		//err = SDL_ConvertAudio(&cvt);
		//if (err < 0)
		//	std::cout << "SDL_ConvertAudio error" << std::endl;

		//len1 = spec.size;

		memcpy(stream, inBuff, len1);
		len -= len1;
		stream += len1;
		is->audio_buf_index += len1;
	}
}

void video_display(VideoState *is)
{
	VideoPicture *vp = &is->pictq[is->pictq_rindex];
	if (vp->bmp)
	{
		screen_mutex->lock();

		SDL_Rect distRect = { 0,0,0,0 };
		SDL_GetWindowSize(screen, &distRect.w, &distRect.h);
		distRect = scaleKeepAspectRatio(vp->width, vp->height, distRect.w, distRect.h);

		SDL_RenderClear(renderer);
		SDL_RenderCopy(renderer, vp->bmp, NULL, &distRect);
		SDL_RenderPresent(renderer);
		screen_mutex->unlock();
	}
}

void video_refresh_timer(void *userdata)
{
	VideoState *is = (VideoState *)userdata;
	VideoPicture *vp;
	double actual_delay, delay, sync_threshold, ref_clock, diff;

	if (is->video_st)
	{
		if (is->pictq_size == 0)
			schedule_refresh(is, 1000);
		else
		{
			vp = &is->pictq[is->pictq_rindex];

			is->video_current_pts = vp->pts;
			is->video_current_pts_time = av_gettime();
			delay = vp->pts - is->frame_last_pts; /* the pts from last time */
			if (delay <= 0 || delay >= 1.0) {
				/* if incorrect delay, use previous one */
				delay = is->frame_last_delay;
			}
			/* save for next time */
			is->frame_last_delay = delay;
			is->frame_last_pts = vp->pts;

			/* update delay to sync to audio if not master source */
			if (is->av_sync_type != AV_SYNC_VIDEO_MASTER) {
				ref_clock = get_master_clock(is);
				diff = vp->pts - ref_clock;

				/* Skip or repeat the frame. Take delay into account
				FFPlay still doesn't "know if this is the best guess." */
				sync_threshold = (delay > AV_SYNC_THRESHOLD) ? delay : AV_SYNC_THRESHOLD;
				if (fabs(diff) < AV_NOSYNC_THRESHOLD)
				{
					if (diff <= -sync_threshold) {
						delay = 0;
					}
					else if (diff >= sync_threshold) {
						delay *= 2;
					}
				}
			}
			is->frame_timer += delay;
			/* computer the REAL delay */
			actual_delay = is->frame_timer - (av_gettime() / 1000000.0);
			if (actual_delay < 0.010) {
				/* Really it should skip the picture instead */
				actual_delay = 0.010;
			}
			schedule_refresh(is, (int)(actual_delay * 1000 + 0.5));

			/* show the picture! */
			video_display(is);

			/* update queue for next picture! */
			if (++is->pictq_rindex == VIDEO_PICTURE_QUEUE_SIZE) {
				is->pictq_rindex = 0;
			}
			is->pictq_mutex->lock();
			is->pictq_size--;
			is->pictq_cond->notify_one();
			is->pictq_mutex->unlock();
		}
	}
	else
		schedule_refresh(is, 100);
}

void alloc_picture(void *userdata)
{
	VideoState *is = (VideoState *)userdata;
	VideoPicture *vp;

	vp = &is->pictq[is->pictq_windex];
	if (vp->bmp)
		SDL_DestroyTexture(vp->bmp );

	// Allocate a place to put our YUV image on that screen
	screen_mutex->lock();
	vp->bmp = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_YV12, SDL_TEXTUREACCESS_STREAMING,
		is->video_ctx->width,
		is->video_ctx->height);
	screen_mutex->unlock();

	vp->width = is->video_ctx->width;
	vp->height = is->video_ctx->height;
	vp->allocated = 1;
}

int queue_picture(VideoState *is, AVFrame *pFrame, double pts)
{
	/* wait until we have space for a new pic */
	is->pictq_mutex->lock();
	while (is->pictq_size >= VIDEO_PICTURE_QUEUE_SIZE &&
		!is->quit)
		is->pictq_cond->wait(std::unique_lock<std::mutex>(*is->pictq_mutex, std::defer_lock));
	is->pictq_mutex->unlock();

	if (is->quit)
		return -1;

	// windex is set to 0 initially
	VideoPicture *vp = &is->pictq[is->pictq_windex];

	/* allocate or resize the buffer! */
	if (!vp->bmp ||
		vp->width != is->video_ctx->width ||
		vp->height != is->video_ctx->height) {
		SDL_Event event;

		vp->allocated = 0;
		alloc_picture(is);
		if (is->quit) {
			return -1;
		}
	}
	/* We have a place to put our picture on the queue */

	is->pictq_mutex->lock();
	if (vp->bmp)
	{
		SDL_Rect rect = { 0, 0, pFrame->width, pFrame->height };
		int lsize = sizeof(pFrame->linesize);
		SDL_UpdateYUVTexture(vp->bmp, &rect, pFrame->data[0], pFrame->linesize[0],
			pFrame->data[1], pFrame->linesize[1],
			pFrame->data[2], pFrame->linesize[2]);
		/* now we inform our display thread that we have a pic ready */
		if (++is->pictq_windex == VIDEO_PICTURE_QUEUE_SIZE)
			is->pictq_windex = 0;

		is->pictq_size++;
	}
	is->pictq_mutex->unlock();
	return 0;
}

double synchronize_video(VideoState *is, AVFrame *src_frame, double pts)
{
	double frame_delay;

	if (pts != 0)
	{
		/* if we have pts, set video clock to it */
		is->video_clock = pts;
	}
	else
	{
		/* if we aren't given a pts, set it to the clock */
		pts = is->video_clock;
	}

	/* update the video clock */
	frame_delay = av_q2d(is->video_ctx->time_base);
	/* if we are repeating a frame, adjust clock accordingly */
	frame_delay += src_frame->repeat_pict * (frame_delay * 0.5);
	is->video_clock += frame_delay;

	return pts;
}

int video_thread(void *arg)
{
	VideoState *is = (VideoState *)arg;
	AVPacket pkt1, *packet = &pkt1;
	
	AVFrame *pFrame = av_frame_alloc();

	for (;;) {
		if (packet_queue_get(&is->videoq, packet, 1) < 0)
		{
			// means we quit getting packets
			break;
		}
		double pts = 0;


		// Decode video frame
		int frameFinished;
		avcodec_decode_video2(is->video_ctx, pFrame, &frameFinished, packet);

		if ((pts = av_frame_get_best_effort_timestamp(pFrame)) == AV_NOPTS_VALUE) {
			pts = av_frame_get_best_effort_timestamp(pFrame);
		}
		else {
			pts = 0;
		}
		pts *= av_q2d(is->video_st->time_base);

		// Did we get a video frame?
		if (frameFinished)
		{
			pts = synchronize_video(is, pFrame, pts);
			if (queue_picture(is, pFrame, pts) < 0)
			{
				break;
			}
		}
		av_free_packet(packet);
	}
	av_frame_free(&pFrame);
	return 0;
}

int stream_component_open(VideoState *is, int stream_index)
{
	AVFormatContext *pFormatCtx = is->pFormatCtx;
	AVCodecContext *codecCtx = NULL;
	AVCodec *codec = NULL;
	SDL_AudioSpec wanted_spec;

	if (stream_index < 0 || stream_index >= pFormatCtx->nb_streams) {
		return -1;
	}

	codec = avcodec_find_decoder(pFormatCtx->streams[stream_index]->codec->codec_id);
	if (!codec)
	{
		std::cout << "Unsupported codec!" << std::endl;
		return -1;
	}

	codecCtx = avcodec_alloc_context3(codec);
	if (avcodec_copy_context(codecCtx, pFormatCtx->streams[stream_index]->codec) != 0)
	{
		std::cout << "Couldn't copy codec context" << std::endl;
		return -1; // Error copying codec context
	}

	if (codecCtx->codec_type == AVMEDIA_TYPE_AUDIO)
	{
		// Set audio settings from codec info
		wanted_spec.freq = codecCtx->sample_rate;
		wanted_spec.format = AUDIO_S16SYS;
		wanted_spec.channels = codecCtx->channels;
		wanted_spec.silence = 0;
		wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;

		wanted_spec.callback = audio_callback;
		wanted_spec.userdata = is;

		if (SDL_OpenAudio(&wanted_spec, &spec) < 0)
		{
			std::cout << "SDL_OpenAudio: " << SDL_GetError() << std::endl;
			return -1;
		}

		is->audio_hw_buf_size = spec.size;
	}
	if (avcodec_open2(codecCtx, codec, NULL) < 0)
	{
		std::cout << "Unsupported codec!" << std::endl;
		return -1;
	}

	switch (codecCtx->codec_type)
	{
	case AVMEDIA_TYPE_AUDIO:
		is->audioStream = stream_index;
		is->audio_st = pFormatCtx->streams[stream_index];
		is->audio_ctx = codecCtx;
		is->audio_buf_size = 0;
		is->audio_buf_index = 0;
		memset(&is->audio_pkt, 0, sizeof(is->audio_pkt));
		packet_queue_init(&is->audioq);
		SDL_PauseAudio(0);
		break;
	case AVMEDIA_TYPE_VIDEO:
		is->videoStream = stream_index;
		is->video_st = pFormatCtx->streams[stream_index];
		is->video_ctx = codecCtx;

		is->frame_timer = (double)av_gettime() / 1000000.0;
		is->frame_last_delay = 40e-3;
		is->video_current_pts_time = av_gettime();

		packet_queue_init(&is->videoq);
		is->video_tid = new std::thread(video_thread, is);
		is->sws_ctx = sws_getContext(is->video_ctx->width, is->video_ctx->height,
			is->video_ctx->pix_fmt, is->video_ctx->width,
			is->video_ctx->height, AV_PIX_FMT_YUV420P,
			SWS_BILINEAR, NULL, NULL, NULL
		);
		break;
	default:
		break;
	}
	return 0;
}

int decode_thread(void *arg)
{
	VideoState *is = static_cast<VideoState *>(arg);
	
	is->videoStream = -1;
	is->audioStream = -1;

	global_video_state = is;

	// Open video file
	AVFormatContext *pFormatCtx = NULL;
	if (avformat_open_input(&pFormatCtx, is->filename, NULL, NULL) != 0)
	{
		std::cout << "cannot open " << is->filename << std::endl;
		return -1; // Couldn't open file
	}

	is->pFormatCtx = pFormatCtx;

	// Retrieve stream information
	if (avformat_find_stream_info(pFormatCtx, NULL)<0)
		return -1; // Couldn't find stream information

				   // Dump information about file onto standard error
	av_dump_format(pFormatCtx, 0, is->filename, 0);

	// Find the first video stream
	int video_index = -1;
	int audio_index = -1;
	for (int i = 0; i<pFormatCtx->nb_streams; i++)
	{
		if (pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO &&
			video_index < 0)
			video_index = i;
		if (pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO &&
			audio_index < 0)
			audio_index = i;
	}
	if (audio_index >= 0)
		stream_component_open(is, audio_index);
	if (video_index >= 0)
		stream_component_open(is, video_index);

	if (is->videoStream < 0 || is->audioStream < 0)
	{
		std::cout << is->filename << ": could not open codecs" << std::endl;
		goto fail;
	}

	// main decode loop
	for (;;)
	{
		AVPacket packet;
		if (is->quit)
			break;
		// seek stuff goes here
		if (is->seek_req)
		{
			int stream_index = -1;
			int64_t seek_target = is->seek_pos;

			if (is->videoStream >= 0) stream_index = is->videoStream;
			else if (is->audioStream >= 0) stream_index = is->audioStream;

			if (stream_index >= 0)
				seek_target = av_rescale_q(seek_target, { 1, AV_TIME_BASE },
					pFormatCtx->streams[stream_index]->time_base);
			
			if (av_seek_frame(is->pFormatCtx, stream_index,
				seek_target, is->seek_flags) < 0)
				std::cout << is->pFormatCtx->filename << ": error while seeking" << std::endl;
			else
			{
				if (is->audioStream >= 0)
				{
					packet_queue_flush(&is->audioq);
					packet_queue_put(&is->audioq, &flush_pkt);
				}
				if (is->videoStream >= 0)
				{
					packet_queue_flush(&is->videoq);
					packet_queue_put(&is->videoq, &flush_pkt);
				}
			}
			is->seek_req = 0;
		}

		if (is->audioq.size > MAX_AUDIOQ_SIZE ||
			is->videoq.size > MAX_VIDEOQ_SIZE)
		{
			SDL_Delay(10);
			continue;
		}
		if (av_read_frame(is->pFormatCtx, &packet) < 0)
		{
			if (is->pFormatCtx->pb->error == 0)
			{
				SDL_Delay(100); /* no error; wait for user input */
				continue;
			}
			else break;
		}
		// Is this a packet from the video stream?
		if (packet.stream_index == is->videoStream)
			packet_queue_put(&is->videoq, &packet);
		else if (packet.stream_index == is->audioStream)
			packet_queue_put(&is->audioq, &packet);
		else
			av_free_packet(&packet);
	}
	/* all done - wait for it */
	while (!is->quit)
		SDL_Delay(100);

fail:
	SDL_Event event;
	event.type = FF_QUIT_EVENT;
	event.user.data1 = is;
	SDL_PushEvent(&event);
	return 0;
}

void stream_seek(VideoState *is, int64_t pos, int rel)
{
	if (!is->seek_req)
	{
		is->seek_pos += pos;
		is->seek_flags = rel < 0 ? AVSEEK_FLAG_BACKWARD : 0;
		is->seek_req = 1;
	}
}

int eventLoop( char* fileName )
{
	VideoState* is = static_cast<VideoState*>(av_mallocz(sizeof(VideoState)));
	av_register_all();

	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER))
	{
		std::cout << "Could not initialize SDL - " << SDL_GetError() << std::endl;
		exit(1);
	}

	// Make a screen to put our video
	screen = SDL_CreateWindow("My Game Window",
		SDL_WINDOWPOS_UNDEFINED,
		SDL_WINDOWPOS_UNDEFINED,
		640, 480,
		SDL_WINDOW_RESIZABLE);
	renderer = SDL_CreateRenderer(screen, -1, 0);
	if (!screen)
	{
		std::cout << "Could not set video mode - exiting" << SDL_GetError() << std::endl;
		exit(1);
	}

	screen_mutex = new std::mutex();

	strcpy_s(is->filename, fileName);

	is->pictq_mutex = new std::mutex();
	is->pictq_cond = new std::condition_variable();

	schedule_refresh(is, 40);

	is->av_sync_type = DEFAULT_AV_SYNC_TYPE;
	is->parse_tid = new std::thread(decode_thread, is);
	if (!is->parse_tid)
	{
		av_free(is);
		return -1;
	}

	av_init_packet(&flush_pkt);
	flush_pkt.data = reinterpret_cast<uint8_t*>("FLUSH");

	auto startTime = av_gettime();
	int ret = 1;
	while (ret == 1)
	{
		SDL_Event event;
		double incr, pos;
		SDL_WaitEvent(&event);
		switch (event.type)
		{
		case SDL_KEYDOWN:
			switch (event.key.keysym.sym)
			{
			case SDLK_LEFT:
				incr = -10.0;
				goto do_seek;
			case SDLK_RIGHT:
				incr = 10.0;
				goto do_seek;
			case SDLK_UP:
				incr = 60.0;
				goto do_seek;
			case SDLK_DOWN:
				incr = -60.0;
				goto do_seek;
			do_seek:
				if (global_video_state)
				{
					pos = get_master_clock(global_video_state);
					pos += incr;
					stream_seek(global_video_state, (int64_t)(pos * AV_TIME_BASE), incr);
				}
				break;
			case SDLK_ESCAPE:
				ret = 0;
				break;
			case SDLK_SPACE:
				static int64_t flag;
				if (flag)
				{
					is->video_current_pts_time = av_gettime();

					global_video_state->audioq.mutex->unlock();
					global_video_state->videoq.mutex->unlock();

					flag = 0;
				}
				else
				{
 					flag = 1;

					global_video_state->audioq.mutex->lock();
					global_video_state->videoq.mutex->lock();

					std::cout << "STOP TIME" << (av_gettime() - startTime ) / 1000000.0 << std::endl;
				}
				//std::cout << "audio_clock - " << get_audio_clock(global_video_state) << std::endl
				//	<< "video_clock - " << get_video_clock(global_video_state) << std::endl
				//	<< "video_current_pts - " << is->video_current_pts << std::endl
				//	<< "video_current_pts_time - " << ( av_gettime() - is->video_current_pts_time ) / 1000000.0 << std::endl;
				
				break;
			default:
				break;
			}
			break;
		case FF_QUIT_EVENT:
		case SDL_QUIT:
			ret = 0;
			break;
		case FF_REFRESH_EVENT:
			video_refresh_timer(event.user.data1);
			break;
		default:
			break;
		}
	};

	global_video_state->quit = 1;
	/*
	* If the video has finished playing, then both the picture and
	* audio queues are waiting for more data.  Make them stop
	* waiting and terminate normally.
	*/
	global_video_state->audioq.cond->notify_one();
	global_video_state->videoq.cond->notify_one();
	SDL_Quit();
	return 0;
}
