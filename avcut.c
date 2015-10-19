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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

AVBitStreamFilterContext *bsf_dump_extra;

// buffer management struct for a stream
struct packet_buffer {
	unsigned int stream_index;
	
	AVPacket *pkts;
	size_t n_pkts; // number of pkts in .pkts
	
	AVFrame **frames;
	size_t n_frames; // number of frames in .frames
	
	size_t length; // allocated array length of .pkts and .frames
	
	long next_dts; // DTS of the next packet that will be written
};

// global state struct
struct project {
	AVFormatContext *in_fctx;
	AVFormatContext *out_fctx;
	
	double *cuts; // [ first_excluded_frame, first_included_frame, ...]
	size_t n_cuts;
};

// debug function that dumps a frame into a PPM file
// TODO: does not work reliably
void dumpframe(AVFrame *frame) {
	FILE *pFile;
	char szFilename[32];
	int  y;
	
	sprintf(szFilename, "frame%d.ppm", frame->coded_picture_number);
	pFile=fopen(szFilename, "wb");
	if(pFile==NULL)
		return;
	
	fprintf(pFile, "P6\n%d %d\n255\n", frame->width, frame->height);
	
	for(y=0; y<frame->height; y++)
		fwrite(frame->data[0]+y*frame->linesize[0], 1, frame->width*3, pFile);
	
	fclose(pFile);
}

// encode a frame and write the resulting packet into the output file
int encode_write_frame(struct project *pr, struct packet_buffer *s, AVFrame *frame, int *got_frame_p) {
	AVPacket *out_pkt;
	AVPacket enc_pkt = { .data = NULL, .size = 0 };
	int got_frame, ret;
	AVStream *ostream = pr->out_fctx->streams[s->stream_index];
	
	
	if (frame)
		av_log(NULL, AV_LOG_DEBUG, "enc frame pts: %" PRId64 " pkt_pts: %" PRId64 " pkt_dts: %" PRId64 " type: %c\n",
			frame->pts, frame->pkt_pts, frame->pkt_dts, av_get_picture_type_char(frame->pict_type));
	
	av_init_packet(&enc_pkt);
	
	ret = avcodec_encode_video2(ostream->codec, &enc_pkt, frame, &got_frame);
	if (ret < 0) {
		av_log(NULL, AV_LOG_ERROR, "error while encoding frame, error %d\n", ret);
		return ret;
	}
	
	if (got_frame) {
		AVCodecContext *codec = pr->out_fctx->streams[s->stream_index]->codec;
		
		if (enc_pkt.duration == 0)
			enc_pkt.duration = ostream->codec->ticks_per_frame;
		enc_pkt.stream_index = s->stream_index;
		
		av_packet_rescale_ts(&enc_pkt, ostream->codec->time_base, ostream->time_base);
		
		enc_pkt.dts = s->next_dts;
		s->next_dts += enc_pkt.duration;
		
		out_pkt = &enc_pkt;
		
		// copy the header to the beginning of each key frame if we use a global header
		if (codec->flags & CODEC_FLAG_GLOBAL_HEADER)
			av_bitstream_filter_filter(bsf_dump_extra, codec, NULL, &out_pkt->data, &out_pkt->size, out_pkt->data, out_pkt->size, out_pkt->flags & AV_PKT_FLAG_KEY);
		
		av_log(NULL, AV_LOG_DEBUG, "write v enc size: %d pts: %" PRId64 " dts: %" PRId64 " - to %f\n", out_pkt->size, out_pkt->pts, out_pkt->dts,
			out_pkt->pts*ostream->time_base.num/(double)ostream->time_base.den);
		
		ret = av_interleaved_write_frame(pr->out_fctx, out_pkt);
		if (ret < 0) {
			av_log(NULL, AV_LOG_ERROR, "error while writing packet, error %d\n", ret);
			return ret;
		}
		
		av_frame_free(&frame);
		av_free_packet(&enc_pkt);
	}
	
	if (got_frame_p)
		*got_frame_p = got_frame;
	
	return 0;
}

// calculate stream timestamp of frame using its PTS
double frame_pts2ts(struct project *pr, struct packet_buffer *s, AVFrame *frame) {
	return frame->pts * av_q2d(pr->in_fctx->streams[s->stream_index]->codec->time_base);
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

// check if the complete buffer will be used or if a cutpoint lies in this interval
char is_buffer_uncut(struct project *pr, struct packet_buffer *s, unsigned long last_iframe) {
	double buf_start, buf_end;
	size_t i;
	
	if (!last_iframe) {
		buf_start = s->pkts[0].pts * av_q2d(pr->in_fctx->streams[s->stream_index]->time_base);
		buf_end = s->pkts[s->n_pkts-1].pts * av_q2d(pr->in_fctx->streams[s->stream_index]->time_base);
	} else {
		buf_start = frame_pts2ts(pr, s, s->frames[0]);
		buf_end = frame_pts2ts(pr, s, s->frames[last_iframe]);
	}
	
	av_log(NULL, AV_LOG_DEBUG, "check buffer %f : %f\n", buf_start, buf_end);
	
	for (i=0; i < pr->n_cuts; i++) {
		// check if any cutpoint lies between buffer start and end
		if (buf_start < pr->cuts[i] && pr->cuts[i] < buf_end)
			return 0;
		
		// check if buffer lies between cutpoints
		if ((i % 2 == 0) && (pr->cuts[i] <= buf_start && buf_end <= pr->cuts[i+1]))
			return 0;
		
		// stop if further cuts lie behind current buffer
		if (buf_end < pr->cuts[i])
			return 1;
	}
	
	return 1;
}

// get the number of dropped frames prior to source timestamp $ts
double get_n_dropped_pkgs_at_ts(struct project *pr, struct packet_buffer *s, double ts) {
	size_t i;
	double result = 0;
	
	for (i=0; i < pr->n_cuts; i+=2) {
		if (pr->cuts[i+1] <= ts) {
			result += (pr->cuts[i+1] - pr->cuts[i]) / av_q2d(pr->in_fctx->streams[s->stream_index]->time_base);
		} else {
			break;
		}
	}
	
	av_log(NULL, AV_LOG_DEBUG, "dropped frames at %f: %f\n", ts, result);
	
	return result;
}

// process packet buffer - either copy, ignore or reencode packets in the packet buffer according to the cut list
void flush_packet_buffer(struct project *pr, struct packet_buffer *s) {
	size_t i, j, inter_pkt;
	int ret;
	char copy_complete_buffer = 0;
	double offset = 0;
	char last_frame_dropped = 1;
	double ts;
	
	
	if (pr->in_fctx->streams[s->stream_index]->codec->codec_type != AVMEDIA_TYPE_VIDEO) {
		// check if we can copy the complete buffer or if we have to check each packet individually
		copy_complete_buffer = is_buffer_uncut(pr, s, 0);
		
		for (i=0;i<s->n_pkts;i++) {
			ts = s->pkts[i].pts * av_q2d(pr->in_fctx->streams[s->stream_index]->time_base);
			
			if (copy_complete_buffer || ts_included(pr, ts)) {
				// recalculate PTS offset after we continue to write frames
				if (last_frame_dropped)
					offset = get_n_dropped_pkgs_at_ts(pr, s, ts);
				
				s->pkts[i].pts -= offset;
				
				av_packet_rescale_ts(&s->pkts[i], pr->in_fctx->streams[s->stream_index]->time_base,
								 pr->out_fctx->streams[s->stream_index]->time_base);
				
				s->pkts[i].dts = s->next_dts;
				s->next_dts += s->pkts[i].duration;
				
				av_log(NULL, AV_LOG_DEBUG, "write a cpy pts: %" PRId64 " dts: %" PRId64 " - %f to %f\n", s->pkts[i].pts, s->pkts[i].dts,
					ts, s->pkts[i].pts * av_q2d(pr->out_fctx->streams[s->stream_index]->time_base));
				
				ret = av_interleaved_write_frame(pr->out_fctx, &s->pkts[i]);
				if (ret < 0) {
					av_log(NULL, AV_LOG_ERROR, "error while writing packet, error %d\n", ret);
					exit(ret);
				}
				
				last_frame_dropped = 0;
			} else {
				last_frame_dropped = 1;
			}
			
			av_free_packet(&s->pkts[i]);
		}
		
		s->n_pkts = 0;
		return;
	}
	
	// check if we can copy the complete buffer or if we have to check each packet individually
	copy_complete_buffer = is_buffer_uncut(pr, s, s->n_frames-2);
	
	inter_pkt = s->n_pkts;
	// get packet with second (and therefore last) inter frame
	for (i=0;i<s->n_pkts;i++) {
		// we cannot compare packet.dts with frame.pkt_dts as they differ by 2 (maybe caused by multithreading)
		if ((s->pkts[i].pts != AV_NOPTS_VALUE && s->pkts[i].pts == s->frames[s->n_frames-1]->pkt_pts) ||
			(s->pkts[i].pts == AV_NOPTS_VALUE && s->pkts[i].dts == s->frames[s->n_frames-1]->coded_picture_number))
		{
			#ifndef USING_LIBAV
			// libav is missing pkt_size
			if (s->frames[s->n_frames-1]->pkt_size != s->pkts[i].size) {
				av_log(NULL, AV_LOG_ERROR, "size mismatch %" PRId64 ":%d %" PRId64 ":%d\n", s->n_frames-1, s->frames[s->n_frames-1]->pkt_size, i, s->pkts[i].size);
				exit(1);
			}
			#endif
			inter_pkt = i;
			break;
		}
		if (!copy_complete_buffer)
			av_free_packet(&s->pkts[i]);
	}
	if (inter_pkt == s->n_pkts) {
		av_log(NULL, AV_LOG_ERROR, "packet for second I frame (cpn %d) not found\n", s->frames[s->n_frames-1]->coded_picture_number);
		exit(1);
	}
	
	// TODO: check if we can simply cut if the last frame is a P-frame
	
	if (!copy_complete_buffer) {
		char frame_written = 0;
		
		// check which frames will be included and encode them
		for (i=0;i<s->n_frames-1;i++) {
			if (!s->frames[i]) {
				av_log(NULL, AV_LOG_ERROR, "no frame %zu\n", i);
				exit(1);
			}
			
			ts = frame_pts2ts(pr, s, s->frames[i]);
			
			// recalculate PTS offset after we continue to write frames
			if (last_frame_dropped)
				offset = get_n_dropped_pkgs_at_ts(pr, s, ts);
			
			s->frames[i]->pts -= av_rescale_q(offset, pr->in_fctx->streams[s->stream_index]->time_base, pr->in_fctx->streams[s->stream_index]->codec->time_base);
			
			if (ts_included(pr, ts)) {
				if (last_frame_dropped)
					av_log(NULL, AV_LOG_DEBUG, "new PTS offset %f\n", offset);
				
				// force last to-be-written frame to be an I-frame
				// this frame is the last if next frame is not included anymore
// 				if (last_frame_dropped || (i+1 < s->n_frames-1 && !ts_included(pr, frame_pts2ts(pr, s, s->frames[i+1]))) ) {
// 					s->frames[i]->pict_type = AV_PICTURE_TYPE_I;
// 					s->frames[i]->key_frame = 1;
// 				} else {
					#ifndef USING_LIBAV
					s->frames[i]->pict_type = AV_PICTURE_TYPE_NONE;
					#else
					s->frames[i]->pict_type = 0;
					#endif
// 				}
				
				ret = encode_write_frame(pr, s, s->frames[i], 0);
				if (ret < 0) {
					av_log(NULL, AV_LOG_ERROR, "encode_write_frame failed, error %d\n", ret);
					exit(ret);
				}
				
				last_frame_dropped = 0;
				frame_written = 1;
			} else {
				last_frame_dropped = 1;
				av_frame_free(&s->frames[i]);
			}
		}
		
		// if the encoder emits packets delayed in time, flush the encoder to receive all remaining packets in the queue
		if (frame_written && pr->out_fctx->streams[s->stream_index]->codec->codec->capabilities & CODEC_CAP_DELAY) {
			av_log(NULL, AV_LOG_DEBUG, "Local flushing stream #%u\n", s->stream_index);
			while (1) {
				int got_frame;
				
				ret = encode_write_frame(pr, s, 0, &got_frame);
				if (ret < 0) {
					#ifndef USING_LIBAV
					av_log(NULL, AV_LOG_ERROR, "encode_write_frame failed, error %d: %s\n", ret, av_err2str(ret));
					#else
					char errbuf[256];
					av_strerror(ret, errbuf, 256);
					av_log(NULL, AV_LOG_ERROR, "encode_write_frame failed, error %d: %s\n", ret, errbuf);
					#endif
					exit(ret);
				}
				if (!got_frame) {
					av_log(NULL, AV_LOG_DEBUG, "flush end\n");
					break;
				}
			}
		}
		
		if (frame_written) {
			// Some encoders do not like new frames after we flushed them.
			// Hence, we restart the encoder.
			AVCodecContext *out_cctx = pr->out_fctx->streams[s->stream_index]->codec;
			
			out_cctx->codec->close(out_cctx);
			out_cctx->codec->init(out_cctx);
		}
	} else {
		for (i=0;i<inter_pkt;i++) {
			AVFrame *frame;
			
			// find frame for current packet to determine the PTS
			frame = 0;
			for (j=0;j<s->n_frames;j++) {
				if ((s->pkts[i].pts != AV_NOPTS_VALUE && s->pkts[i].pts == s->frames[j]->pkt_pts) ||
					(s->pkts[i].pts == AV_NOPTS_VALUE && s->pkts[i].dts == s->frames[j]->coded_picture_number))
				{
					#ifndef USING_LIBAV
					// libav is missing pkt_size
					if (s->frames[j]->pkt_size != s->pkts[i].size) {
						av_log(NULL, AV_LOG_ERROR, "size mismatch %" PRId64 ":%d %" PRId64 ":%d (dts %" PRId64 ")\n", j, s->frames[j]->pkt_size, i, s->pkts[i].size, s->pkts[i].dts);
						exit(1);
					}
					#endif
					
					frame = s->frames[j];
					break;
				}
			}
			if (!frame) {
				av_log(NULL, AV_LOG_ERROR, "frame for pkt #%zd (dts %" PRId64 ") not found\n", i, s->pkts[i].dts);
				exit(1);
			}
			
			s->pkts[i].pts = av_rescale_q(frame->pts, pr->in_fctx->streams[s->stream_index]->codec->time_base, pr->in_fctx->streams[s->stream_index]->time_base);
			
			ts = frame_pts2ts(pr, s, s->frames[i]);
			
			// recalculate PTS offset after we continue to write frames
			if (last_frame_dropped)
				offset = get_n_dropped_pkgs_at_ts(pr, s, ts);
			
			s->pkts[i].pts -= offset;
			
			if (ts_included(pr, ts)) {
				av_packet_rescale_ts(&s->pkts[i], pr->in_fctx->streams[s->stream_index]->time_base,
								 pr->out_fctx->streams[s->stream_index]->time_base);
				
				s->pkts[i].dts = s->next_dts;
				s->next_dts += s->pkts[i].duration;
				
				av_log(NULL, AV_LOG_DEBUG, "write v cpy size: %d pts: %" PRId64 " dts: %" PRId64 " - %f to %f\n", s->pkts[i].size, s->pkts[i].pts, s->pkts[i].dts,
						ts, s->pkts[i].pts * pr->out_fctx->streams[s->stream_index]->time_base.num /
							(double)pr->out_fctx->streams[s->stream_index]->time_base.den
						);
				ret = av_interleaved_write_frame(pr->out_fctx, &s->pkts[i]);
				if (ret < 0) {
					av_log(NULL, AV_LOG_ERROR, "error while writing packet, error %d\n", ret);
					exit(ret);
				}
				last_frame_dropped = 0;
			} else {
				last_frame_dropped = 1;
			}
			
			av_free_packet(&s->pkts[i]);
		}
		for (j=0;j<s->n_frames-1;j++)
			av_frame_free(&s->frames[j]);
	}
	
	// update buffer structures
	memmove(&s->pkts[0], &s->pkts[inter_pkt], sizeof(AVPacket)*(s->n_pkts-inter_pkt));
	s->n_pkts = s->n_pkts - inter_pkt;
	s->frames[0] = s->frames[s->n_frames-1];
	s->n_frames = 1;
}

// decode a video packet and store it in the buffer
int decode_packet(struct project *pr, struct packet_buffer *sbuffer, unsigned int stream_index, AVPacket *packet) {
	enum AVMediaType mtype;
	int got_frame, ret, i;
	AVFrame *frame = NULL;
	AVPacket nullpacket = { .data = NULL, .size = 0 };
	
	if (!packet)
		packet = &nullpacket;
	
	mtype = pr->in_fctx->streams[stream_index]->codec->codec_type;
	
	if (mtype == AVMEDIA_TYPE_VIDEO) {
		if (!(frame = av_frame_alloc())) {
			av_log(NULL, AV_LOG_ERROR, "error while allocating frame\n");
			return AVERROR(ENOMEM);
		}
		
		ret = avcodec_decode_video2(pr->in_fctx->streams[stream_index]->codec, frame, &got_frame, packet);
		if (ret <= 0) {
			av_frame_free(&frame);
			if (ret < 0 )
				av_log(NULL, AV_LOG_ERROR, "Decoding frame failed\n");
			return 0;
		}
		
		if (got_frame) {
			sbuffer[stream_index].frames[sbuffer[stream_index].n_frames] = frame;
			sbuffer[stream_index].n_frames++;
			
			#ifndef USING_LIBAV
			frame->pts = av_frame_get_best_effort_timestamp(frame);
			#else
			if (frame->pkt_pts != AV_NOPTS_VALUE)
				frame->pts = frame->pkt_pts;
			else
				frame->pts = frame->pkt_dts;
			#endif
			
			
			// convert from packet to frame time_base, if necessary
			if (frame->pts == frame->pkt_dts || frame->pts == frame->pkt_pts)
				frame->pts = av_rescale_q(frame->pts, pr->in_fctx->streams[stream_index]->time_base, pr->in_fctx->streams[stream_index]->codec->time_base);
			
			// the first packet in the video buffer is an I frame, if the
			// current packet contains another I frame, flush the buffer
			if (sbuffer[stream_index].n_frames > 1) {
				switch (frame->pict_type) {
					case AV_PICTURE_TYPE_I:
						for (i = 0; i < pr->in_fctx->nb_streams; i++) {
							flush_packet_buffer(pr, &sbuffer[i]);
						}
						break;
					case AV_PICTURE_TYPE_B:
					case AV_PICTURE_TYPE_P:
					default: break;
				}
			}
		} else {
			av_frame_free(&frame);
		}
	}
	
	return 1;
}

int main(int argc, char **argv) {
	unsigned int i;
	int ret;
	char *inputf, *outputf;
	struct project project;
	struct project *pr;
	
	if (argc < 3) {
		av_log(NULL, AV_LOG_ERROR, "Usage: %s <input file> <output file> [<drop_from_ts> <continue_with_ts> ...]\n", argv[0]);
		return 1;
	}
	
	if (argc % 2 != 1) {
		av_log(NULL, AV_LOG_ERROR, "only even number of cut points allowed\n");
		av_log(NULL, AV_LOG_ERROR, "Usage: %s <input file> <output file> [<drop_from_ts> <continue_with_ts> ...]\n", argv[0]);
		return 1;
	}
	
	inputf = argv[1];
	outputf = argv[2];
	
	pr = &project;
	
	pr->n_cuts = argc - 3;
	pr->cuts = (double*) malloc(sizeof(double)*pr->n_cuts);
	
	for (i=3; i < argc; i++) {
		char *end;
		pr->cuts[i-3] = strtod(argv[i], &end);
		if (end == argv[i] || *end != 0) {
			av_log(NULL, AV_LOG_ERROR, "error while parsing cut point: %s\n", argv[i]);
		}
	}
	
	#ifdef DEBUG
	av_log_set_level(AV_LOG_DEBUG);
	#endif
	
	av_register_all();
	
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
	
	for (i = 0; i < pr->in_fctx->nb_streams; i++) {
		AVCodecContext *codec_ctx;
		
		codec_ctx = pr->in_fctx->streams[i]->codec;
		
		// we buffer multiple frames, this avoids that avcodec_decode_video2 overwrites our data
		codec_ctx->refcounted_frames = 1;
		
		if (codec_ctx->codec_type == AVMEDIA_TYPE_VIDEO) { // || codec_ctx->codec_type == AVMEDIA_TYPE_AUDIO) {
			ret = avcodec_open2(codec_ctx, avcodec_find_decoder(codec_ctx->codec_id), NULL);
			if (ret < 0) {
				av_log(NULL, AV_LOG_ERROR, "Failed to open decoder for stream %u, error %d\n", i, ret);
				return ret;
			}
		}
	}
	
	av_dump_format(pr->in_fctx, 0, inputf, 0);
	
	/*
	 * open output file
	 */
	
	#ifndef USING_LIBAV
	avformat_alloc_output_context2(&pr->out_fctx, NULL, NULL, outputf);
	#else
	pr->out_fctx = avformat_alloc_context();
	#endif
	if (!pr->out_fctx) {
		av_log(NULL, AV_LOG_ERROR, "Could not create output context\n");
		return AVERROR_UNKNOWN;
	}
	
	#ifdef USING_LIBAV
	av_strlcpy(pr->out_fctx->filename, outputf, sizeof(pr->out_fctx->filename));
	pr->out_fctx->oformat = av_guess_format(NULL, outputf, NULL);
	if (!pr->out_fctx->oformat) {
		avformat_free_context(pr->out_fctx);
		av_log(NULL, AV_LOG_ERROR, "Could not determine output format\n");
		return AVERROR_UNKNOWN;
	}
	#endif
	
	// copy most properties from the input streams to the output streams
	for (i = 0; i < pr->in_fctx->nb_streams; i++) {
		AVStream *out_stream;
		AVCodecContext *dec_cctx, *enc_cctx;
		
		out_stream = avformat_new_stream(pr->out_fctx, NULL);
		if (!out_stream) {
			av_log(NULL, AV_LOG_ERROR, "Failed allocating output stream\n");
			return AVERROR_UNKNOWN;
		}
		
		dec_cctx = pr->in_fctx->streams[i]->codec;
		enc_cctx = out_stream->codec;
		
		out_stream->time_base = pr->in_fctx->streams[i]->time_base;
		
		// copy stream metadata
		av_dict_copy(&out_stream->metadata, pr->in_fctx->streams[i]->metadata, 0);
		
		if (dec_cctx->codec_type == AVMEDIA_TYPE_VIDEO) {
			AVCodec *encoder;
			
			encoder = avcodec_find_encoder(dec_cctx->codec_id);
			if (!encoder) {
				av_log(NULL, AV_LOG_ERROR, "Encoder not found\n");
				return AVERROR_INVALIDDATA;
			}
			
			ret = avcodec_copy_context(enc_cctx, dec_cctx);
			if (ret < 0) {
				av_log(NULL, AV_LOG_ERROR, "Copying stream context failed\n");
				return ret;
			}
			
			// TODO good values?
			enc_cctx->qmin = 16;
			enc_cctx->qmax = 26;
			enc_cctx->max_qdiff = 4;
			enc_cctx->max_b_frames = 3;
// 			enc_cctx->keyint_min = 200;
// 			enc_cctx->gop_size = 250;
			enc_cctx->thread_count = 1; // spawning more threads causes avcodec_close to free threads multiple times
			
			out_stream->sample_aspect_ratio = pr->in_fctx->streams[i]->sample_aspect_ratio;
			
			if (pr->out_fctx->oformat->flags & AVFMT_GLOBALHEADER)
				enc_cctx->flags |= CODEC_FLAG_GLOBAL_HEADER;
			
			ret = avcodec_open2(enc_cctx, encoder, NULL);
			if (ret < 0) {
				av_log(NULL, AV_LOG_ERROR, "Failed to open encoder for stream %u, error %d\n", i, ret);
				return ret;
			}
		} else if (dec_cctx->codec_type == AVMEDIA_TYPE_UNKNOWN) {
			av_log(NULL, AV_LOG_ERROR, "Error: input stream #%d is of unknown type\n", i);
			return AVERROR_INVALIDDATA;
		} else {
			ret = avcodec_copy_context(pr->out_fctx->streams[i]->codec, pr->in_fctx->streams[i]->codec);
			if (ret < 0) {
				av_log(NULL, AV_LOG_ERROR, "Copying codec context failed, error %d\n", ret);
				return ret;
			}
			
			if (pr->out_fctx->oformat->flags & AVFMT_GLOBALHEADER)
				enc_cctx->flags |= CODEC_FLAG_GLOBAL_HEADER;
		}
	}
	
	bsf_dump_extra = av_bitstream_filter_init("dump_extra");
	if (!bsf_dump_extra) {
		av_log(NULL, AV_LOG_ERROR, "bitstream filter \"dump_extra\" not found");
		exit(1);
	}
	
	if (!(pr->out_fctx->oformat->flags & AVFMT_NOFILE)) {
		ret = avio_open(&pr->out_fctx->pb, outputf, AVIO_FLAG_WRITE);
		if (ret < 0) {
			av_log(NULL, AV_LOG_ERROR, "Cannot open output file '%s', error %d", outputf, ret);
			return ret;
		}
	}
	
	// copy main metadata
	av_dict_copy(&pr->out_fctx->metadata, pr->in_fctx->metadata, 0);
	
	ret = avformat_write_header(pr->out_fctx, NULL);
	if (ret < 0) {
		av_log(NULL, AV_LOG_ERROR, "Cannot write header to '%s', error %d\n", outputf, ret);
		return ret;
	}
	
	av_dump_format(pr->out_fctx, 0, outputf, 1);
	
	for (i = 0; i < pr->in_fctx->nb_streams; i++) {
		#define DUMP_TB(tb) "%5d / %5d\n", (tb)->num, (tb)->den
		av_log(NULL, AV_LOG_DEBUG, "\n");
		av_log(NULL, AV_LOG_DEBUG, "stream %d in:\n", i);
		av_log(NULL, AV_LOG_DEBUG, "stream: " DUMP_TB(&pr->in_fctx->streams[i]->time_base));
		av_log(NULL, AV_LOG_DEBUG, "codec:  " DUMP_TB(&pr->in_fctx->streams[i]->codec->time_base));
		av_log(NULL, AV_LOG_DEBUG, "ticks_per_frame: %d\n", pr->in_fctx->streams[i]->codec->ticks_per_frame);
		av_log(NULL, AV_LOG_DEBUG, "stream %d out:\n", i);
		av_log(NULL, AV_LOG_DEBUG, "stream: " DUMP_TB(&pr->out_fctx->streams[i]->time_base));
		av_log(NULL, AV_LOG_DEBUG, "codec:  " DUMP_TB(&pr->out_fctx->streams[i]->codec->time_base));
		av_log(NULL, AV_LOG_DEBUG, "ticks_per_frame: %d\n", pr->out_fctx->streams[i]->codec->ticks_per_frame);
	}
	av_log(NULL, AV_LOG_DEBUG, "\n");
	
	
	struct packet_buffer *sbuffer;
	sbuffer = (struct packet_buffer*) calloc(1, sizeof(struct packet_buffer) * pr->in_fctx->nb_streams);
	
	// determine a safe starting value for the DTS from the GOP size
	long dts_offset = 0;
	long newo;
	for (i = 0; i < pr->in_fctx->nb_streams; i++) {
		// use -gop_size as start for DTS
		newo = pr->in_fctx->streams[i]->codec->gop_size;
		newo = 0 - pr->out_fctx->streams[i]->codec->ticks_per_frame * 
			av_rescale_q(newo, pr->out_fctx->streams[i]->codec->time_base, pr->out_fctx->streams[i]->time_base);
		if (newo < dts_offset)
			dts_offset = newo;
	}
	
	// set the initial DTS for each stream
	for (i = 0; i < pr->in_fctx->nb_streams; i++) {
		sbuffer[i].next_dts = dts_offset;
		av_log(NULL, AV_LOG_DEBUG, "initial DTS %u: %ld\n", i, sbuffer[i].next_dts);
	}
	
	// start processing the input
	while (1) {
		unsigned int stream_index;
		AVPacket packet = { .data = NULL, .size = 0 };
		
		if ((ret = av_read_frame(pr->in_fctx, &packet)) < 0) {
			if (ret != AVERROR_EOF)
				av_log(NULL, AV_LOG_ERROR, "error while reading next frame: %d\n", ret);
			break;
		}
		
		stream_index = packet.stream_index;
		
		// check if our buffer is large enough
		if (sbuffer[stream_index].n_pkts == sbuffer[stream_index].length) {
			if (sbuffer[stream_index].pkts == 0)
				sbuffer[stream_index].length = 4;
			sbuffer[stream_index].length *= 2;
			sbuffer[stream_index].pkts = (AVPacket*) realloc(sbuffer[stream_index].pkts, sizeof(AVPacket)*sbuffer[stream_index].length);
			sbuffer[stream_index].frames = (AVFrame**) realloc(sbuffer[stream_index].frames, sizeof(AVFrame*)*sbuffer[stream_index].length);
			sbuffer[stream_index].stream_index = stream_index;
		}
		
		memcpy(&sbuffer[stream_index].pkts[sbuffer[stream_index].n_pkts], &packet, sizeof(AVPacket));
		sbuffer[stream_index].n_pkts++;
		
		// decode packet (and start flushing the buffer if necessary)
		ret = decode_packet(pr, sbuffer, stream_index, &packet);
		if (ret == 0)
			break;
		if (ret < 0)
			return ret;
	}
	
	// if no more packet can be read, there might be still data in the queues, hence flush them if necessary
	for (i = 0; i < pr->in_fctx->nb_streams; i++) {
		if (!pr->out_fctx->streams[i]->codec->codec || !(pr->out_fctx->streams[i]->codec->codec->capabilities & CODEC_CAP_DELAY))
			continue;
		
		ret = decode_packet(pr, sbuffer, i, 0);
		if (ret < 0)
			return ret;
	}
	
	// more flushing
	for (i = 0; i < pr->in_fctx->nb_streams; i++) {
		if (!pr->out_fctx->streams[i]->codec->codec || !(pr->out_fctx->streams[i]->codec->codec->capabilities & CODEC_CAP_DELAY))
			continue;
		flush_packet_buffer(pr, &sbuffer[i]);
	}
	
	// conclude writing
	av_write_trailer(pr->out_fctx);
	
	/*
	 * cleanup
	 */
	
	av_bitstream_filter_close(bsf_dump_extra);
	
	for (i = 0; i < pr->in_fctx->nb_streams; i++) {
		free(sbuffer[i].pkts);
		free(sbuffer[i].frames);
		
		if (avcodec_is_open(pr->in_fctx->streams[i]->codec))
			avcodec_close(pr->in_fctx->streams[i]->codec);
		
		if (i < pr->out_fctx->nb_streams && pr->out_fctx->streams[i]->codec && avcodec_is_open(pr->out_fctx->streams[i]->codec))
			avcodec_close(pr->out_fctx->streams[i]->codec);
	}
	
	avformat_close_input(&pr->in_fctx);
	if (!(pr->out_fctx->oformat->flags & AVFMT_NOFILE))
		avio_closep(&pr->out_fctx->pb);
	avformat_free_context(pr->out_fctx);
	
	// print some info to check the cutpoints of the resulting video
	av_log(NULL, AV_LOG_INFO, "cutpoints in \"%s\": ", outputf);
	double removed = 0;
	for (i=0;i<pr->n_cuts;i+=2) {
		double c = pr->cuts[i]-removed;
		av_log(NULL, AV_LOG_INFO, "%fs (%um %2us) ", c, (unsigned int)c/60, (unsigned int)c % 60);
		removed += pr->cuts[i+1] - pr->cuts[i];
	}
	av_log(NULL, AV_LOG_INFO, "\n");
	
	free(pr->cuts);
	free(sbuffer);
	
	return 0;
}