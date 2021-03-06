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
#include <poll.h>

#include "s3c_pp.h"
#include "SsbSipH264Decode.h"
#include "SsbSipMpeg4Decode.h"
#include "SsbSipVC1Decode.h"
#include "FrameExtractor.h"
#include "MPEG4Frames.h"
#include "H263Frames.h"
#include "H264Frames.h"
#include "SsbSipLogMsg.h"
#include "performance.h"
#include "lcd.h"
#include "MfcDriver.h"
#include "FileRead.h"


#define PP_DEV_NAME		"/dev/misc/s3c-pp"

#define LCD_WIDTH 		800
#define LCD_HEIGHT 		480

#define FB0_WIDTH       800
#define FB0_HEIGHT      480

#ifdef RGB24BPP
#define FB0_BPP         24
#define FB0_COLOR_SPACE RGB24
#else
#define FB0_BPP         16
#define FB0_COLOR_SPACE RGB16
#endif


static unsigned char delimiter_mpeg4[3] = {0x00, 0x00, 0x01};
static unsigned char delimiter_h264[4]  = {0x00, 0x00, 0x00, 0x01};


#define INPUT_BUFFER_SIZE		(204800)

static void		*handle;
static int		in_fd;
static int		file_size;
static char		*in_addr;
static int		fb_size;
static int		pp_fd, fb_fd;
static char		*fb_addr;	

static void sig_del_h264(int signo);
static void sig_del_mpeg4(int signo);
static void sig_del_vc1(int signo);


int Test_Display_H264(int argc, char **argv)
{

	void			*pStrmBuf;
	int				nFrameLeng = 0;
	unsigned int	pYUVBuf[2];

	int             is_first;
	struct pollfd   test_fd;

	struct stat		s;
	FRAMEX_CTX				*pFrameExCtx;	// frame extractor context
	FRAMEX_STRM_PTR 		file_strm;
	SSBSIP_H264_STREAM_INFO stream_info;	

	s3c_pp_params_t	pp_param;
	s3c_win_info_t	osd_info_to_driver;

	struct fb_fix_screeninfo	lcd_info;		

#ifdef FPS
	struct timeval	start, stop;
	unsigned int	time = 0;
	int				frame_cnt = 0;
#endif

	if(signal(SIGINT, sig_del_h264) == SIG_ERR) {
		printf("Sinal Error\n");
	}

	if (argc != 3) {
		printf("Usage : #./mfc <file name> <run mode>\n");
		printf("   - <file name> : H.264 file to be displayed.\n");
		printf("   - <run mode>  : 0 (PP DMA Mode), 1 (PP FIFO Mode)\n");
		return -1;
	}

	// in file open
	in_fd	= open(argv[1], O_RDONLY);
	if(in_fd < 0) {
		printf("Input file open failed\n");
		return -1;
	}

	// get input file size
	fstat(in_fd, &s);
	file_size = s.st_size;

	// mapping input file to memory
	in_addr = (char *)mmap(0, file_size, PROT_READ, MAP_SHARED, in_fd, 0);
	if(in_addr == NULL) {
		printf("input file memory mapping failed\n");
		return -1;
	}

	// Post processor open
	pp_fd = open(PP_DEV_NAME, O_RDWR|O_NONBLOCK);
	if(pp_fd < 0)
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

	///////////////////////////////////
	// FrameExtractor Initialization //
	///////////////////////////////////
	pFrameExCtx = FrameExtractorInit(FRAMEX_IN_TYPE_MEM, delimiter_h264, sizeof(delimiter_h264), 1);   
	file_strm.p_start = file_strm.p_cur = (unsigned char *)in_addr;
	file_strm.p_end = (unsigned char *)(in_addr + file_size);
	FrameExtractorFirst(pFrameExCtx, &file_strm);


	//////////////////////////////////////
	///    1. Create new instance      ///
	///      (SsbSipH264DecodeInit)    ///
	//////////////////////////////////////
	handle = SsbSipH264DecodeInit();
	if (handle == NULL) {
		printf("H264_Dec_Init Failed.\n");
		return -1;
	}

	/////////////////////////////////////////////
	///    2. Obtaining the Input Buffer      ///
	///      (SsbSipH264DecodeGetInBuf)       ///
	/////////////////////////////////////////////
	pStrmBuf = SsbSipH264DecodeGetInBuf(handle, nFrameLeng);
	if (pStrmBuf == NULL) {
		printf("SsbSipH264DecodeGetInBuf Failed.\n");
		SsbSipH264DecodeDeInit(handle);
		return -1;
	}

	////////////////////////////////////
	//  H264 CONFIG stream extraction //
	////////////////////////////////////
	nFrameLeng = ExtractConfigStreamH264(pFrameExCtx, &file_strm, pStrmBuf, INPUT_BUFFER_SIZE, NULL);
	printf("nFrameLeng = %d\n", nFrameLeng);

	////////////////////////////////////////////////////////////////
	///    3. Configuring the instance with the config stream    ///
	///       (SsbSipH264DecodeExe)                             ///
	////////////////////////////////////////////////////////////////
	if (SsbSipH264DecodeExe(handle, nFrameLeng) != SSBSIP_H264_DEC_RET_OK) {
		printf("H.264 Decoder Configuration Failed.\n");
		return -1;
	}


	/////////////////////////////////////
	///   4. Get stream information   ///
	/////////////////////////////////////
	SsbSipH264DecodeGetConfig(handle, H264_DEC_GETCONF_STREAMINFO, &stream_info);

	printf("\t<STREAMINFO> width=%d   height=%d    buf_width=%d    buf_height=%d.\n", 	\
			stream_info.width, stream_info.height, stream_info.buf_width, stream_info.buf_height);


	// set post processor configuration
	pp_param.src_full_width	    = stream_info.buf_width;
	pp_param.src_full_height	= stream_info.buf_height;
	pp_param.src_start_x		= 0;
	pp_param.src_start_y		= 0;
	pp_param.src_width			= pp_param.src_full_width;
	pp_param.src_height			= pp_param.src_full_height;
	pp_param.src_color_space	= YC420;
	pp_param.dst_start_x		= 0;
	pp_param.dst_start_y		= 0;
	pp_param.dst_full_width	    = FB0_WIDTH;		// destination width
	pp_param.dst_full_height	= FB0_HEIGHT;		// destination height
	pp_param.dst_width			= pp_param.dst_full_width;
	pp_param.dst_height			= pp_param.dst_full_height;
	pp_param.dst_color_space	= FB0_COLOR_SPACE;
	
	if ( atoi (argv[2]) == 0 )
		pp_param.out_path           = DMA_ONESHOT;
	else {
		pp_param.out_path           = FIFO_FREERUN;
		pp_param.scan_mode			= PROGRESSIVE_MODE;
	}
	ioctl(pp_fd, S3C_PP_SET_PARAMS, &pp_param);
	

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

	osd_info_to_driver.Bpp			= FB0_BPP;	// RGB16
	osd_info_to_driver.LeftTop_x	= 0;	
	osd_info_to_driver.LeftTop_y	= 0;
	osd_info_to_driver.Width		= FB0_WIDTH;	// display width
	osd_info_to_driver.Height		= FB0_HEIGHT;	// display height

	// set OSD's information 
	if(ioctl(fb_fd, SET_OSD_INFO, &osd_info_to_driver)) {
		printf("Some problem with the ioctl SET_OSD_INFO\n");
		return -1;
	}

	ioctl(fb_fd, SET_OSD_START);


	if ( FIFO_FREERUN == pp_param.out_path )
	{
		is_first = 1;
	}

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


		/////////////////////////////
		// Next H.264 VIDEO stream //
		/////////////////////////////
		nFrameLeng = NextFrameH264(pFrameExCtx, &file_strm, pStrmBuf, INPUT_BUFFER_SIZE, NULL);
		if (nFrameLeng < 4)
			break;

		// Post processing
		// pp_param.SrcFrmSt에는 MFC의 output buffer의 physical address가
		// pp_param.DstFrmSt에는 LCD frame buffer의 physical address가 입력으로 넣어야 한다.
		if ( FIFO_FREERUN == pp_param.out_path )
		{
			if ( is_first )
			{
				pp_param.src_buf_addr_phy = pYUVBuf[0];

				ioctl(pp_fd, S3C_PP_SET_SRC_BUF_ADDR_PHY, &pp_param);
				ioctl(pp_fd, S3C_PP_START);  

				is_first = 0;
			}
			else
			{
				pp_param.src_next_buf_addr_phy = pYUVBuf[0];

				ioctl(pp_fd, S3C_PP_SET_SRC_BUF_NEXT_ADDR_PHY, &pp_param);
			}
		}
		else
		{
			pp_param.src_buf_addr_phy = pYUVBuf[0];
			ioctl(pp_fd, S3C_PP_SET_SRC_BUF_ADDR_PHY, &pp_param);

			ioctl(fb_fd, FBIOGET_FSCREENINFO, &lcd_info);
			pp_param.dst_buf_addr_phy		= lcd_info.smem_start;			// LCD frame buffer
			ioctl(pp_fd, S3C_PP_SET_DST_BUF_ADDR_PHY, &pp_param);

			test_fd.fd = pp_fd;
			test_fd.events = POLLOUT|POLLERR;
			poll(&test_fd, 1, 3000);

			ioctl(pp_fd, S3C_PP_START);
		}

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

int Test_Display_MPEG4(int argc, char **argv)
{	
	void			*pStrmBuf;
	int				nFrameLeng = 0;
	unsigned int	pYUVBuf[2];
	int             is_first;   
	struct pollfd   test_fd;    

	struct stat				s;
	FRAMEX_CTX				*pFrameExCtx;	// frame extractor context
	FRAMEX_STRM_PTR 		file_strm;
	SSBSIP_H264_STREAM_INFO stream_info;	

	s3c_pp_params_t	pp_param;
	s3c_win_info_t	osd_info_to_driver;

	struct fb_fix_screeninfo	lcd_info;		

#ifdef FPS
	struct timeval	start, stop;
	unsigned int	time = 0;
	int				frame_cnt = 0;
#endif

	unsigned int	value[2];


	if(signal(SIGINT, sig_del_mpeg4) == SIG_ERR) {
		printf("Sinal Error\n");
	}

	if (argc != 3) {
		printf("Usage : mfc <file name> <run mode>\n");
		printf("   - <file name> : MPEG-4 file to be displayed.\n");
		printf("   - <run mode>  : 0 (PP DMA Mode), 1 (PP FIFO Mode)\n");
		return -1;
	}

	// in file open
	in_fd	= open(argv[1], O_RDONLY);
	if(in_fd < 0) {
		printf("Input file open failed\n");
		return -1;
	}

	// get input file size
	fstat(in_fd, &s);
	file_size = s.st_size;

	// mapping input file to memory
	in_addr = (char *)mmap(0, file_size, PROT_READ, MAP_SHARED, in_fd, 0);
	if(in_addr == NULL) {
		printf("input file memory mapping failed\n");
		return -1;
	}

	// Post processor open
	pp_fd = open(PP_DEV_NAME, O_RDWR|O_NONBLOCK);
	if(pp_fd < 0)
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

	///////////////////////////////////
	// FrameExtractor Initialization //
	///////////////////////////////////
	pFrameExCtx = FrameExtractorInit(FRAMEX_IN_TYPE_MEM, delimiter_mpeg4, sizeof(delimiter_mpeg4), 1);   
	file_strm.p_start = file_strm.p_cur = (unsigned char *)in_addr;
	file_strm.p_end = (unsigned char *)(in_addr + file_size);
	FrameExtractorFirst(pFrameExCtx, &file_strm);


	//////////////////////////////////////
	///    1. Create new instance      ///
	///      (SsbSipMPEG4DecodeInit)    ///
	//////////////////////////////////////
	handle = SsbSipMPEG4DecodeInit();
	if (handle == NULL) {
		printf("MPEG4_Dec_Init Failed.\n");
		return -1;
	}

	/////////////////////////////////////////////
	///    2. Obtaining the Input Buffer      ///
	///      (SsbSipMPEG4DecodeGetInBuf)       ///
	/////////////////////////////////////////////
	pStrmBuf = SsbSipMPEG4DecodeGetInBuf(handle, nFrameLeng);
	if (pStrmBuf == NULL) {
		printf("SsbSipMPEG4DecodeGetInBuf Failed.\n");
		SsbSipMPEG4DecodeDeInit(handle);
		return -1;
	}

	////////////////////////////////////
	//  MPEG4 CONFIG stream extraction //
	////////////////////////////////////
	nFrameLeng = ExtractConfigStreamMpeg4(pFrameExCtx, &file_strm, pStrmBuf, INPUT_BUFFER_SIZE, NULL);

#ifdef DIVX_ENABLE
	value[0] = 16;
	SsbSipMPEG4DecodeSetConfig(handle, MPEG4_DEC_SETCONF_PADDING_SIZE, value);
#endif

	////////////////////////////////////////////////////////////////
	///    3. Configuring the instance with the config stream    ///
	///       (SsbSipMPEG4DecodeExe)                             ///
	////////////////////////////////////////////////////////////////
	if (SsbSipMPEG4DecodeExe(handle, nFrameLeng) != SSBSIP_MPEG4_DEC_RET_OK) {
		printf("MPEG-4 Decoder Configuration Failed.\n");
		return -1;
	}


	/////////////////////////////////////
	///   4. Get stream information   ///
	/////////////////////////////////////
	SsbSipMPEG4DecodeGetConfig(handle, MPEG4_DEC_GETCONF_STREAMINFO, &stream_info);

	printf("\t<STREAMINFO> width=%d   height=%d    buf_width=%d    buf_height=%d.\n", 	\
			stream_info.width, stream_info.height, stream_info.buf_width, stream_info.buf_height);


	memset(&pp_param, 0, sizeof(s3c_pp_params_t));


#ifdef DIVX_ENABLE
	pp_param.src_full_width	    = stream_info.buf_width + 2*16;
	pp_param.src_full_height	= stream_info.buf_height + 2*16;
	pp_param.src_start_x		= 16;
	pp_param.src_start_y		= 16;
	pp_param.src_width		    = pp_param.src_full_width - 2*pp_param.src_start_x;
	pp_param.src_height		    = pp_param.src_full_height - 2*pp_param.src_start_y;
	pp_param.src_color_space	= YC420;

#else // DIVX_ENABLE
	pp_param.src_full_width	    = stream_info.buf_width;
	pp_param.src_full_height	= stream_info.buf_height;
	pp_param.src_start_x		= 0;
	pp_param.src_start_y		= 0;
	pp_param.src_width		    = pp_param.src_full_width;
	pp_param.src_height		    = pp_param.src_full_height;
	pp_param.src_color_space	= YC420;

#endif // DIVX_ENABLE

	pp_param.dst_full_width	    = FB0_WIDTH;		// destination width
	pp_param.dst_full_height	= FB0_HEIGHT;		// destination height
	pp_param.dst_start_x        = 0;
	pp_param.dst_start_y        = 0;
	pp_param.dst_width          = pp_param.dst_full_width;
	pp_param.dst_height         = pp_param.dst_full_height;
	pp_param.dst_color_space	= FB0_COLOR_SPACE;
	
	if ( atoi (argv[2]) == 0 )
		pp_param.out_path           = DMA_ONESHOT;
	else {
		pp_param.out_path           = FIFO_FREERUN;
		pp_param.scan_mode			= PROGRESSIVE_MODE;
	}
	
	ioctl(pp_fd, S3C_PP_SET_PARAMS, &pp_param);

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

	osd_info_to_driver.Bpp			= FB0_BPP;	
	osd_info_to_driver.LeftTop_x	= 0;	
	osd_info_to_driver.LeftTop_y	= 0;
	osd_info_to_driver.Width		= FB0_WIDTH;	// display width
	osd_info_to_driver.Height		= FB0_HEIGHT;	// display height

	// set OSD's information 
	if(ioctl(fb_fd, SET_OSD_INFO, &osd_info_to_driver)) {
		printf("Some problem with the ioctl SET_OSD_INFO\n");
		return -1;
	}

	ioctl(fb_fd, SET_OSD_START);

	if ( FIFO_FREERUN == pp_param.out_path )
	{
		is_first = 1;
	}

	while(1)
	{

#ifdef FPS
		gettimeofday(&start, NULL);
#endif

		//////////////////////////////////
		///       5. DECODE            ///
		///    (SsbSipMPEG4DecodeExe)   ///
		//////////////////////////////////
		if (SsbSipMPEG4DecodeExe(handle, nFrameLeng) != SSBSIP_MPEG4_DEC_RET_OK)
			break;

		//////////////////////////////////////////////
		///    6. Obtaining the Output Buffer      ///
		///      (SsbSipMPEG4DecodeGetOutBuf)       ///
		//////////////////////////////////////////////
		SsbSipMPEG4DecodeGetConfig(handle, MPEG4_DEC_GETCONF_PHYADDR_FRAM_BUF, pYUVBuf);

#ifdef DIVX_ENABLE
		SsbSipMPEG4DecodeGetConfig(handle, MPEG4_DEC_GETCONF_MPEG4_MV_ADDR, value);
		printf("MPEG4 MV ADDR = 0x%X, MV SIZE = %d\n", value[0], value[1]);
		SsbSipMPEG4DecodeGetConfig(handle, MPEG4_DEC_GETCONF_MPEG4_MBTYPE_ADDR, value);
		printf("MPEG4 MBTYPE ADDR = 0x%X, MBTYPE SIZE = %d\n", value[0], value[1]);
		SsbSipMPEG4DecodeGetConfig(handle, MPEG4_DEC_GETCONF_BYTE_CONSUMED, value);
		printf("BYTE CONSUMED = %d\n", value[0]);
		SsbSipMPEG4DecodeGetConfig(handle, MPEG4_DEC_GETCONF_MPEG4_FCODE, value);
		printf("MPEG4 FCODE = %d\n", value[0]);
		SsbSipMPEG4DecodeGetConfig(handle, MPEG4_DEC_GETCONF_MPEG4_VOP_TIME_RES, value);
		printf("MPEG4 VOP TIME RES = %d\n", value[0]);
		SsbSipMPEG4DecodeGetConfig(handle, MPEG4_DEC_GETCONF_MPEG4_TIME_BASE_LAST, value);
		printf("MPEG4 TIME BASE LAST = %d\n", value[0]);
		SsbSipMPEG4DecodeGetConfig(handle, MPEG4_DEC_GETCONF_MPEG4_NONB_TIME_LAST, value);
		printf("MPEG4 NONB TIME LAST = %d\n", value[0]);
		SsbSipMPEG4DecodeGetConfig(handle, MPEG4_DEC_GETCONF_MPEG4_TRD, value);
		printf("MPEG4 TRD = %d\n", value[0]);
#endif

		/////////////////////////////
		// Next MPEG4 VIDEO stream //
		/////////////////////////////
		nFrameLeng = NextFrameMpeg4(pFrameExCtx, &file_strm, pStrmBuf, INPUT_BUFFER_SIZE, NULL);
		if (nFrameLeng < 4)
			break;

		// Post processing
		// pp_param.SrcFrmSt에는 MFC의 output buffer의 physical address가
		// pp_param.DstFrmSt에는 LCD frame buffer의 physical address가 입력으로 넣어야 한다.

		if ( FIFO_FREERUN == pp_param.out_path )
		{
			if ( is_first )
			{
				pp_param.src_buf_addr_phy = pYUVBuf[0];

				ioctl(pp_fd, S3C_PP_SET_SRC_BUF_ADDR_PHY, &pp_param);
				ioctl(pp_fd, S3C_PP_START);  

				is_first = 0;
			}
			else
			{
				pp_param.src_next_buf_addr_phy = pYUVBuf[0];

				ioctl(pp_fd, S3C_PP_SET_SRC_BUF_NEXT_ADDR_PHY, &pp_param);
			}
		}
		else
		{
			pp_param.src_buf_addr_phy = pYUVBuf[0];
			ioctl(pp_fd, S3C_PP_SET_SRC_BUF_ADDR_PHY, &pp_param);

			ioctl(fb_fd, FBIOGET_FSCREENINFO, &lcd_info);
			pp_param.dst_buf_addr_phy		= lcd_info.smem_start;			// LCD frame buffer
			ioctl(pp_fd, S3C_PP_SET_DST_BUF_ADDR_PHY, &pp_param);

			test_fd.fd = pp_fd;
			test_fd.events = POLLOUT|POLLERR;
			poll(&test_fd, 1, 3000);

			ioctl(pp_fd, S3C_PP_START);
		}


#ifdef FPS
		gettimeofday(&stop, NULL);
		time += measureTime(&start, &stop);
		frame_cnt++;
#endif

	}

#ifdef FPS
	printf("Display Time : %u, Frame Count : %d, FPS : %f\n", time, frame_cnt, (float)frame_cnt*1000/time);
#endif

	SsbSipMPEG4DecodeDeInit(handle);

	munmap(in_addr, file_size);
	munmap(fb_addr, fb_size);
	close(pp_fd);
	close(fb_fd);
	close(in_fd);

	return 0;
}


int Test_Display_H263(int argc, char **argv)
{

	void			*pStrmBuf;
	int				nFrameLeng = 0;
	unsigned int	pYUVBuf[2];

	int             is_first;
	struct pollfd   test_fd;

	struct stat				s;
	MMAP_STRM_PTR 			file_strm;
	SSBSIP_MPEG4_STREAM_INFO stream_info;	

	s3c_pp_params_t	pp_param;
	s3c_win_info_t	osd_info_to_driver;

	struct fb_fix_screeninfo	lcd_info;		

#ifdef FPS
	struct timeval	start, stop;
	unsigned int	time = 0;
	int				frame_cnt = 0;
#endif


	if(signal(SIGINT, sig_del_mpeg4) == SIG_ERR) {
		printf("Sinal Error\n");
	}

	if (argc != 3) {
		printf("Usage : #./mfc <file name> <run mode>\n");
		printf("   - <file name> : H.263 file to be displayed.\n");
		printf("   - <run mode>  : 0 (PP DMA Mode), 1 (PP FIFO Mode)\n");
		return -1;
	}

	// in file open
	in_fd	= open(argv[1], O_RDONLY);
	if(in_fd < 0) {
		printf("Input file open failed\n");
		return -1;
	}

	// get input file size
	fstat(in_fd, &s);
	file_size = s.st_size;

	// mapping input file to memory
	in_addr = (char *)mmap(0, file_size, PROT_READ, MAP_SHARED, in_fd, 0);
	if(in_addr == NULL) {
		printf("input file memory mapping failed\n");
		return -1;
	}

	// Post processor open
	pp_fd = open(PP_DEV_NAME, O_RDWR|O_NONBLOCK);
	if(pp_fd < 0)
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

	//////////////////////////////////////
	///    1. Create new instance      ///
	///      (SsbSipMPEG4DecodeInit)    ///
	//////////////////////////////////////
	handle = SsbSipMPEG4DecodeInit();
	if (handle == NULL) {
		printf("H263_Dec_Init Failed.\n");
		return -1;
	}

	/////////////////////////////////////////////
	///    2. Obtaining the Input Buffer      ///
	///      (SsbSipMPEG4DecodeGetInBuf)       ///
	/////////////////////////////////////////////
	pStrmBuf = SsbSipMPEG4DecodeGetInBuf(handle, 200000);
	if (pStrmBuf == NULL) {
		printf("SsbSipMPEG4DecodeGetInBuf Failed.\n");
		SsbSipMPEG4DecodeDeInit(handle);
		return -1;
	}


	////////////////////////////////////
	//  MPEG4 CONFIG stream extraction //
	////////////////////////////////////
	file_strm.p_start = file_strm.p_cur = (unsigned char *)in_addr;
	file_strm.p_end = (unsigned char *)(in_addr + file_size);
	nFrameLeng = ExtractConfigStreamH263(&file_strm, pStrmBuf, INPUT_BUFFER_SIZE, NULL);


	////////////////////////////////////////////////////////////////
	///    3. Configuring the instance with the config stream    ///
	///       (SsbSipMPEG4DecodeExe)                             ///
	////////////////////////////////////////////////////////////////
	if (SsbSipMPEG4DecodeExe(handle, nFrameLeng) != SSBSIP_MPEG4_DEC_RET_OK) {
		printf("H.263 Decoder Configuration Failed.\n");
		return -1;
	}


	/////////////////////////////////////
	///   4. Get stream information   ///
	/////////////////////////////////////
	SsbSipMPEG4DecodeGetConfig(handle, MPEG4_DEC_GETCONF_STREAMINFO, &stream_info);

	printf("\t<STREAMINFO> width=%d   height=%d    buf_width=%d    buf_height=%d.\n", 	\
			stream_info.width, stream_info.height, stream_info.buf_width, stream_info.buf_height);


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

	if ( atoi (argv[2]) == 0 )
		pp_param.out_path           = DMA_ONESHOT;
	else {
		pp_param.out_path           = FIFO_FREERUN;
		pp_param.scan_mode			= PROGRESSIVE_MODE;
	}
	
	ioctl(pp_fd, S3C_PP_SET_PARAMS, &pp_param);

	// get LCD frame buffer address
	fb_size = pp_param.dst_full_width * pp_param.dst_full_height* 2;	// RGB565
#ifdef RGB24BPP
	fb_size = pp_param.dst_full_width * pp_param.dst_full_height * 4;	// RGB888
#endif

	fb_addr = (char *)mmap(0, fb_size, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
	if (fb_addr == NULL) {
		printf("LCD frame buffer mmap failed\n");
		return -1;
	}

	osd_info_to_driver.Bpp			= FB0_BPP;	// RGB16
	osd_info_to_driver.LeftTop_x	= 0;	
	osd_info_to_driver.LeftTop_y	= 0;
	osd_info_to_driver.Width		= FB0_WIDTH;	// display width
	osd_info_to_driver.Height		= FB0_HEIGHT;	// display height

	// set OSD's information 
	if(ioctl(fb_fd, SET_OSD_INFO, &osd_info_to_driver)) {
		printf("Some problem with the ioctl SET_OSD_INFO\n");
		return -1;
	}

	ioctl(fb_fd, SET_OSD_START);

	if ( FIFO_FREERUN == pp_param.out_path )
	{
		is_first = 1;
	}

	while(1)
	{

#ifdef FPS
		gettimeofday(&start, NULL);
#endif

		//////////////////////////////////
		///       5. DECODE            ///
		///    (SsbSipMPEG4DecodeExe)   ///
		//////////////////////////////////
		if (SsbSipMPEG4DecodeExe(handle, nFrameLeng) != SSBSIP_MPEG4_DEC_RET_OK)
			break;


		//////////////////////////////////////////////
		///    6. Obtaining the Output Buffer      ///
		///      (SsbSipMPEG4DecodeGetOutBuf)       ///
		//////////////////////////////////////////////
		SsbSipMPEG4DecodeGetConfig(handle, MPEG4_DEC_GETCONF_PHYADDR_FRAM_BUF, pYUVBuf);


		/////////////////////////////
		// Next MPEG4 VIDEO stream //
		/////////////////////////////
		nFrameLeng = NextFrameH263(&file_strm, pStrmBuf, INPUT_BUFFER_SIZE, NULL);
		if (nFrameLeng < 4)
			break;

		// Post processing
		// pp_param.SrcFrmSt에는 MFC의 output buffer의 physical address가
		// pp_param.DstFrmSt에는 LCD frame buffer의 physical address가 입력으로 넣어야 한다.
		if ( FIFO_FREERUN == pp_param.out_path )
		{
			if ( is_first )
			{
				pp_param.src_buf_addr_phy = pYUVBuf[0];

				ioctl(pp_fd, S3C_PP_SET_SRC_BUF_ADDR_PHY, &pp_param);
				ioctl(pp_fd, S3C_PP_START);  

				is_first = 0;
			}
			else
			{
				pp_param.src_next_buf_addr_phy = pYUVBuf[0];

				ioctl(pp_fd, S3C_PP_SET_SRC_BUF_NEXT_ADDR_PHY, &pp_param);
			}
		}
		else
		{
			pp_param.src_buf_addr_phy = pYUVBuf[0];
			ioctl(pp_fd, S3C_PP_SET_SRC_BUF_ADDR_PHY, &pp_param);

			ioctl(fb_fd, FBIOGET_FSCREENINFO, &lcd_info);
			pp_param.dst_buf_addr_phy		= lcd_info.smem_start;			// LCD frame buffer
			ioctl(pp_fd, S3C_PP_SET_DST_BUF_ADDR_PHY, &pp_param);

			test_fd.fd = pp_fd;
			test_fd.events = POLLOUT|POLLERR;
			poll(&test_fd, 1, 3000);

			ioctl(pp_fd, S3C_PP_START);
		}

#ifdef FPS
		gettimeofday(&stop, NULL);
		time += measureTime(&start, &stop);
		frame_cnt++;
#endif

	}

#ifdef FPS
	printf("Display Time : %u, Frame Count : %d, FPS : %f\n", time, frame_cnt, (float)frame_cnt*1000/time);
#endif

	SsbSipMPEG4DecodeDeInit(handle);

	munmap(in_addr, file_size);
	munmap(fb_addr, fb_size);
	close(pp_fd);
	close(fb_fd);
	close(in_fd);

	return 0;
}


int Test_Display_VC1(int argc, char **argv)
{

	void			*pStrmBuf;
	int				nFrameLeng = 0;
	unsigned int	pYUVBuf[2];

	int             is_first;
	struct pollfd   test_fd;
	
	struct stat				s;
	MMAP_STRM_PTR 			file_strm;
	SSBSIP_H264_STREAM_INFO stream_info;	

	s3c_pp_params_t	pp_param;
	s3c_win_info_t	osd_info_to_driver;

	struct fb_fix_screeninfo	lcd_info;		

#ifdef FPS
	struct timeval	start, stop;
	unsigned int	time = 0;
	int				frame_cnt = 0;
#endif

	int r = 0;


	if(signal(SIGINT, sig_del_vc1) == SIG_ERR) {
		printf("Sinal Error\n");
	}

	if (argc != 3) {
		printf("Usage : #./mfc <file name> <run mode>\n");
		printf("   - <file name> : VC-1 file to be displayed.\n");
		printf("   - <run mode>  : 0 (PP DMA Mode), 1 (PP FIFO Mode)\n");
		return -1;
	}

	// in file open
	in_fd	= open(argv[1], O_RDONLY);
	if(in_fd < 0) {
		printf("Input file open failed\n");
		return -1;
	}

	// get input file size
	fstat(in_fd, &s);
	file_size = s.st_size;

	// mapping input file to memory
	in_addr = (char *)mmap(0, file_size, PROT_READ, MAP_SHARED, in_fd, 0);
	if(in_addr == NULL) {
		printf("input file memory mapping failed\n");
		return -1;
	}

	// Post processor open
	pp_fd = open(PP_DEV_NAME, O_RDWR|O_NONBLOCK);
	if(pp_fd < 0)
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

	//////////////////////////////////////
	///    1. Create new instance      ///
	///      (SsbSipMPEG4DecodeInit)    ///
	//////////////////////////////////////
	handle = SsbSipVC1DecodeInit();
	if (handle == NULL) {
		printf("VC1_Dec_Init Failed.\n");
		return -1;
	}

	/////////////////////////////////////////////
	///    2. Obtaining the Input Buffer      ///
	///      (SsbSipMPEG4DecodeGetInBuf)       ///
	/////////////////////////////////////////////
	pStrmBuf = SsbSipVC1DecodeGetInBuf(handle, 200000);
	if (pStrmBuf == NULL) {
		printf("SsbSipVC1DecodeGetInBuf Failed.\n");
		SsbSipVC1DecodeDeInit(handle);
		return -1;
	}


	////////////////////////////////////
	//  MPEG4 CONFIG stream extraction //
	////////////////////////////////////
	file_strm.p_start = file_strm.p_cur = (unsigned char *)in_addr;
	file_strm.p_end = (unsigned char *)(in_addr + file_size);
	nFrameLeng = ExtractConfigStreamVC1(&file_strm, pStrmBuf, INPUT_BUFFER_SIZE, NULL);


	////////////////////////////////////////////////////////////////
	///    3. Configuring the instance with the config stream    ///
	///       (SsbSipMPEG4DecodeExe)                             ///
	////////////////////////////////////////////////////////////////
	r = SsbSipVC1DecodeExe(handle, nFrameLeng);
	if (r != SSBSIP_VC1_DEC_RET_OK) {
		printf("VC-1 Decoder Configuration Failed. : %d\n", r);
		return -1;
	}


	/////////////////////////////////////
	///   4. Get stream information   ///
	/////////////////////////////////////
	SsbSipVC1DecodeGetConfig(handle, VC1_DEC_GETCONF_STREAMINFO, &stream_info);

	printf("\t<STREAMINFO> width=%d   height=%d    buf_width=%d    buf_height=%d.\n", 	\
			stream_info.width, stream_info.height, stream_info.buf_width, stream_info.buf_height);


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

	if ( atoi (argv[2]) == 0 )
		pp_param.out_path           = DMA_ONESHOT;
	else {
		pp_param.out_path           = FIFO_FREERUN;
		pp_param.scan_mode			= PROGRESSIVE_MODE;	
	}
	
	ioctl(pp_fd, S3C_PP_SET_PARAMS, &pp_param);

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

	osd_info_to_driver.Bpp			= FB0_BPP;	// RGB16
	osd_info_to_driver.LeftTop_x	= 0;	
	osd_info_to_driver.LeftTop_y	= 0;
	osd_info_to_driver.Width		= FB0_WIDTH;	// display width
	osd_info_to_driver.Height		= FB0_HEIGHT;	// display height

	// set OSD's information 
	if(ioctl(fb_fd, SET_OSD_INFO, &osd_info_to_driver)) {
		printf("Some problem with the ioctl SET_OSD_INFO\n");
		return -1;
	}

	ioctl(fb_fd, SET_OSD_START);


	if ( FIFO_FREERUN == pp_param.out_path )
	{
		is_first = 1;
	}


	while(1)
	{

#ifdef FPS
		gettimeofday(&start, NULL);
#endif

		//////////////////////////////////
		///       5. DECODE            ///
		///    (SsbSipMPEG4DecodeExe)   ///
		//////////////////////////////////
		if (SsbSipVC1DecodeExe(handle, nFrameLeng) != SSBSIP_VC1_DEC_RET_OK)
			break;


		//////////////////////////////////////////////
		///    6. Obtaining the Output Buffer      ///
		///      (SsbSipMPEG4DecodeGetOutBuf)       ///
		//////////////////////////////////////////////
		SsbSipVC1DecodeGetConfig(handle, VC1_DEC_GETCONF_PHYADDR_FRAM_BUF, pYUVBuf);


		/////////////////////////////
		// Next MPEG4 VIDEO stream //
		/////////////////////////////
		nFrameLeng = NextFrameVC1(&file_strm, pStrmBuf, INPUT_BUFFER_SIZE, NULL);

		if (nFrameLeng < 4)
			break;

		// Post processing
		// pp_param.SrcFrmSt에는 MFC의 output buffer의 physical address가
		// pp_param.DstFrmSt에는 LCD frame buffer의 physical address가 입력으로 넣어야 한다.
		if ( FIFO_FREERUN == pp_param.out_path )
		{
			if ( is_first )
			{
				pp_param.src_buf_addr_phy = pYUVBuf[0];

				ioctl(pp_fd, S3C_PP_SET_SRC_BUF_ADDR_PHY, &pp_param);
				ioctl(pp_fd, S3C_PP_START);  

				is_first = 0;
			}
			else
			{
				pp_param.src_next_buf_addr_phy = pYUVBuf[0];

				ioctl(pp_fd, S3C_PP_SET_SRC_BUF_NEXT_ADDR_PHY, &pp_param);
			}
		}
		else
		{
			pp_param.src_buf_addr_phy = pYUVBuf[0];
			ioctl(pp_fd, S3C_PP_SET_SRC_BUF_ADDR_PHY, &pp_param);

			ioctl(fb_fd, FBIOGET_FSCREENINFO, &lcd_info);
			pp_param.dst_buf_addr_phy		= lcd_info.smem_start;			// LCD frame buffer
			ioctl(pp_fd, S3C_PP_SET_DST_BUF_ADDR_PHY, &pp_param);

			test_fd.fd = pp_fd;
			test_fd.events = POLLOUT|POLLERR;
			poll(&test_fd, 1, 3000);

			ioctl(pp_fd, S3C_PP_START);
		}

#ifdef FPS
		gettimeofday(&stop, NULL);
		time += measureTime(&start, &stop);
		frame_cnt++;
#endif

	}

#ifdef FPS
	printf("Display Time : %u, Frame Count : %d, FPS : %f\n", time, frame_cnt, (float)frame_cnt*1000/time);
#endif

	SsbSipVC1DecodeDeInit(handle);

	munmap(in_addr, file_size);
	munmap(fb_addr, fb_size);
	close(pp_fd);
	close(fb_fd);
	close(in_fd);

	return 0;
}


static void sig_del_h264(int signo)
{
	printf("signal handling\n");

	SsbSipH264DecodeDeInit(handle);

	munmap(in_addr, file_size);
	munmap(fb_addr, fb_size);
	close(pp_fd);
	close(fb_fd);
	close(in_fd);
}

static void sig_del_mpeg4(int signo)
{
	printf("signal handling\n");

	SsbSipMPEG4DecodeDeInit(handle);

	munmap(in_addr, file_size);
	munmap(fb_addr, fb_size);
	close(pp_fd);
	close(fb_fd);
	close(in_fd);
}

static void sig_del_vc1(int signo)
{
	printf("signal handling\n");

	SsbSipVC1DecodeDeInit(handle);

	munmap(in_addr, file_size);
	munmap(fb_addr, fb_size);
	close(pp_fd);
	close(fb_fd);
	close(in_fd);
}


