#ifndef _H_VIDEODECODER_
#define _H_VIDEODECODER_

#include <stdio.h>
#include <vector>
#include <queue>

#include "Def.h"

class VideoDecoder {
public:
	VideoDecoder() = default;
	~VideoDecoder();
	bool Init(VideoCodec type);
	void SetFrameCB(VideoFrameCB cb, void * user_data);
	bool SetInputStream(unsigned char * buffer, int len, int64_t pts);
	bool Dump();
	void Close();
private:
	bool InitVA(mfxSession session);
	void UnInitVA();
	int m_device_fd = -1;
	void * m_va_dpy = nullptr;
private:
	std::queue<int64_t> m_pts_queue;
	VideoCodec m_codec_type = VideoCodec::NONE;
	VideoFrameCB m_frame_cb = nullptr;
	void * m_user_data = nullptr;
	bool m_inited = false;
	unsigned char * m_input_buffer_cache = nullptr;
	int m_current_buffer_cache_len = 0;
	unsigned char * m_raw_frame_buffer = nullptr;
	std::vector<MFXSurface*> m_surfaces;
	std::vector<MFXSurface*> m_output_surfaces;
private:
	mfxSession m_session = nullptr;
	mfxFrameInfo m_frame_info;
private:
	bool AllocSuface(mfxFrameInfo *info, int num);
	void FreeSurface();
	bool InitCodec();
	void OuputFrame(mfxFrameSurface1 *outsurf);
	MFXSurface * GetSurface();
	int Decode(bool dump);
};
#endif
