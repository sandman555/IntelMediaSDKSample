/*
 * Def.h
 *
 *  Created on: Apr 7, 2021
 *      Author: jason
 */

#ifndef SRC_DEF_H_
#define SRC_DEF_H_

#include <stdint.h>

extern "C"{
	#include "mfxvideo.h"
}

enum class VideoCodec{
	NONE,
	AVC,
	HEVC
};

enum class VideoBaseBandFmt{
	NONE,
	YUV420P,
	YUV420P10LE,
	NV12,
	P010LE
};

struct VideoParams{
	VideoCodec codec = VideoCodec::NONE;
	int width = 0;
	int height = 0;
	int frame_rate_num = 0;
	int frame_rate_den = 0;
	int gop_size = 0;
	int b_frames = 0;
	int bit_rate = 0;
	int bit_depth = 8;
};

struct VideoRawData{
	int width = 0;
	int height = 0;
	int line_size[3] = {0};
	VideoBaseBandFmt fmt = VideoBaseBandFmt::NONE;
	int64_t pts = 0;
	unsigned char * buffer[3] = {0};
};

struct VideoBitStream {
	mfxBitstream *mfx_bit_stream = nullptr;
	mfxSyncPoint * sync_p = nullptr;
};

struct MFXSurface {
	mfxFrameSurface1 * surface = nullptr;
	mfxSyncPoint sync = nullptr;
	bool used = false;
};

typedef void(*VideoFrameCB)(VideoRawData *data, void * user_data);


#endif /* SRC_DEF_H_ */
