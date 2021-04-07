#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

#include "VideoEncoder.h"
#include "va/va.h"
#include "va/va_drm.h"

#define MSDK_ALIGN16(value)  (((value + 15) >> 4) << 4)
#define MSDK_ALIGN32(X) (((mfxU32)((X)+31)) & (~ (mfxU32)31))

#define MFX_BITSTREAM_BUFFER_LEN 1024*1024*10
#define MSDK_ENC_WAIT_INTERVAL 1000

template <class T>
static void ConvertYUVpitchtoNV12(const T * psrc_y, const T * psrc_u, const T * psrc_v,
								 T * pdst_y, T * pdst_uv,
								 int width, int height , int srcStride, int dstStride){

    if (srcStride == 0)
        srcStride = width;
    if (dstStride == 0)
        dstStride = width;

    for (int y = 0 ; y < height ; y++){
        memcpy(pdst_y + (dstStride*y), psrc_y + (srcStride*y),width * sizeof(T));
    }

    for (int y = 0 ; y < height/2 ; y++){
        for (int x= 0 ; x < width; x=x+2){
        	pdst_uv[(y*dstStride) + x] =    psrc_u[((srcStride/2)*y) + (x >>1)];
        	pdst_uv[(y*dstStride) +(x+1)] = psrc_v[((srcStride/2)*y) + (x >>1)];
        }
    }
}

VideoEncoder::~VideoEncoder(){
	Close();
}


bool VideoEncoder::InitVA(mfxSession m_Session){
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

void VideoEncoder::UnInitVA(){
	if(m_va_dpy){
		vaTerminate(m_va_dpy);
		m_va_dpy = nullptr;
	}
	if(m_device_fd >= 0){
		close(m_device_fd);
		m_device_fd = -1;
	}

}

bool VideoEncoder::Init(VideoParams & param){
	Close();
	mfxIMPL impl = MFX_IMPL_HARDWARE_ANY;
	mfxVersion ver{ 0,1 };
	mfxStatus ret = MFXInit(impl, &ver, &m_session);
	if (ret != MFX_ERR_NONE)
		return false;
	if(!InitVA(m_session))
		return false;
	if(!InitCodec(m_session,param))
		return false;
	return true;
}

bool VideoEncoder::InitCodec(mfxSession session,VideoParams & param) {
	/*specifies the codec format identifier in the FOURCC code.
	MFX_CODEC_AVC
	MFX_CODEC_MPEG2
	MFX_CODEC_VC1
	MFX_CODEC_HEVC
	*/
	mfxVideoParam mfx_param;
	memset(&mfx_param,0,sizeof(mfxVideoParam));
	if(param.codec == VideoCodec::AVC)
		mfx_param.mfx.CodecId = MFX_CODEC_AVC;
	else if(param.codec == VideoCodec::HEVC)
		mfx_param.mfx.CodecId = MFX_CODEC_HEVC;
	else{
		return false;
	}
	/*
	number of pictures within the current GOP(Group of Pictures);
	if GopSize=0,then the Gop Size is unspecified.
	if GopSize=1,only I-frames are used.
	*/
	mfx_param.mfx.GopPicSize = param.gop_size;
	/*
	distance between I or P key frames;if it is zero,the GOP structure is unspecified.
	if GopRefDist=1,there are no B frames used.
	*/
	mfx_param.mfx.GopRefDist = param.b_frames;
	/*
	additional flags for the GOP specification.
	MFX_GOP_CLOSED
	MFX_GOP_STRICT
	*/
	mfx_param.mfx.GopOptFlag = 0;
	/*
	specifies idrInterval IDR-frame interval in terms of I-frames;
	if idrInterval=0,then every I-frame is an IDR-frame.
	if idrInterval=1,then every other I-frame is an IDR-frame.
	*/
	mfx_param.mfx.IdrInterval = 0;
	/*
	number of slices in each video frame.if Numslice equals zero,the encoder may choose any
	slice partitioning allower by the codec standard
	*/
	mfx_param.mfx.NumSlice = 0;
	/*
	Target usage model that guides the encoding process;
	it indicates trade-offs between quality and speed.
	*/
	mfx_param.mfx.TargetUsage = MFX_TARGETUSAGE_BEST_SPEED;//balanced quality and speed
										/*
										Rate control method.
										*/
	mfx_param.mfx.RateControlMethod = MFX_RATECONTROL_VBR;
	/*
	TargetKbps must be specified for encoding initialization.
	*/
	mfx_param.mfx.TargetKbps = param.bit_rate;
	mfx_param.mfx.MaxKbps = param.bit_rate * 2;
	mfx_param.mfx.BufferSizeInKB =param.bit_rate * 2 / 8;
	/*
	*/
	mfx_param.mfx.FrameInfo.FrameRateExtN = param.frame_rate_num;
	mfx_param.mfx.FrameInfo.FrameRateExtD = param.frame_rate_den;

	mfx_param.mfx.FrameInfo.CropX = 0;
	mfx_param.mfx.FrameInfo.CropY = 0;
	mfx_param.mfx.FrameInfo.CropW = param.width;
	mfx_param.mfx.FrameInfo.CropH = param.height;

	if(param.bit_depth == 10){
		mfx_param.mfx.FrameInfo.FourCC = MFX_FOURCC_P010;
		mfx_param.mfx.FrameInfo.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
		mfx_param.mfx.FrameInfo.BitDepthChroma = 10;
		mfx_param.mfx.FrameInfo.BitDepthLuma = 10;
		mfx_param.mfx.FrameInfo.Shift = 0;
	}else{
		mfx_param.mfx.FrameInfo.FourCC = MFX_FOURCC_NV12;
		mfx_param.mfx.FrameInfo.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
		mfx_param.mfx.FrameInfo.BitDepthChroma = 8;
		mfx_param.mfx.FrameInfo.BitDepthLuma = 8;
		mfx_param.mfx.FrameInfo.Shift = 0;
	}

	mfx_param.mfx.FrameInfo.Height = MSDK_ALIGN16(param.height);
	mfx_param.mfx.FrameInfo.Width = MSDK_ALIGN16(param.width);
	mfx_param.mfx.FrameInfo.AspectRatioH = 1;
	mfx_param.mfx.FrameInfo.AspectRatioW = 1;
	mfx_param.mfx.FrameInfo.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
	mfx_param.IOPattern = MFX_IOPATTERN_IN_SYSTEM_MEMORY;
	mfx_param.AsyncDepth = 4;

	mfxStatus sts = MFXVideoENCODE_Init(session, &mfx_param);
	if (sts != MFX_ERR_NONE) {
		return false;
	}
	m_frame_info = mfx_param.mfx.FrameInfo;
	mfxFrameAllocRequest request;
	memset(&request, 0, sizeof(mfxFrameAllocRequest));
	sts = MFXVideoENCODE_QueryIOSurf(session, &mfx_param, &request);
	if(sts == MFX_ERR_NONE){
		AllocSuface(&mfx_param.mfx.FrameInfo,request.NumFrameSuggested);
	}else{
		AllocSuface(&mfx_param.mfx.FrameInfo,10);
	}
	m_inited_encoder = true;
	return true;
}

void VideoEncoder::AllocSuface(mfxFrameInfo *info, int num){
	for (int i = 0; i < num; i++){
		mfxFrameSurface1 *surface = new mfxFrameSurface1;
		memset(surface, 0, sizeof(mfxFrameSurface1));
		surface->Info = *info;
		mfxU32 width2 = (mfxU16)MSDK_ALIGN32(info->Width);
		mfxU32 height2 = (mfxU16)MSDK_ALIGN32(info->Height);

		if(info->FourCC == MFX_FOURCC_NV12){
			mfxU32 frame_size = width2*height2 + (width2 >> 1)*(height2 >> 1) + (width2 >> 1)*(height2 >> 1);
			mfxU8 *frame_data = (mfxU8*)calloc(frame_size + 32, 1);
			surface->Data.Y = frame_data;
			surface->Data.U = surface->Data.Y + width2 * height2;
			surface->Data.V = surface->Data.U + 1;
			surface->Data.Pitch = width2;
		}else if(info->FourCC == MFX_FOURCC_P010){
			mfxU32 frame_size = (width2*height2 + (width2 >> 1)*(height2 >> 1) + (width2 >> 1)*(height2 >> 1)) * 2;
			mfxU8 *frame_data = (mfxU8*)calloc(frame_size + 32, 1);
			surface->Data.Y = frame_data;
			surface->Data.U = surface->Data.Y + width2 * height2 * 2;
			surface->Data.V = surface->Data.U + 2;
			surface->Data.Pitch = width2 * 2;
		}else
			return;

		m_surfaces.push_back(surface);
	}
	for (int i = 0; i < num; i++){
		VideoBitStream *bit_stream = new VideoBitStream();
		bit_stream->mfx_bit_stream = new mfxBitstream();
		memset((void*)bit_stream->mfx_bit_stream, 0, sizeof(mfxBitstream));
		bit_stream->mfx_bit_stream->Data = new mfxU8[MFX_BITSTREAM_BUFFER_LEN];
		bit_stream->mfx_bit_stream->MaxLength = MFX_BITSTREAM_BUFFER_LEN;
		memset((void*)bit_stream->mfx_bit_stream->Data, 0, MFX_BITSTREAM_BUFFER_LEN);
		bit_stream->sync_p = new mfxSyncPoint;
		memset((void*)bit_stream->sync_p, 0, sizeof(mfxSyncPoint));
		m_bitstreams.push_back(bit_stream);
	}
}

void VideoEncoder::FreeSurface(){
	for (auto & s : m_surfaces){
		if (s){
			if(s->Data.Y){
				free(s->Data.Y);
			}
			delete s;
		}
	}
	m_surfaces.clear();
	for(auto & b : m_bitstreams){
		if(b){
			if(b->mfx_bit_stream){
				delete b->mfx_bit_stream;
			}
			if(b->sync_p){
				delete b->sync_p;
			}
			delete b;
		}
	}
	m_bitstreams.clear();
}

VideoBitStream *VideoEncoder::GetFreebitstream(){
	for (auto iter = m_bitstreams.begin();iter != m_bitstreams.end(); iter++){
		if (*(*iter)->sync_p == nullptr){
			return *iter;
		}
	}
	VideoBitStream *bit = new VideoBitStream();
	bit->mfx_bit_stream = new mfxBitstream();
	memset((void*)bit->mfx_bit_stream, 0, sizeof(mfxBitstream));
	bit->mfx_bit_stream->Data = new mfxU8[MFX_BITSTREAM_BUFFER_LEN];
	bit->mfx_bit_stream->MaxLength = MFX_BITSTREAM_BUFFER_LEN;
	memset((void*)bit->mfx_bit_stream->Data, 0, MFX_BITSTREAM_BUFFER_LEN);
	bit->sync_p = new mfxSyncPoint;
	memset((void*)bit->sync_p, 0, sizeof(mfxSyncPoint));
	m_bitstreams.push_back(bit);
	return bit;
}

mfxFrameSurface1* VideoEncoder::GetSuface(){
	for (auto iter = m_surfaces.begin();iter != m_surfaces.end(); iter++){
		if (!(*iter)->Data.Locked) {
			return *iter;
		}
	}
	mfxFrameSurface1 *surface = new mfxFrameSurface1();
	memset(surface, 0, sizeof(mfxFrameSurface1));
	surface->Info = m_frame_info;
	mfxU32 width2 = (mfxU16)MSDK_ALIGN32(m_frame_info.Width);
	mfxU32 height2 = (mfxU16)MSDK_ALIGN32(m_frame_info.Height);
	if(m_frame_info.FourCC == MFX_FOURCC_NV12){
		mfxU32 frame_size = width2*height2 + (width2 >> 1)*(height2 >> 1) + (width2 >> 1)*(height2 >> 1);
		mfxU8 *frame_data = (mfxU8*)calloc(frame_size + 32, 1);
		surface->Data.Y = frame_data;
		surface->Data.U = surface->Data.Y + width2 * height2;
		surface->Data.V = surface->Data.U + 1;
		surface->Data.Pitch = width2;
	}else if(m_frame_info.FourCC == MFX_FOURCC_P010){
		mfxU32 frame_size = (width2*height2 + (width2 >> 1)*(height2 >> 1) + (width2 >> 1)*(height2 >> 1)) * 2;
		mfxU8 *frame_data = (mfxU8*)calloc(frame_size + 32, 1);
		surface->Data.Y = frame_data;
		surface->Data.U = surface->Data.Y + width2 * height2 * 2;
		surface->Data.V = surface->Data.U + 2;
		surface->Data.Pitch = width2 * 2;
	}else
		return nullptr;

	surface->Info = m_frame_info;
	m_surfaces.push_back(surface);
	return surface;
}

void VideoEncoder::Close(){
	m_framenum = 0;
	UnInitVA();
	FreeSurface();
	if (m_session) {
		if(m_inited_encoder)
			MFXVideoENCODE_Close(m_session);
		MFXClose(m_session);
		m_session = nullptr;
	}
	memset(&m_frame_info, 0, sizeof(mfxFrameInfo));
	m_codec_type = VideoCodec::NONE;
	m_inited_encoder = false;
}

bool VideoEncoder::EncodeSync(VideoRawData & pic,VideoBitStream & stream){

	if(!m_session || !m_inited_encoder)
		return false;

	mfxStatus sts = MFX_ERR_NONE;
	mfxFrameSurface1 *surface = GetSuface();

	switch(pic.fmt){
		case VideoBaseBandFmt::YUV420P:
		case VideoBaseBandFmt::YUV420P10LE:{
			if(surface->Info.FourCC == MFX_FOURCC_NV12){
				ConvertYUVpitchtoNV12((mfxU8*)pic.buffer[0],(mfxU8*)pic.buffer[1],(mfxU8*)pic.buffer[2],(mfxU8*)surface->Data.Y,
						(mfxU8*)surface->Data.UV,pic.width,pic.height,pic.width,surface->Info.Width);
			}
			else if(surface->Info.FourCC == MFX_FOURCC_P010){
				ConvertYUVpitchtoNV12((mfxU16*)pic.buffer[0],(mfxU16*)pic.buffer[1],(mfxU16*)pic.buffer[2],(mfxU16*)surface->Data.Y,
						(mfxU16*)surface->Data.UV,pic.width,pic.height,pic.width,surface->Info.Width);
			}
			break;
		}
		case VideoBaseBandFmt::NV12:
		case VideoBaseBandFmt::P010LE:{
			surface->Data.Y = pic.buffer[0];
			surface->Data.UV =  pic.buffer[1];
			break;
		}
		default:
			return false;
	}

	VideoBitStream *bit_stream = GetFreebitstream();
	do {
		sts = MFXVideoENCODE_EncodeFrameAsync(m_session, nullptr, surface, bit_stream->mfx_bit_stream, bit_stream->sync_p);
	} while (sts == MFX_WRN_DEVICE_BUSY);

	if (sts == MFX_ERR_NONE) {
		if (*bit_stream->sync_p) {
			sts = MFXVideoCORE_SyncOperation(m_session, *bit_stream->sync_p, MSDK_ENC_WAIT_INTERVAL);
			if (sts == MFX_ERR_NONE) {
				stream = *bit_stream;

			}
			bit_stream->mfx_bit_stream->DataLength = 0;
			*bit_stream->sync_p = nullptr;
		}
	}
	return true;

}


