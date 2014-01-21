#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/mman.h>
#include <string.h>
#include <errno.h>
#include <sys/time.h>
#include <signal.h>
#include <linux/vt.h>
#include <linux/fb.h>

#include "s3c_pp.h"

#include "SsbSipH264Decode.h"
#include "SsbSipMpeg4Decode.h"
#include "FrameExtractor.h"
#include "MPEG4Frames.h"
#include "H263Frames.h"
#include "H264Frames.h"
#include "SsbSipLogMsg.h"
#include "performance.h"
#include "lcd.h"
#include "MfcDriver.h"
#include "demo.h"


#define PP_DEV_NAME		"/dev/misc/s3c-pp"

#define LCD_WIDTH 	    800
#define LCD_HEIGHT 		480
#define LCD_WIDTH2 	    320
#define LCD_HEIGHT2 	240

#define FB0_WIDTH      	800
#define FB0_HEIGHT     	480
#define FB1_WIDTH	    320
#define FB1_HEIGHT      240


#ifdef RGB24BPP
#define FB0_BPP         24
#define FB1_BPP			24
#define FB0_COLOR_SPACE RGB24
#else
#define FB0_BPP         16
#define FB1_BPP			16
#define FB0_COLOR_SPACE RGB16
#endif

static unsigned char delimiter_h264[4]  = {0x00, 0x00, 0x00, 0x01};


#define INPUT_BUFFER_SIZE		(204800)



int Demo(int argc, char **argv)
{
	void    		*handle, *handle2;
	int				in_fd;
	int				in_fd2;	
	int				file_size;
	int				file_size2;
	char			*in_addr, *in_addr2;
	int				fb_size, fb_size2;
	int				pp_fd, pp_fd2, fb_fd;
	int				fb_fd2;	
	char			*fb_addr, *fb_addr2;
	
	void			*pStrmBuf, *pStrmBuf2;
	int				nFrameLeng = 0, nFrameLeng2 = 0;
	unsigned int	pYUVBuf[2], pYUVBuf2[2];

	int				frame_count=0, frame_count2=0;
	
	struct stat				s, s2;
	FRAMEX_CTX				*pFrameExCtx, *pFrameExCtx2;	// frame extractor context
	FRAMEX_STRM_PTR 		file_strm, file_strm2;
	SSBSIP_H264_STREAM_INFO stream_info, stream_info2;	
	
	s3c_pp_params_t	pp_param, pp_param2;
	s3c_win_info_t	osd_info_to_driver, osd_info_to_driver2;

	struct fb_fix_screeninfo	lcd_info, lcd_info2;		
	
#ifdef FPS
	struct timeval	start, stop;
	unsigned int	time = 0;
	int				frame_cnt = 0;
#endif

	if (argc != 3) {
		printf("Usage : mfc <input filename1> <input filename2>\n");
		return -1;
	}

	// in/out file open
	in_fd	= open(argv[1], O_RDONLY);
	if( (in_fd < 0) ) {
		LOG_MSG(LOG_ERROR, "Test_H264_Decoder_Line_Buffer", "Input/Output file open failed\n");
		return -1;
	}

	in_fd2	= open(argv[2], O_RDONLY);
	if (in_fd2 < 0) {
		printf("2nd file open error\n");
		return -1;
	}

	// get input file size
	fstat(in_fd, &s);
	file_size = s.st_size;

	fstat(in_fd2, &s2);
	file_size2 = s2.st_size;
	

	// mapping input file to memory
	in_addr = (char *)mmap(0, file_size, PROT_READ, MAP_SHARED, in_fd, 0);
	if(in_addr == NULL) {
		printf("input file memory mapping failed\n");
		return -1;
	}
	in_addr2 = (char *)mmap(0, file_size2, PROT_READ, MAP_SHARED, in_fd2, 0);
	if(in_addr2 == NULL) {
		printf("input file memory mapping failed\n");
		return -1;
	}
	
	// Post processor open
	pp_fd = open(PP_DEV_NAME, O_RDWR);
	if(pp_fd < 0)
	{
		printf("Post processor open error\n");
		return -1;
	}

	pp_fd2 = open(PP_DEV_NAME, O_RDWR);
	if(pp_fd2 < 0)
	{
		printf("Post processor open error\n");
		return -1;
	}
	
	// LCD frame buffer open
	fb_fd = open(FB_DEV_NAME, O_RDWR|O_NDELAY);
	if(fb_fd < 0)
	{
		printf("LCD frame buffer open error\n");
		return -1;
	}
	// LCD frame buffer open
	fb_fd2 = open(FB_DEV_NAME1, O_RDWR|O_NDELAY);
	if(fb_fd2 < 0)
	{
		printf("LCD frame buffer open error\n");
		return -1;
	}


	///////////////////////////////////
	// FrameExtractor Initialization //
	///////////////////////////////////
	pFrameExCtx = FrameExtractorInit(FRAMEX_IN_TYPE_MEM, delimiter_h264, sizeof(delimiter_h264), 1);   
	file_strm.p_start = file_strm.p_cur = (unsigned char *)in_addr;
	file_strm.p_end = (unsigned char *)(in_addr + file_size);
	FrameExtractorFirst(pFrameExCtx, &file_strm);

	pFrameExCtx2 = FrameExtractorInit(FRAMEX_IN_TYPE_MEM, delimiter_h264, sizeof(delimiter_h264), 1);   
	file_strm2.p_start = file_strm2.p_cur = (unsigned char *)in_addr2;
	file_strm2.p_end = (unsigned char *)(in_addr2 + file_size2);
	FrameExtractorFirst(pFrameExCtx2, &file_strm2);
	

	//////////////////////////////////////
	///    1. Create new instance      ///
	///      (SsbSipH264DecodeInit)    ///
	//////////////////////////////////////
	handle = SsbSipH264DecodeInit();
	if (handle == NULL) {
		LOG_MSG(LOG_ERROR, "Test_H264_Decoder_Line_Buffer", "H264_Dec_Init Failed.\n");
		return -1;
	}

	handle2 = SsbSipH264DecodeInit();
	if (handle2 == NULL) {
		LOG_MSG(LOG_ERROR, "Test_H264_Decoder_Line_Buffer", "H264_Dec_Init Failed.\n");
		return -1;
	}
	

	/////////////////////////////////////////////
	///    2. Obtaining the Input Buffer      ///
	///      (SsbSipH264DecodeGetInBuf)       ///
	/////////////////////////////////////////////
	pStrmBuf = SsbSipH264DecodeGetInBuf(handle, nFrameLeng);
	if (pStrmBuf == NULL) {
		LOG_MSG(LOG_ERROR, "Test_H264_Decoder_Line_Buffer", "SsbSipH264DecodeGetInBuf Failed.\n");
		SsbSipH264DecodeDeInit(handle);
		return -1;
	}

	pStrmBuf2 = SsbSipH264DecodeGetInBuf(handle2, nFrameLeng2);
	if (pStrmBuf2 == NULL) {
		LOG_MSG(LOG_ERROR, "Test_H264_Decoder_Line_Buffer", "SsbSipH264DecodeGetInBuf Failed.\n");
		SsbSipH264DecodeDeInit(handle2);
		return -1;
	}


	////////////////////////////////////
	//  H264 CONFIG stream extraction //
	////////////////////////////////////
	nFrameLeng = ExtractConfigStreamH264(pFrameExCtx, &file_strm, pStrmBuf, INPUT_BUFFER_SIZE, NULL);
	printf("nFrmaeLeng : %d\n", nFrameLeng);

	nFrameLeng2 = ExtractConfigStreamH264(pFrameExCtx2, &file_strm2, pStrmBuf2, INPUT_BUFFER_SIZE, NULL);
	printf("nFrmaeLeng : %d\n", nFrameLeng2);


	////////////////////////////////////////////////////////////////
	///    3. Configuring the instance with the config stream    ///
	///       (SsbSipH264DecodeExe)                             ///
	////////////////////////////////////////////////////////////////
	if (SsbSipH264DecodeExe(handle, nFrameLeng) != SSBSIP_H264_DEC_RET_OK) {
		LOG_MSG(LOG_ERROR, "Test_H264_Decoder_Line_Buffer", "H.264 Decoder Configuration Failed.\n");
		return -1;
	}

	if (SsbSipH264DecodeExe(handle2, nFrameLeng2) != SSBSIP_H264_DEC_RET_OK) {
		LOG_MSG(LOG_ERROR, "Test_H264_Decoder_Line_Buffer", "H.264 Decoder Configuration Failed.\n");
		return -1;
	}


	/////////////////////////////////////
	///   4. Get stream information   ///
	/////////////////////////////////////
	SsbSipH264DecodeGetConfig(handle, H264_DEC_GETCONF_STREAMINFO, &stream_info);
	LOG_MSG(LOG_TRACE, "Test_H264_Decoder_Line_Buffer", 	\
		"\t<STREAMINFO> width=%d   height=%d    buf_width=%d    buf_height=%d.\n",	\
		stream_info.width, stream_info.height, stream_info.buf_width, stream_info.buf_height);

	SsbSipH264DecodeGetConfig(handle2, H264_DEC_GETCONF_STREAMINFO, &stream_info2);
	LOG_MSG(LOG_TRACE, "Test_H264_Decoder_Line_Buffer",		\
		"\t<STREAMINFO> width=%d   height=%d    buf_width=%d    buf_height=%d.\n",	\
		stream_info2.width, stream_info2.height, stream_info2.buf_width, stream_info2.buf_height);

		
	// set post processor configuration
	pp_param.src_full_width	    = stream_info.buf_width;
	pp_param.src_full_height	= stream_info.buf_height;
	pp_param.src_start_x		= 0;
	pp_param.src_start_y		= 0;
	pp_param.src_width		    = pp_param.src_full_width;
	pp_param.src_height		    = pp_param.src_full_height;
	pp_param.src_color_space	= YC420;
	pp_param.dst_start_x		= 0;
	pp_param.dst_start_y		= 0;
	pp_param.dst_full_width	    = FB0_WIDTH;		// destination width
	pp_param.dst_full_height	= FB0_HEIGHT;		// destination height
	pp_param.dst_width		    = pp_param.dst_full_width;
	pp_param.dst_height		    = pp_param.dst_full_height;
	pp_param.dst_color_space	= FB0_COLOR_SPACE;
	pp_param.out_path			= DMA_ONESHOT;

	ioctl(pp_fd, S3C_PP_SET_PARAMS, &pp_param);

	
	// set post processor configuration
	pp_param2.src_full_width	= stream_info2.buf_width;
	pp_param2.src_full_height	= stream_info2.buf_height;
	pp_param2.src_start_x		= 0;
	pp_param2.src_start_y		= 0;
	pp_param2.src_width		    = pp_param2.src_full_width;
	pp_param2.src_height		= pp_param2.src_full_height;
	pp_param2.src_color_space	= YC420;
	pp_param2.dst_start_x		= 0;
	pp_param2.dst_start_y		= 0;
	pp_param2.dst_full_width	= FB1_WIDTH;		// destination width
	pp_param2.dst_full_height	= FB1_HEIGHT;		// destination height
	pp_param2.dst_width		    = pp_param2.dst_full_width;
	pp_param2.dst_height		= pp_param2.dst_full_height;
	pp_param2.dst_color_space	= FB0_COLOR_SPACE;
	pp_param2.out_path			= DMA_ONESHOT;

	ioctl(pp_fd2, S3C_PP_SET_PARAMS, &pp_param2);


	// get LCD frame buffer address
	fb_size = pp_param.dst_full_width * pp_param.dst_full_height * 2;	// RGB565
#ifdef RGB24BPP
	fb_size = pp_param.dst_full_width * pp_param.dst_full_height * 4;	// RGB888
#endif

	fb_addr = (char *)mmap(0, fb_size, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
	if (fb_addr == NULL) {
		printf("LCD frame buffer mmap failed\n");
		return -1;
	}

	fb_size2 = pp_param2.dst_full_width * pp_param2.dst_full_height * 2;	// RGB565
#ifdef RGB24BPP
	fb_size2 = pp_param2.dst_full_width * pp_param2.dst_full_height * 4;	// RGB565
#endif
	fb_addr2 = (char *)mmap(0, fb_size2, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd2, 0);
	if (fb_addr == NULL) {
		printf("LCD frame buffer mmap failed\n");
		return -1;
	}


	osd_info_to_driver.Bpp			= FB0_BPP;	// RGB16
	osd_info_to_driver.LeftTop_x	= 0;	
	osd_info_to_driver.LeftTop_y	= 0;
	osd_info_to_driver.Width		= FB0_WIDTH;	// display width
	osd_info_to_driver.Height		= FB0_HEIGHT;	// display height


	osd_info_to_driver2.Bpp			= FB1_BPP;	// RGB16
	osd_info_to_driver2.LeftTop_x	= 0;	
	osd_info_to_driver2.LeftTop_y	= 0;
	osd_info_to_driver2.Width		= FB1_WIDTH;	// display width
	osd_info_to_driver2.Height		= FB1_HEIGHT;	// display height
	

	// set OSD's information 
	if(ioctl(fb_fd, SET_OSD_INFO, &osd_info_to_driver)) {
		printf("Some problem with the ioctl SET_OSD_INFO\n");
		return -1;
	}

	if(ioctl(fb_fd2, SET_OSD_INFO, &osd_info_to_driver2)) {
		printf("Some problem with the ioctl SET_OSD_INFO\n");
		return -1;
	}

	ioctl(fb_fd, SET_OSD_START);
	ioctl(fb_fd2, SET_OSD_START);
	
	while(1)
	{

	#ifdef FPS
		gettimeofday(&start, NULL);
	#endif
	
		//////////////////////////////////
		///       5. DECODE            ///
		///    (SsbSipH264DecodeExe)   ///
		//////////////////////////////////
		if (SsbSipH264DecodeExe(handle, nFrameLeng) != SSBSIP_H264_DEC_RET_OK)
			break;

		//////////////////////////////////////////////
		///    6. Obtaining the Output Buffer      ///
		///      (SsbSipH264DecodeGetOutBuf)       ///
		//////////////////////////////////////////////
		SsbSipH264DecodeGetConfig(handle, H264_DEC_GETCONF_PHYADDR_FRAM_BUF, pYUVBuf);
		//printf("Buffer address : 0x%X, Length : %d\n", pYUVBuf[0], pYUVBuf[1]);
	
		/////////////////////////////
		// Next H.264 VIDEO stream //
		/////////////////////////////
		nFrameLeng = NextFrameH264(pFrameExCtx, &file_strm, pStrmBuf, INPUT_BUFFER_SIZE, NULL);
		//printf("nFrameLeng : %d\n", nFrameLeng);
		frame_count++;
		if (nFrameLeng < 4)
			break;

		if(frame_count == 1843) {
			frame_count = 0;
			file_strm.p_start 	= file_strm.p_cur = (unsigned char *)in_addr;
			file_strm.p_end	= (unsigned char *)(in_addr + file_size);
			FrameExtractorFirst(pFrameExCtx, &file_strm);
			nFrameLeng = ExtractConfigStreamH264(pFrameExCtx, &file_strm, (unsigned char *)pStrmBuf, INPUT_BUFFER_SIZE, NULL);
			continue;			
		}
					
		// Post processing
		// pp_param.SrcFrmSt에는 MFC의 output buffer의 physical address가
		// pp_param.DstFrmSt에는 LCD frame buffer의 physical address가 입력으로 넣어야 한다.
		pp_param.src_buf_addr_phy = pYUVBuf[0];
		ioctl(pp_fd, S3C_PP_SET_SRC_BUF_ADDR_PHY, &pp_param);
		ioctl(fb_fd, FBIOGET_FSCREENINFO, &lcd_info);
		pp_param.dst_buf_addr_phy		= lcd_info.smem_start;			// LCD frame buffer
		ioctl(pp_fd, S3C_PP_SET_DST_BUF_ADDR_PHY, &pp_param);
		ioctl(pp_fd, S3C_PP_START);	

////////////////////////////////////////////////////////////////////////////////////////////////////

		//////////////////////////////////
		///       5. DECODE            ///
		///    (SsbSipH264DecodeExe)   ///
		//////////////////////////////////
		if (SsbSipH264DecodeExe(handle2, nFrameLeng2) != SSBSIP_H264_DEC_RET_OK)
			break;

		//////////////////////////////////////////////
		///    6. Obtaining the Output Buffer      ///
		///      (SsbSipH264DecodeGetOutBuf)       ///
		//////////////////////////////////////////////
		SsbSipH264DecodeGetConfig(handle2, H264_DEC_GETCONF_PHYADDR_FRAM_BUF, pYUVBuf2);
		//printf("Buffer address : 0x%X, Length : %d\n", pYUVBuf[0], pYUVBuf[1]);
	
		/////////////////////////////
		// Next H.264 VIDEO stream //
		/////////////////////////////
		nFrameLeng2 = NextFrameH264(pFrameExCtx2, &file_strm2, pStrmBuf2, INPUT_BUFFER_SIZE, NULL);
		frame_count2++;
		//printf("frame count : %d\n", frame_count2);
		if (nFrameLeng2 < 4)
			break;

		if(frame_count2 == 1843) {
			frame_count2 = 0;
			file_strm2.p_start 	= file_strm2.p_cur = (unsigned char *)in_addr2;
			file_strm2.p_end	= (unsigned char *)(in_addr2 + file_size2);
			FrameExtractorFirst(pFrameExCtx2, &file_strm2);
			nFrameLeng2 = ExtractConfigStreamH264(pFrameExCtx2, &file_strm2, (unsigned char *)pStrmBuf2, INPUT_BUFFER_SIZE, NULL);
			continue;			
		}
				
		// Post processing
		// pp_param.SrcFrmSt에는 MFC의 output buffer의 physical address가
		// pp_param.DstFrmSt에는 LCD frame buffer의 physical address가 입력으로 넣어야 한다.
		pp_param2.src_buf_addr_phy		= pYUVBuf2[0];	// MFC output buffer
		ioctl(pp_fd2, S3C_PP_SET_SRC_BUF_ADDR_PHY, &pp_param2);
		ioctl(fb_fd2, FBIOGET_FSCREENINFO, &lcd_info2);
		pp_param2.dst_buf_addr_phy		= lcd_info2.smem_start;			// LCD frame buffer
		ioctl(pp_fd2, S3C_PP_SET_DST_BUF_ADDR_PHY, &pp_param2);
		ioctl(pp_fd2, S3C_PP_START);	
		
	#ifdef FPS
		gettimeofday(&stop, NULL);
		time += measureTime(&start, &stop);
		frame_cnt++;
	#endif
	
	
	}

#ifdef FPS
	printf("Display Time : %u, Frame Count : %d, FPS : %f\n", time, frame_cnt, (float)frame_cnt*1000/time);
#endif

	SsbSipH264DecodeDeInit(handle);

	munmap(in_addr, file_size);
	munmap(fb_addr, fb_size);
	close(pp_fd);
	close(fb_fd);
	close(in_fd);
	
	return 0;
}

