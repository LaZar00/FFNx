/****************************************************************************/
//    Copyright (C) 2009 Aali132                                            //
//    Copyright (C) 2018 quantumpencil                                      //
//    Copyright (C) 2018 Maxime Bacoux                                      //
//    Copyright (C) 2020 myst6re                                            //
//    Copyright (C) 2020 Chris Rizzitello                                   //
//    Copyright (C) 2020 John Pritchard                                     //
//    Copyright (C) 2023 Julian Xhokaxhiu                                   //
//                                                                          //
//    This file is part of FFNx                                             //
//                                                                          //
//    FFNx is free software: you can redistribute it and/or modify          //
//    it under the terms of the GNU General Public License as published by  //
//    the Free Software Foundation, either version 3 of the License         //
//                                                                          //
//    FFNx is distributed in the hope that it will be useful,               //
//    but WITHOUT ANY WARRANTY; without even the implied warranty of        //
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         //
//    GNU General Public License for more details.                          //
/****************************************************************************/

#include "../audio.h"
#include "../renderer.h"

#include "movies.h"

// 10 frames
#define VIDEO_BUFFER_SIZE 10

#define LAG (((now - start_time) - (timer_freq / movie_fps) * movie_frame_counter) / (timer_freq / 1000))

uint32_t audio_must_be_converted = false;

AVFormatContext *format_ctx = 0;
AVCodecContext *codec_ctx = 0;
const AVCodec *codec = 0;
AVCodecContext *acodec_ctx = 0;
const AVCodec *acodec = 0;
AVFrame *movie_frame = 0;
struct SwsContext *sws_ctx = 0;
SwrContext* swr_ctx = NULL;

int videostream;
int audiostream;

struct video_frame
{
	uint32_t yuv_textures[3] = { 0 };
};

struct video_frame video_buffer[VIDEO_BUFFER_SIZE];
uint32_t vbuffer_read = 0;
uint32_t vbuffer_write = 0;

uint32_t movie_frame_counter = 0;
uint32_t movie_frames;
uint32_t movie_width, movie_height;
double movie_fps;
double movie_duration;
bool movie_jpeg_range = false;

bool first_audio_packet;

time_t timer_freq;
time_t start_time;

void ffmpeg_movie_init()
{
	ffnx_info("FFMpeg movie player plugin loaded\n");

	QueryPerformanceFrequency((LARGE_INTEGER *)&timer_freq);
}

// clean up anything we have allocated
void ffmpeg_release_movie_objects()
{
	uint32_t i;

	if (movie_frame) av_frame_free(&movie_frame);
	if (codec_ctx) avcodec_free_context(&codec_ctx);
	if (acodec_ctx) avcodec_free_context(&acodec_ctx);
	if (format_ctx) avformat_close_input(&format_ctx);
	if (swr_ctx) {
		swr_close(swr_ctx);
		swr_free(&swr_ctx);
	}

	codec_ctx = 0;
	acodec_ctx = 0;
	format_ctx = 0;

	audio_must_be_converted = false;

	for(i = 0; i < VIDEO_BUFFER_SIZE; i++)
	{
		// Cleanup YUV textures
		for (uint32_t idx = 0; idx < 3; idx++)
		{
			newRenderer.deleteTexture(video_buffer[i].yuv_textures[idx]);
			video_buffer[i].yuv_textures[idx] = 0;
		}
	}

	// Unset slot U and V as they are used only for YUV textures
	newRenderer.useTexture(0, RendererTextureSlot::TEX_U);
	newRenderer.useTexture(0, RendererTextureSlot::TEX_V);
}

// prepare a movie for playback
uint32_t ffmpeg_prepare_movie(char *name, bool with_audio)
{
	uint32_t i;
	WAVEFORMATEX sound_format;
	DSBUFFERDESC1 sbdesc;
	uint32_t ret;

	if(ret = avformat_open_input(&format_ctx, name, NULL, NULL))
	{
		ffnx_error("prepare_movie: couldn't open movie file: %s\n", name);
		ffmpeg_release_movie_objects();
		goto exit;
	}

	if(avformat_find_stream_info(format_ctx, NULL) < 0)
	{
		ffnx_error("prepare_movie: couldn't find stream info\n");
		ffmpeg_release_movie_objects();
		goto exit;
	}

	videostream = av_find_best_stream(format_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
	if (videostream < 0) {
		ffnx_error("prepare_movie: no video stream found\n");
		ffmpeg_release_movie_objects();
		goto exit;
	}

	audiostream = av_find_best_stream(format_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, &acodec, 0);
	if(with_audio && audiostream < 0 && trace_movies) ffnx_trace("prepare_movie: no audio stream found\n");

	codec_ctx = avcodec_alloc_context3(codec);
	if (!codec_ctx) {
		ffnx_error("prepare_movie: could not allocate video codec context\n");
		codec_ctx = 0;
		ffmpeg_release_movie_objects();
		goto exit;
	}
	avcodec_parameters_to_context(codec_ctx, format_ctx->streams[videostream]->codecpar);

	if(avcodec_open2(codec_ctx, codec, NULL) < 0)
	{
		ffnx_error("prepare_movie: couldn't open video codec\n");
		ffmpeg_release_movie_objects();
		goto exit;
	}

	if(audiostream >= 0)
	{
		acodec_ctx = avcodec_alloc_context3(acodec);
		if (!acodec_ctx) {
			ffnx_error("prepare_movie: could not allocate audio codec context\n");
			codec_ctx = 0;
			ffmpeg_release_movie_objects();
			goto exit;
		}
		avcodec_parameters_to_context(acodec_ctx, format_ctx->streams[audiostream]->codecpar);

		if(avcodec_open2(acodec_ctx, acodec, NULL) < 0)
		{
			ffnx_error("prepare_movie: couldn't open audio codec\n");
			ffmpeg_release_movie_objects();
			goto exit;
		}
	}

	movie_width = codec_ctx->width;
	movie_height = codec_ctx->height;
	movie_fps = av_q2d(av_guess_frame_rate(format_ctx, format_ctx->streams[videostream], NULL));
	movie_duration = (double)format_ctx->duration / (double)AV_TIME_BASE;
	movie_frames = (uint32_t)::round(movie_fps * movie_duration);
	movie_jpeg_range = codec_ctx->color_range == AVCOL_RANGE_JPEG;

	if (trace_movies)
	{
		if (movie_fps < 100.0) ffnx_info("prepare_movie: %s; %s/%s %ix%i, %f FPS, duration: %f, frames: %i, color_range: %d\n", name, codec->name, acodec_ctx ? acodec->name : "null", movie_width, movie_height, movie_fps, movie_duration, movie_frames, codec_ctx->color_range);
		// bogus FPS value, assume the codec provides frame limiting
		else ffnx_info("prepare_movie: %s; %s/%s %ix%i, duration: %f, color_range: %d\n", name, codec->name, acodec_ctx ? acodec->name : "null", movie_width, movie_height, movie_duration, codec_ctx->color_range);
	}

	if(movie_width > max_texture_size || movie_height > max_texture_size)
	{
		ffnx_error("prepare_movie: movie dimensions exceed max texture size, skipping\n");
		ffmpeg_release_movie_objects();
		goto exit;
	}

	if(!movie_frame) movie_frame = av_frame_alloc();

	if(sws_ctx) sws_freeContext(sws_ctx);

	vbuffer_read = 0;
	vbuffer_write = 0;

	if(codec_ctx->pix_fmt != AV_PIX_FMT_YUVJ420P)
	{
		if (trace_movies)
		{
			ffnx_trace("prepare_movie: Video must be converted: IN codec_ctx->colorspace: %s\n", av_color_space_name(codec_ctx->colorspace));
			ffnx_trace("prepare_movie: Video must be converted: IN codec_ctx->pix_fmt: %s\n", av_pix_fmt_desc_get(codec_ctx->pix_fmt)->name);
		}

		sws_ctx = sws_getContext(
			movie_width,
			movie_height,
			codec_ctx->pix_fmt,
			movie_width,
			movie_height,
			AV_PIX_FMT_YUVJ420P,
			SWS_FAST_BILINEAR | SWS_ACCURATE_RND,
			NULL,
			NULL,
			NULL
		);

		// change the range of input data by first reading the current color space and then setting it's range as yuvj.
    int dummy[4];
    int srcRange, dstRange;
    int brightness, contrast, saturation;
    sws_getColorspaceDetails(sws_ctx, (int**)&dummy, &srcRange, (int**)&dummy, &dstRange, &brightness, &contrast, &saturation);
    const int* coefs = sws_getCoefficients(SWS_CS_DEFAULT);
    dstRange = srcRange = movie_jpeg_range = true; // this marks that values are according to yuvj
    sws_setColorspaceDetails(sws_ctx, coefs, srcRange, coefs, dstRange, brightness, contrast, saturation);
	}
	else sws_ctx = 0;

	if(audiostream >= 0)
	{
		if (acodec_ctx->sample_fmt != AV_SAMPLE_FMT_FLT) {
			audio_must_be_converted = true;
			if (trace_movies)
			{
				ffnx_trace("prepare_movie: Audio must be converted: IN acodec_ctx->sample_fmt: %s\n", av_get_sample_fmt_name(acodec_ctx->sample_fmt));
				ffnx_trace("prepare_movie: Audio must be converted: IN acodec_ctx->sample_rate: %d\n", acodec_ctx->sample_rate);
				ffnx_trace("prepare_movie: Audio must be converted: IN acodec_ctx->channel_layout: %u\n", acodec_ctx->channel_layout);
				ffnx_trace("prepare_movie: Audio must be converted: IN acodec_ctx->channels: %u\n", acodec_ctx->channels);
			}

			// Prepare software conversion context
			swr_ctx = swr_alloc_set_opts(
				// Create a new context
				NULL,
				// OUT
				acodec_ctx->channel_layout == 0 ? AV_CH_LAYOUT_STEREO : acodec_ctx->channel_layout,
				AV_SAMPLE_FMT_FLT,
				acodec_ctx->sample_rate,
				// IN
				acodec_ctx->channel_layout == 0 ? AV_CH_LAYOUT_STEREO : acodec_ctx->channel_layout,
				acodec_ctx->sample_fmt,
				acodec_ctx->sample_rate,
				// LOG
				0,
				NULL
			);

			swr_init(swr_ctx);
		}

		nxAudioEngine.initStream(
			movie_duration,
			acodec_ctx->sample_rate,
			acodec_ctx->channels
		);

		first_audio_packet = true;
	}

exit:
	movie_frame_counter = 0;

	return movie_frames;
}

// stop movie playback, no video updates will be requested after this so all we have to do is stop the audio
void ffmpeg_stop_movie()
{
	nxAudioEngine.stopStream();
}

void upload_yuv_texture(uint8_t **planes, int *strides, uint32_t num, uint32_t buffer_index)
{
	uint32_t upload_width = strides[num];
	uint32_t tex_width = num == 0 ? movie_width : movie_width / 2;;
	uint32_t tex_height = num == 0 ? movie_height : movie_height / 2;

	if (upload_width > tex_width) tex_width = upload_width;

	if (video_buffer[buffer_index].yuv_textures[num])
		newRenderer.deleteTexture(video_buffer[buffer_index].yuv_textures[num]);

	video_buffer[buffer_index].yuv_textures[num] = newRenderer.createTexture(
		planes[num],
		tex_width,
		tex_height,
		upload_width,
		RendererTextureType::YUV,
		false
	);
}

void buffer_yuv_frame(uint8_t **planes, int *strides)
{
	upload_yuv_texture(planes, strides, 0, vbuffer_write); // Y
	upload_yuv_texture(planes, strides, 1, vbuffer_write); // U
	upload_yuv_texture(planes, strides, 2, vbuffer_write); // V

	vbuffer_write = (vbuffer_write + 1) % VIDEO_BUFFER_SIZE;
}

void draw_yuv_frame(uint32_t buffer_index)
{
	if(gl_defer_yuv_frame(buffer_index)) return;

	for (uint32_t idx = 0; idx < 3; idx++)
		newRenderer.useTexture(video_buffer[buffer_index].yuv_textures[idx], idx);

	newRenderer.isMovie(true);
	newRenderer.isYUV(true);
	newRenderer.isFullRange(movie_jpeg_range);
	gl_draw_movie_quad(movie_width, movie_height);
	newRenderer.isFullRange(false);
	newRenderer.isYUV(false);
	newRenderer.isMovie(false);
}

// display the next frame
uint32_t ffmpeg_update_movie_sample(bool use_movie_fps)
{
	AVPacket packet;
	int ret;
	time_t now;
	DWORD DSStatus;

	// no playable movie loaded, skip it
	if(!format_ctx) return false;

	// keep track of when we started playing this movie
	if(movie_frame_counter == 0) QueryPerformanceCounter((LARGE_INTEGER *)&start_time);

	while((ret = av_read_frame(format_ctx, &packet)) >= 0)
	{
		if(packet.stream_index == videostream)
		{
			ret = avcodec_send_packet(codec_ctx, &packet);

			if (ret < 0)
			{
				ffnx_trace("%s: avcodec_send_packet -> %d\n", __func__, ret);
				av_packet_unref(&packet);
				break;
			}

			ret = avcodec_receive_frame(codec_ctx, movie_frame);

			if (ret == AVERROR_EOF)
			{
				ffnx_trace("%s: avcodec_receive_frame -> %d\n", __func__, ret);
				av_packet_unref(&packet);
				break;
			}

			if (ret >= 0)
			{
				QueryPerformanceCounter((LARGE_INTEGER *)&now);

				if(sws_ctx)
				{
					AVFrame* frame = av_frame_alloc();
					frame->width = movie_width;
					frame->height = movie_height;
					frame->format = AV_PIX_FMT_YUVJ420P;

					av_image_alloc(frame->data, frame->linesize, frame->width, frame->height, AVPixelFormat(frame->format), 1);

					sws_scale(sws_ctx, movie_frame->extended_data, movie_frame->linesize, 0, frame->height, frame->data, frame->linesize);
					buffer_yuv_frame(frame->data, frame->linesize);

					av_freep(&frame->data[0]);
					av_frame_free(&frame);
				}
				else buffer_yuv_frame(movie_frame->extended_data, movie_frame->linesize);

				if(vbuffer_write == vbuffer_read)
				{
					draw_yuv_frame(vbuffer_read);

					vbuffer_read = (vbuffer_read + 1) % VIDEO_BUFFER_SIZE;

					av_packet_unref(&packet);

					break;
				}
			}
		}

		if(packet.stream_index == audiostream)
		{
			QueryPerformanceCounter((LARGE_INTEGER *)&now);

			ret = avcodec_send_packet(acodec_ctx, &packet);

			if (ret < 0)
			{
				ffnx_trace("%s: avcodec_send_packet -> %d\n", __func__, ret);
				av_packet_unref(&packet);
				break;
			}

			ret = avcodec_receive_frame(acodec_ctx, movie_frame);

			if (ret == AVERROR_EOF)
			{
				ffnx_trace("%s: avcodec_receive_frame -> %d\n", __func__, ret);
				av_packet_unref(&packet);
				break;
			}

			if (ret >= 0)
			{
				uint32_t bytesperpacket = audio_must_be_converted ? av_get_bytes_per_sample(AV_SAMPLE_FMT_FLT) : av_get_bytes_per_sample(acodec_ctx->sample_fmt);
				uint32_t _size = bytesperpacket * movie_frame->nb_samples * acodec_ctx->channels;

				// Sometimes the captured frame may have no sound samples. Just skip and move forward
				if (_size)
				{
					uint8_t *buffer;

					av_samples_alloc(&buffer, movie_frame->linesize, acodec_ctx->channels, movie_frame->nb_samples, (audio_must_be_converted ? AV_SAMPLE_FMT_FLT : acodec_ctx->sample_fmt), 0);
					if (audio_must_be_converted) swr_convert(swr_ctx, &buffer, movie_frame->nb_samples, (const uint8_t**)movie_frame->extended_data, movie_frame->nb_samples);
					else av_samples_copy(&buffer, movie_frame->extended_data, 0, 0, movie_frame->nb_samples, acodec_ctx->channels, acodec_ctx->sample_fmt);

					nxAudioEngine.pushStreamData(buffer, _size);

					av_freep(&buffer);
				}
			}
		}

		av_packet_unref(&packet);
	}

	if (first_audio_packet)
	{
		first_audio_packet = false;

		// reset start time so video syncs up properly
		QueryPerformanceCounter((LARGE_INTEGER *)&start_time);

		nxAudioEngine.playStream();
	}

	movie_frame_counter++;

	// could not read any more frames, exhaust video buffer then end movie
	if(ret < 0)
	{
		if(vbuffer_write != vbuffer_read)
		{
			draw_yuv_frame(vbuffer_read);

			vbuffer_read = (vbuffer_read + 1) % VIDEO_BUFFER_SIZE;
		}

		if(vbuffer_write == vbuffer_read) return false;
	}

	// Pure movie playback has no frame limiter, although it is not always required. Use it only when necessary
	if (use_movie_fps)
	{
		// wait for the next frame
		do
		{
			QueryPerformanceCounter((LARGE_INTEGER *)&now);
		} while(LAG < 0.0);
	}

	// keep going
	return true;
}

// draw the current frame, don't update anything
void ffmpeg_draw_current_frame()
{
	draw_yuv_frame((vbuffer_read - 1) % VIDEO_BUFFER_SIZE);
}

// loop back to the beginning of the movie
void ffmpeg_loop()
{
	if(format_ctx) avformat_seek_file(format_ctx, -1, 0, 0, 0, 0);
}

// get the current frame number
uint32_t ffmpeg_get_movie_frame()
{
	return movie_frame_counter;
}

short ffmpeg_get_fps_ratio()
{
	return ceil(movie_fps / 15.0f);
}
