#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

#include "VideoDecoder.h"
#include "va/va.h"
#include "va/va_drm.h"


#define INPUT_BUFFER_CACHE_LEN 1024*1024*20
#define MSDK_DEC_WAIT_INTERVAL 1000
#define MFX_ASYNCDEPTH 4
#define MSDK_ALIGN32(X) (((mfxU32)((X)+31)) & (~ (mfxU32)31))

template <class T>
static void TransferToYUV(const T *psrc_y,const T * psrc_uv,T *pdst,
	int width, int height, int pitch,
	bool bitDepthMinus8 = true) {
	T * pdst_Y = pdst;
	T * pdst_U = pdst + width * height;
	T * pdst_V = pdst + width * height * 5 /4;

	int x, y, width_2, height_2;
	int rsh = bitDepthMinus8 ? 0 : 6;

	// luma
	for (y = 0; y < height; y++){
		for (int x = 0; x < width; x++) {
			pdst_Y[y*width + x] = psrc_y[x] >> rsh;
		}
		psrc_y += pitch / sizeof(T);
	}

	// De-interleave chroma

	width_2 = width >> 1;
	height_2 = height >> 1;
	for (y = 0; y < height_2; y++){
		for (x = 0; x < width_2; x++){
			pdst_U[y*width_2 + x] = psrc_uv[x * 2] >> rsh;
			pdst_V[y*width_2 + x] = psrc_uv[x * 2 + 1] >> rsh;
		}
		psrc_uv += pitch / sizeof(T);
	}

}

VideoDecoder::~VideoDecoder(){
	Close();
}

bool VideoDecoder::InitVA(mfxSession m_Session){
	UnInitVA();
	m_device_fd = open("/dev/dri/renderD128", O_RDWR);
	if(m_device_fd < 0)
		return false;
	m_va_dpy = vaGetDisplayDRM(m_device_fd);
	if(!m_va_dpy)
		return false;
	int major_version,minor_version;
	VAStatus va_status = vaInitialize(m_va_dpy, &major_version, &minor_version);
	if(va_status != VA_STATUS_SUCCESS)
		return false;
	mfxStatus mfx_status = MFXVideoCORE_SetHandle(m_Session, MFX_HANDLE_VA_DISPLAY, m_va_dpy);
	if(mfx_status < MFX_ERR_NONE)
		return false;
	return true;
}

void VideoDecoder::UnInitVA(){
	if(m_va_dpy){
		vaTerminate(m_va_dpy);
		m_va_dpy = nullptr;
	}
	if(m_device_fd >= 0){
		close(m_device_fd);
		m_device_fd = -1;
	}
}

bool VideoDecoder::Init(VideoCodec type){
	mfxIMPL impl = MFX_IMPL_HARDWARE_ANY;
	mfxVersion ver{ 0,1 };
	mfxStatus ret = MFXInit(impl, &ver, &m_session);
	if (ret != MFX_ERR_NONE)
		return false;
	if(!InitVA(m_session))
		return false;
	m_codec_type = type;
	if (!m_input_buffer_cache)
		m_input_buffer_cache = new unsigned char[INPUT_BUFFER_CACHE_LEN];
	return true;
}

void VideoDecoder::SetFrameCB(VideoFrameCB cb, void * user_data){
	m_frame_cb = cb;
	m_user_data = user_data;
}

bool VideoDecoder::AllocSuface(mfxFrameInfo *info, int num){
	FreeSurface();
	for (int i = 0; i < num; i++){
		mfxFrameSurface1 *surface = new mfxFrameSurface1();
		memset(surface, 0, sizeof(mfxFrameSurface1));

		mfxU32 width2 = (mfxU16)MSDK_ALIGN32(info->Width);
		mfxU32 height2 = (mfxU16)MSDK_ALIGN32(info->Height);
		mfxU32 frame_size = 0;
		int factor = 0;

		switch(info->FourCC){
			case MFX_FOURCC_NV12: {
				frame_size = width2*height2 + (width2 >> 1)*(height2 >> 1) + (width2 >> 1)*(height2 >> 1);
				factor = 1;
				break;
			}
			case MFX_FOURCC_P010: {
				frame_size = (width2*height2 + (width2 >> 1)*(height2 >> 1) + (width2 >> 1)*(height2 >> 1)) << 1;
				factor = 2;
				break;
			}
			default:
				return false;
		}

		mfxU8 *frame_data = (mfxU8*)calloc(frame_size + 32, 1);

		surface->Data.Y = frame_data;
		surface->Data.B = surface->Data.Y;
		surface->Data.U = surface->Data.Y + width2 * height2 * factor;
		surface->Data.V = surface->Data.U + 1*factor;
		surface->Data.Pitch = width2 * factor;

		surface->Info = *info;

		MFXSurface * s = new MFXSurface();
		s->surface = surface;
		m_surfaces.push_back(s);
	}
	return true;
}

void VideoDecoder::FreeSurface(){
	for (auto & s : m_surfaces){
		if (s){
			if (s->surface){
				if (s->surface->Data.Y)
					free(s->surface->Data.Y);
				delete s->surface;
			}
			delete s;
		}
	}
	m_surfaces.clear();
	m_output_surfaces.clear();
}

bool VideoDecoder::InitCodec(){
	mfxVideoParam par;
	memset(&par, 0, sizeof(mfxVideoParam));
	if (m_codec_type == VideoCodec::AVC)
		par.mfx.CodecId = MFX_CODEC_AVC;
	else if (m_codec_type == VideoCodec::HEVC){
		par.mfx.CodecId = MFX_CODEC_HEVC;
	}else
		return false;

	mfxBitstream bs;
	memset(&bs, 0, sizeof(mfxBitstream));
	bs.Data = m_input_buffer_cache;
	bs.DataLength = m_current_buffer_cache_len;
	bs.MaxLength = m_current_buffer_cache_len;

	mfxStatus ret = MFXVideoDECODE_DecodeHeader(m_session, &bs , &par);
	if (ret == MFX_ERR_MORE_DATA)
		return true;
	else if (ret == MFX_ERR_NONE){
		par.IOPattern = MFX_IOPATTERN_OUT_SYSTEM_MEMORY;
		par.AsyncDepth = MFX_ASYNCDEPTH;

		ret = MFXVideoDECODE_Init(m_session, &par);

		if (ret != MFX_ERR_NONE)
			return false;

		mfxFrameAllocRequest request;
		memset(&request, 0, sizeof(mfxFrameAllocRequest));

		ret = MFXVideoDECODE_QueryIOSurf(m_session, &par, &request);
		if (ret != MFX_ERR_NONE)
			return false;

		if(!AllocSuface(&par.mfx.FrameInfo, request.NumFrameSuggested))
			return false;

		if (!m_raw_frame_buffer){
			int framesize = 0;
			switch(par.mfx.FrameInfo.FourCC){
				case MFX_FOURCC_NV12: {
					framesize = par.mfx.FrameInfo.Width * par.mfx.FrameInfo.Height * 3 / 2;
					break;
				}
				case MFX_FOURCC_P010: {
					framesize = par.mfx.FrameInfo.Width * par.mfx.FrameInfo.Height * 3 / 2 * 2;
					break;
				}
				default:
					return false;
			}
			m_raw_frame_buffer = new unsigned char[framesize];
		}

		memcpy(&m_frame_info, &par.mfx.FrameInfo, sizeof(mfxFrameInfo));
		return true;
	}else
		return false;

}


MFXSurface * VideoDecoder::GetSurface(){

	for (auto iter = m_surfaces.begin(); iter != m_surfaces.end(); iter++){
		if (!(*iter)->used && !(*iter)->surface->Data.Locked){
			return (*iter);
		}
	}

	mfxFrameSurface1 *surface = new mfxFrameSurface1();
	memset(surface, 0, sizeof(mfxFrameSurface1));

	mfxU32 width2 = (mfxU16)MSDK_ALIGN32(m_frame_info.Width);
	mfxU32 height2 = (mfxU16)MSDK_ALIGN32(m_frame_info.Width);
	mfxU32 frame_size = 0;
	int factor = 0;

	switch(m_frame_info.FourCC){
		case MFX_FOURCC_NV12: {
			frame_size = width2*height2 + (width2 >> 1)*(height2 >> 1) + (width2 >> 1)*(height2 >> 1);
			factor = 1;
			break;
		}
		case MFX_FOURCC_P010: {
			frame_size = (width2*height2 + (width2 >> 1)*(height2 >> 1) + (width2 >> 1)*(height2 >> 1)) << 1;
			factor = 2;
			break;
		}
		default:
			return nullptr;
	}

	mfxU8 *pFrameData = (mfxU8*)calloc(frame_size + 32, 1);

	surface->Data.Y = pFrameData;
	surface->Data.B = surface->Data.Y;
	surface->Data.U = surface->Data.Y + width2 * height2 * factor;
	surface->Data.V = surface->Data.U + 1 * factor;
	surface->Data.Pitch = width2 * factor;

	surface->Info = m_frame_info;
	
	MFXSurface * s = new MFXSurface();
	s->surface = surface;
	m_surfaces.push_back(s);
	return s;
}

void VideoDecoder::OuputFrame(mfxFrameSurface1 *outsurf){
	if(!m_frame_cb){
		return;
	}
	if(outsurf->Info.FourCC == MFX_FOURCC_NV12){
		TransferToYUV((mfxU8*)outsurf->Data.Y,(mfxU8*)outsurf->Data.UV,(mfxU8*)m_raw_frame_buffer,outsurf->Info.CropW,outsurf->Info.CropH,outsurf->Data.Pitch,true);
		VideoRawData pic;
		pic.width = outsurf->Info.CropW;
		pic.height = outsurf->Info.CropH;
		pic.buffer[0] = m_raw_frame_buffer;
		pic.line_size[0] = outsurf->Info.CropW;
		pic.buffer[1] = m_raw_frame_buffer + outsurf->Info.CropW * outsurf->Info.CropH;
		pic.line_size[1] = outsurf->Info.CropW / 2;
		pic.buffer[2] = m_raw_frame_buffer + outsurf->Info.CropW * outsurf->Info.CropH * 5 / 4;
		pic.line_size[2] = outsurf->Info.CropW / 2;
		pic.fmt = VideoBaseBandFmt::YUV420P;
		if(!m_pts_queue.empty()){
			pic.pts = m_pts_queue.front();
			m_pts_queue.pop();
		}else
			pic.pts = 0;
		m_frame_cb(&pic,m_user_data);
	}else if(outsurf->Info.FourCC == MFX_FOURCC_P010){
		TransferToYUV((mfxU16*)outsurf->Data.Y,(mfxU16*)outsurf->Data.UV,(mfxU16*)m_raw_frame_buffer,outsurf->Info.CropW,outsurf->Info.CropH,outsurf->Data.Pitch,false);
		VideoRawData pic;
		pic.width = outsurf->Info.CropW;
		pic.height = outsurf->Info.CropH;
		pic.buffer[0] = m_raw_frame_buffer;
		pic.line_size[0] = outsurf->Info.CropW * 2;
		pic.buffer[1] = m_raw_frame_buffer + (outsurf->Info.CropW * outsurf->Info.CropH) * 2;
		pic.line_size[1] = outsurf->Info.CropW / 2 * 2;
		pic.buffer[2] = m_raw_frame_buffer + (outsurf->Info.CropW * outsurf->Info.CropH * 5 / 4) *2;
		pic.line_size[2] = outsurf->Info.CropW / 2 * 2;
		pic.fmt = VideoBaseBandFmt::YUV420P;
		if(!m_pts_queue.empty()){
			pic.pts = m_pts_queue.front();
			m_pts_queue.pop();
		}else
			pic.pts = 0;
		m_frame_cb(&pic,m_user_data);
	}
}

int VideoDecoder::Decode(bool dump){
	mfxFrameSurface1 *insurf = nullptr;
	mfxFrameSurface1 *outsurf = nullptr;

	mfxSyncPoint sync;
	mfxStatus ret = MFX_ERR_NONE;
	int left_buffer_len = 0;
	mfxBitstream bs = { 0 };

	bs.Data = m_input_buffer_cache;
	bs.DataLength = m_current_buffer_cache_len;
	bs.DataOffset = 0;
	bs.MaxLength = m_current_buffer_cache_len;

	MFXSurface * surface = nullptr;
	do {
		surface = GetSurface();
		insurf = surface->surface;

		ret = MFXVideoDECODE_DecodeFrameAsync(m_session, bs.DataLength ? &bs : nullptr, insurf, &outsurf, &sync);
		if (ret == MFX_ERR_DEVICE_FAILED) {
			printf("MFX_ERR_DEVICE_FAILED");
		}
		left_buffer_len = bs.DataLength;
		if (sync){
			surface->used = true;
			surface->surface = outsurf;
			surface->sync = sync;
			m_output_surfaces.push_back(surface);
		}
		
		if (m_output_surfaces.size() >= MFX_ASYNCDEPTH || ret == MFX_WRN_DEVICE_BUSY){
			mfxStatus ret_;
			auto iter = m_output_surfaces.begin();
			if ((*iter)->sync){
				do{
					ret_ = MFXVideoCORE_SyncOperation(m_session, (*iter)->sync, MSDK_DEC_WAIT_INTERVAL);
				} while (ret_ == MFX_WRN_IN_EXECUTION);
				
				if (ret_ == MFX_ERR_DEVICE_FAILED) {
					printf("MFX_ERR_DEVICE_FAILED");
				}

				if (ret_ == MFX_ERR_NONE){
					OuputFrame((*iter)->surface);
					//printf("%d\n", outsurf->Data.FrameOrder);
					(*iter)->used = false;
					m_output_surfaces.erase(iter);
				}

			}
		}
	}while (ret == MFX_WRN_DEVICE_BUSY || ret == MFX_ERR_MORE_SURFACE || ((dump || bs.DataLength) && ret != MFX_ERR_MORE_DATA));

	if (dump){
		while (m_output_surfaces.size()){
			mfxStatus ret_;
			auto iter = m_output_surfaces.begin();
			if ((*iter)->sync){
				do{
					ret_ = MFXVideoCORE_SyncOperation(m_session, (*iter)->sync, MSDK_DEC_WAIT_INTERVAL);
				} while (ret_ == MFX_WRN_IN_EXECUTION);

				if (ret_ == MFX_ERR_DEVICE_FAILED) {
					printf("MFX_ERR_DEVICE_FAILED");
				}

				if (ret_ == MFX_ERR_NONE){
					OuputFrame((*iter)->surface);
					(*iter)->used = false;
					m_output_surfaces.erase(iter);
				}

			}
		}
	}

	return left_buffer_len;
}


bool VideoDecoder::SetInputStream(unsigned char * buffer, int len, int64_t pts){
	if (m_current_buffer_cache_len >= INPUT_BUFFER_CACHE_LEN || len > INPUT_BUFFER_CACHE_LEN){
		unsigned char * new_buffer = new unsigned char[INPUT_BUFFER_CACHE_LEN * 2];
		memcpy(new_buffer, m_input_buffer_cache, m_current_buffer_cache_len);
		delete [] m_input_buffer_cache;
		m_input_buffer_cache = new_buffer;
	}

	memcpy(m_input_buffer_cache + m_current_buffer_cache_len, buffer, len);
	m_current_buffer_cache_len += len;

	if (!m_inited){
		if (!InitCodec())
			return false;
		else
			m_inited = true;
	}
	m_pts_queue.push(pts);
	int remain = Decode(false);
	memmove(m_input_buffer_cache, m_input_buffer_cache + (m_current_buffer_cache_len - remain), remain);
	m_current_buffer_cache_len = remain;

	return true;;
}

bool VideoDecoder::Dump(){
	if (m_inited){
		m_current_buffer_cache_len = 0;
		Decode(true);
		return true;
	}else
		return false;
	
}

void VideoDecoder::Close(){
	UnInitVA();
	if (m_session) {
		if(m_inited)
			MFXVideoDECODE_Close(m_session);
		MFXClose(m_session);
		m_session = nullptr;
	}
	memset(&m_frame_info, 0, sizeof(mfxFrameInfo));

	if (m_input_buffer_cache){
		delete[] m_input_buffer_cache;
		m_input_buffer_cache = nullptr;
	}
	m_current_buffer_cache_len = 0;

	if (m_raw_frame_buffer){
		delete[] m_raw_frame_buffer;
		m_raw_frame_buffer = nullptr;
	}

	FreeSurface();

	m_inited = false;
	m_codec_type = VideoCodec::NONE;
	m_frame_cb = nullptr;
	m_user_data = nullptr;

	while(!m_pts_queue.empty()){
		m_pts_queue.pop();
	}
}
