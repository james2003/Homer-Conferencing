/*****************************************************************************
 *
 * Copyright (C) 2011 Thomas Volkert <thomas@homer-conferencing.com>
 *
 * This software is free software.
 * Your are allowed to redistribute it and/or modify it under the terms of
 * the GNU General Public License version 2 as published by the Free Software
 * Foundation.
 *
 * This source is published in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License version 2 for more details.
 *
 * You should have received a copy of the GNU General Public License version 2
 * along with this program. Otherwise, you can write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111, USA.
 * Alternatively, you find an online version of the license text under
 * http://www.gnu.org/licenses/gpl-2.0.html.
 *
 *****************************************************************************/

/*
 * Purpose: Implementation of a ffmpeg based memory media source
 * Author:  Thomas Volkert
 * Since:   2011-05-05
 */

//HINT: This memory/network based media source tries to use the half of the entire frame buffer during grabbing.
//HINT: The remaining parts of the frame buffer are used to compensate situations with a high system load.

#include <MediaSourceMem.h>
#include <MediaSource.h>
#include <ProcessStatisticService.h>
#include <RTP.h>

#include <Logger.h>
#include <HBSystem.h>

#include <string>
#include <stdint.h>
#include <unistd.h>

namespace Homer { namespace Multimedia {

using namespace std;
using namespace Homer::Monitor;
using namespace Homer::Base;

///////////////////////////////////////////////////////////////////////////////

// maximum packet size, including encoded data and RTP/TS
#define MEDIA_SOURCE_MEM_STREAM_PACKET_BUFFER_SIZE                          MEDIA_SOURCE_AV_CHUNK_BUFFER_SIZE

// seeking: expected maximum GOP size, used if frames are dropped after seeking to find the next key frame close to the target frame
#define MEDIA_SOURCE_MEM_SEEK_MAX_EXPECTED_GOP_SIZE                         30 // every x frames a key frame

// should we use reordered PTS values from ffmpeg video decoder?
#define MEDIA_SOURCE_MEM_USE_REORDERED_PTS                                  0 // on/off

// how much time do we want to buffer at maximum?
#define MEDIA_SOURCE_MEM_FRAME_INPUT_QUEUE_MAX_TIME                         ((System::GetTargetMachineType() != "x86") ? 6.0 : 2.0) // use less memory for 32 bit targets

#define MEDIA_SOURCE_MEM_PRE_BUFFER_TIME                                    (MEDIA_SOURCE_MEM_FRAME_INPUT_QUEUE_MAX_TIME / 2) // leave some seconds for high system load situations so that this part of the input queue can be used for compensating it

// how long do we want to wait for the first key frame when starting decoder loop?
#define MSM_WAITING_FOR_FIRST_KEY_FRAME_TIMEOUT                             7 // seconds

///////////////////////////////////////////////////////////////////////////////

// for debugging purposes: define threshold for dropping a video frame
#define MEDIA_SOURCE_MEM_VIDEO_FRAME_DROP_THRESHOLD                          0 // 0-deactivated, 35 ms for a 30 fps video stream

///////////////////////////////////////////////////////////////////////////////

MediaSourceMem::MediaSourceMem(string pName, bool pRtpActivated):
    MediaSource(pName), RTP()
{
    mDecoderFrameBufferTimeMax = MEDIA_SOURCE_MEM_FRAME_INPUT_QUEUE_MAX_TIME;
    mDecoderFramePreBufferTime = MEDIA_SOURCE_MEM_PRE_BUFFER_TIME;
    mDecoderTargetFrameIndex = 0;
    mDecoderRecalibrateRTGrabbingAfterSeeking = true;
    mDecoderSinglePictureGrabbed = false;
    mDecoderFifo = NULL;
    mDecoderMetaDataFifo = NULL;
    mDecoderFragmentFifo = NULL;
	mResXLastGrabbedFrame = 0;
	mResYLastGrabbedFrame = 0;
    mDecoderSinglePictureResX = 0;
    mDecoderSinglePictureResY = 0;
    mDecoderUsesPTSFromInputPackets = false;
    mCurrentOutputFrameIndex = 0;
    mLastBufferedOutputFrameIndex = 0;
	mWrappingHeaderSize= 0;
    mGrabberProvidesRTGrabbing = true;
    mSourceType = SOURCE_MEMORY;
    mDecoderAudioSamplesFifo = NULL;
    mStreamPacketBuffer = (char*)malloc(MEDIA_SOURCE_MEM_STREAM_PACKET_BUFFER_SIZE);
    mFragmentBuffer = (char*)malloc(MEDIA_SOURCE_MEM_FRAGMENT_BUFFER_SIZE);
    mFragmentNumber = 0;
    mPacketStatAdditionalFragmentSize = 0;
    mOpenInputStream = false;
    mRtpActivated = pRtpActivated;
    RTPRegisterPacketStatistic(this);

    mRtpSourceCodecIdHint = CODEC_ID_NONE;
    mSourceCodecId = CODEC_ID_NONE;

    mDecoderFragmentFifo = new MediaFifo(MEDIA_SOURCE_MEM_FRAGMENT_INPUT_QUEUE_SIZE_LIMIT, MEDIA_SOURCE_MEM_FRAGMENT_BUFFER_SIZE, "MediaSourceMem");
	LOG(LOG_VERBOSE, "Listen for video/audio frames with queue of %d bytes", MEDIA_SOURCE_MEM_FRAGMENT_INPUT_QUEUE_SIZE_LIMIT * MEDIA_SOURCE_MEM_FRAGMENT_BUFFER_SIZE);
}

MediaSourceMem::~MediaSourceMem()
{
    StopGrabbing();

    if (mMediaSourceOpened)
        CloseGrabDevice();

    mDecoderFragmentFifoDestructionMutex.lock();
    if (mDecoderFragmentFifo != NULL)
    {
        delete mDecoderFragmentFifo;
        mDecoderFragmentFifo = NULL;
    }
    mDecoderFragmentFifoDestructionMutex.unlock();
	free(mStreamPacketBuffer);
    free(mFragmentBuffer);
}

///////////////////////////////////////////////////////////////////////////////
// static call back function:
//      function reads frame packet and delivers it upwards to ffmpeg library
// HINT: result can consist of many network packets, which represent one frame (packet)
int MediaSourceMem::GetNextPacket(void *pOpaque, uint8_t *pBuffer, int pBufferSize)
{
	MediaSourceMem *tMediaSourceMemInstance = (MediaSourceMem*)pOpaque;
    char *tBuffer = (char*)pBuffer;
    int tBufferSize = pBufferSize;

    #ifdef MSMEM_DEBUG_PACKETS
        LOGEX(MediaSourceMem, LOG_VERBOSE, "Got a call for GetNextPacket() with a packet buffer at %p and size of %d bytes", pBuffer, pBufferSize);
    #endif
    if (tMediaSourceMemInstance->mRtpActivated)
    {// rtp is active, fragmentation possible!
     // we have to parse every incoming network packet and create a frame packet from the network packets (fragments)
        char *tFragmentData;
        int tFragmentBufferSize;
        int tFragmentDataSize;
        bool tLastFragment;
        bool tFragmenHasAVData;
        bool tFragmentIsSenderReport;

        tBufferSize = 0;

        do{
            tLastFragment = false;
            tFragmenHasAVData = false;
            tFragmentData = &tMediaSourceMemInstance->mFragmentBuffer[0];
            tFragmentBufferSize = MEDIA_SOURCE_MEM_FRAGMENT_BUFFER_SIZE; // maximum size of one single fragment of a frame packet
            // receive a fragment
            tMediaSourceMemInstance->ReadFragment(tFragmentData, tFragmentBufferSize);
            #ifdef MSMEM_DEBUG_PACKET_RECEIVER
                LOGEX(MediaSourceMem, LOG_VERBOSE, "Got packet fragment of size %d at address %p", tFragmentBufferSize, tFragmentData);
            #endif
//            for (unsigned int i = 0; i < 64; i++)
//                if (tFragmentBufferSize >= i)
//                    LOG(LOG_VERBOSE, "stream data (%2u): RTP+12 %02hx(%3d)  RTP %02hx(%3d)", i, tFragmentData[i + 12] & 0xFF, tFragmentData[i + 12] & 0xFF, tFragmentData[i] & 0xFF, tFragmentData[i] & 0xFF);
            if (tMediaSourceMemInstance->mGrabbingStopped)
            {
                LOGEX(MediaSourceMem, LOG_VERBOSE, "Grabbing was stopped meanwhile");
                return AVERROR(ENODEV); //force negative resulting buffer size to signal error and force a return to the calling GUI!
            }
            if (tFragmentBufferSize < 0)
            {
                LOGEX(MediaSourceMem, LOG_VERBOSE, "Received invalid fragment");
                return 0;
            }
            if (tFragmentBufferSize > 0)
            {
                tFragmentDataSize = tFragmentBufferSize;
                // parse and remove the RTP header, extract the encapsulated frame fragment
                tFragmenHasAVData = tMediaSourceMemInstance->RtpParse(tFragmentData, tFragmentDataSize, tLastFragment, tFragmentIsSenderReport, tMediaSourceMemInstance->mSourceCodecId, false);

                // relay new data to registered sinks
                if(tFragmenHasAVData)
                {// fragment is okay
                    // relay the received fragment to registered sinks
                    tMediaSourceMemInstance->RelayPacketToMediaSinks(tFragmentData, tFragmentDataSize);

                    // store the received fragment locally
                    if (tBufferSize + tFragmentDataSize < pBufferSize)
                    {
                        if (tFragmentDataSize > 0)
                        {
                            // copy the fragment to the final buffer
                            memcpy(tBuffer, tFragmentData, tFragmentDataSize);
                            tBuffer += tFragmentDataSize;
                            tBufferSize += tFragmentDataSize;
                        }
                        #ifdef MSMEM_DEBUG_PACKETS
                            LOGEX(MediaSourceMem, LOG_VERBOSE, "Resulting temporary buffer size: %d", tBufferSize);
                        #endif
                    }else
                    {
                        tLastFragment = false;
                        tBuffer = (char*)pBuffer;
                        tBufferSize = pBufferSize;
                        LOGEX(MediaSourceMem, LOG_ERROR, "Stream buffer of %d bytes too small for input, dropping received stream", pBufferSize);
                    }
                }else
                {// fragment has no valid A/V data
                    if (tFragmentIsSenderReport)
                    {// we have received a sender report
                        unsigned int tPacketCountReportedBySender = 0;
                        unsigned int tOctetCountReportedBySender = 0;
                        if (tMediaSourceMemInstance->RtcpParseSenderReport(tFragmentData, tFragmentDataSize, tMediaSourceMemInstance->mEndToEndDelay, tPacketCountReportedBySender, tOctetCountReportedBySender, tMediaSourceMemInstance->mRelativeLoss))
                        {
                            tMediaSourceMemInstance->mDecoderSynchPoints++;
                            #ifdef MSMEM_DEBUG_SENDER_REPORTS
                                LOGEX(MediaSourceMem, LOG_VERBOSE, "Sender reports: %d packets and %d bytes transmitted", tPacketCountReportedBySender, tOctetCountReportedBySender);
                            #endif
                        }else
                            LOGEX(MediaSourceMem, LOG_ERROR, "Unable to parse sender report in received RTCP packet");
                    }else
                    {// we have a received an unsupported RTCP packet/RTP payload or something went completely wrong
                        if (tMediaSourceMemInstance->HasInputStreamChanged())
                        {// we have to reset the source
                            LOGEX(MediaSourceMem, LOG_VERBOSE, "Detected source change at remote side, signaling EOF and returning immediately");
                            return AVERROR(ENODEV);
                        }else
                        {// something went wrong
                            LOGEX(MediaSourceMem, LOG_WARN, "Current RTP packet was reported as invalid by RTP parser, ignoring this data");
                        }
                    }
                }
            }else
            {
                if (tMediaSourceMemInstance->mGrabbingStopped)
                {
                    LOGEX(MediaSourceMem, LOG_VERBOSE, "Detected empty signaling fragment, grabber should be stopped, returning zero data");
                    return 0;
                }else if (!tMediaSourceMemInstance->mDecoderNeeded)
                {
                    LOGEX(MediaSourceMem, LOG_VERBOSE, "Detected empty signaling fragment, decoder should be stopped, returning zero data");
                    return 0;
                }else
                    LOGEX(MediaSourceMem, LOG_VERBOSE, "Detected empty signaling fragment, ignoring it");
            }
        }while(!tLastFragment);
    }else
    {// rtp is inactive
        tMediaSourceMemInstance->ReadFragment(tBuffer, tBufferSize);
        if (tMediaSourceMemInstance->mGrabbingStopped)
        {
            LOGEX(MediaSourceMem, LOG_VERBOSE, "Grabbing was stopped meanwhile");
            return -1; //force negative resulting buffer size to signal error and force a return to the calling GUI!
        }

        if (tBufferSize < 0)
        {
        	LOGEX(MediaSourceMem, LOG_ERROR, "Error when receiving network data");
            return 0;
        }

        // relay the received fragment to registered sinks
        tMediaSourceMemInstance->RelayPacketToMediaSinks(tBuffer, (unsigned int)tBufferSize);
    }

    #ifdef MSMEM_DEBUG_PACKETS
        LOGEX(MediaSourceMem, LOG_VERBOSE, "Returning frame of size %d", tBufferSize);
    #endif

    return tBufferSize;
}

void MediaSourceMem::WriteFragment(char *pBuffer, int pBufferSize)
{
    if (mDecoderFragmentFifo == NULL)
    {
        return;
    }

	if (pBufferSize > 0)
	{
        // log statistics
        // mFragmentHeaderSize to add additional TCPFragmentHeader to the statistic if TCP is used, this is triggered by MediaSourceNet
        AnnouncePacket((int)pBufferSize + mPacketStatAdditionalFragmentSize);
	}
	
    if (mDecoderFragmentFifo->GetUsage() >= mDecoderFragmentFifo->GetSize() - 4)
    {
        LOG(LOG_WARN, "Decoder packet FIFO is near overload situation in WriteFragmet(), deleting all stored frames");

        // delete all stored frames: it is a better for the decoding!
        mDecoderFragmentFifo->ClearFifo();
    }

    mDecoderFragmentFifo->WriteFifo(pBuffer, pBufferSize);
}

void MediaSourceMem::ReadFragment(char *pData, int &pDataSize)
{
    if (mDecoderFragmentFifo == NULL)
    {
        return;
    }

    mDecoderFragmentFifo->ReadFifo(&pData[0], pDataSize);

    if (pDataSize > 0)
    {
        #ifdef MSMEM_DEBUG_PACKETS
            LOG(LOG_VERBOSE, "Read fragment with number %5u at %p with size %5d towards decoder", (unsigned int)++mFragmentNumber, pData, pDataSize);
        #endif
    }

    // is FIFO near overload situation?
    if (mDecoderFragmentFifo->GetUsage() >= mDecoderFragmentFifo->GetSize() - 4)
    {
        LOG(LOG_WARN, "Decoder packet FIFO is near overload situation in ReadFragment(), deleting all stored frames");

        // delete all stored frames: it is a better for the decoding!
        mDecoderFragmentFifo->ClearFifo();
    }
}

bool MediaSourceMem::SupportsDecoderFrameStatistics()
{
    return (mMediaType == MEDIA_VIDEO);
}

GrabResolutions MediaSourceMem::GetSupportedVideoGrabResolutions()
{
    VideoFormatDescriptor tFormat;

    mSupportedVideoFormats.clear();

    if (mMediaType == MEDIA_VIDEO)
    {
        tFormat.Name="SQCIF";      //      128 �  96
        tFormat.ResX = 128;
        tFormat.ResY = 96;
        mSupportedVideoFormats.push_back(tFormat);

        tFormat.Name="QCIF";       //      176 � 144
        tFormat.ResX = 176;
        tFormat.ResY = 144;
        mSupportedVideoFormats.push_back(tFormat);

        tFormat.Name="CIF";        //      352 � 288
        tFormat.ResX = 352;
        tFormat.ResY = 288;
        mSupportedVideoFormats.push_back(tFormat);

        tFormat.Name="VGA";       //
        tFormat.ResX = 640;
        tFormat.ResY = 480;
        mSupportedVideoFormats.push_back(tFormat);

        tFormat.Name="CIF4";       //      704 � 576
        tFormat.ResX = 704;
        tFormat.ResY = 576;
        mSupportedVideoFormats.push_back(tFormat);

        tFormat.Name="DVD";        //      720 � 576
        tFormat.ResX = 720;
        tFormat.ResY = 576;
        mSupportedVideoFormats.push_back(tFormat);

        tFormat.Name="SVGA";       //
        tFormat.ResX = 800;
        tFormat.ResY = 600;
        mSupportedVideoFormats.push_back(tFormat);

        tFormat.Name="XGA";       //
        tFormat.ResX = 1024;
        tFormat.ResY = 768;
        mSupportedVideoFormats.push_back(tFormat);

        tFormat.Name="CIF9";       //     1056 � 864
        tFormat.ResX = 1056;
        tFormat.ResY = 864;
        mSupportedVideoFormats.push_back(tFormat);

        tFormat.Name="SXGA";       //     1280 � 1024
        tFormat.ResX = 1280;
        tFormat.ResY = 1024;
        mSupportedVideoFormats.push_back(tFormat);

        tFormat.Name="WXGA+";      //     1440 � 900
        tFormat.ResX = 1440;
        tFormat.ResY = 900;
        mSupportedVideoFormats.push_back(tFormat);

        tFormat.Name="SXGA+";       //     1440 � 1050
        tFormat.ResX = 1440;
        tFormat.ResY = 1050;
        mSupportedVideoFormats.push_back(tFormat);

        tFormat.Name="CIF16";      //     1408 � 1152
        tFormat.ResX = 1408;
        tFormat.ResY = 1152;
        mSupportedVideoFormats.push_back(tFormat);

        tFormat.Name="HDTV";       //     1920 � 1080
        tFormat.ResX = 1920;
        tFormat.ResY = 1080;
        mSupportedVideoFormats.push_back(tFormat);

        if (mMediaSourceOpened)
        {
            tFormat.Name="Original";
            tFormat.ResX = mCodecContext->width;
            tFormat.ResY = mCodecContext->height;
            mSupportedVideoFormats.push_back(tFormat);
        }
    }

    return mSupportedVideoFormats;
}

void MediaSourceMem::SetFrameRate(float pFps)
{
    LOG(LOG_VERBOSE, "Calls to SetFrameRate() are ignored here, otherwise the real-time playback would delay input data for the wrong FPS value");
    LOG(LOG_VERBOSE, "Ignoring %f, keeping FPS of %f", pFps, GetInputFrameRate());
}

bool MediaSourceMem::SupportsRecording()
{
	return true;
}

bool MediaSourceMem::SupportsRelaying()
{
    return true;
}

bool MediaSourceMem::HasInputStreamChanged()
{
    bool tResult = HasSourceChangedFromRTP();
    enum CodecID tNewCodecId = CODEC_ID_NONE;

    // try to detect the right source codec based on RTP data
    if ((tResult) && (mRtpActivated))
    {
        switch(GetRTPPayloadType())
        {
            //video
            case 31:
                    tNewCodecId = CODEC_ID_H261;
                    break;
            case 32:
                    tNewCodecId = CODEC_ID_MPEG2VIDEO;
                    break;
            case 34:
            case 118:
                    tNewCodecId = CODEC_ID_H263;
                    break;
            case 119:
                    tNewCodecId = CODEC_ID_H263P;
                    break;
            case 120:
                    tNewCodecId = CODEC_ID_H264;
                    break;
            case 121:
                    tNewCodecId = CODEC_ID_MPEG4;
                    break;
            case 122:
                    tNewCodecId = CODEC_ID_THEORA;
                    break;
            case 123:
                    tNewCodecId = CODEC_ID_VP8;
                    break;

            //audio
            case 0:
                    tNewCodecId = CODEC_ID_PCM_MULAW;
                    break;
            case 3:
                    tNewCodecId = CODEC_ID_GSM;
                    break;
            case 8:
                    tNewCodecId = CODEC_ID_PCM_ALAW;
                    break;
            case 9:
                    tNewCodecId = CODEC_ID_ADPCM_G722;
                    break;
            case 10:
                    tNewCodecId = CODEC_ID_PCM_S16BE;
                    break;
            case 11:
                    tNewCodecId = CODEC_ID_PCM_S16BE;
                    break;
            case 14:
                    tNewCodecId = CODEC_ID_MP3;
                    break;
            case 100:
                    tNewCodecId = CODEC_ID_AAC;
                    break;
            case 101:
                    tNewCodecId = CODEC_ID_AMR_NB;
                    break;
            default:
                    break;
        }
        if ((tNewCodecId != CODEC_ID_NONE) && (tNewCodecId != mSourceCodecId))
        {
            LOG(LOG_VERBOSE, "Suggesting codec change from %d(%s) to %d(%s)", mSourceCodecId, avcodec_get_name(mSourceCodecId), tNewCodecId, avcodec_get_name(tNewCodecId));
            mRtpSourceCodecIdHint = tNewCodecId;
        }
    }

    return tResult;
}

void MediaSourceMem::StopGrabbing()
{
	MediaSource::StopGrabbing();

	LOG(LOG_VERBOSE, "Going to stop memory based %s source", GetMediaTypeStr().c_str());

    char tData[4];
    if (mDecoderFragmentFifo != NULL)
    {
        mDecoderFragmentFifo->WriteFifo(tData, 0);
        mDecoderFragmentFifo->WriteFifo(tData, 0);
    }

    LOG(LOG_VERBOSE, "Memory based %s source successfully stopped", GetMediaTypeStr().c_str());
}

int MediaSourceMem::GetChunkDropCounter()
{
    if (mRtpActivated)
        return GetLostPacketsFromRTP();
    else
        return mChunkDropCounter;
}

int MediaSourceMem::GetFragmentBufferCounter()
{
	int tResult = 0;

	mDecoderFragmentFifoDestructionMutex.lock();
    if (mDecoderFragmentFifo != NULL)
        tResult =  mDecoderFragmentFifo->GetUsage();
	mDecoderFragmentFifoDestructionMutex.unlock();

	return tResult;
}

int MediaSourceMem::GetFragmentBufferSize()
{
	int tResult = 0;

	mDecoderFragmentFifoDestructionMutex.lock();
    if (mDecoderFragmentFifo != NULL)
        tResult = mDecoderFragmentFifo->GetSize();
    mDecoderFragmentFifoDestructionMutex.unlock();

	return tResult;
}

int MediaSourceMem::CalculateFrameBufferSize()
{
    int tResult = 0;

    // how many frame input buffers do we want to store, depending on mDecoderBufferTimeMax
    //HINT: assume 44.1 kHz playback sample rate, 1024 samples per audio buffer
    //HINT: reserve one buffer for internal signaling
    switch(GetMediaType())
    {
        case MEDIA_AUDIO:
            tResult = rint(mDecoderFrameBufferTimeMax * mOutputFrameRate /* 44100 samples per second / 1024 samples per frame */);
            break;
        case MEDIA_VIDEO:
            tResult = rint(mDecoderFrameBufferTimeMax * mOutputFrameRate /* r_frame_rate.num / r_frame_rate.den from the AVStream */);
            break;
        default:
            LOG(LOG_ERROR, "Unsupported media type");
            break;
    }

    // add one entry for internal signaling purposes
    tResult++;

    return tResult;
}

void MediaSourceMem::DoSetVideoGrabResolution(int pResX, int pResY)
{
    LOG(LOG_VERBOSE, "Going to execute DoSetVideoGrabResolution()");

    if (mMediaSourceOpened)
    {
        if (mMediaType == MEDIA_UNKNOWN)
        {
            LOG(LOG_WARN, "Media type is still unknown when DoSetVideoGrabResolution is called, setting type to VIDEO ");
            mMediaType = MEDIA_VIDEO;
        }

        LOG(LOG_VERBOSE, "Stopping decoder from DoSetVideoGrabResolution()");
        StopDecoder();
        LOG(LOG_VERBOSE, "Starting decoder from DoSetVideoGrabResolution()");
        StartDecoder();
    }
    LOG(LOG_VERBOSE, "DoSetVideoGrabResolution() finished");
}

bool MediaSourceMem::SetInputStreamPreferences(std::string pStreamCodec, bool pDoReset)
{
    bool tResult = false;
    enum CodecID tStreamCodecId = GetCodecIDFromGuiName(pStreamCodec);

    if (mSourceCodecId != tStreamCodecId)
    {
        LOG(LOG_VERBOSE, "Setting new input streaming preferences");

        tResult = true;

        // set new codec
        LOG(LOG_VERBOSE, "    ..stream codec: %d => %d (%s)", mSourceCodecId, tStreamCodecId, pStreamCodec.c_str());
        mSourceCodecId = tStreamCodecId;
        mRtpSourceCodecIdHint = tStreamCodecId;

        if ((pDoReset) && (mMediaSourceOpened))
        {
            LOG(LOG_VERBOSE, "Do reset now...");
            Reset();
        }
    }

    return tResult;
}

int MediaSourceMem::GetFrameBufferCounter()
{
    if (mDecoderFifo != NULL)
        return mDecoderFifo->GetUsage();
    else
        return 0;
}

int MediaSourceMem::GetFrameBufferSize()
{
    if (mDecoderFifo != NULL)
        return mDecoderFifo->GetSize();
    else
        return 0;
}

void MediaSourceMem::SetFrameBufferPreBufferingTime(float pTime)
{
	if (pTime > MEDIA_SOURCE_MEM_FRAME_INPUT_QUEUE_MAX_TIME - 0.5)
		pTime = MEDIA_SOURCE_MEM_FRAME_INPUT_QUEUE_MAX_TIME - 0.5;

	MediaSource::SetFrameBufferPreBufferingTime(pTime);
}

bool MediaSourceMem::OpenVideoGrabDevice(int pResX, int pResY, float pFps)
{
    AVIOContext         *tIoContext;
    AVInputFormat       *tFormat;

    mMediaType = MEDIA_VIDEO;

    if (pFps > 29.97)
        pFps = 29.97;
    if (pFps < 5)
        pFps = 5;

    LOG(LOG_VERBOSE, "Trying to open the video source");

    SVC_PROCESS_STATISTIC.AssignThreadName("Video-Grabber(MEM)");

    // set category for packet statistics
    ClassifyStream(DATA_TYPE_VIDEO, SOCKET_RAW);

    // check if we have a suggestion from RTP parser
    if (mRtpSourceCodecIdHint != CODEC_ID_NONE)
        mSourceCodecId = mRtpSourceCodecIdHint;

    // there is no differentiation between H.263+ and H.263 when decoding an incoming video stream
    if (mSourceCodecId == CODEC_ID_H263P)
        mSourceCodecId = CODEC_ID_H263;

    // get a format description
    if (!DescribeInput(mSourceCodecId, &tFormat))
    	return false;

	// build corresponding "AVIOContext"
    CreateIOContext(mStreamPacketBuffer, MEDIA_SOURCE_MEM_STREAM_PACKET_BUFFER_SIZE, GetNextPacket, NULL, this, &tIoContext);

    // open the input for the described format and via the provided I/O control
    mOpenInputStream = true;
    bool tRes = OpenInput("", tFormat, tIoContext);
    mOpenInputStream = false;
    if (!tRes)
    	return false;

    // detect all available video/audio streams in the input
    if (!DetectAllStreams())
    	return false;

    // select the first matching stream according to mMediaType
    if (!SelectStream())
    	return false;

    if (mFormatContext->start_time > 0)
    {
        LOG(LOG_WARN, "Found a start time of: %"PRId64", will assume 0 instead", mFormatContext->start_time);
        mFormatContext->start_time = 0;
    }

    // finds and opens the correct decoder
    if (!OpenDecoder())
    	return false;

	if (!OpenFormatConverter())
		return false;

    // overwrite FPS by the playout FPS value
    mInputFrameRate = mOutputFrameRate;

    MarkOpenGrabDeviceSuccessful();

    if (!mGrabbingStopped)
        StartDecoder();

    // enforce pre-buffering for the first frame
    mDecoderRecalibrateRTGrabbingAfterSeeking = (mDecoderFramePreBufferTime > 0);

    return true;
}

bool MediaSourceMem::OpenAudioGrabDevice(int pSampleRate, int pChannels)
{
    AVIOContext       	*tIoContext;
    AVInputFormat       *tFormat;

    mMediaType = MEDIA_AUDIO;
    mOutputAudioChannels = pChannels;
    mOutputAudioSampleRate = pSampleRate;
    mOutputAudioFormat = AV_SAMPLE_FMT_S16; // assume we always want signed 16 bit

    LOG(LOG_VERBOSE, "Trying to open the audio source");

    SVC_PROCESS_STATISTIC.AssignThreadName("Audio-Grabber(MEM)");

    // set category for packet statistics
    ClassifyStream(DATA_TYPE_AUDIO, SOCKET_RAW);

    // check if we have a suggestion from RTP parser
    if (mRtpSourceCodecIdHint != CODEC_ID_NONE)
        mSourceCodecId = mRtpSourceCodecIdHint;

    // get a format description
    if (!DescribeInput(mSourceCodecId, &tFormat))
    	return false;

	// build corresponding "AVIOContext"
    CreateIOContext(mStreamPacketBuffer, MEDIA_SOURCE_MEM_STREAM_PACKET_BUFFER_SIZE, GetNextPacket, NULL, this, &tIoContext);

    // open the input for the described format and via the provided I/O control
    mOpenInputStream = true;
    bool tRes = OpenInput("", tFormat, tIoContext);
    mOpenInputStream = false;
    if (!tRes)
    	return false;

    // detect all available video/audio streams in the input
    if (!DetectAllStreams())
    	return false;

    // select the first matching stream according to mMediaType
    if (!SelectStream())
    	return false;

    if (mFormatContext->start_time > 0)
    {
        LOG(LOG_WARN, "Found a start time of: %"PRId64", will assume 0 instead", mFormatContext->start_time);
        mFormatContext->start_time = 0;
    }

    // ffmpeg might have difficulties detecting the correct input format, enforce correct audio parameters
    AVCodecContext *tCodec = mFormatContext->streams[mMediaStreamIndex]->codec;
    switch(mSourceCodecId)
    {
    	case CODEC_ID_ADPCM_G722:
    		tCodec->channels = 1;
    		tCodec->sample_rate = 16000;
			break;
    	case CODEC_ID_GSM:
    	case CODEC_ID_PCM_ALAW:
		case CODEC_ID_PCM_MULAW:
			tCodec->channels = 1;
			tCodec->sample_rate = 8000;
			break;
    	case CODEC_ID_PCM_S16BE:
    		tCodec->channels = 2;
    		tCodec->sample_rate = 44100;
			break;
		default:
			break;
    }

    // finds and opens the correct decoder
    if (!OpenDecoder())
    	return false;

    if (!OpenFormatConverter())
		return false;

    // overwrite FPS by the playout FPS value
    mInputFrameRate = mOutputFrameRate;

    MarkOpenGrabDeviceSuccessful();

    if (!mGrabbingStopped)
        StartDecoder();

    // enforce pre-buffering for the first frame
    mDecoderRecalibrateRTGrabbingAfterSeeking = (mDecoderFramePreBufferTime > 0);

    return true;
}

bool MediaSourceMem::CloseGrabDevice()
{
    bool tResult = false;

    LOG(LOG_VERBOSE, "Going to close %s stream", GetMediaTypeStr().c_str());

    if (mMediaSourceOpened)
    {
        // make sure we can free the memory structures
        StopDecoder();

        CloseAll();

        tResult = true;
    }else
        LOG(LOG_INFO, "...%s stream was already closed", GetMediaTypeStr().c_str());

    mGrabbingStopped = false;

    // reset the FIFO to have a clean FIFO next time we open the media source again
    if (mDecoderFragmentFifo != NULL)
        mDecoderFragmentFifo->ClearFifo();

    ResetPacketStatistic();

    mCurrentOutputFrameIndex = 0;
    mLastBufferedOutputFrameIndex = 0;
    mSourceTimeShiftForRTGrabbing = 0;
    mDecoderSinglePictureGrabbed = false;
    mEOFReached = false;
    mDecoderRecalibrateRTGrabbingAfterSeeking = true;
    mResXLastGrabbedFrame = 0;
    mResYLastGrabbedFrame = 0;
    mDecoderSynchPoints = 0;

    // reinit. RTP parser
    RTP::Init();

    return tResult;
}

int MediaSourceMem::GrabChunk(void* pChunkBuffer, int& pChunkSize, bool pDropChunk)
{
    int64_t tCurrentFramePts;

    #if defined(MSMEM_DEBUG_PACKETS) || defined(MSMEM_DEBUG_DECODER_STATE)
        LOG(LOG_VERBOSE, "Going to grab new chunk");
    #endif

    if (pChunkBuffer == NULL)
    {
        // acknowledge failed"
        MarkGrabChunkFailed(GetMediaTypeStr() + " grab buffer is NULL");

        return GRAB_RES_INVALID;
    }

    bool tShouldGrabNext = false;
    int tGrabLoops = 0;

    do{
        tShouldGrabNext = false;

        // lock grabbing
        mGrabMutex.lock();

        if (!mMediaSourceOpened)
        {
            // unlock grabbing
            mGrabMutex.unlock();

            // acknowledge failed"
            MarkGrabChunkFailed(GetMediaTypeStr() + " source is closed");

            return GRAB_RES_INVALID;
        }

        if (mGrabbingStopped)
        {
            // unlock grabbing
            mGrabMutex.unlock();

            // acknowledge failed"
            MarkGrabChunkFailed(GetMediaTypeStr() + " source is paused");

            return GRAB_RES_INVALID;
        }

        int tAvailableFrames = (mDecoderFifo != NULL) ? mDecoderFifo->GetUsage() : -1;

        // missing input?
        if (tAvailableFrames == 0)
        {
            // EOF reached?
            if (mEOFReached)
            {// queue empty and EOF reached
                mFrameNumber++;

                // unlock grabbing
                mGrabMutex.unlock();

                LOG(LOG_VERBOSE, "No %s frame in FIFO available and EOF marker is active", GetMediaTypeStr().c_str());

                // acknowledge "success"
                MarkGrabChunkSuccessful(mFrameNumber); // don't panic, it is only EOF

                LOG(LOG_WARN, "Signaling EOF");
                return GRAB_RES_EOF;
            }else
            {// queue empty but EOF not reached
                if ((!mDecoderWaitForNextKeyFrame) && (!mDecoderWaitForNextKeyFramePackets))
                {// okay, we want more output from the decoder thread
                    if (GetSourceType() == SOURCE_FILE)
                    {// source is a file
                        LOG(LOG_WARN, "System too slow?, %s grabber detected a buffer underrun", GetMediaTypeStr().c_str());
                    }else
                    {// source is memory/network
                        if ((mDecoderFramePreBufferingAutoRestart) && (mDecoderFramePreBufferTime > 0))
                        {// time to restart pre-buffering
                            LOG(LOG_VERBOSE, "Buffer is empty, restarting pre-buffering now..");

                            // pretend that we had a seeking step and have to recalibrate the RT grabbing
                            mDecoderRecalibrateRTGrabbingAfterSeeking = true;
                        }
                    }
                }else
                {// we have to wait until the decoder thread has new data after we triggered a seeking process
                    // nothing to complain about
                }
            }
        }

        // read chunk data from FIFO
        if (mDecoderFifo != NULL)
        {
            // need more input but EOF is not reached?
            if ((tAvailableFrames < mDecoderFifo->GetSize()) && ((!mEOFReached) || (InputIsPicture())))
            {
                #ifdef MSMEM_DEBUG_DECODER_STATE
                    LOG(LOG_VERBOSE, "Signal to decoder that new data is needed");
                #endif
                mDecoderNeedWorkConditionMutex.lock();
                mDecoderNeedWorkCondition.Signal();
                mDecoderNeedWorkConditionMutex.unlock();
                #ifdef MSMEM_DEBUG_DECODER_STATE
                    LOG(LOG_VERBOSE, "Signaling to decoder done");
                #endif
            }

            ReadFrameOutputBuffer((char*)pChunkBuffer, pChunkSize, tCurrentFramePts);
            #ifdef MSMEM_DEBUG_PACKETS
                LOG(LOG_VERBOSE, "Remaining buffered frames in decoder FIFO: %d", tAvailableFrames);
            #endif
        }else
        {// decoder thread not started yet
            LOG(LOG_WARN, "Decoder main loop not ready yet");
            tShouldGrabNext = true;
        }

        // increase chunk number which will be the result of the grabbing call
        mFrameNumber++;

        // did we read an empty frame?
        if (pChunkSize == 0)
        {
            // unlock grabbing
            mGrabMutex.unlock();

            // acknowledge "success"
            MarkGrabChunkSuccessful(mFrameNumber); // don't panic, it is only EOF

            LOG(LOG_WARN, "Signaling invalid result");
            return GRAB_RES_INVALID;
        }

        #ifdef MSMEM_DEBUG_PACKETS
            LOG(LOG_VERBOSE, "Grabbed chunk %d of size %d with pts %"PRId64" from decoder FIFO", mFrameNumber, pChunkSize, tCurrentFramePts);
        #endif

        if ((mDecoderTargetFrameIndex != 0) && (mCurrentOutputFrameIndex < mDecoderTargetFrameIndex))
        {// we are waiting for some special frame number
            LOG(LOG_VERBOSE, "Dropping grabbed %s frame %.2f because we are still waiting for frame %.2f", GetMediaTypeStr().c_str(), (float)mCurrentOutputFrameIndex, mDecoderTargetFrameIndex);
            tShouldGrabNext = true;
        }

        // update current PTS value if the read frame is okay
        if (!tShouldGrabNext)
        {
            #ifdef MSMEM_DEBUG_PACKETS
                LOG(LOG_VERBOSE, "Setting current frame index to %"PRId64, tCurrentFramePts);
            #endif
            mCurrentOutputFrameIndex = tCurrentFramePts;
        }

        // check for EOF
        int64_t tCurrentRelativeFramePts = tCurrentFramePts - CalculateOutputFrameNumber(mInputStartPts);
        if ((tCurrentRelativeFramePts >= mNumberOfFrames) && (mNumberOfFrames != 0) && (!InputIsPicture()))
        {// PTS value is bigger than possible max. value, EOF reached
            LOG(LOG_VERBOSE, "Returning EOF for %s input because PTS value %"PRId64" (%"PRId64" - %.2f) is bigger than or equal to maximum %.2f", GetMediaTypeStr().c_str(), tCurrentRelativeFramePts, tCurrentFramePts, (float)mInputStartPts, (float)mNumberOfFrames);
            mEOFReached = true;
            tShouldGrabNext = false;
        }

        // unlock grabbing
        mGrabMutex.unlock();

        #ifdef MSMEM_DEBUG_PACKETS
            if (tShouldGrabNext)
                LOG(LOG_VERBOSE, "Have to grab another frame, loop: %d", ++tGrabLoops);
        #endif
    }while (tShouldGrabNext);

    // reset seeking flag
    mDecoderTargetFrameIndex = 0;

    // #########################################
    // frame rate emulation
    // #########################################
    // inspired by "output_packet" from ffmpeg.c
    if (mGrabberProvidesRTGrabbing)
    {
        // are we waiting for first valid frame after we seeked within the input stream?
        if (mDecoderRecalibrateRTGrabbingAfterSeeking)
        {
            #ifdef MSMEM_DEBUG_CALIBRATION
                LOG(LOG_VERBOSE, "Recalibrating RT %s grabbing after seeking in input stream", GetMediaTypeStr().c_str());
            #endif

            // adapt the start pts value to the time shift once more because we maybe dropped several frames during seek process
            CalibrateRTGrabbing();

            #ifdef MSMEM_DEBUG_SEEKING
                LOG(LOG_VERBOSE, "Read valid %s frame %.2f after seeking in input stream", GetMediaTypeStr().c_str(), (float)mCurrentOutputFrameIndex);
            #endif

            mDecoderRecalibrateRTGrabbingAfterSeeking = false;
        }

        // RT grabbing - do we have to wait?
        WaitForRTGrabbing();
    }

    // acknowledge success
    MarkGrabChunkSuccessful(mFrameNumber);

    return mFrameNumber;
}

bool MediaSourceMem::InputIsPicture()
{
    bool tResult = false;

//    LOG(LOG_VERBOSE, "Source opened: %d", mMediaSourceOpened);
//    if ((mFormatContext != NULL) && (mFormatContext->streams[mMediaStreamIndex]))
//        LOG(LOG_VERBOSE, "Media type: %d", mFormatContext->streams[mMediaStreamIndex]->codec->codec_type);

    // do we have a picture?
    if ((mMediaSourceOpened) &&
        (mFormatContext != NULL) && (mFormatContext->streams[mMediaStreamIndex]) &&
        (mFormatContext->streams[mMediaStreamIndex]->codec->codec_type == AVMEDIA_TYPE_VIDEO) &&
        (mFormatContext->streams[mMediaStreamIndex]->duration == 1))
        tResult = true;

    return tResult;
}

void MediaSourceMem::StartDecoder()
{
    LOG(LOG_VERBOSE, "Starting %s decoder with FIFO", GetMediaTypeStr().c_str());

    mDecoderTargetResX = mTargetResX;
    mDecoderTargetResY = mTargetResY;

    // trigger a RT playback calibration
    mDecoderRecalibrateRTGrabbingAfterSeeking = true;

    mDecoderNeeded = false;

    if (!IsRunning())
    {
        // start decoder main loop
        StartThread();

        int tLoops = 0;

        // wait until thread is running
        while ((!IsRunning() /* wait until thread is started */) || (!mDecoderNeeded /* wait until thread has finished the init. process */))
        {
            if (tLoops % 10 == 0)
                LOG(LOG_VERBOSE, "Waiting for start of %s decoding thread, loop count: %d", GetMediaTypeStr().c_str(), ++tLoops);
            Thread::Suspend(25 * 1000);
        }
    }
}

void MediaSourceMem::StopDecoder()
{
	char tmp[4];
    int tSignalingRound = 0;

    LOG(LOG_VERBOSE, "Stopping decoder");

    mDecoderFragmentFifoDestructionMutex.lock();
    if (mDecoderFifo != NULL)
    {
        // tell decoder thread it isn't needed anymore
        mDecoderNeeded = false;

        // wait for termination of decoder thread
        do
        {
            if(tSignalingRound > 0)
                LOG(LOG_WARN, "Signaling attempt %d to stop %s decoder", tSignalingRound, GetMediaTypeStr().c_str());
            tSignalingRound++;

            WriteFragment(tmp, 0);

            // force a wake up of decoder thread
            mDecoderNeedWorkCondition.Signal();

            Suspend(25 * 1000);
        }while(IsRunning());
    }
    mDecoderFragmentFifoDestructionMutex.unlock();

    LOG(LOG_VERBOSE, "Decoder stopped");
}

VideoScaler* MediaSourceMem::CreateVideoScaler()
{
    VideoScaler *tResult;

    LOG(LOG_VERBOSE, "Starting video scaler thread..");
    tResult = new VideoScaler("Video-Decoder(" + GetSourceTypeStr() + ")");
    if(tResult == NULL)
        LOG(LOG_ERROR, "Invalid video scaler instance, possible out of memory");
    tResult->StartScaler(CalculateFrameBufferSize(), mSourceResX, mSourceResY, mCodecContext->pix_fmt, mDecoderTargetResX, mDecoderTargetResY, PIX_FMT_RGB32);

    return tResult;
}

void MediaSourceMem::DestroyVideoScaler(VideoScaler *pScaler)
{
    pScaler->StopScaler();
}

void MediaSourceMem::ReadPacketFromInputStream(AVPacket *pPacket, int64_t &pPacketPts)
{
    int             tRes;

    // #########################################
    // read new packet
    // #########################################
    bool tShouldReadNext;
    int tReadIteration = 0;
    do
    {
        tReadIteration++;
        tShouldReadNext = false;

        if (tReadIteration > 1)
        {
            // free packet buffer
            #ifdef MSMEM_DEBUG_PACKETS
                LOG(LOG_WARN, "Freeing the ffmpeg packet structure..(read loop: %d)", tReadIteration);
            #endif
            av_free_packet(pPacket);
        }

        // reset packet
        pPacket->data = NULL;
        pPacket->size = 0;

        // read next sample from source - blocking
        if ((tRes = av_read_frame(mFormatContext, pPacket)) < 0)
        {// failed to read frame
            #ifdef MSMEM_DEBUG_PACKETS
                if (!tInputIsPicture)
                    LOG(LOG_VERBOSE, "Read bad frame %d with %d bytes from stream %d and result %s(%d)", pPacket->pts, pPacket->size, pPacket->stream_index, strerror(AVUNERROR(tRes)), tRes);
                else
                    LOG(LOG_VERBOSE, "Read bad picture %d with %d bytes from stream %d and result %s(%d)", pPacket->pts, pPacket->size, pPacket->stream_index, strerror(AVUNERROR(tRes)), tRes);
            #endif

            // error reporting
            if (!mGrabbingStopped)
            {
                if (HasInputStreamChanged())
                {
                    mEOFReached = true;
                }else if (tRes == (int)AVERROR_EOF)
                {
                    pPacketPts = mNumberOfFrames;
                    LOG(LOG_WARN, "%s-Decoder reached EOF", GetMediaTypeStr().c_str());
                    mEOFReached = true;
                }else if (tRes == (int)AVUNERROR(EIO))
                {
                    // acknowledge failed"
                    MarkGrabChunkFailed(GetMediaTypeStr() + " source has I/O error");

                    // signal EOF instead of I/O error
                    LOG(LOG_VERBOSE, "Returning EOF in %s stream because of I/O error", GetMediaTypeStr().c_str());
                    mEOFReached = true;
                }else if (tRes != (int)AVUNERROR(EAGAIN))
                {// we should grab again, we signaled this ourself
                    if (mDecoderNeeded)
                        tShouldReadNext = true;
                }else
                {// actually an error
                    LOG(LOG_ERROR, "Couldn't grab a %s frame because \"%s\"(%d)", GetMediaTypeStr().c_str(), strerror(AVUNERROR(tRes)), tRes);
                }
            }
        }else
        {// new frame was read
            if (pPacket->stream_index == mMediaStreamIndex)
            {
                #ifdef MSMEM_DEBUG_PACKETS
                    if (!tInputIsPicture)
                        LOG(LOG_VERBOSE, "Read good frame %d with %d bytes from stream %d", pPacket->pts, pPacket->size, pPacket->stream_index);
                    else
                        LOG(LOG_VERBOSE, "Read good picture %d with %d bytes from stream %d", pPacket->pts, pPacket->size, pPacket->stream_index);
                #endif

                // is "presentation timestamp" stored within media stream?
                if (pPacket->dts != (int64_t)AV_NOPTS_VALUE)
                { // DTS value
                    pPacketPts = pPacket->dts;
                    #ifdef MSMEM_DEBUG_PACKETS
                        if ((pPacket->dts < mDecoderLastReadPts) && (mDecoderLastReadPts != 0) && (pPacket->dts > 16 /* ignore the first frames */))
                            LOG(LOG_VERBOSE, "%s-DTS values are non continuous in stream, read DTS: %"PRId64", last %"PRId64", alternative PTS: %"PRId64, GetMediaTypeStr().c_str(), pPacket->dts, mDecoderLastReadPts, pPacket->pts);
                    #endif
                }else
                {// PTS value
                    pPacketPts = pPacket->pts;
                    #ifdef MSMEM_DEBUG_PACKETS
                        if ((pPacket->pts < mDecoderLastReadPts) && (mDecoderLastReadPts != 0) && (pPacket->pts > 16 /* ignore the first frames */))
                            LOG(LOG_VERBOSE, "%s-PTS values are non continuous in stream, %"PRId64" is lower than last %"PRId64", alternative DTS: %"PRId64", difference is: %"PRId64, GetMediaTypeStr().c_str(), pPacket->pts, mDecoderLastReadPts, pPacket->dts, mDecoderLastReadPts - pPacket->pts);
                    #endif
                }

                if (mRtpActivated)
                {// derive the frame number from the RTP timestamp, which works independent from packet loss
                    pPacketPts = CalculateFrameNumberFromRTP();
                    pPacket->pts = pPacketPts;
                    pPacket->dts = pPacketPts;
                    #ifdef MSMEM_DEBUG_PRE_BUFFERING
                        LOG(LOG_VERBOSE, "Got from RTP data the frame number: %d", pPacketPts);
                    #endif
                }

                // for seeking: is the currently read frame close to target frame index?
                //HINT: we need a key frame in the remaining distance to the target frame)
                if ((mDecoderTargetFrameIndex != 0) && (pPacketPts < mDecoderTargetFrameIndex - MEDIA_SOURCE_MEM_SEEK_MAX_EXPECTED_GOP_SIZE))
                {// we are still waiting for a special frame number
                    #ifdef MSMEM_DEBUG_SEEKING
                        LOG(LOG_VERBOSE, "Dropping %s frame %"PRId64" because we are waiting for frame %.2f", GetMediaTypeStr().c_str(), pPacketPts, mDecoderTargetFrameIndex);
                    #endif
                    tShouldReadNext = true;
                }else
                {
                    if (mDecoderRecalibrateRTGrabbingAfterSeeking)
                    {
                        // wait for next key frame packets (either i-frame or p-frame
                        if (mDecoderWaitForNextKeyFramePackets)
                        {
                            if (pPacket->flags & AV_PKT_FLAG_KEY)
                            {
                                #ifdef MSMEM_DEBUG_SEEKING
                                    LOG(LOG_VERBOSE, "Read first %s key packet in packet frame number %"PRId64" with flags %d from input stream after seeking", GetMediaTypeStr().c_str(), pPacketPts, pPacket->flags);
                                #endif
                                mDecoderWaitForNextKeyFramePackets = false;
                            }else
                            {
                                #ifdef MSMEM_DEBUG_SEEKING
                                    LOG(LOG_VERBOSE, "Dropping %s frame packet %"PRId64" because we are waiting for next key frame packets after seek target frame %.2f", GetMediaTypeStr().c_str(), pPacketPts, mDecoderTargetFrameIndex);
                                #endif
                               tShouldReadNext = true;
                            }
                        }
                        #ifdef MSMEM_DEBUG_SEEKING
                            LOG(LOG_VERBOSE, "Read %s frame number %"PRId64" from input stream after seeking", GetMediaTypeStr().c_str(), pPacketPts);
                        #endif
                    }else
                    {
                        #if defined(MSMEM_DEBUG_DECODER_STATE) || defined(MSMEM_DEBUG_PACKETS)
                            LOG(LOG_WARN, "Read %s frame number %"PRId64" from input stream, last frame was %"PRId64, GetMediaTypeStr().c_str(), pPacketPts, mDecoderLastReadPts);
                        #endif
                    }
                }
                // check if PTS value is continuous
                if (pPacketPts < mDecoderLastReadPts)
                {// current packet is older than the last packet
                    LOG(LOG_WARN, "Found interleaved %s packets in %s source, non continuous PTS: %"PRId64" => %d", GetMediaTypeStr().c_str(), GetSourceTypeStr().c_str(), mDecoderLastReadPts, pPacketPts);
                }
                mDecoderLastReadPts = pPacketPts;
            }else
            {
                tShouldReadNext = true;
                #ifdef MSMEM_DEBUG_PACKETS
                    LOG(LOG_VERBOSE, "Read frame %d of stream %d instead of desired stream %d", pPacket->pts, pPacket->stream_index, mMediaStreamIndex);
                #endif
            }
        }
    }while ((tShouldReadNext) && (!mEOFReached) && (mDecoderNeeded));

    #ifdef MSMEM_DEBUG_PACKETS
        if (tReadIteration > 1)
            LOG(LOG_VERBOSE, "Needed %d read iterations to get next %s  packet from source stream", tReadIteration, GetMediaTypeStr().c_str());
        //LOG(LOG_VERBOSE, "New %s chunk with size: %d and stream index: %d", GetMediaTypeStr().c_str(), pPacket->size, pPacket->stream_index);
    #endif
}

//###################################
//### Decoder thread
//###################################
void* MediaSourceMem::Run(void* pArgs)
{
    AVFrame             *tSourceFrame = NULL;
    AVPacket            tPacketStruc, *tPacket = &tPacketStruc;
    int                 tFrameFinished = 0;
    int                 tDecoderResult = 0;
    int                 tRes = 0;
    int                 tChunkBufferSize = 0;
    uint8_t             *tChunkBuffer;
    int                 tWaitLoop;
    /* current chunk */
    int                 tCurrentChunkSize = 0;
    int64_t             tCurrrentInputPacketPts = 0; // input PTS/DTS stream from the packets
    int64_t             tCurrentOutputFrameNumber = 0; // ordered PTS/DTS stream from the decoder
    /* video scaler */
    VideoScaler         *tVideoScaler = NULL;
    /* picture as input */
    AVFrame             *tRGBFrame = NULL;
    bool                tInputIsPicture = false;
    /* audio */
    AVFifoBuffer        *tSampleFifo = NULL;
    AVFrame				*tAudioFrame = NULL;

    // reset EOF marker
    mEOFReached = false;

    tInputIsPicture = InputIsPicture();

    LOG(LOG_WARN, ">>>>>>>>>>>>>>>> %s-Decoding thread for %s media source started", GetMediaTypeStr().c_str(), GetSourceTypeStr().c_str());
    switch(mMediaType)
    {
        case MEDIA_VIDEO:
            SVC_PROCESS_STATISTIC.AssignThreadName("Video-Decoder(" + GetSourceTypeStr() + ")");

            // Allocate video frame for YUV format
            if ((tSourceFrame = AllocFrame()) == NULL)
                LOG(LOG_ERROR, "Out of video memory");

            if (!tInputIsPicture)
            {
                LOG(LOG_VERBOSE, "Allocating all objects for a video input");

                tChunkBufferSize = avpicture_get_size(mCodecContext->pix_fmt, mSourceResX, mSourceResY) + FF_INPUT_BUFFER_PADDING_SIZE;

                // allocate chunk buffer
                tChunkBuffer = (uint8_t*)malloc(tChunkBufferSize);

                // create video scaler
                tVideoScaler = CreateVideoScaler();

                // set the video scaler as FIFO for the decoder
                mDecoderFifo = tVideoScaler;
            }else
            {// we have single frame (a picture) as input
                LOG(LOG_VERBOSE, "Allocating all objects for a picture input");

                tChunkBufferSize = avpicture_get_size(PIX_FMT_RGB32, mDecoderTargetResX, mDecoderTargetResY) + FF_INPUT_BUFFER_PADDING_SIZE;

                // allocate chunk buffer
                tChunkBuffer = (uint8_t*)malloc(tChunkBufferSize);

                LOG(LOG_VERBOSE, "Decoder thread does not need the scaler thread because input is a picture");

                // Allocate video frame for RGB format
                if ((tRGBFrame = AllocFrame()) == NULL)
                    LOG(LOG_ERROR, "Out of video memory");

                // Assign appropriate parts of buffer to image planes in tRGBFrame
                avpicture_fill((AVPicture *)tRGBFrame, (uint8_t *)tChunkBuffer, PIX_FMT_RGB32, mDecoderTargetResX, mDecoderTargetResY);

                LOG(LOG_VERBOSE, "Going to create in-thread scaler context..");
                mVideoScalerContext = sws_getContext(mCodecContext->width, mCodecContext->height, mCodecContext->pix_fmt, mDecoderTargetResX, mDecoderTargetResY, PIX_FMT_RGB32, SWS_BICUBIC, NULL, NULL, NULL);

                mDecoderFifo = new MediaFifo(CalculateFrameBufferSize(), tChunkBufferSize, GetMediaTypeStr() + "-MediaSource" + GetSourceTypeStr() + "(Data)");
            }

            break;
        case MEDIA_AUDIO:
            SVC_PROCESS_STATISTIC.AssignThreadName("Audio-Decoder(" + GetSourceTypeStr() + ")");

            // Allocate video frame for YUV format
             if ((tAudioFrame = AllocFrame()) == NULL)
                 LOG(LOG_ERROR, "Out of audio memory");

            tChunkBufferSize = AVCODEC_MAX_AUDIO_FRAME_SIZE + FF_INPUT_BUFFER_PADDING_SIZE;

            // allocate chunk buffer
            tChunkBuffer = (uint8_t*)malloc(tChunkBufferSize);

            mDecoderFifo = new MediaFifo(CalculateFrameBufferSize(), tChunkBufferSize, GetMediaTypeStr() + "-MediaSource" + GetSourceTypeStr() + "(Data)");

            // init fifo buffer
            mDecoderAudioSamplesFifo = HM_av_fifo_alloc(MEDIA_SOURCE_SAMPLES_MULTI_BUFFER_SIZE * 2);

            break;
        default:
            SVC_PROCESS_STATISTIC.AssignThreadName("Decoder(" + GetSourceTypeStr() + ")");
            LOG(LOG_ERROR, "Unknown media type");
            return NULL;
            break;
    }

    mDecoderMetaDataFifo = new MediaFifo(CalculateFrameBufferSize(), sizeof(ChunkDescriptor), GetMediaTypeStr() + "-MediaSource" + GetSourceTypeStr() + "(MetaData)");

    // reset last PTS
    mDecoderLastReadPts = 0;
    mCurrentOutputFrameIndex = 0;
    mLastBufferedOutputFrameIndex = 0;

    // signal that decoder thread has finished init.
    mDecoderNeeded = true;

    CalculateExpectedOutputPerInputFrame();

    // trigger a avcodec_flush_buffers()
    ResetDecoderBuffers();

    LOG(LOG_WARN, "================ Entering main %s decoding loop for %s media source", GetMediaTypeStr().c_str(), GetSourceTypeStr().c_str());

    while(mDecoderNeeded)
    {
        #ifdef MSMEM_DEBUG_DECODER_STATE
            LOG(LOG_VERBOSE, "%s-decoder loop", GetMediaTypeStr().c_str());
        #endif

        mDecoderNeedWorkConditionMutex.lock();
        if (!DecoderFifoFull())
        {
            mDecoderNeedWorkConditionMutex.unlock();

            if (mEOFReached)
                LOG(LOG_WARN, "We started %s grabbing when EOF was already reached", GetMediaTypeStr().c_str());

            tWaitLoop = 0;

            if ((!tInputIsPicture) || (!mDecoderSinglePictureGrabbed))
            {// we try to read packet(s) from input stream -> either the desired picture or a single frame
                ReadPacketFromInputStream(tPacket, tCurrrentInputPacketPts);
            }else
            {// no packet was read
                //LOG(LOG_VERBOSE, "No packet was read");

                // generate dummy packet (av_free_packet() will destroy it later)
                av_init_packet(tPacket);
            }

            // #########################################
            // start packet processing
            // #########################################
            if (((tPacket->data != NULL) && (tPacket->size > 0)) || (mDecoderSinglePictureGrabbed /* we already grabbed the single frame from the picture input */))
            {
                #ifdef MSMEM_DEBUG_PACKET_RECEIVER
                    if ((tPacket->data != NULL) && (tPacket->size > 0))
                    {
                        LOG(LOG_VERBOSE, "New %s packet..", GetMediaTypeStr().c_str());
                        LOG(LOG_VERBOSE, "      ..duration: %d", tPacket->duration);
                        LOG(LOG_VERBOSE, "      ..pts: %"PRId64" stream [%d] pts: %"PRId64, tPacket->pts, mMediaStreamIndex, mFormatContext->streams[mMediaStreamIndex]->pts.val);
                        LOG(LOG_VERBOSE, "      ..stream: %"PRId64, tPacket->stream_index);
                        LOG(LOG_VERBOSE, "      ..dts: %"PRId64, tPacket->dts);
                        LOG(LOG_VERBOSE, "      ..size: %d", tPacket->size);
                        LOG(LOG_VERBOSE, "      ..pos: %"PRId64, tPacket->pos);
                        LOG(LOG_VERBOSE, "      ..frame number: %"PRId64, (int64_t)tPacket->pts / (tPacket->duration > 0 ? tPacket->duration : 1) /* works only for cfr, otherwise duration is variable */);
                        if (tPacket->flags == AV_PKT_FLAG_KEY)
                            LOG(LOG_VERBOSE, "      ..flags: key frame");
                        else
                            LOG(LOG_VERBOSE, "      ..flags: %d", tPacket->flags);
                    }
                #endif

                //LOG(LOG_VERBOSE, "New %s packet: dts: %"PRId64", pts: %"PRId64", pos: %"PRId64", duration: %d", GetMediaTypeStr().c_str(), tPacket->dts, tPacket->pts, tPacket->pos, tPacket->duration);

                // #########################################
                // process packet
                // #########################################
                mDecoderSeekMutex.lock();
                tCurrentChunkSize = 0;
                switch(mMediaType)
                {
                    case MEDIA_VIDEO:
                        {
                            if ((!tInputIsPicture) || (!mDecoderSinglePictureGrabbed))
                            {// we try to decode packet(s) from input stream -> either the desired picture or a single frame from the stream
                                // log statistics
                                AnnouncePacket(tPacket->size);
                                #ifdef MSMEM_DEBUG_PACKETS
                                    LOG(LOG_VERBOSE, "Decoding video frame (input is picture: %d)..", tInputIsPicture);
                                #endif

                                // did we read the single frame of a picture?
                                if ((tInputIsPicture) && (!mDecoderSinglePictureGrabbed))
                                {// store it
                                    LOG(LOG_VERBOSE, "Found picture packet of size %d, pts: %"PRId64", dts: %"PRId64" and store it in the picture buffer", tPacket->size, tPacket->pts, tPacket->dts);
                                    mDecoderSinglePictureGrabbed = true;
                                    mEOFReached = false;
                                }

                                // ############################
                                // ### DECODE FRAME
                                // ############################
                                tFrameFinished = 0;
                                tDecoderResult = HM_avcodec_decode_video(mCodecContext, tSourceFrame, &tFrameFinished, tPacket);

                                #ifdef MSMEM_DEBUG_VIDEO_FRAME_RECEIVER
                                    LOG(LOG_VERBOSE, "New video frame before PTS adaption..");
                                    LOG(LOG_VERBOSE, "      ..key frame: %d", tSourceFrame->key_frame);
                                    LOG(LOG_VERBOSE, "      ..picture type: %s-frame", GetFrameType(tSourceFrame).c_str());
                                    LOG(LOG_VERBOSE, "      ..pts: %"PRId64, tSourceFrame->pts);
                                    LOG(LOG_VERBOSE, "      ..pkt pts: %"PRId64, tSourceFrame->pkt_pts);
                                    LOG(LOG_VERBOSE, "      ..pkt dts: %"PRId64, tSourceFrame->pkt_dts);
//                                    LOG(LOG_VERBOSE, "      ..resolution: %d * %d", tSourceFrame->width, tSourceFrame->height);
//                                    LOG(LOG_VERBOSE, "      ..coded pic number: %d", tSourceFrame->coded_picture_number);
//                                    LOG(LOG_VERBOSE, "      ..display pic number: %d", tSourceFrame->display_picture_number);
                                #endif

                                // ############################
                                // ### store data planes and line size (for decoder loops >= 2)
                                // ############################
                                if ((tInputIsPicture) && (mDecoderSinglePictureGrabbed))
                                {
                                    for (int i = 0; i < AV_NUM_DATA_POINTERS; i++)
                                    {
                                        mDecoderSinglePictureData[i] = tSourceFrame->data[i];
                                        mDecoderSinglePictureLineSize[i] = tSourceFrame->linesize[i];
                                    }
                                }

                                //LOG(LOG_VERBOSE, "New %s source frame: dts: %"PRId64", pts: %"PRId64", pos: %"PRId64", pic. nr.: %d", GetMediaTypeStr().c_str(), tSourceFrame->pkt_dts, tSourceFrame->pkt_pts, tSourceFrame->pkt_pos, tSourceFrame->display_picture_number);

                                // MPEG2 picture repetition
                                if (tSourceFrame->repeat_pict != 0)
                                    LOG(LOG_ERROR, "MPEG2 picture should be repeated for %d times, unsupported feature!", tSourceFrame->repeat_pict);

                                #ifdef MSMEM_DEBUG_PACKETS
                                    LOG(LOG_VERBOSE, "    ..video decoding ended with result %d and %d bytes of output", tFrameFinished, tDecoderResult);
                                #endif

                                // log lost packets: difference between currently received frame number and the number of locally processed frames
                                SetLostPacketCount(tSourceFrame->coded_picture_number - mFrameNumber);

                                #ifdef MSMEM_DEBUG_PACKETS
                                    LOG(LOG_VERBOSE, "Video frame %d decoded", tSourceFrame->coded_picture_number);
                                #endif

                                // ############################
                                // ### check codec ID
                                // ############################
                                // do we have a video codec change at sender side?
                                if ((mSourceCodecId != 0) && (mSourceCodecId != mCodecContext->codec_id))
                                {
                                	if ((mCodecContext->codec_id == 5 /* h263 */) && (mSourceCodecId == 20 /* h263+ */))
                                	{// changed from h263+ to h263
                                		// no difference during decoding
                                	}else
                                	{// unsupported code change
										LOG(LOG_WARN, "Incoming video stream in %s source changed codec from %s(%d) to %s(%d)", GetSourceTypeStr().c_str(), GetFormatName(mSourceCodecId).c_str(), mSourceCodecId, GetFormatName(mCodecContext->codec_id).c_str(), mCodecContext->codec_id);
										LOG(LOG_ERROR, "Unsupported video codec change");
                                	}
									mSourceCodecId = mCodecContext->codec_id;
                                }

                                // ############################
                                // ### check video resolution
                                // ############################
                                // check if video resolution has changed within input stream (at remode side for network streams!)
                                if ((mResXLastGrabbedFrame != mCodecContext->width) || (mResYLastGrabbedFrame != mCodecContext->height))
                                {
                                    if ((mSourceResX != mCodecContext->width) || (mSourceResY != mCodecContext->height))
                                    {
                                        LOG(LOG_WARN, "Video resolution change in input stream from %s source detected", GetSourceTypeStr().c_str());

                                        // let the video scaler update the (ffmpeg based) scaler context
                                        tVideoScaler->ChangeInputResolution(mCodecContext->width, mCodecContext->height);

                                        // free the old chunk buffer
                                        free(tChunkBuffer);

                                        // calculate a new chunk buffer size for the new source video resolution
                                        tChunkBufferSize = avpicture_get_size(mCodecContext->pix_fmt, mCodecContext->width, mCodecContext->height) + FF_INPUT_BUFFER_PADDING_SIZE;

                                        // allocate the new chunk buffer
                                        tChunkBuffer = (uint8_t*)malloc(tChunkBufferSize);

                                        LOG(LOG_INFO, "Video resolution changed from %d*%d to %d * %d", mSourceResX, mSourceResY, mCodecContext->width, mCodecContext->height);

                                        // set grabbing resolution to the resolution from the codec, which has automatically detected the new resolution
                                        mSourceResX = mCodecContext->width;
                                        mSourceResY = mCodecContext->height;
                                    }

                                    mResXLastGrabbedFrame = mCodecContext->width;
                                    mResYLastGrabbedFrame = mCodecContext->height;
                                }

                                // ############################
                                // ### calculate PTS value
                                // ############################
                                if (!mRtpActivated)
                                {
                                    // save PTS value to deliver it later to the frame grabbing thread
                                    if ((tSourceFrame->pkt_dts != (int64_t)AV_NOPTS_VALUE) && (!MEDIA_SOURCE_MEM_USE_REORDERED_PTS))
                                    {// use DTS value from decoder
                                        #ifdef MSMEM_DEBUG_TIMING
                                            LOG(LOG_VERBOSE, "Setting current frame PTS to frame packet DTS %"PRId64, tSourceFrame->pkt_dts);
                                        #endif
                                        tCurrentOutputFrameNumber = rint(CalculateOutputFrameNumber(tSourceFrame->pkt_dts));
                                    }else if (tSourceFrame->pkt_pts != (int64_t)AV_NOPTS_VALUE)
                                    {// fall back to reordered PTS value
                                        #ifdef MSMEM_DEBUG_TIMING
                                            LOG(LOG_VERBOSE, "Setting current frame PTS to frame packet PTS %"PRId64, tSourceFrame->pkt_pts);
                                        #endif
                                        tCurrentOutputFrameNumber = rint(CalculateOutputFrameNumber(tSourceFrame->pkt_pts));
                                    }else
                                    {// fall back to packet's PTS value
                                        #ifdef MSMEM_DEBUG_TIMING
                                            LOG(LOG_VERBOSE, "Setting current frame PTS to packet PTS %"PRId64, tCurrrentInputPacketPts);
                                        #endif
                                        tCurrentOutputFrameNumber = rint(CalculateOutputFrameNumber(tCurrrentInputPacketPts));
                                    }
                                }else
                                {// RTP active
                                    #ifdef MSMEM_DEBUG_PRE_BUFFERING
                                        LOG(LOG_VERBOSE, "Setting current frame PTS to packet PTS (derived from RTP timestamp): %"PRId64, tCurrrentInputPacketPts);
                                    #endif

                                    // use the frame numbers which were derived from RTP timestamps
                                    tCurrentOutputFrameNumber = rint(CalculateOutputFrameNumber(tCurrrentInputPacketPts));// -mDecoderOutputFrameDelay;
                                }

                                if ((tSourceFrame->pkt_pts != tSourceFrame->pkt_dts) && (tSourceFrame->pkt_pts != (int64_t)AV_NOPTS_VALUE) && (tSourceFrame->pkt_dts != (int64_t)AV_NOPTS_VALUE))
                                    LOG(LOG_VERBOSE, "PTS(%"PRId64") and DTS(%"PRId64") differ after %s decoding step", tSourceFrame->pkt_pts, tSourceFrame->pkt_dts, GetMediaTypeStr().c_str());
                            }else
                            {// reuse the stored picture
                                // ############################
                                // ### restore data planes and line sizes
                                // ############################
                                #ifdef MSMEM_DEBUG_PACKETS
                                    LOG(LOG_VERBOSE, "Restoring the source frame's data planes and line sizes from the stored values");
                                #endif
                                for (int i = 0; i < AV_NUM_DATA_POINTERS; i++)
                                {
                                    tSourceFrame->data[i] = mDecoderSinglePictureData[i];
                                    tSourceFrame->linesize[i] = mDecoderSinglePictureLineSize[i];
                                    tSourceFrame->pict_type = AV_PICTURE_TYPE_I;
                                }

                                // simulate a monotonous increasing PTS value
                                tCurrrentInputPacketPts++;

                                // prepare the PTS value which is later delivered to the grabbing thread
                                tCurrentOutputFrameNumber = rint(CalculateOutputFrameNumber(tCurrrentInputPacketPts));

                                // simulate a successful decoding step
                                tFrameFinished = 1;
                                // some value above 0
                                tDecoderResult = INT_MAX;
                            }

                            // ############################
                            // ### process valid frame
                            // ############################
                            if (tDecoderResult >= 0)
                            {
                                if (tFrameFinished == 1)
                                {
                                    #ifdef MSMEM_DEBUG_VIDEO_FRAME_RECEIVER
                                        LOG(LOG_VERBOSE, "New video frame..");
                                        LOG(LOG_VERBOSE, "      ..key frame: %d", tSourceFrame->key_frame);
                                        LOG(LOG_VERBOSE, "      ..picture type: %s-frame", GetFrameType(tSourceFrame).c_str());
                                        LOG(LOG_VERBOSE, "      ..pts: %"PRId64", original PTS: %"PRId64, tSourceFrame->pts, tCurrentOutputFrameNumber);
                                        LOG(LOG_VERBOSE, "      ..pkt pts: %"PRId64, tSourceFrame->pkt_pts);
                                        LOG(LOG_VERBOSE, "      ..pkt dts: %"PRId64, tSourceFrame->pkt_dts);
//                                        LOG(LOG_VERBOSE, "      ..resolution: %d * %d", tSourceFrame->width, tSourceFrame->height);
//                                        LOG(LOG_VERBOSE, "      ..coded pic number: %d", tSourceFrame->coded_picture_number);
//                                        LOG(LOG_VERBOSE, "      ..display pic number: %d", tSourceFrame->display_picture_number);
//                                        LOG(LOG_VERBOSE, "      ..context delay: %d", mCodecContext->delay);
//                                        LOG(LOG_VERBOSE, "      ..context frame nr.: %d", mCodecContext->frame_number);
                                    #endif

                                    // use the PTS value from the packet stream because it refers to the original (without decoder delays) timing
                                    tSourceFrame->pts = tCurrentOutputFrameNumber;
                                    tSourceFrame->coded_picture_number = tCurrentOutputFrameNumber;
                                    tSourceFrame->display_picture_number = tCurrentOutputFrameNumber;

                                    // wait for next key frame packets (either an i-frame or a p-frame)
                                    if (mDecoderWaitForNextKeyFrame)
                                    {// we are still waiting for the next key frame after seeking in the input stream
                                        if (IsKeyFrame(tSourceFrame))
                                        {
                                            mDecoderWaitForNextKeyFrame = false;
                                            mDecoderWaitForNextKeyFrameTimeout = 0;
                                        }else
                                        {
                                            if (av_gettime() > mDecoderWaitForNextKeyFrameTimeout)
                                            {
                                                LOG(LOG_WARN, "We haven't found a key frame in the input stream within a specified time, giving up, continuing anyways");
                                                mDecoderWaitForNextKeyFrame = false;
                                                mDecoderWaitForNextKeyFrameTimeout = 0;
                                            }
                                        }

                                        if (!mDecoderWaitForNextKeyFrame)
                                        {
                                            LOG(LOG_VERBOSE, "Read first %s key frame at frame number %"PRId64" with flags %d from input stream after seeking", GetMediaTypeStr().c_str(), tCurrentOutputFrameNumber, tPacket->flags);
                                        }else
                                        {
                                            #ifdef MSMEM_DEBUG_SEEKING
                                                LOG(LOG_VERBOSE, "Dropping %s frame %"PRId64" because we are waiting for next key frame after seeking", GetMediaTypeStr().c_str(), tCurrentOutputFrameNumber);
                                            #endif
                                            #ifdef MSMEM_DEBUG_FRAME_QUEUE
                                                LOG(LOG_VERBOSE, "No %s frame will be written to frame queue, we ware waiting for the next key frame, current frame type: %s, EOF: %d", GetMediaTypeStr().c_str(), GetFrameType(tSourceFrame).c_str(), mEOFReached);
                                            #endif

                                        }
                                    }
                                    if (!mDecoderWaitForNextKeyFrame)
                                    {// we are not waiting for next key frame and can proceed as usual
                                        // ############################
                                        // ### ANNOUNCE FRAME (statistics)
                                        // ############################
                                        AnnounceFrame(tSourceFrame);

                                        // ############################
                                        // ### RECORD FRAME
                                        // ############################
                                        // re-encode the frame and write it to file
                                        if (mRecording)
                                            RecordFrame(tSourceFrame);

                                        // ############################
                                        // ### SCALE FRAME (CONVERT): is done inside a separate thread
                                        // ############################
                                        if (!tInputIsPicture)
                                        {// we decode one frame of a stream
                                            #ifdef MSMEM_DEBUG_PACKETS
                                                LOG(LOG_VERBOSE, "Scale (separate thread) video frame..");
                                                LOG(LOG_VERBOSE, "Video frame data: %p, %p, %p, %p", tSourceFrame->data[0], tSourceFrame->data[1], tSourceFrame->data[2], tSourceFrame->data[3]);
                                                LOG(LOG_VERBOSE, "Video frame line size: %d, %d, %d, %d", tSourceFrame->linesize[0], tSourceFrame->linesize[1], tSourceFrame->linesize[2], tSourceFrame->linesize[3]);
                                            #endif

                                            //LOG(LOG_VERBOSE, "New %s RGB frame: dts: %"PRId64", pts: %"PRId64", pos: %"PRId64", pic. nr.: %d", GetMediaTypeStr().c_str(), tRGBFrame->pkt_dts, tRGBFrame->pkt_pts, tRGBFrame->pkt_pos, tRGBFrame->display_picture_number);

                                            if ((tRes = avpicture_layout((AVPicture*)tSourceFrame, mCodecContext->pix_fmt, mSourceResX, mSourceResY, tChunkBuffer, tChunkBufferSize)) < 0)
                                            {
                                                LOG(LOG_WARN, "Couldn't copy AVPicture/AVFrame pixel data into chunk buffer because \"%s\"(%d)", strerror(AVUNERROR(tRes)), tRes);
                                            }else
                                            {// everything is okay, we have the current frame in tChunkBuffer and can send the frame to the video scaler
                                                tCurrentChunkSize = avpicture_get_size(mCodecContext->pix_fmt, mSourceResX, mSourceResY);
                                            }
                                        }else
                                        {// we decode a picture
                                            if  ((mDecoderSinglePictureResX != mDecoderTargetResX) || (mDecoderSinglePictureResY != mDecoderTargetResY))
                                            {
                                                #ifdef MSMEM_DEBUG_PACKETS
                                                    LOG(LOG_VERBOSE, "Scale (within decoder thread) video frame..");
                                                    LOG(LOG_VERBOSE, "Video frame data: %p, %p", tSourceFrame->data[0], tSourceFrame->data[1]);
                                                    LOG(LOG_VERBOSE, "Video frame line size: %d, %d", tSourceFrame->linesize[0], tSourceFrame->linesize[1]);
                                                #endif

                                                // scale the video frame
                                                LOG(LOG_VERBOSE, "Scaling video input picture..");
                                                tRes = HM_sws_scale(mVideoScalerContext, tSourceFrame->data, tSourceFrame->linesize, 0, mCodecContext->height, tRGBFrame->data, tRGBFrame->linesize);
                                                if (tRes == 0)
                                                    LOG(LOG_ERROR, "Failed to scale the video frame");

                                                LOG(LOG_VERBOSE, "Decoded picture into RGB frame: dts: %"PRId64", pts: %"PRId64", pos: %"PRId64", pic. nr.: %d", tRGBFrame->pkt_dts, tRGBFrame->pkt_pts, tRGBFrame->pkt_pos, tRGBFrame->display_picture_number);

                                                mDecoderSinglePictureResX = mDecoderTargetResX;
                                                mDecoderSinglePictureResY = mDecoderTargetResY;
                                            }else
                                            {// use stored RGB frame

                                                //HINT: tCurremtChunkSize will be updated later
                                                //HINT: tChunkBuffer is still valid from first decoder loop
                                            }
                                            // return size of decoded frame
                                            tCurrentChunkSize = avpicture_get_size(PIX_FMT_RGB32, mDecoderTargetResX, mDecoderTargetResY);
                                        }

                                        // ############################
                                        // ### WRITE FRAME TO OUTPUT FIFO
                                        // ############################
                                        // add new chunk to FIFO
                                        if (tCurrentChunkSize <= mDecoderFifo->GetEntrySize())
                                        {
                                            #ifdef MSMEM_DEBUG_PACKETS
                                                LOG(LOG_VERBOSE, "Writing %d %s bytes at %p to FIFO with frame nr.%"PRId64, tCurrentChunkSize, GetMediaTypeStr().c_str(), tChunkBuffer, tCurrentOutputFrameNumber);
                                            #endif
                                            WriteFrameOutputBuffer((char*)tChunkBuffer, tCurrentChunkSize, tCurrentOutputFrameNumber);

                                            // prepare frame number for next loop
                                            tCurrentOutputFrameNumber++;

                                            #ifdef MSMEM_DEBUG_DECODER_STATE
                                                LOG(LOG_VERBOSE, "Successful audio buffer loop");
                                            #endif
                                        }else
                                        {
                                            LOG(LOG_ERROR, "Cannot write a %s chunk of %d bytes to the FIFO with %d bytes slots", GetMediaTypeStr().c_str(),  tCurrentChunkSize, mDecoderFifo->GetEntrySize());
                                        }
                                    }else
                                    {// still waiting for first key frame
                                        //nothing to do
                                    }
                                }else
                                {// tFrameFinished != 1
                                    // we shouldn't have frame buffering because we use CODEC_FLAG_LOW_DELAY
                                    LOG(LOG_WARN, "Video frame was buffered, codec context flags: 0x%X", mCodecContext->flags);
                                    mDecoderOutputFrameDelay++;
                                }
                            }else
                            {// tDecoderResult < 0
                                LOG(LOG_ERROR, "Couldn't decode video frame %"PRId64" because \"%s\"(%d), got a decoder result: %d", tCurrrentInputPacketPts, strerror(AVUNERROR(tDecoderResult)), AVUNERROR(tDecoderResult), tFrameFinished);
                            }
                        }
                        break;

                    case MEDIA_AUDIO:
                        {
                            // assign the only existing plane because the output will be packed audio data
                            uint8_t *tDecodedAudioSamples = tChunkBuffer;

                            // ############################
                            // ### DECODE FRAME
                            // ############################
                            // log statistics
                            AnnouncePacket(tPacket->size);

                            int tOutputAudioBytesPerSample = av_get_bytes_per_sample(mOutputAudioFormat);
                            int tInputAudioBytesPerSample = av_get_bytes_per_sample(mInputAudioFormat);

                            //printf("DecodeFrame..\n");
                            // Decode the next chunk of data
                            tDecoderResult = avcodec_decode_audio4(mCodecContext, tAudioFrame, &tFrameFinished, tPacket);
                            if (tDecoderResult >= 0)
                            {
                                if (tFrameFinished == 1)
                                {
                                    // ############################
                                    // ### ANNOUNCE FRAME (statistics)
                                    // ############################
                                    AnnounceFrame(tAudioFrame);

                                    // ############################
                                    // ### RECORD FRAME
                                    // ############################
                                    // re-encode the frame and write it to file
                                    if (mRecording)
                                        RecordFrame(tAudioFrame);

                                    #ifdef MSMEM_DEBUG_AUDIO_FRAME_RECEIVER
                                        LOG(LOG_VERBOSE, "New audio frame..");
                                        LOG(LOG_VERBOSE, "      ..pts: %"PRId64", original PTS: %"PRId64, tAudioFrame->pts, tCurrrentInputPacketPts);
                                        LOG(LOG_VERBOSE, "      ..size: %d bytes (%d samples of format %s)", tAudioFrame->nb_samples * tOutputAudioBytesPerSample * mOutputAudioChannels, tAudioFrame->nb_samples, av_get_sample_fmt_name(mOutputAudioFormat));
                                    #endif

                                    if (mAudioResampleContext != NULL)
                                    {// audio resampling needed: we have to insert an intermediate step, which resamples the audio chunk

                                        // ############################
                                        // ### RESAMPLE FRAME (CONVERT)
                                        // ############################
                                        const uint8_t **tAudioFramePlanes = (const uint8_t **)tAudioFrame->extended_data;
                                        int tResampledBytes = (tOutputAudioBytesPerSample * mOutputAudioChannels) * HM_swr_convert(mAudioResampleContext, (uint8_t**)&tDecodedAudioSamples /* only one plane because this is packed audio data */, AVCODEC_MAX_AUDIO_FRAME_SIZE / (tOutputAudioBytesPerSample * mOutputAudioChannels) /* amount of possible output samples */, (const uint8_t**)tAudioFramePlanes, tAudioFrame->nb_samples);
                                        if(tResampledBytes > 0)
                                        {
                                            tCurrentChunkSize = tResampledBytes;
                                        }else
                                        {
                                            LOG(LOG_ERROR, "Amount of resampled bytes (%d) is invalid", tResampledBytes);
                                        }
                                    }else
                                    {// no audio resampling needed
                                        // we can use the input buffer without modifications
                                        tCurrentChunkSize = av_samples_get_buffer_size(NULL, mCodecContext->channels, tAudioFrame->nb_samples, (enum AVSampleFormat) tAudioFrame->format, 1);
                                        tDecodedAudioSamples = tAudioFrame->data[0];
                                    }

                                    if (tCurrentChunkSize > 0)
                                    {
                                        // get the buffered samples (= pts)
                                        int tAudioFifoBufferedSamples = av_fifo_size(mDecoderAudioSamplesFifo) /* buffer sie in bytes */ / (tOutputAudioBytesPerSample * mOutputAudioChannels);

                                        // ############################
                                        // ### WRITE FRAME TO FIFO
                                        // ############################
                                        #ifdef MSMEM_DEBUG_PACKETS
                                            LOG(LOG_VERBOSE, "Adding %d bytes to AUDIO FIFO with buffer of %d bytes (%d samples), packet pts: %"PRId64, tCurrentChunkSize, av_fifo_size(mDecoderAudioSamplesFifo), tAudioFifoBufferedSamples, tCurrrentInputPacketPts);
                                        #endif
                                        // is there enough space in the FIFO?
                                        if (av_fifo_space(mDecoderAudioSamplesFifo) < tCurrentChunkSize)
                                        {// no, we need reallocation
                                            if (av_fifo_realloc2(mDecoderAudioSamplesFifo, av_fifo_size(mDecoderAudioSamplesFifo) + tCurrentChunkSize - av_fifo_space(mDecoderAudioSamplesFifo)) < 0)
                                            {
                                                // acknowledge failed
                                                LOG(LOG_ERROR, "Reallocation of FIFO audio buffer failed");
                                            }
                                        }

                                        //LOG(LOG_VERBOSE, "ChunkSize: %d", pChunkSize);
                                        // write new samples into fifo buffer
                                        av_fifo_generic_write(mDecoderAudioSamplesFifo, tDecodedAudioSamples, tCurrentChunkSize, NULL);

                                        tCurrentOutputFrameNumber = (int64_t)rint(CalculateOutputFrameNumber(tCurrrentInputPacketPts) - (double)tAudioFifoBufferedSamples / MEDIA_SOURCE_SAMPLES_PER_BUFFER);

                                        // save PTS value to deliver it later to the frame grabbing thread
                                        #ifdef MSMEM_DEBUG_PRE_BUFFERING
                                            LOG(LOG_VERBOSE, "Setting current frame nr. to %"PRId64", packet PTS: %"PRId64, tCurrentOutputFrameNumber, tCurrrentInputPacketPts);
                                        #endif

                                        // the amount of bytes we want to have in an output frame
                                        int tDesiredOutputSize = MEDIA_SOURCE_SAMPLES_PER_BUFFER * tOutputAudioBytesPerSample * mOutputAudioChannels;

                                        int tLoops = 0;
                                        while (av_fifo_size(mDecoderAudioSamplesFifo) >= MEDIA_SOURCE_SAMPLES_PER_BUFFER * tOutputAudioBytesPerSample * mOutputAudioChannels)
                                        {
                                            tLoops++;
                                            // ############################
                                            // ### READ FRAME FROM FIFO
                                            // ############################
                                            #ifdef MSM_DEBUG_PACKETS
                                                LOG(LOG_VERBOSE, "Loop %d-Reading %d bytes from %d bytes of fifo, current frame nr.: %"PRId64, tLoops, MSF_DESIRED_AUDIO_INPUT_SIZE, av_fifo_size(mDecoderAudioSamplesFifo), tCurrentOutputFrameNumber);
                                            #endif
                                            // read sample data from the fifo buffer
                                            HM_av_fifo_generic_read(mDecoderAudioSamplesFifo, (void*)tChunkBuffer, tDesiredOutputSize);
                                            tCurrentChunkSize = tDesiredOutputSize;

                                            // ############################
                                            // ### WRITE FRAME TO OUTPUT FIFO
                                            // ############################
                                            // add new chunk to FIFO
                                            if (tCurrentChunkSize <= mDecoderFifo->GetEntrySize())
                                            {
                                                #ifdef MSMEM_DEBUG_AUDIO_FRAME_RECEIVER
                                                    LOG(LOG_VERBOSE, "Writing %d %s bytes at %p to output FIFO with frame nr. %"PRId64", remaining audio data: %d bytes", tCurrentChunkSize, GetMediaTypeStr().c_str(), tChunkBuffer, tCurrentOutputFrameNumber, av_fifo_size(mDecoderAudioSamplesFifo));
                                                #endif
                                                WriteFrameOutputBuffer((char*)tChunkBuffer, tCurrentChunkSize, tCurrentOutputFrameNumber);

                                                // prepare frame number for next loop
                                                tCurrentOutputFrameNumber++;

                                                #ifdef MSMEM_DEBUG_DECODER_STATE
                                                    LOG(LOG_VERBOSE, "Successful audio buffer loop");
                                                #endif
                                            }else
                                            {
                                                LOG(LOG_ERROR, "Cannot write a %s chunk of %d bytes to the FIFO with %d bytes slots", GetMediaTypeStr().c_str(),  tCurrentChunkSize, mDecoderFifo->GetEntrySize());
                                            }
                                        }
                                    }
                                }else
                                {// audio frame was buffered by ffmpeg
                                    LOG(LOG_VERBOSE, "Audio frame was buffered");
                                    mDecoderOutputFrameDelay++;
                                }
                            }else
                            {// tDecoderResult < 0
                                LOG(LOG_WARN, "Couldn't decode audio samples %"PRId64"  because \"%s\"(%d)", tCurrrentInputPacketPts, strerror(AVUNERROR(tDecoderResult)), AVUNERROR(tDecoderResult));
                            }
                        }
                        break;
                    default:
                            LOG(LOG_ERROR, "Media type unknown");
                            break;
                }
                mDecoderSeekMutex.unlock();
            }

            // free packet buffer
            av_free_packet(tPacket);

            mDecoderNeedWorkConditionMutex.lock();
            if (mEOFReached)
            {// EOF, wait until restart
                // add empty chunk to FIFO
                #ifdef MSMEM_DEBUG_PACKETS
                    LOG(LOG_VERBOSE, "EOF reached, writing empty chunk to %s stream decoder FIFO", GetMediaTypeStr().c_str());
                #endif

                char tData[4];
                WriteFrameOutputBuffer(tData, 0, 0);

                // time to sleep
                #ifdef MSMEM_DEBUG_DECODER_STATE
                    LOG(LOG_VERBOSE, "EOF for %s source reached, wait some time and check again, loop %d", GetMediaTypeStr().c_str(), ++tWaitLoop);
                #endif
                mDecoderNeedWorkCondition.Wait(&mDecoderNeedWorkConditionMutex);
                mDecoderLastReadPts = 0;
                mEOFReached = false;
                #ifdef MSMEM_DEBUG_DECODER_STATE
                    LOG(LOG_VERBOSE, "Continuing after stream decoding was restarted");
                #endif
            }
            mDecoderNeedWorkConditionMutex.unlock();
        }else
        {// decoder FIFO is full, nothing to be done
            #ifdef MSMEM_DEBUG_DECODER_STATE
                if(mDecoderFifo != NULL)
                    LOG(LOG_VERBOSE, "Nothing to do for %s decoder, FIFO has %d of %d entries, wait some time and check again, loop %d", GetMediaTypeStr().c_str(), mDecoderFifo->GetUsage(), mDecoderFifo->GetSize(), ++tWaitLoop);
                else
                    LOG(LOG_VERBOSE, "Nothing to do for %s decoder, wait some time and check again, loop %d", GetMediaTypeStr().c_str(), ++tWaitLoop);
            #endif
            mDecoderNeedWorkCondition.Wait(&mDecoderNeedWorkConditionMutex);
            #ifdef MSMEM_DEBUG_DECODER_STATE
                if(mDecoderFifo != NULL)
                    LOG(LOG_VERBOSE, "Continuing after new data is needed, current FIFO size is: %d of %d", mDecoderFifo->GetUsage(), mDecoderFifo->GetSize());
                else
                    LOG(LOG_VERBOSE, "Continuing after new data is needed");
            #endif
            mDecoderNeedWorkConditionMutex.unlock();
        }
    }

    switch(mMediaType)
    {
        case MEDIA_VIDEO:
                if (!tInputIsPicture)
                {
                    LOG(LOG_WARN, "VIDEO decoder thread stops scaler thread..");
                    tVideoScaler->StopScaler();
                    LOG(LOG_VERBOSE, "..VIDEO decoder thread stopped scaler thread");
                }else
                {
                    // Free the RGB frame
                    av_free(tRGBFrame);

                    sws_freeContext(mVideoScalerContext);
                    mVideoScalerContext = NULL;
                }

                // Free the YUV frame
                av_free(tSourceFrame);

                break;
        case MEDIA_AUDIO:
                // free fifo buffer
                av_fifo_free(mDecoderAudioSamplesFifo);
                mDecoderAudioSamplesFifo = NULL;

                av_free(tAudioFrame);

                break;
        default:
                break;
    }

    free(tChunkBuffer);

    delete mDecoderFifo;
    delete mDecoderMetaDataFifo;

    mDecoderFifo = NULL;

    LOG(LOG_WARN, "%s decoder main loop finished for %s media source <<<<<<<<<<<<<<<<", GetMediaTypeStr().c_str(), GetSourceTypeStr().c_str());

    return NULL;
}

void MediaSourceMem::ResetDecoderBuffers()
{
    mDecoderSeekMutex.lock();

    // flush ffmpeg internal buffers
    LOG(LOG_WARN, "Reseting %s decoder internal buffers after seeking in input stream", GetMediaTypeStr().c_str());
    avcodec_flush_buffers(mCodecContext);

    // reset the library internal frame FIFO
    LOG(LOG_VERBOSE, "Reseting %s decoder internal FIFO after seeking in input stream", GetMediaTypeStr().c_str());
    mDecoderFifo->ClearFifo();
    mDecoderMetaDataFifo->ClearFifo();

    if ((mMediaType == MEDIA_AUDIO) && (mDecoderAudioSamplesFifo != NULL) && (av_fifo_size(mDecoderAudioSamplesFifo) > 0))
    {
        LOG(LOG_VERBOSE, "Reseting %s decoder internal buffers resample FIFO after seeking in input stream", GetMediaTypeStr().c_str());
        av_fifo_drain(mDecoderAudioSamplesFifo, av_fifo_size(mDecoderAudioSamplesFifo));
    }

    mDecoderOutputFrameDelay = 0;

    // wait for next key frame before we do anything
    LOG(LOG_VERBOSE, "Waiting for first %s key frame after reset of decoder buffers", GetMediaTypeStr().c_str());
    mDecoderWaitForNextKeyFramePackets = true;
    mDecoderWaitForNextKeyFrame = true;
    mDecoderWaitForNextKeyFrameTimeout = av_gettime() + MSM_WAITING_FOR_FIRST_KEY_FRAME_TIMEOUT * 1000 * 1000;

    mDecoderSeekMutex.unlock();
}

void MediaSourceMem::WriteFrameOutputBuffer(char* pBuffer, int pBufferSize, int64_t pOutputFrameNumber)
{
    if (mDecoderFifo == NULL)
        LOG(LOG_ERROR, "Invalid decoder FIFO");

    #ifdef MSMEM_DEBUG_FRAME_QUEUE
        LOG(LOG_VERBOSE, ">>> Writing %s frame of %d bytes and pts %"PRId64", FIFOs: %d/%d", GetMediaTypeStr().c_str(), pBufferSize, pOutputFrameNumber, mDecoderFifo->GetUsage(), mDecoderMetaDataFifo->GetUsage());
    #endif

    if (pOutputFrameNumber != 0)
        mLastBufferedOutputFrameIndex = pOutputFrameNumber;

    // write A/V data to output FIFO
    mDecoderFifo->WriteFifo(pBuffer, pBufferSize);

    // add meta description about current chunk to different FIFO
    struct ChunkDescriptor tChunkDesc;
    tChunkDesc.Pts = pOutputFrameNumber;
    mDecoderMetaDataFifo->WriteFifo((char*) &tChunkDesc, sizeof(tChunkDesc));

    // update pre-buffer time value
    UpdateBufferTime();
}

void MediaSourceMem::ReadFrameOutputBuffer(char *pBuffer, int &pBufferSize, int64_t &pOutputFrameNumber)
{
    if (mDecoderFifo == NULL)
        LOG(LOG_ERROR, "Invalid decoder FIFO");

    // read A/V data from output FIFO
    mDecoderFifo->ReadFifo(pBuffer, pBufferSize);

    // read meta description about current chunk from different FIFO
    struct ChunkDescriptor tChunkDesc;
    int tChunkDescSize = sizeof(tChunkDesc);
    mDecoderMetaDataFifo->ReadFifo((char*)&tChunkDesc, tChunkDescSize);
    pOutputFrameNumber = tChunkDesc.Pts;
    if (tChunkDescSize != sizeof(tChunkDesc))
        LOG(LOG_ERROR, "Read from FIFO a chunk with wrong size of %d bytes, expected size is %d bytes", tChunkDescSize, sizeof(tChunkDesc));

    #ifdef MSMEM_DEBUG_FRAME_QUEUE
        LOG(LOG_VERBOSE, "Returning from decoder FIFO the %s frame (PTS = %"PRId64"), remaing frames in FIFO: %"PRId64, GetMediaTypeStr().c_str(), pOutputFrameNumber, mDecoderFifo->GetUsage());
    #endif

    // update pre-buffer time value
    UpdateBufferTime();
}

double MediaSourceMem::CalculateInputFrameNumber(double pFrameNumber)
{
    double tTransformationFactor = GetInputFrameRate() / GetOutputFrameRate();

    return pFrameNumber * tTransformationFactor;
}

double MediaSourceMem::CalculateOutputFrameNumber(double pFrameNumber)
{
    double tTransformationFactor = GetOutputFrameRate() / GetInputFrameRate();

    return pFrameNumber * tTransformationFactor;
}

void MediaSourceMem::CalculateExpectedOutputPerInputFrame()
{
    switch(mMediaType)
    {
        case MEDIA_AUDIO:
                if (mCodecContext != NULL)
                {
                    int tInputSamplesPerFrame = mCodecContext->frame_size;
                    if (tInputSamplesPerFrame == 0)
                        tInputSamplesPerFrame = MEDIA_SOURCE_SAMPLES_PER_BUFFER; // fall back to the default value

                    int64_t tInputFrameSize = tInputSamplesPerFrame * av_get_bytes_per_sample(mInputAudioFormat) * mInputAudioChannels;

                    float tSizeRatio = ((float)mOutputAudioChannels / mInputAudioChannels) *
                                       ((float)av_get_bytes_per_sample(mOutputAudioFormat) / av_get_bytes_per_sample(mInputAudioFormat)) *
                                       ((float)mOutputAudioSampleRate / mInputAudioSampleRate);

                    int64_t tMaxOutputSize = (int64_t)((float)tInputFrameSize * tSizeRatio);

                    int tBytesPerOutputSample = av_get_bytes_per_sample(mOutputAudioFormat) * mOutputAudioChannels;

                    // round up to next valid size depending on the amount of bytes per output sample
                    tMaxOutputSize = tMaxOutputSize / (tBytesPerOutputSample) * tBytesPerOutputSample + tBytesPerOutputSample;

                    int64_t tOutputFrameSize = MEDIA_SOURCE_SAMPLES_PER_BUFFER * av_get_bytes_per_sample(mOutputAudioFormat) * mOutputAudioChannels;

                    float tMaxPossibleOutputFrames = (float)tMaxOutputSize / tOutputFrameSize;

                    mDecoderExpectedMaxOutputPerInputFrame = rint(tMaxPossibleOutputFrames);

                    //LOG(LOG_ERROR, "Expected max. output frames: %d(%.2f), size ratio: %.2f (%d/%d + %d/%d + %d/%d), max. output size: %"PRId64, tOutputOfNextInputFrame, tMaxPossibleOutputFrames, tSizeRatio, mOutputAudioChannels, mInputAudioChannels, av_get_bytes_per_sample(mOutputAudioFormat), av_get_bytes_per_sample(mInputAudioFormat), mOutputAudioSampleRate, mInputAudioSampleRate, tMaxOutputSize);
                }else
                    mDecoderExpectedMaxOutputPerInputFrame = 8; // a fallback to a "good" value
                break;
        case MEDIA_VIDEO:
                mDecoderExpectedMaxOutputPerInputFrame = 1;
                break;
        default:
                mDecoderExpectedMaxOutputPerInputFrame = 1;
                break;
    }
}

bool MediaSourceMem::DecoderFifoFull()
{
    bool tResult;

    tResult = ((mDecoderFifo == NULL)/* decoder FIFO is invalid */ || (mDecoderMetaDataFifo == NULL) /* decoder meta data FIFO is invalid */) ||
               (mDecoderFifo->GetUsage() + mDecoderExpectedMaxOutputPerInputFrame > mDecoderFifo->GetSize() - 1 /* check the usage, one slot for a 0 byte signaling chunk*/);
    //HINT: meta data FIFO has always the same size like the data FIFO => we don't have to check the usage of meta data FIFO

    return tResult;
}

void MediaSourceMem::UpdateBufferTime()
{
    //LOG(LOG_VERBOSE, "Updating pre-buffer time value");

    float tBufferSize = 0;
    if (mDecoderFifo != NULL)
        tBufferSize = mDecoderFifo->GetUsage();

    mDecoderFrameBufferTime = tBufferSize / GetOutputFrameRate();
}

void MediaSourceMem::CalibrateRTGrabbing()
{
    // adopt the stored pts value which represent the start of the media presentation in real-time useconds
    float  tRelativeFrameIndex = mLastBufferedOutputFrameIndex - CalculateOutputFrameNumber(mInputStartPts);
    double tRelativeTime = (int64_t)((double)AV_TIME_BASE * tRelativeFrameIndex / GetOutputFrameRate());
    #ifdef MSMEM_DEBUG_CALIBRATION
        LOG(LOG_WARN, "Calibrating %s RT playback, old PTS start: %.2f, pre-buffer time: %.2f", GetMediaTypeStr().c_str(), mSourceStartTimeForRTGrabbing, mDecoderFramePreBufferTime);
    #endif
	if (tRelativeTime < 0)
	{
		LOG(LOG_ERROR, "Found invalid relative PTS value of: %.2f", (float)tRelativeTime);
		tRelativeTime = 0;
	}
	mSourceStartTimeForRTGrabbing = av_gettime() - tRelativeTime  + mSourceTimeShiftForRTGrabbing + mDecoderFramePreBufferTime * AV_TIME_BASE;
    #ifdef MSMEM_DEBUG_CALIBRATION
        LOG(LOG_WARN, "Calibrating %s RT playback: new PTS start: %.2f, rel. frame index: %.2f, rel. time: %.2f ms", GetMediaTypeStr().c_str(), mSourceStartTimeForRTGrabbing, tRelativeFrameIndex, (float)(tRelativeTime / 1000));
    #endif
}

void MediaSourceMem::WaitForRTGrabbing()
{
	// return immediately if PTS from grabber is invalid
	if ((mRtpActivated) && (mCurrentOutputFrameIndex == 0))
	{
		LOG(LOG_WARN, "PTS from grabber is invalid: %.2f", (float)mCurrentOutputFrameIndex);
		return;
	}

    // calculate the current (normalized) frame index of the grabber
    float tNormalizedFrameIndexFromGrabber = mCurrentOutputFrameIndex - CalculateOutputFrameNumber(mInputStartPts); // the normalized frame index
    // return immediately if RT-grabbing is not possible
    if (tNormalizedFrameIndexFromGrabber < 0)
    {
        LOG(LOG_WARN, "Normalized frame index is invalid");
        return;
    }

    // the PTS value of the last output frame
    uint64_t tCurrentPtsFromGrabber = (uint64_t)(1000 * tNormalizedFrameIndexFromGrabber / GetOutputFrameRate()); // in ms

    // calculate the PTS offset between the RTCP PTS reference and the last grabbed frame
    int64_t tDesiredPlayOutTime = 1000 * ((int64_t)tCurrentPtsFromGrabber); // in us

    // calculate the current (normalized) play-out time of the current A/V stream
    int64_t tCurrentPlayOutTime = av_gettime() - (int64_t)mSourceStartTimeForRTGrabbing; // in us

    // check if we have already reached the pre-buffer threshold time
    if (tCurrentPlayOutTime < 0)
    {// no pre-buffering is currently running, we should wait until pre-buffer time is reached
        #ifdef MSMEM_DEBUG_TIMING
            LOG(LOG_WARN, "Play-out time difference %"PRId64" points into the future", tCurrentPlayOutTime);
        #endif

        // transform into waiting time
        tCurrentPlayOutTime *= (-1);

        // wait until pre-buffer time is reached
        if (tCurrentPlayOutTime <= MEDIA_SOURCE_MEM_FRAME_INPUT_QUEUE_MAX_TIME * AV_TIME_BASE)
            Thread::Suspend(tCurrentPlayOutTime);
        else
        {
            LOG(LOG_WARN, "Found in %s %s source an invalid delay time of %"PRId64" s for reaching pre-buffer threshold time, pre-buffer time: %.2f, PTS of last queued frame: %"PRId64", PTS of last grabbed frame: %.2f", GetMediaTypeStr().c_str(), GetSourceTypeStr().c_str(), tCurrentPlayOutTime / 1000, mDecoderFramePreBufferTime, mDecoderLastReadPts, (float)mCurrentOutputFrameIndex);
            LOG(LOG_WARN, "Triggering re-calibration of RT grabbing");
            mDecoderRecalibrateRTGrabbingAfterSeeking = true;
        }

        // update play-out time
        tCurrentPlayOutTime = av_gettime() - (int64_t)mSourceStartTimeForRTGrabbing; // in us
    }

    // calculate the time offset between the desired and current play-out time, which can be used for a wait cycle (Thread::Suspend)
    int64_t tResultingTimeOffset = tDesiredPlayOutTime - tCurrentPlayOutTime; // in us

    #ifdef MSMEM_DEBUG_TIMING
        LOG(LOG_VERBOSE, "%s-current relative frame index: %f, relative time: %"PRIu64" ms (Fps: %3.2f), stream start time: %f us, packet's relative play out time: %f us, time difference: %f us", GetMediaTypeStr().c_str(), tNormalizedFrameIndexFromGrabber, tCurrentPtsFromGrabber, GetInputFrameRate(), (float)mInputStartPts, tDesiredPlayOutTime, tResultingTimeOffset);
    #endif
    // adapt timing to real-time
    if (tResultingTimeOffset > 0)
    {
        #ifdef MSMEM_DEBUG_TIMING
            LOG(LOG_WARN, "%s-%s-sleeping for %"PRId64" ms (%"PRId64" - %"PRId64") for frame %.2f, RT ref. time: %.2f", GetMediaTypeStr().c_str(), GetSourceTypeStr().c_str(), tResultingTimeOffset / 1000, tDesiredPlayOutTime, tCurrentPlayOutTime, (float)mCurrentOutputFrameIndex, (float)mSourceStartTimeForRTGrabbing);
        #endif
		if (tResultingTimeOffset <= MEDIA_SOURCE_MEM_FRAME_INPUT_QUEUE_MAX_TIME * AV_TIME_BASE)
			Thread::Suspend(tResultingTimeOffset);
		else
		{
			LOG(LOG_WARN, "Found in %s %s source an invalid delay time of %"PRId64" s, pre-buffer time: %.2f, PTS of last queued frame: %"PRId64", PTS of last grabbed frame: %.2f", GetMediaTypeStr().c_str(), GetSourceTypeStr().c_str(), tResultingTimeOffset / 1000, mDecoderFramePreBufferTime, mDecoderLastReadPts, (float)mCurrentOutputFrameIndex);
			LOG(LOG_WARN, "Triggering re-calibration of RT grabbing");
			mDecoderRecalibrateRTGrabbingAfterSeeking = true;
		}
	}else
    {
        #ifdef MSMEM_DEBUG_TIMING
            if (tResultingTimeOffset < -MEDIA_SOURCE_MEM_VIDEO_FRAME_DROP_THRESHOLD)
                LOG(LOG_VERBOSE, "System too slow?, %s-grabbing is %"PRId64" ms too late", GetMediaTypeStr().c_str(), tResultingTimeOffset / (-1000));
        #endif
    }
}

int64_t MediaSourceMem::GetSynchronizationTimestamp()
{
    int64_t tResult = 0;

    if ((mCurrentOutputFrameIndex != 0) && (!mDecoderRecalibrateRTGrabbingAfterSeeking))
    {// we have some first passed A/V frames, the decoder does not need to re-calibrate the RT grabber
        /******************************************
         * The following lines do the following:
         *   - use the RTCP sender reports and extract the reference RTP timestamps and NTP time
         *     =>> we know when exactly (NTP time!) a given RTP play-out time index passed the processing chain at sender side
         *   - interpolate the given NTP time from sender side to conclude the NTP time (at sender side!) for the currently grabbed frame
         *     =>> we know (this is the hope!) exactly when the current frame passed the processing chain at sender side
         *   - return the calculated NTP time as synchronization timestamp in micro seconds
         *
         *   - when this approach is applied for both the video and audio stream (which is received from the same physical host with the same real-time clock inside!),
         *     a time difference can be derived which corresponds to the A/V drift in micro seconds of the video and audio playback on the local(!) machine
         ******************************************/
        if (mRtpActivated)
        {// RTP active
            // get the reference from RTCP
            uint64_t tReferenceNtpTime;
            unsigned int tReferencePts;
            GetSynchronizationReferenceFromRTP(tReferenceNtpTime, tReferencePts);
            #ifdef MSMEM_DEBUG_AV_SYNC
                LOG(LOG_VERBOSE, "Got %s synchronization: at %"PRIu64" was pts %u", GetMediaTypeStr().c_str(), tReferenceNtpTime, tReferencePts);
            #endif

            // calculate the current (normalized) frame index of the grabber
            float tNormalizedFrameIndexFromGrabber = CalculateInputFrameNumber(mCurrentOutputFrameIndex) - mInputStartPts; // the normalized frame index

            // the PTS value of the last output frame
            uint64_t tCurrentPtsFromGrabber = (uint64_t)(1000 * tNormalizedFrameIndexFromGrabber / GetInputFrameRate()); // in ms

            // calculate the PTS offset between the RTCP PTS reference and the last grabbed frame
            int64_t tTimeOffsetFromReference = 1000 * ((int64_t)tCurrentPtsFromGrabber - tReferencePts); // in us

            if (tReferenceNtpTime != 0)
            {
                // calculate the NTP time (we refer to the NTP time of the RTP source) of the currently grabbed frame
                tResult = (int64_t)tReferenceNtpTime + tTimeOffsetFromReference;

                #ifdef MSMEM_DEBUG_AV_SYNC
                    int64_t tLocalNtpTime = av_gettime();

                    // the PTS value of the last (received) input frame
					uint64_t tCurrentPtsFromRTP = GetCurrentPtsFromRTP(); // in ms

                	// calculate the play time of the RTP source [in us]
					uint64_t tCurrentPlayTimeofRTPSource = ((uint64_t) tReferencePts /* playout time in ms */) * 1000; // in us

                	LOG(LOG_VERBOSE, "%s reference PTS from RTP: %u, PTS from grabber: %"PRIu64"(frame: %"PRIu64", play-out fps: %.2f), PTS from RTP: %"PRIu64"(frame: %"PRIu64"), play time from RTP source: %.2f s", GetMediaTypeStr().c_str(), tReferencePts, tCurrentPtsFromGrabber, (uint64_t)mCurrentOutputFrameIndex, GetOutputFrameRate(), tCurrentPtsFromRTP, CalculateFrameNumberFromRTP(), (float)tCurrentPlayTimeofRTPSource / AV_TIME_BASE);
                    LOG(LOG_VERBOSE, "%s reference NTP time: %10lu, offset: %10ld (reference PTS: %10u, grabber PTS: %10lu), resulting synch. timestamp: %10ld", GetMediaTypeStr().c_str(), tReferenceNtpTime, tTimeOffsetFromReference * 1000, tReferencePts, tCurrentPtsFromGrabber, tResult);
                    //HINT: "diff" value should correlate with the frame buffer time! otherwise something went wrong in the processing chain
                    LOG(LOG_VERBOSE, "         local timestamp: %10ld                                                                                      local timestamp: %10ld, diff: %"PRId64" ms", tLocalNtpTime, tLocalNtpTime, (tLocalNtpTime - tResult) / 1000);
                #endif
                //if (tCurrentPtsFromRTP < tReferencePts)
                //    LOG(LOG_WARN, "Received %s reference (from RTP) PTS value: %u is in the future, last received (RTP) PTS value: %"PRIu64")", GetMediaTypeStr().c_str(), tReferencePts, tCurrentPtsFromRTP);
            }else
            {// reference values from RTP are still invalid, RTCP packet is needed (expected in some seconds)
                // nothing to complain about, we return 0 to signal we have no valid synchronization timestamp yet
            }
        }else
        {// no RTP available
            //TODO: we have to implement support for A/V sync. of plain (without RTP encapsulation) A/V streams
        }
    }

    return tResult;
}

bool MediaSourceMem::TimeShift(int64_t pOffset)
{
    LOG(LOG_WARN, "Shifting %s time by: %"PRId64, GetMediaTypeStr().c_str(), pOffset);
    mSourceTimeShiftForRTGrabbing -= pOffset;
    mDecoderRecalibrateRTGrabbingAfterSeeking = true;
    return true;
}

uint64_t MediaSourceMem::CalculateFrameNumberFromRTP()
{
    uint64_t tResult = 0;
    float tFrameRate = GetInputFrameRate();
    if (tFrameRate < 1.0)
        return 0;

    // the following sequence delivers the frame number independent from packet loss because
    // it calculates the frame number based on the current timestamp from the RTP header
    float tTimeBetweenFrames = 1000 / tFrameRate;
    tResult = rint((float) GetCurrentPtsFromRTP()/ tTimeBetweenFrames);

    #ifdef MSMEM_DEBUG_PRE_BUFFERING
        LOG(LOG_VERBOSE, "Calculated a frame number: %"PRId64" (RTP timestamp: %"PRId64"), fps: %.2f", tResult, GetCurrentPtsFromRTP(), tFrameRate);
    #endif

    return tResult;
}

///////////////////////////////////////////////////////////////////////////////

}} //namespace
