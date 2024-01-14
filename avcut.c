/*
 * avcut
 *
 *  Copyright (C) 2015 Mario Kicherer (dev@kicherer.org)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 */

/*
 * To inspect how ffmpeg would decode frames:
 * 
 * ffprobe -show_frames  -of compact -select_streams v:0 -show_entries \
 * 		frame=key_frame,pkt_pts,pkt_dts,best_effort_timestamp,pkt_pts_time \
 * 		<file>
 * 
 * fmpeg -i <file> -c copy -bsf:v trace_headers -f null - 2>&1 | less
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <float.h>
#include <sys/stat.h>
#include <unistd.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavutil/opt.h>

#define AVCUT_DUMP_CHAR(var, length) { size_t i; for (i=0; i<(length); i++) { printf("%x ", ((char*) (var))[i]); } printf("\n"); }

#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(55,28,1)
#define av_frame_alloc avcodec_alloc_frame
#define av_frame_free avcodec_free_frame
#endif

#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(56,56,0)
#ifndef AV_CODEC_FLAG_GLOBAL_HEADER
#define AV_CODEC_FLAG_GLOBAL_HEADER CODEC_FLAG_GLOBAL_HEADER
#endif
#ifndef AV_CODEC_CAP_DELAY
#define AV_CODEC_CAP_DELAY CODEC_CAP_DELAY
#endif
#endif

#ifdef DEBUG
// only enable debug output for this file
#define av_log(null, level, fmt, ...) printf(fmt, ##__VA_ARGS__)
#endif


// buffer management struct for a stream
struct packet_buffer {
	unsigned int stream_index;
	char stop_reading_stream; // do we have all required packets for this stream?
	
	AVPacket *pkts;
	size_t n_pkts; // number of pkts in .pkts
	
	AVFrame **frames;
	size_t n_frames; // number of frames in .frames
	
	size_t length; // allocated array length of .pkts and .frames
	
	unsigned long duration_dropped_in_pkts; // accumulated duration of dropped packets
	
	double next_dts; // DTS of the next packet that will be written
	long last_pts; // store last PTS in case we're dealing with AVIs where the
				// last two frames may have a zero DTS
};

struct profile {
	char *preset;
	char *tune;
	char *x264opts;
};

// project state context
struct project {
	AVFormatContext *in_fctx;
	AVFormatContext *out_fctx;
	char has_b_frames;
	
	struct profile *profile;
	
	unsigned int n_stream_ids; // number of streams in output file
	unsigned int *stream_ids; // mapping of output stream to input stream
	
	AVCodecContext **in_codec_ctx;
	AVCodecContext **out_codec_ctx;
	
	double *cuts; // [ first_excluded_frame, first_included_frame, ...]
	size_t n_cuts;
	
	double stop_after_ts; // stop after this timestamp
	char stop_reading; // signal to stop reading new packets
	char last_flush;
	
	unsigned long size_diff; // accept packet size differences with this value
	
	size_t video_packets_read;
	size_t other_packets_read;
	size_t video_packets_decoded;
	size_t video_frames_encoded;
	size_t video_packets_written;
	size_t other_packets_written;
	
	struct AVBSFContext *bsf_h264_to_annexb;
	struct AVBSFContext *bsf_dump_extra;
};

// private data avcut may store with each AVCodecContext
struct codeccontext {
	char h264_avcc_format; // flag: h264 stream with annexb = 0, or avcc = 1
};



#if (LIBAVCODEC_VERSION_MAJOR < 55) || \
	( (LIBAVCODEC_VERSION_MAJOR == 55) && (LIBAVCODEC_VERSION_MINOR < 55) )
// backported function to fix compilation with versions < v2.3
void av_packet_rescale_ts(AVPacket *pkt, AVRational src_tb, AVRational dst_tb)
{
	if (pkt->pts != AV_NOPTS_VALUE)
		pkt->pts = av_rescale_q(pkt->pts, src_tb, dst_tb);
	if (pkt->dts != AV_NOPTS_VALUE)
		pkt->dts = av_rescale_q(pkt->dts, src_tb, dst_tb);
	if (pkt->duration > 0)
		pkt->duration = av_rescale_q(pkt->duration, src_tb, dst_tb);
	#if FF_API_CONVERGENCE_DURATION
	FF_DISABLE_DEPRECATION_WARNINGS
	if (pkt->convergence_duration > 0)
		pkt->convergence_duration = av_rescale_q(pkt->convergence_duration, src_tb, dst_tb);
	FF_ENABLE_DEPRECATION_WARNINGS
	#endif
}
#endif

// encode a frame and write the resulting packet into the output file
int encode_write_frame(struct project *pr, struct packet_buffer *s, AVFrame *frame) {
	AVPacket *enc_pkt;
	AVPacket *enc_pkt2;
	int ret;
	AVStream *ostream = pr->out_fctx->streams[s->stream_index];
	AVCodecContext *codec_ctx = pr->out_codec_ctx[s->stream_index];
	
	
	if (frame) {
		av_log(NULL, AV_LOG_DEBUG, "enc frame pts: %" PRId64 " pkt_pts: %" PRId64 " pkt_dts: %" PRId64 " pkt_size: %d type: %c to %f\n",
			frame->pts, frame->best_effort_timestamp, frame->pkt_dts, frame->pkt_size, av_get_picture_type_char(frame->pict_type),
			frame->pts * av_q2d(codec_ctx->time_base)
			);
		
		// we have to unset the pict_type as an encoder might react to it
		frame->pict_type = AV_PICTURE_TYPE_NONE;
	}
	
	// we send no or one frame to the library and receive zero or more frames in return below
	ret = avcodec_send_frame(codec_ctx, frame);
	if (ret < 0) {
		av_log(NULL, AV_LOG_ERROR, "avcodec_send_frame() failed: %s\n", av_err2str(ret));
		return ret;
	}
	
	while (1) {
		enc_pkt = av_packet_alloc();
		if (!enc_pkt) {
			av_log(NULL, AV_LOG_ERROR, "av_packet_alloc() failed\n");
			return ret;
		}
		
		ret = avcodec_receive_packet(codec_ctx, enc_pkt);
		if (ret == -EAGAIN || ret == AVERROR_EOF) {
			return ret;
		}
		
		if (ret < 0) {
			av_log(NULL, AV_LOG_ERROR, "avcodec_receive_packet failed: %s (%d)\n", av_err2str(ret), ret);
			return ret;
		}
		
		pr->video_frames_encoded++;
		
		enc_pkt->stream_index = s->stream_index;
		
		if (enc_pkt->duration == 0) {
			enc_pkt->duration = codec_ctx->ticks_per_frame;
			av_packet_rescale_ts(enc_pkt, codec_ctx->time_base, ostream->time_base);
		}
		
		enc_pkt->dts = s->next_dts;
		s->next_dts += enc_pkt->duration;
		
		// copy the header to the beginning of each key frame if we use a global header
		if (codec_ctx->flags & AV_CODEC_FLAG_GLOBAL_HEADER) {
			ret = av_bsf_send_packet(pr->bsf_dump_extra, enc_pkt);
			if (ret) {
				av_log(NULL, AV_LOG_ERROR, "error av_bsf_send_packet(): %d\n", ret);
			}
			while (1) {
				enc_pkt2 = av_packet_alloc();
				if (!enc_pkt2) {
					av_log(NULL, AV_LOG_ERROR, "av_packet_alloc() failed\n");
					return ret;
				}
				
				ret = av_bsf_receive_packet(pr->bsf_dump_extra, enc_pkt2);
				
				if (ret == -EAGAIN) {
					break;
				}
				if (ret == AVERROR_EOF)
					break;
				if (ret < 0) {
					av_log(NULL, AV_LOG_ERROR, "av_bsf_receive_packet() failed: %s\n", av_err2str(ret));
					exit(1);
				}
				
				av_log(NULL, AV_LOG_DEBUG,
					   "write video pkt (filtered), enc size: %d pts: %" PRId64 " dts: %" PRId64 " - to %f\n",
						enc_pkt2->size, enc_pkt2->pts, enc_pkt2->dts,
						enc_pkt2->pts*ostream->time_base.num/(double)ostream->time_base.den);
				
				pr->video_packets_written++;
				ret = av_interleaved_write_frame(pr->out_fctx, enc_pkt2);
				if (ret < 0) {
					av_log(NULL, AV_LOG_ERROR, "error while writing packet, error %d\n", ret);
					return ret;
				}
				
				av_frame_free(&frame);
				// TODO av_frame_unref?
				av_packet_free(&enc_pkt);
				av_packet_free(&enc_pkt2);
			}
			if (ret == -EAGAIN) {
				continue;
			}
		} else {
			av_log(NULL, AV_LOG_DEBUG,
				"write video pkt, enc size: %d pts: %" PRId64 " dts: %" PRId64 " - to %f\n",
				enc_pkt->size, enc_pkt->pts, enc_pkt->dts,
				enc_pkt->pts*ostream->time_base.num/(double)ostream->time_base.den);
			
			pr->video_packets_written++;
			ret = av_interleaved_write_frame(pr->out_fctx, enc_pkt);
			if (ret < 0) {
				av_log(NULL, AV_LOG_ERROR, "error while writing packet, error %d\n", ret);
				return ret;
			}
			
			av_frame_free(&frame);
			// TODO av_frame_unref?
			av_packet_free(&enc_pkt);
		}
	}
	
	return 0;
}

// calculate stream timestamp of frame using its PTS
double frame_pts2ts(struct project *pr, struct packet_buffer *s, AVFrame *frame) {
	return frame->pts * av_q2d(pr->in_codec_ctx[s->stream_index]->time_base);
}

// check if pkt/frame at timestamp $ts shall be included
char ts_included(struct project *pr, double ts) {
	size_t i;
	
	// check if timestampe lies in a cut interval
	for (i=0; i < pr->n_cuts; i+=2) {
		if (pr->cuts[i] <= ts && ts < pr->cuts[i+1])
			return 0;
		
		// list is ordered
		if (ts < pr->cuts[i])
			return 1;
	}
	
	return 1;
}

#define BUF_COPY_COMPLETE (1<<0)
#define BUF_DROP_COMPLETE (1<<1)
#define BUF_CUT_IN_BETWEEN (1<<2)

// check if the complete buffer will be used or if a cutpoint lies in this interval
char get_buffer_processing_mode(struct project *pr, struct packet_buffer *s, unsigned long last_iframe) {
	double buf_start, buf_end;
	size_t i;
	char rv;
	
	
	if (!last_iframe) {
		buf_start = s->pkts[0].pts * av_q2d(pr->in_fctx->streams[s->stream_index]->time_base);
		buf_end = s->pkts[s->n_pkts-1].pts * av_q2d(pr->in_fctx->streams[s->stream_index]->time_base);
	} else {
		buf_start = frame_pts2ts(pr, s, s->frames[0]);
		buf_end = frame_pts2ts(pr, s, s->frames[last_iframe]);
	}
	
	rv = 0;
	for (i=0; i < pr->n_cuts; i++) {
		// check if any cutpoint lies between buffer start and end
		if (buf_start < pr->cuts[i] && pr->cuts[i] < buf_end)
			rv = BUF_CUT_IN_BETWEEN;
		
		// check if buffer lies between cutpoints
		if (pr->cuts[i] <= buf_start && buf_end <= pr->cuts[i+1]) {
			if (i % 2 == 0)
				rv = BUF_DROP_COMPLETE;
			else
				rv = BUF_COPY_COMPLETE;
		}
		
		// stop this loop if further cuts lie behind current buffer
		if (buf_end < pr->cuts[i])
			rv = BUF_COPY_COMPLETE;
		
		if (rv)
			break;
	}
	
	// if we are past all cutpoints, copy the remaining video
	if (rv == 0)
		rv = BUF_COPY_COMPLETE;
	
	av_log(NULL, AV_LOG_DEBUG, "check buffer stream %u: %f to %f -> %s\n",
		   s->stream_index, buf_start, buf_end,
		   (rv == BUF_COPY_COMPLETE ? "copy": (rv == BUF_DROP_COMPLETE ? "drop": "cut in between"))
		   );
	
	return rv;
}

char find_packet_for_frame(struct project *pr, struct packet_buffer *s, size_t frame_idx, size_t *packet_idx) {
	size_t i;
	
	for (i=0;i<s->n_pkts;i++) {
		// we cannot compare packet.dts with frame.pkt_dts as they differ by 2 (maybe caused by multithreading)
		if (
			(s->pkts[i].pts != AV_NOPTS_VALUE &&
				s->pkts[i].pts == s->frames[frame_idx]->best_effort_timestamp) ||
			(s->pkts[i].pts == AV_NOPTS_VALUE && s->pkts[i].dts == AV_NOPTS_VALUE &&
				s->frames[frame_idx]->pkt_size == s->pkts[i].size - pr->size_diff)
		)
		{
			// libav is missing pkt_size
			if (s->frames[frame_idx]->pkt_size != s->pkts[i].size - pr->size_diff) {
				av_log(NULL, AV_LOG_ERROR,
						"size mismatch %zu:%d %zu:%d (diff: %lu, try: %d)\n",
						frame_idx, s->frames[frame_idx]->pkt_size, i,
						s->pkts[i].size, pr->size_diff,
						s->pkts[i].size - s->frames[frame_idx]->pkt_size);
				continue;
			}
			
			*packet_idx = i;
			
			return 1;
		}
	}
	
	av_log(NULL, AV_LOG_ERROR, "packet for frame %zu not found\n",
			frame_idx);
	
	for (i=0;i<s->n_pkts;i++) {
		av_log(NULL, AV_LOG_DEBUG, "%" PRId64 " %" PRId64 " %" PRId64 " %" PRId64 " - %d %d (NOPTS: %" PRId64 ")\n",
				s->pkts[i].pts, s->pkts[i].dts, s->frames[frame_idx]->pkt_dts,
				s->frames[frame_idx]->best_effort_timestamp,
				s->frames[frame_idx]->pkt_size, s->pkts[i].size,
				AV_NOPTS_VALUE);
	}
	
	return 0;
}

int open_encoder(struct project *pr, unsigned int enc_stream_idx) {
	unsigned int i;
	int ret;
	AVStream *out_stream;
	AVCodecContext *dec_cctx, *enc_cctx;
	const AVCodec *encoder;
	
	
	i = pr->stream_ids[enc_stream_idx];
	
// 	if (pr->in_fctx->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC)
// 		continue;
	
	dec_cctx = pr->in_codec_ctx[i];
	
	out_stream = avformat_new_stream(pr->out_fctx, NULL);
	if (!out_stream) {
		av_log(NULL, AV_LOG_ERROR, "Failed allocating output stream\n");
		return AVERROR_UNKNOWN;
	}
	
	// NOTE setting this will cause segfaults during app cleanup later, looks
	// like it's automatically registered
	if (0)
		pr->out_fctx->streams[enc_stream_idx] = out_stream;
	
	// copy stream metadata
	av_dict_copy(&out_stream->metadata, pr->in_fctx->streams[i]->metadata, 0);
	
	if (dec_cctx->codec_type == AVMEDIA_TYPE_VIDEO) {
		encoder = avcodec_find_encoder(dec_cctx->codec_id);
		if (!encoder) {
			av_log(NULL, AV_LOG_ERROR, "Encoder not found\n");
			return AVERROR_INVALIDDATA;
		}
	} else {
		encoder = NULL;
	}
	
	enc_cctx = avcodec_alloc_context3(encoder);
	if (!enc_cctx) {
		av_log(NULL, AV_LOG_ERROR, "Failed to alloc video decoder context\n");
		return AVERROR_UNKNOWN;
	}
	ret = avcodec_parameters_copy(out_stream->codecpar, pr->in_fctx->streams[i]->codecpar);
	if (ret < 0) {
		av_log(NULL, AV_LOG_ERROR, "Failed to copy codec parameters to decoder context\n");
		return AVERROR_UNKNOWN;
	}
	
	out_stream->time_base = pr->in_fctx->streams[i]->time_base;
	
	ret = avcodec_parameters_to_context(enc_cctx, out_stream->codecpar);
	if (ret < 0) {
		av_log(NULL, AV_LOG_ERROR, "Failed to copy codec parameters to decoder context\n");
		return AVERROR_UNKNOWN;
	}
	pr->out_codec_ctx[enc_stream_idx] = enc_cctx;
	
	if (dec_cctx->codec_type == AVMEDIA_TYPE_VIDEO) {
		enc_cctx->time_base = dec_cctx->time_base;
		enc_cctx->ticks_per_frame = dec_cctx->ticks_per_frame;
		enc_cctx->delay = dec_cctx->delay;
		enc_cctx->width = dec_cctx->width;
		enc_cctx->height = dec_cctx->height;
		enc_cctx->pix_fmt = dec_cctx->pix_fmt;
		enc_cctx->sample_aspect_ratio = dec_cctx->sample_aspect_ratio;
		enc_cctx->color_primaries = dec_cctx->color_primaries;
		enc_cctx->color_trc = dec_cctx->color_trc;
		enc_cctx->colorspace = dec_cctx->colorspace;
		enc_cctx->color_range = dec_cctx->color_range;
		enc_cctx->chroma_sample_location = dec_cctx->chroma_sample_location;
		enc_cctx->profile = dec_cctx->profile;
		enc_cctx->level = dec_cctx->level;
		
		if (pr->profile) {
			av_log(NULL, AV_LOG_INFO, "Settings from profile:\n");
			av_log(NULL, AV_LOG_INFO, "  Preset: %s\n", pr->profile->preset);
			av_log(NULL, AV_LOG_INFO, "  Tune: %s\n", pr->profile->tune);
			av_log(NULL, AV_LOG_INFO, "  x264opts: %s\n", pr->profile->x264opts);
			
			av_opt_set(enc_cctx->priv_data, "preset", pr->profile->preset, AV_OPT_SEARCH_CHILDREN);
			av_opt_set(enc_cctx->priv_data, "tune", pr->profile->tune, AV_OPT_SEARCH_CHILDREN);
			av_opt_set(enc_cctx->priv_data, "x264opts", pr->profile->x264opts, AV_OPT_SEARCH_CHILDREN);
		} else {
			// TODO good values?
			enc_cctx->qmin = 16;
			enc_cctx->qmax = 26;
			enc_cctx->max_qdiff = 4;
		}
		
		if (dec_cctx->has_b_frames) {
			enc_cctx->max_b_frames = 3;
			if (pr->has_b_frames < dec_cctx->has_b_frames)
				pr->has_b_frames = dec_cctx->has_b_frames;
		}
// 		enc_cctx->keyint_min = 200;
// 		enc_cctx->gop_size = 250;
		enc_cctx->thread_count = 1; // spawning more threads causes avcodec_close to free threads multiple times
		enc_cctx->codec_tag = 0; // reset tag to avoid incompatibilities while changing container
		
		out_stream->sample_aspect_ratio = pr->in_fctx->streams[i]->sample_aspect_ratio;
		
		// we do not want a global header, we need the data "in stream" as the
		// resulting stream is a concatenation of streams with (possibly)
		// different properties
		if (0)
			if (pr->out_fctx->oformat->flags & AVFMT_GLOBALHEADER)
				enc_cctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
		
		ret = avcodec_open2(enc_cctx, encoder, NULL);
		if (ret < 0) {
			av_log(NULL, AV_LOG_ERROR, "Failed to open encoder for stream %u, error %d\n", i, ret);
			return ret;
		}
	} else if (dec_cctx->codec_type == AVMEDIA_TYPE_UNKNOWN) {
		av_log(NULL, AV_LOG_ERROR, "Error: input stream #%d is of unknown type\n", i);
		avcodec_free_context(&dec_cctx);
		avcodec_free_context(&enc_cctx);
		return AVERROR_INVALIDDATA;
	} else {
		AVCodecParameters params = {0};
		
		ret = avcodec_parameters_from_context(&params, dec_cctx);
		if (ret < 0) {
			av_log(NULL, AV_LOG_ERROR, "Failed to copy codec parameters 1\n");
			avcodec_free_context(&dec_cctx);
			avcodec_free_context(&enc_cctx);
			return ret;
		}
		ret = avcodec_parameters_to_context(enc_cctx, &params);
		if (ret < 0) {
			av_log(NULL, AV_LOG_ERROR, "Failed to copy codec parameters 2\n");
			avcodec_free_context(&dec_cctx);
			avcodec_free_context(&enc_cctx);
			return ret;
		}
		
		enc_cctx->codec_tag = 0;
		
		if (pr->out_fctx->oformat->flags & AVFMT_GLOBALHEADER)
			enc_cctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	}
	
	// We have to call this here as avformat_write_header() changes the time_base
	// and if we reopen the encoder we do not call that fct again to set the same
	// time base as before.
	ret = avformat_init_output(pr->out_fctx, 0);
	if (ret < 0) {
		av_log(NULL, AV_LOG_ERROR, "avformat_init_output failed: %s\n", av_err2str(ret));
		return ret;
	}
	
	return 0;
}

// process packet buffer - either copy, ignore or reencode packets in the packet buffer according to the cut list
void flush_packet_buffer(struct project *pr, struct packet_buffer *s, char last_flush) {
	size_t i, j, last_pkt;
	int ret;
	char buffer_mode = 0;
	double ts;
	size_t last_frame;
	
	
	if (!s->pkts || s->n_pkts == 0)
		return;
	
	if (pr->in_fctx->streams[s->stream_index]->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) {
		// check if we can copy/drop the complete buffer or if we have to check each packet individually
		buffer_mode = get_buffer_processing_mode(pr, s, 0);
		
		for (i=0;i<s->n_pkts;i++) {
			ts = s->pkts[i].pts * av_q2d(pr->in_fctx->streams[s->stream_index]->time_base);
			
			if (pr->stop_after_ts < ts)
				s->stop_reading_stream = 1;
			
			if   ( !( buffer_mode & BUF_DROP_COMPLETE) &&
					( (buffer_mode & BUF_COPY_COMPLETE) || ts_included(pr, ts) )
				)
			{
				s->pkts[i].pts -= s->duration_dropped_in_pkts;
				
				// calculate duration precisely to avoid deviation between PTS and DTS
				double dur = s->pkts[i].duration * av_q2d(pr->in_fctx->streams[s->stream_index]->time_base) /
							av_q2d( pr->out_fctx->streams[s->stream_index]->time_base);
				
				av_packet_rescale_ts(&s->pkts[i], pr->in_fctx->streams[s->stream_index]->time_base,
								 pr->out_fctx->streams[s->stream_index]->time_base);
				
				#define ROUND(x) ((int64_t)((x)+0.5))
				s->pkts[i].dts = ROUND(s->next_dts);
				s->next_dts += dur;
				
				av_log(NULL, AV_LOG_DEBUG,
					"write stream %d copy, pts: %" PRId64 " dts: %" PRId64 " - %f to %f\n",
					s->stream_index, s->pkts[i].pts, s->pkts[i].dts, ts,
					s->pkts[i].pts * av_q2d(pr->out_fctx->streams[s->stream_index]->time_base));
				
				pr->other_packets_written++;
				ret = av_interleaved_write_frame(pr->out_fctx, &s->pkts[i]);
				if (ret < 0) {
					av_log(NULL, AV_LOG_ERROR, "error while writing packet, error %d\n", ret);
					exit(ret);
				}
			} else {
				s->duration_dropped_in_pkts += s->pkts[i].duration;
			}
			
			av_packet_unref(&s->pkts[i]);
		}
		
		s->n_pkts = 0;
		return;
	}
	
	if (s->n_frames > 1) {
		// If this is the last flush, process all frames. If not, ignore the last
		// frame (that is an I frame) as we process it during the next buffer flush
		if (last_flush)
			last_frame = s->n_frames-1;
		else
			last_frame = s->n_frames-2;
		
		// check if we can copy the complete buffer or if we have to check each packet individually
		buffer_mode = get_buffer_processing_mode(pr, s, last_frame);
	} else {
		buffer_mode = BUF_COPY_COMPLETE;
	}
	
	// determine the last packet we have to look at for this GOP
	if (last_flush) {
		// we consider all packets during the last flush
		last_pkt = s->n_pkts-1;
	} else {
		// find the packet with the highest DTS in this GOP
		
		/*
		 * The packet that belongs to the last frame in a GOP might not have
		 * the highest DTS of the GOP's packets. Hence, we search backwards
		 * for the packet with the highest DTS. Otherwise, we might miss packets,
		 * e.g., if the last frame is a P frame, its DTS is lower as the DTS
		 * of the preceding B frames.
		 * 
		 * We stop with the search if we have found two P frames or after a
		 * B frame and a P frame has been found.
		 */
		
		size_t pkt_idx;
		char b_found = 0;
		char n_p_frames = 0;
		
		last_pkt = 0;
		for (i = s->n_frames-2; i > 0; i--) {
			if (s->frames[i]->pict_type == AV_PICTURE_TYPE_B)
				b_found=1;
			if (s->frames[i]->pict_type == AV_PICTURE_TYPE_P) {
				n_p_frames++;
				if (b_found || n_p_frames > 2)
					break;
			}
			
			if (!find_packet_for_frame(pr, s, i, &pkt_idx))
				exit(1);
			
			if (pkt_idx > last_pkt)
				last_pkt = pkt_idx;
		}
		
		if (buffer_mode & BUF_CUT_IN_BETWEEN) {
			// if we encode all frames we don't need the original packets
			for (i=0;i<=last_pkt;i++)
				av_packet_unref(&s->pkts[i]);
		}
	}
	
	// TODO: check if we can simply cut if the last frame is a P-frame?
	
	if (buffer_mode & BUF_CUT_IN_BETWEEN) {
		char frame_written = 0;
		
		// check which frames will be included and encode them
		for (i=0;i<s->n_frames-1;i++) {
			if (!s->frames[i]) {
				av_log(NULL, AV_LOG_ERROR, "no frame %zu\n", i);
				exit(1);
			}
			
			ts = frame_pts2ts(pr, s, s->frames[i]);
			
			// calculate the output PTS from the input PTS by substracting the
			// number of dropped frames which we calculate by rescaling from
			// the number of dropped packets (stream- to codec timebase)
			s->frames[i]->pts -= av_rescale_q(s->duration_dropped_in_pkts,
						pr->in_fctx->streams[s->stream_index]->time_base,
						pr->in_codec_ctx[s->stream_index]->time_base);
			
			if (pr->stop_after_ts < ts)
				s->stop_reading_stream = 1;
			
			if (ts_included(pr, ts)) {
				ret = encode_write_frame(pr, s, s->frames[i]);
				if (ret == -EAGAIN) {
					// ignore, no complete pkt to write yet
				} else
				if (ret < 0) {
					av_log(NULL, AV_LOG_ERROR, "encode_write_frame failed: %s\n", av_err2str(ret));
					exit(ret);
				}
				
				frame_written = 1;
			} else {
				#if (LIBAVUTIL_VERSION_MAJOR >= 59)
				s->duration_dropped_in_pkts += av_rescale_q(s->frames[i]->duration,
						pr->in_codec_ctx[s->stream_index]->time_base,
						pr->in_fctx->streams[s->stream_index]->time_base);
				#else
				s->duration_dropped_in_pkts += s->frames[i]->pkt_duration;
				#endif
				av_frame_free(&s->frames[i]);
			}
		}
		
		// if the encoder emits packets delayed in time, flush the encoder to receive all remaining packets in the queue
		if (frame_written && pr->out_codec_ctx[s->stream_index]->codec->capabilities & AV_CODEC_CAP_DELAY) {
			av_log(NULL, AV_LOG_DEBUG, "Local flushing stream #%u\n", s->stream_index);
			while (1) {
				ret = encode_write_frame(pr, s, 0);
				if (ret == AVERROR_EOF) {
					av_log(NULL, AV_LOG_DEBUG, "flush end\n");
					break;
				}
				if (ret < 0) {
					av_log(NULL, AV_LOG_ERROR, "encode_write_frame failed, error %d: %s\n", ret, av_err2str(ret));
					exit(ret);
				}
			}
		}
		
		if (frame_written) {
			// Some encoders do not like new frames after we flushed them.
			// Hence, we restart the encoder.
			AVCodecContext *out_cctx = pr->out_codec_ctx[s->stream_index];
			
			// if the encoder does not support a restart after a flush, we have
			// to close and reopen the encoder
			#ifndef AV_CODEC_CAP_ENCODER_FLUSH
			#define AV_CODEC_CAP_ENCODER_FLUSH 0
			#endif
			if (out_cctx->codec->capabilities & AV_CODEC_CAP_ENCODER_FLUSH) {
				avcodec_flush_buffers(out_cctx);
			} else {
				avcodec_close(out_cctx);
				ret = open_encoder(pr, s->stream_index);
				if (ret) {
					av_log(NULL, AV_LOG_ERROR, "reopening encoder failed\n");
					exit(1);
				}
			}
		}
	} else
	if (buffer_mode & BUF_COPY_COMPLETE) {
		for (i=0;i<=last_pkt;i++) {
			AVFrame *frame;
			
			// find frame for current packet to determine the PTS
			frame = 0;
			for (j=0;j<s->n_frames;j++) {
				if (
					(s->pkts[i].pts != AV_NOPTS_VALUE &&
						s->pkts[i].pts == s->frames[j]->best_effort_timestamp) ||
					(s->pkts[i].pts == AV_NOPTS_VALUE && s->pkts[i].dts == AV_NOPTS_VALUE &&
						s->frames[j]->pkt_size == s->pkts[i].size - pr->size_diff)
					)
				{
					// libav is missing pkt_size
					if (s->frames[j]->pkt_size != s->pkts[i].size - pr->size_diff) {
						av_log(NULL, AV_LOG_ERROR,
							"size mismatch %zu:%d %zu:%d (diff %lu, dts %" PRId64 ")\n",
							j, s->frames[j]->pkt_size, i, s->pkts[i].size,  - pr->size_diff, s->pkts[i].dts);
						continue;
					}
					
					frame = s->frames[j];
					break;
				}
			}
			if (!frame) {
				av_log(NULL, AV_LOG_ERROR,
					"frame for pkt #%zd (dts %" PRId64 " pts %" PRId64 ") not found\n",
					i, s->pkts[i].dts, s->pkts[i].pts);
				
				av_log(NULL, AV_LOG_DEBUG, "pkt_pts pkt_dts frame->pkt_dts frame->pts frame->best_eff - frame->pkt_size pkt_size - cpn\n");
				for (j=0;j<s->n_frames;j++) {
					av_log(NULL, AV_LOG_DEBUG, "%" PRId64 " %" PRId64 " %" PRId64 " %" PRId64 " %" PRId64 " - %6d %6d - type: %c (NOPTS: %" PRId64 ")\n",
						s->pkts[i].pts, s->pkts[i].dts, s->frames[j]->pkt_dts,
						s->frames[j]->pts,
						s->frames[j]->best_effort_timestamp,
						s->frames[j]->pkt_size, s->pkts[i].size,
						av_get_picture_type_char(s->frames[j]->pict_type),
						AV_NOPTS_VALUE);
				}
				
				// TODO should we continue und discard this pkt as it may provide
				// a frame we do not use? An example to verify this is needed.
				exit(1);
				
// 				s->pkts[i].size = 0;
// 				continue;
			}
			
			ts = frame_pts2ts(pr, s, frame);
			
			if (pr->stop_after_ts < ts)
				s->stop_reading_stream = 1;
			
			if (ts_included(pr, ts)) {
				AVPacket *pkt;
				
				
				// rescale the frame PTS to packet PTS
				s->pkts[i].pts = av_rescale_q(frame->pts,
						pr->in_codec_ctx[s->stream_index]->time_base,
						pr->in_fctx->streams[s->stream_index]->time_base);
				
				// now we can just substract the number of dropped packets
				s->pkts[i].pts -= s->duration_dropped_in_pkts;
				
				// switch from input timebase to output timebase
				av_packet_rescale_ts(&s->pkts[i], pr->in_fctx->streams[s->stream_index]->time_base,
								 pr->out_fctx->streams[s->stream_index]->time_base);
				
				s->pkts[i].dts = s->next_dts;
				s->next_dts += s->pkts[i].duration;
				
				// if the original h264 stream is in AVCC format, convert it to Annex B
				if (pr->in_codec_ctx[s->stream_index]->opaque &&
					((struct codeccontext*) pr->in_codec_ctx[s->stream_index]->opaque)->h264_avcc_format)
				{
					ret = av_bsf_send_packet(pr->bsf_h264_to_annexb, &s->pkts[i]);
					if (ret) {
						av_log(NULL, AV_LOG_ERROR, "error av_bsf_send_packet(bsf_h264_to_annexb): %d\n", ret);
					}
					
					while (1) {
						pkt = av_packet_alloc();
						if (!pkt) {
							av_log(NULL, AV_LOG_ERROR, "av_packet_alloc() failed\n");
							exit(1);
						}
						
						ret = av_bsf_receive_packet(pr->bsf_h264_to_annexb, pkt);
						
						if (ret == -EAGAIN) {
							break;
						}
						if (ret == AVERROR_EOF)
							break;
						if (ret < 0) {
							av_log(NULL, AV_LOG_ERROR, "av_bsf_receive_packet(bsf_h264_to_annexb) failed: %s\n", av_err2str(ret));
							exit(1);
						}
						
						av_log(NULL, AV_LOG_DEBUG,
							"write v cpy pkt_size: %d pkt_pts: %" PRId64 " pkt_dts: %" PRId64 " - frame pts %" PRId64 " - %f to %f\n",
								pkt->size, pkt->pts, pkt->dts, frame->pts, ts,
								pkt->pts * pr->out_fctx->streams[s->stream_index]->time_base.num /
								(double)pr->out_fctx->streams[s->stream_index]->time_base.den
							);
						
						pr->video_packets_written++;
						ret = av_interleaved_write_frame(pr->out_fctx, &s->pkts[i]);
						if (ret < 0) {
							av_log(NULL, AV_LOG_ERROR, "error while writing packet, error %d\n", ret);
							exit(ret);
						}
						av_packet_free(&pkt);
					}
					if (ret == -EAGAIN) {
						continue;
					}
				}
				
				av_log(NULL, AV_LOG_DEBUG,
					"write v cpy pkt_size: %d pkt_pts: %" PRId64 " pkt_dts: %" PRId64 " - frame pts %" PRId64 " - %f to %f\n",
					s->pkts[i].size, s->pkts[i].pts, s->pkts[i].dts, frame->pts, ts,
					s->pkts[i].pts * pr->out_fctx->streams[s->stream_index]->time_base.num /
						(double)pr->out_fctx->streams[s->stream_index]->time_base.den
					);
				
				pr->video_packets_written++;
				ret = av_interleaved_write_frame(pr->out_fctx, &s->pkts[i]);
				if (ret < 0) {
					av_log(NULL, AV_LOG_ERROR, "error while writing packet, error %d\n", ret);
					exit(ret);
				}
			} else {
				s->duration_dropped_in_pkts += s->pkts[i].duration;
			}
			
			av_packet_unref(&s->pkts[i]);
		}
		
		for (j=0;j<s->n_frames-1;j++)
			av_frame_free(&s->frames[j]);
	} else {
		for (i=0;i<=last_pkt;i++) {
			s->duration_dropped_in_pkts += s->pkts[i].duration;
			av_packet_unref(&s->pkts[i]);
		}
		
		for (j=0;j<s->n_frames-1;j++)
			av_frame_free(&s->frames[j]);
	}
	
	// update buffer structures
	AVPacket *newpkts = (AVPacket*) av_realloc(0, sizeof(AVPacket)*s->length);
	j = 0;
	for (i=0;i<s->n_pkts;i++) {
		if (s->pkts[i].size != 0 && s->pkts[i].data != NULL) {
			memcpy(&newpkts[j], &s->pkts[i], sizeof(AVPacket));
			j++;
		}
	}
	av_freep(&s->pkts);
	s->pkts = newpkts;
	s->n_pkts = j;
	
	s->frames[0] = s->frames[s->n_frames-1];
	s->n_frames = 1;
}

// decode a video packet and store it in the buffer
int decode_packet(struct project *pr, struct packet_buffer *sbuffer, unsigned int stream_index, AVPacket *packet) {
	enum AVMediaType mtype;
	int ret, i;
	AVFrame *frame = NULL;
	AVPacket nullpacket = { .data = NULL, .size = 0 };
	
	if (!packet)
		packet = &nullpacket;
	
	mtype = pr->in_fctx->streams[stream_index]->codecpar->codec_type;
	
	if (mtype == AVMEDIA_TYPE_VIDEO) {
		av_log(NULL, AV_LOG_DEBUG, "dec packet %" PRId64 " %" PRId64 " %d\n",
			   packet->pts, packet->dts, packet->size);
		
		// similar to encoding, we can send zero or one packet and get zero or more frames in return
		ret = avcodec_send_packet(pr->in_codec_ctx[stream_index], packet);
		if (ret < 0) {
			av_log(NULL, AV_LOG_ERROR, "avcodec_send_packet() failed: %d\n", ret);
			return ret;
		}
		
		while (1) {
			if (!(frame = av_frame_alloc())) {
				av_log(NULL, AV_LOG_ERROR, "error while allocating frame\n");
				return AVERROR(ENOMEM);
			}
			
			ret = avcodec_receive_frame(pr->in_codec_ctx[stream_index], frame);
			if (ret == -EAGAIN || ret == AVERROR_EOF) {
				av_frame_free(&frame);
				return 0;
			}
			
			if (ret < 0) {
				av_log(NULL, AV_LOG_ERROR, "avcodec_receive_frame failed: %s\n", av_err2str(ret));
				av_frame_free(&frame);
				return ret;
			}
			
			#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(56,56,0)
			av_log(NULL, AV_LOG_DEBUG, "dec frame pts: %" PRId64 " pkt_pts: %" PRId64 " pkt_dts: %" PRId64 " pkt_size: %d type: %c from %f\n",
				  frame->pts, frame->pkt_pts, frame->pkt_dts, frame->pkt_size, av_get_picture_type_char(frame->pict_type),
				  frame->pts*av_q2d(pr->in_fctx->streams[stream_index]->codec->time_base)
				);
			#endif
			
			sbuffer[stream_index].frames[sbuffer[stream_index].n_frames] = frame;
			sbuffer[stream_index].n_frames++;
			
			pr->video_packets_decoded++;
			
			frame->pts = frame->best_effort_timestamp;
			
			// if pts is set from pkt_dts (e.g., through ->best_effort_timestamp),
			// convert from packet (/stream) to frame (/codec) time_base
			if (frame->pts == frame->pkt_dts)
				frame->pts = av_rescale_q(frame->pkt_dts,
						pr->in_fctx->streams[stream_index]->time_base,
						pr->in_codec_ctx[stream_index]->time_base
					);
			
			// The last frames in some AVIs have a DTS of zero. Here, we
			// override the PTS (copied from DTS) in such a case to provide
			// an increasing PTS
			if (frame->pts < sbuffer->last_pts) {
				#if (LIBAVUTIL_VERSION_MAJOR >= 59)
				int64_t new_pts = sbuffer->last_pts + av_rescale_q(frame->duration,
				#else
				int64_t new_pts = sbuffer->last_pts + av_rescale_q(frame->pkt_duration,
				#endif
					pr->in_fctx->streams[stream_index]->time_base,
					pr->in_codec_ctx[stream_index]->time_base);
				av_log(NULL, AV_LOG_DEBUG, "adjusting frame PTS from %" PRId64 " to %" PRId64 "\n", frame->pts, new_pts);
				frame->pts = new_pts;
			}
			
			#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(56,56,0)
			// pkt_pts is deprecated
			if (frame->pkt_pts == AV_NOPTS_VALUE) {
				frame->pkt_pts = av_rescale_q(frame->pts,
						pr->in_codec_ctx[stream_index]->time_base,
						pr->in_fctx->streams[stream_index]->time_base
				)	;
			}
			
			av_log(NULL, AV_LOG_DEBUG, "dec frame res pts: %" PRId64 " pkt_pts: %" PRId64 " pkt_dts: %" PRId64 " pkt_size: %d type: %c for %f\n",
				frame->pts, frame->pkt_pts, frame->pkt_dts, frame->pkt_size, av_get_picture_type_char(frame->pict_type),
				frame->pts*av_q2d(pr->in_codec_ctx[stream_index]->time_base)
				);
			#else
			av_log(NULL, AV_LOG_DEBUG, "dec frame res pts: %" PRId64 " pkt_size: %d type: %c for %f\n",
				frame->pts, frame->pkt_size, av_get_picture_type_char(frame->pict_type),
				frame->pts*av_q2d(pr->in_codec_ctx[stream_index]->time_base)
				);
			#endif
			
			sbuffer->last_pts = frame->pts;
			
			// the first packet in the video buffer is an I frame, if the
			// current packet contains another I frame, flush the buffer
			if (sbuffer[stream_index].n_frames > 1 && frame->key_frame) {
				if (pr->last_flush == 1)
					pr->stop_reading = 1;
				
				char n_finished_streams = 0;
				for (i = 0; i < pr->n_stream_ids; i++) {
					flush_packet_buffer(pr, &sbuffer[i], 0);
					
					if (sbuffer[i].stop_reading_stream)
						n_finished_streams++;
				}
				
				if (n_finished_streams == pr->n_stream_ids)
					pr->last_flush = 1;
			}
		}
	}
	
	return 0;
}

int parse_profile(struct project *pr, char *profile) {
	FILE *f;
	int r;
	size_t slen;
	char key[1024], value[1024];
	
	slen = strlen(profile);
	
	if (!strcmp(&profile[slen-8], ".profile")) {
		f = fopen(profile, "r");
		if (!f) {
			av_log(NULL, AV_LOG_ERROR, "cannot open profile \"%s\": %s\n", profile, strerror(errno));
			return 1;
		}
	} else {
		snprintf(key, 1024, "%s%s.profile", AVCUT_PROFILE_DIRECTORY, profile);
		
		f = fopen(key, "r");
		if (!f) {
			av_log(NULL, AV_LOG_ERROR, "cannot open profile \"%s\": %s\n", key, strerror(errno));
			return 1;
		}
	}
	
	pr->profile = (struct profile*) calloc(1, sizeof(struct profile));
	
	while (!feof(f)) {
		r = fscanf(f, "%1024[^=;\n]=%1024[^;\n]", key, value);
		
		if (r != 2) {
			av_log(NULL, AV_LOG_ERROR, "error while parsing profile\n");
			free(pr->profile);
			pr->profile = 0;
			return 1;
		}
		fscanf(f, " \n");
		
		if (!strcmp(key, "preset")) {
			pr->profile->preset = strdup(value);
		} else
		if (!strcmp(key, "tune")) {
			pr->profile->tune = strdup(value);
		} else 
		if (!strcmp(key, "x264opts")) {
			pr->profile->x264opts = strdup(value);
		} else {
			av_log(NULL, AV_LOG_INFO, "unknown profile key: %s = %s\n", key, value);
		}
	}
	
	fclose(f);
	
	return 0;
}

void help() {
	av_log(NULL, AV_LOG_INFO, "avcut-" AVCUT_VERSION " - Frame-accurate video cutting with only small quality loss\n");
	av_log(NULL, AV_LOG_INFO, "\n");
	av_log(NULL, AV_LOG_INFO, "Usage: avcut [options] [<drop_from_ts> <continue_with_ts> ...]\n");
	av_log(NULL, AV_LOG_INFO, "\n");
	av_log(NULL, AV_LOG_INFO, "Options:\n");
	av_log(NULL, AV_LOG_INFO, "\n");
	av_log(NULL, AV_LOG_INFO, "  -c              Create a shell script to check the cutpoints with mpv\n");
	av_log(NULL, AV_LOG_INFO, "  -d <diff>       Accept this difference in packet sizes during packet matching\n");
	av_log(NULL, AV_LOG_INFO, "  -f <framecount> provide the approximate total frame count to show progress information\n");
	av_log(NULL, AV_LOG_INFO, "  -i <file>       Input file\n");
	av_log(NULL, AV_LOG_INFO, "  -o <file>       Output file\n");
	av_log(NULL, AV_LOG_INFO, "  -p <profile>    Use this encoding profile. If <profile> ends with \".profile\",\n");
	av_log(NULL, AV_LOG_INFO, "                  <profile> is used as a path to the profile file. If not, the profile\n");
	av_log(NULL, AV_LOG_INFO, "                  is loaded from the default profile directory:\n");
	av_log(NULL, AV_LOG_INFO, "                    %s\n", AVCUT_PROFILE_DIRECTORY);
	av_log(NULL, AV_LOG_INFO, "  -s <index>      Skip stream with this index\n");
	av_log(NULL, AV_LOG_INFO, "  -v <level>      Set verbosity level (see https://www.ffmpeg.org/doxygen/2.8/log_8h.html)\n");
	av_log(NULL, AV_LOG_INFO, "\n");
	av_log(NULL, AV_LOG_INFO, "Besides the input and output file, avcut expects a \"blacklist\", i.e. what should\n");
	av_log(NULL, AV_LOG_INFO, "be dropped, as argument. This blacklist consists of timestamps that denote from\n");
	av_log(NULL, AV_LOG_INFO, "where to where frames have to be dropped. The last argument can be a hyphen to\n");
	av_log(NULL, AV_LOG_INFO, "indicate that all remaining frames shall be dropped.\n");
	av_log(NULL, AV_LOG_INFO, "\n");
	av_log(NULL, AV_LOG_INFO, "For example, to drop the frames of the first 10 seconds, the frames between\n");
	av_log(NULL, AV_LOG_INFO, "55.5s and 130s and all frames after 140s in input.avi and write the result to\n");
	av_log(NULL, AV_LOG_INFO, "output.mkv, the following command can be used:\n");
	av_log(NULL, AV_LOG_INFO, "\n");
	av_log(NULL, AV_LOG_INFO, "   avcut -i input.avi -o output.mkv 0 10 55.5 130 140 -\n");
}

int main(int argc, char **argv) {
	unsigned int i, j, n_skip_streams, max_framecount;
	unsigned int *skip_streams;
	unsigned long size_diff;
	int ret, opt, create_check_script;
	char *inputf, *outputf, *profile;
#ifndef DEBUG
	char *verbosity_level;
#endif
	struct project project;
	struct project *pr;
	
	n_skip_streams = 0;
	skip_streams = 0;
	create_check_script = 0;
#ifndef DEBUG
	verbosity_level = 0;
#endif
	inputf = 0;
	outputf = 0;
	profile = 0;
	size_diff = 0;
	max_framecount=0;
	while ((opt = getopt (argc, argv, "hi:o:p:v:cd:s:f:")) != -1) {
		switch (opt) {
			case 'h':
				help();
				return 0;
			case 'i':
				inputf = optarg;
				break;
			case 'o':
				outputf = optarg;
				break;
			case 'p':
				profile = optarg;
				break;
			case 'v':
				#ifndef DEBUG
				verbosity_level = optarg;
				#endif
				break;
			case 'c':
				create_check_script = 1;
				break;
			case 'f':
				{
					char *end;

					max_framecount = strtol(optarg, &end, 10);
					if (end == optarg || *end != 0) {
						av_log(NULL, AV_LOG_ERROR, "error while parsing size_diff: %s\n", optarg);
						help();
						return 1;
					}
					if (max_framecount == 0) {
						av_log(NULL, AV_LOG_ERROR, "framecount must be greater than zero\n");
						help();
						return 1;
					}
				}
				break;
			case 'd':
				{
					char *end;
					
					size_diff = strtol(optarg, &end, 10);
					if (end == optarg || *end != 0) {
						av_log(NULL, AV_LOG_ERROR, "error while parsing size_diff: %s\n", optarg);
						help();
						return 1;
					}
				}
				break;
			case 's':
				{
					char *end;
					unsigned int skip;
					
					skip = strtol(optarg, &end, 10);
					if (end == optarg || *end != 0) {
						av_log(NULL, AV_LOG_ERROR, "error while parsing skip_stream: %s\n", optarg);
						help();
						return 1;
					}
					
					n_skip_streams++;
					skip_streams = (unsigned int*) realloc(skip_streams, sizeof(unsigned int)*n_skip_streams);
					skip_streams[n_skip_streams-1] = skip;
				}
				break;
			default:
				help();
				return 1;
		}
	}
	
	if (!inputf || !outputf) {
		av_log(NULL, AV_LOG_ERROR, "please specify an input and output file\n");
		help();
		return 1;
	}
	
	if ((optind - argc) % 2 != 0) {
		av_log(NULL, AV_LOG_ERROR, "only even number of cut points allowed\n");
		help();
		return 1;
	}
	
	pr = &project;
	memset(pr, 0, sizeof(struct project));
	
	pr->n_cuts = argc - optind;
	pr->cuts = (double*) malloc(sizeof(double)*pr->n_cuts);
	pr->stop_after_ts = DBL_MAX;
	pr->last_flush = 0;
	pr->size_diff = size_diff;
	
	for (i=optind; i < argc; i++) {
		char *end;
		if (((i - optind) % 2 == 1) && !strcmp(argv[i], "-")) {
			pr->stop_after_ts = pr->cuts[i - optind - 1];
			pr->cuts[i - optind] = DBL_MAX;
		} else {
			pr->cuts[i - optind] = strtod(argv[i], &end);
			if (end == argv[i] || *end != 0) {
				av_log(NULL, AV_LOG_ERROR, "error while parsing cut point: %s\n", argv[i]);
			}
		}
	}
	
	#ifdef DEBUG
// 	av_log_set_level(AV_LOG_DEBUG);
	#else
	if (verbosity_level)
		av_log_set_level(atoi(verbosity_level));
	#endif
	
	if (profile) {
		ret = parse_profile(pr, profile);
		if (ret)
			return ret;
	}
	
	// TODO precise version
	#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(56,56,0)
	av_register_all();
	#endif
	
	/*
	 * open input file
	 */
	
	pr->in_fctx = 0;
	if ((ret = avformat_open_input(&pr->in_fctx, inputf, NULL, NULL)) < 0) {
		av_log(NULL, AV_LOG_ERROR, "cannot open input file, error %d\n", ret);
		return ret;
	}
	
	if ((ret = avformat_find_stream_info(pr->in_fctx, NULL)) < 0) {
		av_log(NULL, AV_LOG_ERROR, "Cannot get stream info, error %d\n", ret);
		return ret;
	}
	
	pr->n_stream_ids = 0;
	pr->stream_ids = 0;
	pr->in_codec_ctx = (AVCodecContext**) calloc(1, sizeof(AVCodecContext*) * pr->in_fctx->nb_streams);
	
	for (i = 0; i < pr->in_fctx->nb_streams; i++) {
		AVCodecContext *codec_ctx;
		AVCodecParameters *par;
		const AVCodec *codec;
		char skip;
		
		skip = 0;
		for (j = 0; j < n_skip_streams; j++) {
			if (i == skip_streams[j]) {
				skip = 1;
				break;
			}
		}
		if (skip)
			continue;
		
		par = pr->in_fctx->streams[i]->codecpar;
		codec = avcodec_find_decoder(par->codec_id);
		codec_ctx = avcodec_alloc_context3(codec);
		if (!codec_ctx) {
			av_log(NULL, AV_LOG_ERROR, "Failed to alloc video decoder context\n");
			continue;
		}
		
		ret = avcodec_parameters_to_context(codec_ctx, par);
		if (ret < 0) {
			av_log(NULL, AV_LOG_ERROR, "Failed to copy codec parameters to decoder context\n");
			avcodec_free_context(&codec_ctx);
			continue;
		}
		pr->in_codec_ctx[i] = codec_ctx;
		
		if (codec_ctx->codec_type == AVMEDIA_TYPE_VIDEO) {
			codec_ctx->framerate = av_guess_frame_rate(pr->in_fctx, pr->in_fctx->streams[i], NULL);
			codec_ctx->time_base = av_inv_q(codec_ctx->framerate);
		} else {
			codec_ctx->time_base = (AVRational){1, codec_ctx->sample_rate};
		}
		
		if (codec_ctx->codec_type == AVMEDIA_TYPE_VIDEO) { // || codec_ctx->codec_type == AVMEDIA_TYPE_AUDIO) {
			ret = avcodec_open2(codec_ctx, codec, NULL);
			if (ret < 0) {
				av_log(NULL, AV_LOG_ERROR, "Failed to open decoder for stream %u, error %d\n", i, ret);
				avcodec_free_context(&codec_ctx);
				return ret;
			}
			
			// detect h264 format
			if (codec_ctx->codec_id == AV_CODEC_ID_H264 && codec_ctx->extradata_size > 2) {
				char nalu_start_code1[] = {0x0,0x0,0x1};
				char nalu_start_code2[] = {0x0,0x0,0x0,0x1};
				
				// AVCUT_DUMP_CHAR(codec_ctx->extradata, 4);
				
				struct codeccontext *cctx;
				cctx = av_malloc(sizeof(struct codeccontext));
				if (!cctx) {
					av_log(NULL, AV_LOG_ERROR, "malloc codeccontext failed\n");
					avcodec_free_context(&codec_ctx);
					return AVERROR_UNKNOWN;
				}
				codec_ctx->opaque = cctx;
				
// 				for (j=0;j<16;j++) printf("%x ", codec_ctx->extradata[j] ); printf("\n");
				
				if (!memcmp(codec_ctx->extradata, nalu_start_code1, 3) ||
					!memcmp(codec_ctx->extradata, nalu_start_code2, 4))
				{
					cctx->h264_avcc_format = 0;
					av_log(NULL, AV_LOG_DEBUG, "detected h264 in annexb format\n");
				} else {
					cctx->h264_avcc_format = 1;
					av_log(NULL, AV_LOG_DEBUG, "detected h264 in avcc format\n");
				}
			}
		}
		
		switch (codec_ctx->codec_type) {
			case AVMEDIA_TYPE_VIDEO:
			case AVMEDIA_TYPE_AUDIO:
			case AVMEDIA_TYPE_SUBTITLE:
				pr->n_stream_ids++;
				pr->stream_ids = (unsigned int*) realloc(pr->stream_ids, sizeof(unsigned int)*pr->n_stream_ids);
				pr->stream_ids[pr->n_stream_ids-1] = i;
				break;
			default: break;
		}
	}
	
	av_dump_format(pr->in_fctx, 0, inputf, 0);
	
	/*
	 * open output file
	 */
	
	{
		struct stat st;
		if (stat(outputf, &st) == 0) {
			av_log(NULL, AV_LOG_ERROR, "error, output file \"%s\" already exists\n", outputf);
			exit(1);
		}
	}
	
	avformat_alloc_output_context2(&pr->out_fctx, NULL, NULL, outputf);
	if (!pr->out_fctx) {
		av_log(NULL, AV_LOG_ERROR, "Could not create output context\n");
		return AVERROR_UNKNOWN;
	}
	
	pr->out_codec_ctx = (AVCodecContext**) calloc(1, sizeof(AVCodecContext*) * pr->n_stream_ids);
	
	// copy most properties from the input streams to the output streams
	for (j = 0; j < pr->n_stream_ids; j++) {
		ret = open_encoder(pr, j);
		if (ret)
			return ret;
	}
	
	// initialize bitstream filters (NOTE this expects that the file does not contain
	// more than one video stream!)
	for (i = 0; i < pr->in_fctx->nb_streams; i++) {
		if (pr->in_fctx->streams[i]->codecpar->codec_type != AVMEDIA_TYPE_VIDEO)
			continue;
		
		// initialize bitstream filters
		const AVBitStreamFilter *bsf, *bsf2;
		bsf = av_bsf_get_by_name("h264_mp4toannexb");
		if (!bsf) {
			av_log(NULL, AV_LOG_ERROR, "av_bsf_get_by_name(h264_to_annexb) failed\n");
			return AVERROR_UNKNOWN;
		}
		bsf2 = av_bsf_get_by_name("dump_extra");
		if (!bsf2) {
			av_log(NULL, AV_LOG_ERROR, "av_bsf_get_by_name(dump_extra) failed\n");
			return AVERROR_UNKNOWN;
		}
		ret = av_bsf_alloc(bsf, &pr->bsf_h264_to_annexb);
		if (ret < 0) {
			av_log(NULL, AV_LOG_ERROR, "av_bsf_alloc(bsf_h264_to_annexb)\n");
			return AVERROR_UNKNOWN;
		}
		ret = av_bsf_alloc(bsf2, &pr->bsf_dump_extra);
		if (ret < 0) {
			av_log(NULL, AV_LOG_ERROR, "av_bsf_alloc(bsf_dump_extra)\n");
			return AVERROR_UNKNOWN;
		}
		
		ret = avcodec_parameters_copy(pr->bsf_h264_to_annexb->par_in, pr->in_fctx->streams[i]->codecpar);
		if (ret < 0) {
			av_log(NULL, AV_LOG_ERROR, "Failed to copy codec parameters to bsf_h264_to_annexb\n");
			return AVERROR_UNKNOWN;
		}
		ret = avcodec_parameters_copy(pr->bsf_dump_extra->par_in, pr->in_fctx->streams[i]->codecpar);
		if (ret < 0) {
			av_log(NULL, AV_LOG_ERROR, "Failed to copy codec parameters to bsf_dump_extra\n");
			return AVERROR_UNKNOWN;
		}
		ret = av_bsf_init(pr->bsf_h264_to_annexb);
		if (ret < 0) {
			av_log(NULL, AV_LOG_ERROR, "av_bsf_alloc(bsf_h264_to_annexb)\n");
			return AVERROR_UNKNOWN;
		}
		ret = av_bsf_init(pr->bsf_dump_extra);
		if (ret < 0) {
			av_log(NULL, AV_LOG_ERROR, "av_bsf_alloc(bsf_dump_extra)\n");
			return AVERROR_UNKNOWN;
		}
	}
	
	if (!pr->bsf_dump_extra || !pr->bsf_h264_to_annexb) {
		av_log(NULL, AV_LOG_ERROR, "error while initializing bitstream filters \"dump_extra\" and \"h264_mp4toannexb\"\n");
		exit(1);
	}
	
	if (!(pr->out_fctx->oformat->flags & AVFMT_NOFILE)) {
		ret = avio_open(&pr->out_fctx->pb, outputf, AVIO_FLAG_WRITE);
		if (ret < 0) {
			av_log(NULL, AV_LOG_ERROR, "Cannot open output file '%s', error %d\n", outputf, ret);
			return ret;
		}
	}
	
	// copy main metadata
	av_dict_copy(&pr->out_fctx->metadata, pr->in_fctx->metadata, 0);
	
	av_dump_format(pr->out_fctx, 0, outputf, 1);
	
	ret = avformat_write_header(pr->out_fctx, NULL);
	if (ret < 0) {
		av_log(NULL, AV_LOG_ERROR, "Cannot write header to '%s', error %d\n", outputf, ret);
		return ret;
	}
	
	for (j = 0; j < pr->n_stream_ids; j++) {
		i = pr->stream_ids[j];
		
		#define DUMP_TB(tb) "%5d / %5d\n", (tb)->num, (tb)->den
		av_log(NULL, AV_LOG_DEBUG, "\n");
		av_log(NULL, AV_LOG_DEBUG, "stream %d in:\n", i);
		av_log(NULL, AV_LOG_DEBUG, "stream: " DUMP_TB(&pr->in_fctx->streams[i]->time_base));
		av_log(NULL, AV_LOG_DEBUG, "codec:  " DUMP_TB(&pr->in_codec_ctx[i]->time_base));
		av_log(NULL, AV_LOG_DEBUG, "ticks_per_frame: %d\n", pr->in_codec_ctx[i]->ticks_per_frame);
		av_log(NULL, AV_LOG_DEBUG, "stream %d out:\n", j);
		av_log(NULL, AV_LOG_DEBUG, "stream: " DUMP_TB(&pr->out_fctx->streams[j]->time_base));
		av_log(NULL, AV_LOG_DEBUG, "codec:  " DUMP_TB(&pr->out_codec_ctx[j]->time_base));
		av_log(NULL, AV_LOG_DEBUG, "ticks_per_frame: %d\n", pr->out_codec_ctx[j]->ticks_per_frame);
	}
	av_log(NULL, AV_LOG_DEBUG, "\n");
	
	
	struct packet_buffer *sbuffer;
	sbuffer = (struct packet_buffer*) av_calloc(1, sizeof(struct packet_buffer) * pr->n_stream_ids);
	
	long dts_offset = 0;
	if (pr->has_b_frames) {
		/*
		 * determine a safe starting value for the DTS from the GOP size
		 *
		 * DTS is allowed to be negative in order to satisfy PTS >= DTS requirement.
		 * We might need to decode a P frame before a B frame while the latter has
		 * smaller PTS than the P frame.
		 */
		
		long newo;
		for (j = 0; j < pr->n_stream_ids; j++) {
			i = pr->stream_ids[j];
			
			if (pr->out_codec_ctx[j]->time_base.den == 0)
				continue;
			if (pr->out_codec_ctx[j]->gop_size == 0)
				continue;
			
			// use -gop_size as start for DTS and scale it from codec to stream timebase
			newo = pr->in_codec_ctx[i]->gop_size;
			newo = 0 - pr->out_codec_ctx[j]->ticks_per_frame *
				av_rescale_q(newo, pr->out_codec_ctx[j]->time_base, pr->out_fctx->streams[j]->time_base);
			
			if (newo < dts_offset)
				dts_offset = newo;
		}
	}
	
	av_log(NULL, AV_LOG_DEBUG, "initial DTS %ld\n", dts_offset);
	
	// set the initial DTS for each stream
	for (j = 0; j < pr->n_stream_ids; j++) {
		sbuffer[j].next_dts = dts_offset;
	}
	
	// start processing the input
	int current_percent = -1;
	pr->stop_reading = 0;
	while (!pr->stop_reading) {
		unsigned int stream_index;
		AVPacket packet = { .data = NULL, .size = 0 };
		
		if ((ret = av_read_frame(pr->in_fctx, &packet)) < 0) {
			if (ret != AVERROR_EOF)
				av_log(NULL, AV_LOG_ERROR, "error while reading next frame: %d\n", ret);
			break;
		}
		
		// we buffer multiple frames, this avoids that avcodec_decode_video2 overwrites our data
		av_packet_make_refcounted(&packet);
		
		if (pr->in_fctx->streams[packet.stream_index]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
			pr->video_packets_read++;
			if (max_framecount > 0) {
				int percent = ((pr->video_packets_read) * 100) / max_framecount;
				
				if (percent > current_percent) {
					if (current_percent >= 0) {
						// move cursor up one line, erase line, return cursor to first column
						printf("\033[A\33[2K\r");
					}
					
					fprintf(stdout, "processed %03d percent of all frames\n", percent);
					fflush(stdout);
					current_percent = percent;
				}
			}
		} else {
			pr->other_packets_read++;
		}
		// calculate output stream_index from packet's input stream_index
		for (i=0; i<pr->n_stream_ids; i++) {
			if (pr->stream_ids[i] == packet.stream_index) {
				stream_index = i;
				break;
			}
		}
		if (i==pr->n_stream_ids) {
			// skip packet
			continue;
		}
		
		// check if our buffer is large enough
		if (sbuffer[stream_index].n_pkts == sbuffer[stream_index].length) {
			if (sbuffer[stream_index].pkts == 0)
				sbuffer[stream_index].length = 4;
			sbuffer[stream_index].length *= 2;
			sbuffer[stream_index].pkts = (AVPacket*) av_realloc(sbuffer[stream_index].pkts,
					sizeof(AVPacket)*sbuffer[stream_index].length);
			sbuffer[stream_index].frames = (AVFrame**) av_realloc(sbuffer[stream_index].frames,
					sizeof(AVFrame*)*sbuffer[stream_index].length);
			sbuffer[stream_index].stream_index = stream_index;
		}
		
		memcpy(&sbuffer[stream_index].pkts[sbuffer[stream_index].n_pkts], &packet, sizeof(AVPacket));
		sbuffer[stream_index].n_pkts++;
		
		// decode packet (and start flushing the buffer if necessary)
		ret = decode_packet(pr, sbuffer, stream_index, &packet);
		if (ret < 0 && ret != AVERROR_EOF)
			return ret;
	}
	
	av_log(NULL, AV_LOG_DEBUG, "flushing decoder...\n");
	
	// if no more packet can be read, there might be still data in the queues, hence flush them if necessary
	for (j = 0; j < pr->n_stream_ids; j++) {
		if (!pr->out_codec_ctx[j]->codec || !(pr->out_codec_ctx[j]->codec->capabilities & AV_CODEC_CAP_DELAY))
			continue;
		
		while (1) {
			ret = decode_packet(pr, sbuffer, j, 0);
			if (ret == 0)
				break;
			if (ret < 0)
				return ret;
		}
	}
	
	av_log(NULL, AV_LOG_DEBUG, "flushing buffer...\n");
	
	// more flushing
	for (j = 0; j < pr->n_stream_ids; j++)
		flush_packet_buffer(pr, &sbuffer[j], 1);
	
	// conclude writing
	av_write_trailer(pr->out_fctx);
	
	/*
	 * cleanup
	 */
	
	av_bsf_free(&pr->bsf_h264_to_annexb);
	av_bsf_free(&pr->bsf_dump_extra);
	
	for (j = 0; j < pr->n_stream_ids; j++) {
		av_freep(&sbuffer[j].pkts);
		av_freep(&sbuffer[j].frames);
		
		if (pr->out_codec_ctx[j] && avcodec_is_open(pr->out_codec_ctx[j]))
			avcodec_close(pr->out_codec_ctx[j]);
	}
	
	for (i = 0; i < pr->in_fctx->nb_streams; i++) {
		if (pr->in_codec_ctx[i]) {
			av_freep(&pr->in_codec_ctx[i]->opaque);
			if (avcodec_is_open(pr->in_codec_ctx[i]))
				avcodec_close(pr->in_codec_ctx[i]);
		}
	}
	
	avformat_close_input(&pr->in_fctx);
	if (!(pr->out_fctx->oformat->flags & AVFMT_NOFILE))
		avio_closep(&pr->out_fctx->pb);
	avformat_free_context(pr->out_fctx);
	
	av_log(NULL, AV_LOG_INFO, "Video packets read: %zu\n", pr->video_packets_read);
	av_log(NULL, AV_LOG_INFO, "Video packets decoded: %zu\n", pr->video_packets_decoded);
	av_log(NULL, AV_LOG_INFO, "Video frames encoded: %zu\n", pr->video_frames_encoded);
	av_log(NULL, AV_LOG_INFO, "Video packets written: %zu\n", pr->video_packets_written);
	av_log(NULL, AV_LOG_INFO, "Other packets read: %zu\n", pr->other_packets_read);
	av_log(NULL, AV_LOG_INFO, "Other packets written: %zu\n", pr->other_packets_written);
	
	if (pr->n_cuts > 0) {
		struct stat st;
		size_t size, outputf_len;
		FILE *check_script = 0;
		
		stat(inputf, &st);
		size = st.st_size;
		stat(outputf, &st);
		av_log(NULL, AV_LOG_INFO, "\nFile size reduction: %f MB\n", (size - (double) st.st_size) / (1000*1000) );
		
		// print some info to check the cutpoints of the resulting video
		av_log(NULL, AV_LOG_INFO, "\ncutting points in \"%s\" at: ", outputf);
		double removed = 0;
		for (i=0;i<pr->n_cuts;i+=2) {
			double c = pr->cuts[i]-removed;
			av_log(NULL, AV_LOG_INFO, "%fs (%um %2us) ", c, (unsigned int)c/60, (unsigned int)c % 60);
			
			removed += pr->cuts[i+1] - pr->cuts[i];
		}
		av_log(NULL, AV_LOG_INFO, "\n");
		
		printf("\nYou can check the cutting points with:\nmpv \"edl://");
		
		if (create_check_script) {
			check_script = fopen("avcut_check_cutpoints.sh", "a");
			fprintf(check_script, "mpv \"edl://");
		}
		
		outputf_len = strlen(outputf);
		removed = 0;
		for (i=0;i<pr->n_cuts;i+=2) {
			double c = pr->cuts[i]-removed;
			
			printf("%%%zu%%%s,%f,%f;", outputf_len, outputf, (c>=10 ? c-10 : 0 ), (c>=10 ? 20 : c + 10));
			
			if (check_script)
				fprintf(check_script, "%%%zu%%%s,%f,%f;", outputf_len, outputf, (c>=10 ? c-10 : 0 ), (c>=10 ? 20 : c + 10));
			
			removed += pr->cuts[i+1] - pr->cuts[i];
		}
		if (check_script)
			fprintf(check_script, "\"\n\n");
		
		printf("\"\n");
	}
	
	av_freep(&pr->cuts);
	av_freep(&sbuffer);
	
	free(pr->stream_ids);
	free(pr->in_codec_ctx);
	free(pr->out_codec_ctx);
	
	return 0;
}
