#ifndef _H_VIDEOENCODER_
#define _H_VIDEOENCODER_

#include <stdio.h>
#include <vector>

#include "Def.h"

class VideoEncoder{
public:
	VideoEncoder() = default;
	~VideoEncoder();
	bool Init(VideoParams & param);
	bool EncodeSync(VideoRawData & pic,VideoBitStream & stream);
	void Close();
private:
	bool InitVA(mfxSession m_session);
	void UnInitVA();
	int m_device_fd = -1;
	void * m_va_dpy = nullptr;
private:
	VideoCodec m_codec_type = VideoCodec::NONE;
	std::vector<mfxFrameSurface1*> m_surfaces;
	std::vector<VideoBitStream*> m_bitstreams;
	int m_framenum = 0;
	bool m_inited_encoder = false;
private:
	mfxSession m_session = nullptr;
	mfxFrameInfo m_frame_info;
private:
	void AllocSuface(mfxFrameInfo *info, int num);
	void FreeSurface();
	bool InitCodec(mfxSession session,VideoParams & param);
	mfxFrameSurface1 * GetSuface();
	VideoBitStream *GetFreebitstream();
};
#endif
