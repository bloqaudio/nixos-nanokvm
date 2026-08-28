/*
 * SG2002 V4L2 H.264 bridge:
 *
 *   /dev/video0 (UYVY capture) -> CPU UYVY->NV12/NV21 -> /dev/video1 (Coda)
 *   /dev/video0 (SRGGB12P capture) -> CPU box demosaic/downsample -> NV12
 *                                      -> /dev/video1 (Coda)
 * or, with --scaler vpss:
 *   /dev/video0 -> /dev/video2 (VPSS scaler/CSC mem2mem) -> /dev/video1,
 *   a zero-copy dmabuf chain (capture expbuf -> scaler OUTPUT import;
 *   scaler CAPTURE and encoder OUTPUT share the same CMA-heap buffers).
 *   sinks: Annex-B file/stdout and/or RTSP publisher (mediamtx-style).
 *
 * Performance shape:
 *  - With --io dmabuf (default) raw frames live in CACHED system dma-heap
 *    buffers imported by the encoder; CPU writes stay in cache and a single
 *    DMA_BUF_IOCTL_SYNC(END|RW) before QBUF is the only coherency cost.
 *    NV12 + a direct-DMA-capable kernel is then fully zero-copy; NV21 falls
 *    back to the driver's coherent staging, reading a cached vmap instead of
 *    an uncached vb2 mapping.  --io mmap reproduces the classic vb2 path.
 *  - The UYVY->NVxx conversion is single pass, 8 source bytes / 2 stores
 *    per 4 pixels (SWAR); there is no separate NV12->NV21 swap pass.
 *  - Encoder OUTPUT queue is deep enough that hardware encode overlaps the
 *    next conversion on the single core.
 *
 * Offline mode is useful on a host and is intentionally independent of V4L2:
 *   sg2002-h264-bridge --raw nv12 width height input.uyvy output.raw
 *
 * Live mode is:
 *   sg2002-h264-bridge [capture-node] [encoder-node] [options]
 * Legacy positional form still accepted:
 *   sg2002-h264-bridge [capture-node] [encoder-node] [h264-output|-] [full|half]
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <netdb.h>
#include <unistd.h>

#define CAPTURE_BUFFERS 4
#define ENCODER_OUT_BUFFERS 4
#define ENCODER_CAP_BUFFERS 3
#define SCALER_MID_BUFFERS 4
#define DEFAULT_CAPTURE "/dev/video0"
#define DEFAULT_ENCODER "/dev/video1"
#define DEFAULT_SCALER "/dev/video2"
#define DMA_HEAP_SYSTEM "/dev/dma_heap/system"
/* vb2-dma-contig imports must be single-segment; the system heap can
 * hand a multi-segment 3 MiB buffer, so prefer the guaranteed-contiguous
 * CMA heaps (named default_cma_region in newer kernels, linux,cma in
 * older ones) and fall back to system. */
#define DMA_HEAP_CMA "/dev/dma_heap/default_cma_region"
#define DMA_HEAP_CMA_OLD "/dev/dma_heap/linux,cma"
/* The no-map media pool exported as a heap: contiguous by definition and
 * independent of the colonized default CMA. */
#define DMA_HEAP_RESERVED "/dev/dma_heap/reserved"

#define RTP_MTU 1400
#define RTP_PT 96
#define RTSP_RECONNECT_MS 2000

static volatile sig_atomic_t stop_requested;

static void on_signal(int signal_number)
{
	(void)signal_number;
	stop_requested = 1;
}

static int xioctl(int fd, unsigned long request, void *arg)
{
	int ret;

	do
		/* musl declares the Linux ioctl command as int; Linux truncates
		 * the command to its low 32 bits on entry, so this is safe for the
		 * V4L2 _IOC values and keeps both glibc and musl builds warning-free. */
#ifdef __GLIBC__
		ret = ioctl(fd, request, arg);
#else
		ret = ioctl(fd, (int)request, arg);
#endif
	while (ret < 0 && errno == EINTR);
	return ret;
}

static void die_errno(const char *what)
{
	fprintf(stderr, "%s: %s\n", what, strerror(errno));
}

static uint64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static int write_all(int fd, const void *data, size_t length)
{
	const uint8_t *cursor = data;

	while (length) {
		ssize_t written = write(fd, cursor, length);

		if (written < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (!written) {
			errno = EIO;
			return -1;
		}
		cursor += written;
		length -= (size_t)written;
	}
	return 0;
}

static int parse_u32(const char *text, unsigned int *value)
{
	char *end;
	unsigned long parsed;

	errno = 0;
	parsed = strtoul(text, &end, 10);
	if (errno || end == text || *end || parsed > UINT32_MAX)
		return -1;
	*value = (unsigned int)parsed;
	return 0;
}

/* ------------------------------------------------------------------ */
/* UYVY -> NV12/NV21 conversion, single pass, SWAR                     */
/* ------------------------------------------------------------------ */

/* Convert packed UYVY to a tightly packed, progressive NV12/NV21 frame.
 *
 * src_stride is bytes per source line and dst_stride is bytes per destination
 * luma/chroma line.  Coda commonly rounds 1080 to 1088 macroblock lines; the
 * final dst_height-src_height lines repeat the final source line, so no
 * uninitialised DMA bytes can reach the encoder.  Columns past src_width are
 * edge-extended.  src_width must be a multiple of 4 (the SWAR group).
 */
static int uyvy_to_nvxx(const uint8_t *src, unsigned int src_width,
			unsigned int src_height, unsigned int src_stride,
			uint8_t *dst, unsigned int dst_width,
			unsigned int dst_height, unsigned int dst_stride,
			int nv21)
{
	uint8_t *y_plane = dst;
	uint8_t *uv_plane = dst + (size_t)dst_stride * dst_height;
	unsigned int y, x;

	if (!src || !dst || (src_width & 3) || (dst_width & 3) ||
	    (src_height & 1) || (dst_height & 1) || src_width > dst_width ||
	    src_height > dst_height || src_stride < src_width * 2 ||
	    dst_stride < dst_width)
		return -1;

	for (y = 0; y < dst_height; y++) {
		unsigned int sy = y < src_height ? y : src_height - 1;
		const uint8_t *line = src + (size_t)sy * src_stride;
		uint8_t *out = y_plane + (size_t)y * dst_stride;

		for (x = 0; x < src_width; x += 4) {
			uint64_t w;

			memcpy(&w, line + (size_t)x * 2, 8);
			{
				uint64_t yy = (w >> 8) & 0x00FF00FF00FF00FFULL;
				uint32_t luma32;

				yy = (yy | (yy >> 8)) & 0x0000FFFF0000FFFFULL;
				yy |= yy >> 16;
				luma32 = (uint32_t)yy;
				memcpy(out + x, &luma32, 4);
			}
		}
		for (; x < dst_width; x++)
			out[x] = out[src_width - 1];
	}

	for (y = 0; y < dst_height; y += 2) {
		unsigned int sy0 = y < src_height ? y : src_height - 1;
		unsigned int sy1 = y + 1 < src_height ? y + 1 : src_height - 1;
		const uint8_t *line0 = src + (size_t)sy0 * src_stride;
		const uint8_t *line1 = src + (size_t)sy1 * src_stride;
		uint8_t *out = uv_plane + (size_t)(y / 2) * dst_stride;

		for (x = 0; x < src_width; x += 4) {
			uint64_t w0, w1, u01, v01;
			uint32_t chroma32;

			memcpy(&w0, line0 + (size_t)x * 2, 8);
			memcpy(&w1, line1 + (size_t)x * 2, 8);
			/* Vertical average of the two 4:2:2 chroma rows,
			 * then interleave.  a/b gather U and V lanes with
			 * headroom for the sum. */
			{
				uint64_t u0 = w0 & 0x00FF00FF00FF00FFULL;
				uint64_t u1 = w1 & 0x00FF00FF00FF00FFULL;
				uint64_t s = u0 + u1; /* 9-bit lanes at 0,16,32,48 */
				uint64_t avg = ((s + 0x0001000100010001ULL) >> 1) &
					       0x00FF00FF00FF00FFULL;

				u01 = (avg & 0xFFULL) | ((avg >> 24) & 0xFF00ULL);
				v01 = ((avg >> 16) & 0xFFULL) |
				      ((avg >> 40) & 0xFF00ULL);
			}
			if (nv21)
				chroma32 = (uint32_t)((v01 & 0xFFULL) |
						      ((u01 & 0xFFULL) << 8) |
						      ((v01 & 0xFF00ULL) << 8) |
						      ((u01 & 0xFF00ULL) << 16));
			else
				chroma32 = (uint32_t)((u01 & 0xFFULL) |
						      ((v01 & 0xFFULL) << 8) |
						      ((u01 & 0xFF00ULL) << 8) |
						      ((v01 & 0xFF00ULL) << 16));
			memcpy(out + x, &chroma32, 4);
		}
		for (; x < dst_width; x += 2) {
			out[x] = out[src_width - 2];
			out[x + 1] = out[src_width - 1];
		}
	}

	return 0;
}

/* Box-filter a 2x2 luma area and a 4x4 source chroma area into one half-scale
 * progressive NV12/NV21 frame.  SG2002 capture is fixed at 1080p, so this mode
 * is useful when bandwidth or encoder load argues for 960x540. */
static int uyvy_to_nvxx_half(const uint8_t *src, unsigned int src_width,
			     unsigned int src_height, unsigned int src_stride,
			     uint8_t *dst, unsigned int dst_width,
			     unsigned int visible_height,
			     unsigned int dst_height, unsigned int dst_stride,
			     int nv21)
{
	uint8_t *y_plane = dst;
	uint8_t *uv_plane = dst + (size_t)dst_stride * dst_height;
	unsigned int y, x;

	if (!src || !dst || src_width != dst_width * 2 ||
	    src_height != visible_height * 2 || (dst_width & 3) ||
	    (visible_height & 1) || dst_height < visible_height ||
	    (dst_height & 1) || src_stride < src_width * 2 ||
	    dst_stride < dst_width)
		return -1;

	for (y = 0; y < visible_height; y++) {
		const uint8_t *line0 = src + (size_t)(y * 2) * src_stride;
		const uint8_t *line1 = line0 + src_stride;
		uint8_t *out = y_plane + (size_t)y * dst_stride;

		for (x = 0; x < dst_width; x += 2) {
			uint64_t w0, w1, rows;
			unsigned int s0, s1;
			uint16_t pair;

			memcpy(&w0, line0 + (size_t)x * 4, 8);
			memcpy(&w1, line1 + (size_t)x * 4, 8);
			/* Luma of both rows in 16-bit lanes: [Y0 Y1 Y2 Y3]. */
			rows = ((w0 >> 8) & 0x00FF00FF00FF00FFULL) +
			       ((w1 >> 8) & 0x00FF00FF00FF00FFULL);
			/* Output pixel x averages source (2x,2x+1) over both
			 * rows; x+1 averages (2x+2,2x+3). */
			s0 = (unsigned int)((rows & 0xFFFFULL) +
					    ((rows >> 16) & 0xFFFFULL));
			s1 = (unsigned int)(((rows >> 32) & 0xFFFFULL) +
					    ((rows >> 48) & 0xFFFFULL));
			pair = (uint16_t)(((s0 + 2) / 4) |
					  ((((s1 + 2) / 4) & 0xFF) << 8));
			memcpy(out + x, &pair, 2);
		}
	}
	for (; y < dst_height; y++)
		memcpy(y_plane + (size_t)y * dst_stride,
		       y_plane + (size_t)(visible_height - 1) * dst_stride,
		       dst_width);

	for (y = 0; y < visible_height / 2; y++) {
		uint8_t *out = uv_plane + (size_t)y * dst_stride;
		unsigned int sy = y * 4;

		for (x = 0; x < dst_width; x += 2) {
			unsigned int sx = x * 2;
			unsigned int u = 0, v = 0, row, pair;

			for (row = 0; row < 4; row++) {
				const uint8_t *line = src +
					(size_t)(sy + row) * src_stride;

				for (pair = 0; pair < 2; pair++) {
					u += line[(sx + pair * 2) * 2];
					v += line[(sx + pair * 2) * 2 + 2];
				}
			}
			if (nv21) {
				out[x] = (uint8_t)((v + 4) / 8);
				out[x + 1] = (uint8_t)((u + 4) / 8);
			} else {
				out[x] = (uint8_t)((u + 4) / 8);
				out[x + 1] = (uint8_t)((v + 4) / 8);
			}
		}
	}
	for (; y < dst_height / 2; y++)
		memcpy(uv_plane + (size_t)y * dst_stride,
		       uv_plane + (size_t)(visible_height / 2 - 1) * dst_stride,
		       dst_width);

	return 0;
}

/* ------------------------------------------------------------------ */
/* Packed SRGGB12P -> NV12 conversion                                 */
/* ------------------------------------------------------------------ */

/* The SG2002 camera's V4L2_PIX_FMT_SRGGB12P is pRCC nibble-aligned: two
 * pixels are three bytes, with byte 0/1 holding bits 11:4 and byte 2 holding
 * pixel 0 bits 3:0 in its low nibble and pixel 1 bits 3:0 in its high nibble.
 * (This is not the common low-byte-first MIPI RAW12 spelling.)  GC4653
 * reports black near code 256 in its 12-bit range, so remove that pedestal
 * before scaling to 8 bits. */
#define RAW12_BLACK_LEVEL 256U
#define RAW12_WHITE_LEVEL 4095U

static unsigned int raw12_get(const uint8_t *line, unsigned int x)
{
	const uint8_t *p = line + (size_t)(x / 2) * 3;
	unsigned int low = x & 1 ? p[2] >> 4 : p[2] & 0x0f;

	return ((unsigned int)p[x & 1] << 4) | low;
}

static unsigned int raw12_level8(unsigned int value)
{
	const unsigned int range = RAW12_WHITE_LEVEL - RAW12_BLACK_LEVEL;

	if (value <= RAW12_BLACK_LEVEL)
		return 0;
	if (value >= RAW12_WHITE_LEVEL)
		return 255;
	value -= RAW12_BLACK_LEVEL;
	return (value * 255U + range / 2U) / range;
}

/* Subsample one aligned 2x2 Bayer cell.  For 2x this is the complete source
 * box; for 4x it is the centred cell in that box.  It is a bounded Bayer
 * demosaic/downsample with four source reads per output pixel, rather than a
 * frame-sized intermediate or a 16-sample 4x box walk. */
static void raw12_cell_rgb(const uint8_t *src, unsigned int src_stride,
			   unsigned int sx, unsigned int sy,
			   unsigned int *red, unsigned int *green, unsigned int *blue)
{
	unsigned int r = 0, g = 0, b = 0;
	unsigned int rc = 0, gc = 0, bc = 0;
	unsigned int y, x;

	for (y = 0; y < 2; y++) {
		const uint8_t *line = src + (size_t)(sy + y) * src_stride;

		for (x = 0; x < 2; x++) {
			unsigned int value = raw12_level8(raw12_get(line, sx + x));

			/* SRGGB: row 0 is R G, row 1 is G B. */
			switch (((sy + y) & 1) * 2 + ((sx + x) & 1)) {
			case 0:
				r += value;
				rc++;
				break;
			case 1:
			case 2:
				g += value;
				gc++;
				break;
			default:
				b += value;
				bc++;
				break;
			}
		}
	}
	*red = (r + rc / 2U) / rc;
	*green = (g + gc / 2U) / gc;
	*blue = (b + bc / 2U) / bc;
}

static unsigned int clamp_u8(int value)
{
	if (value < 0)
		return 0;
	if (value > 255)
		return 255;
	return (unsigned int)value;
}

/* Convert packed SRGGB12P to a tightly packed, progressive NV12 frame.
 * `visible_height` is the active image height; dst_height may include the
 * Coda macroblock padding below it.  Only exact 2x and 4x reductions are
 * accepted.  The restriction is intentional: it keeps all source accesses
 * bounded, avoids a frame-sized scratch image, and makes the result easy to
 * reason about at the camera's fixed 2560x1440 mode. */
static int raw12_to_nv12(const uint8_t *src, unsigned int src_width,
			 unsigned int src_height, unsigned int src_stride,
			 uint8_t *dst, unsigned int dst_width,
			 unsigned int visible_height, unsigned int dst_height,
			 unsigned int dst_stride)
{
	uint8_t *y_plane = dst;
	uint8_t *uv_plane;
	unsigned int scale_x, scale_y, scale, cell_offset, y, x;

	if (!src || !dst || !src_width || !src_height || !dst_width ||
	    !visible_height || (src_width & 1) || (src_height & 1) ||
	    (dst_width & 1) || (visible_height & 1) || dst_height < visible_height ||
	    (dst_height & 1) || src_stride < (size_t)src_width * 3 / 2 ||
	    dst_stride < dst_width)
		return -1;
	if (src_width % dst_width || src_height % visible_height)
		return -1;
	scale_x = src_width / dst_width;
	scale_y = src_height / visible_height;
	if (scale_x != scale_y || (scale_x != 2 && scale_x != 4))
		return -1;
	scale = scale_x;
	cell_offset = scale == 4 ? 1 : 0;
	uv_plane = dst + (size_t)dst_stride * dst_height;

	/* Work in 2x2 output groups so chroma is the average of the same four
	 * RGB values whose luma was written above.  This also avoids any temporary
	 * per-frame or per-line storage. */
	for (y = 0; y < visible_height; y += 2) {
		uint8_t *y0 = y_plane + (size_t)y * dst_stride;
		uint8_t *y1 = y0 + dst_stride;
		uint8_t *uv = uv_plane + (size_t)(y / 2) * dst_stride;

		for (x = 0; x < dst_width; x += 2) {
			unsigned int r[4], g[4], b[4];
			unsigned int n, rsum = 0, gsum = 0, bsum = 0;

			raw12_cell_rgb(src, src_stride, x * scale + cell_offset,
				       y * scale + cell_offset, &r[0], &g[0], &b[0]);
			raw12_cell_rgb(src, src_stride, (x + 1) * scale + cell_offset,
				       y * scale + cell_offset, &r[1], &g[1], &b[1]);
			raw12_cell_rgb(src, src_stride, x * scale + cell_offset,
				       (y + 1) * scale + cell_offset,
				       &r[2], &g[2], &b[2]);
			raw12_cell_rgb(src, src_stride,
				       (x + 1) * scale + cell_offset,
				       (y + 1) * scale + cell_offset,
				       &r[3], &g[3], &b[3]);
			for (n = 0; n < 4; n++) {
				int yy = (77 * (int)r[n] + 150 * (int)g[n] +
					  29 * (int)b[n] + 128) >> 8;

				yy = (int)clamp_u8(yy);
				if (!n)
					y0[x] = (uint8_t)yy;
				else if (n == 1)
					y0[x + 1] = (uint8_t)yy;
				else if (n == 2)
					y1[x] = (uint8_t)yy;
				else
					y1[x + 1] = (uint8_t)yy;
				rsum += r[n];
				gsum += g[n];
				bsum += b[n];
			}
			/* BT.601 full-range chroma, rounded and clamped. */
			{
				int ravg = (int)((rsum + 2) / 4);
				int gavg = (int)((gsum + 2) / 4);
				int bavg = (int)((bsum + 2) / 4);
				int uu = ((-43 * ravg - 85 * gavg + 128 * bavg +
					   32768) >> 8);
				int vv = ((128 * ravg - 107 * gavg - 21 * bavg +
					   32768) >> 8);

				uv[x] = (uint8_t)clamp_u8(uu);
				uv[x + 1] = (uint8_t)clamp_u8(vv);
			}
		}
	}
	for (y = visible_height; y < dst_height; y++)
		memcpy(y_plane + (size_t)y * dst_stride,
		       y_plane + (size_t)(visible_height - 1) * dst_stride,
		       dst_width);
	for (y = visible_height / 2; y < dst_height / 2; y++)
		memcpy(uv_plane + (size_t)y * dst_stride,
		       uv_plane + (size_t)(visible_height / 2 - 1) * dst_stride,
		       dst_width);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Offline file conversion                                             */
/* ------------------------------------------------------------------ */

static int uyvy_to_nvxx_file(const char *input_path, const char *output_path,
			     unsigned int width, unsigned int height, int nv21)
{
	FILE *input = NULL, *output = NULL;
	uint8_t *src = NULL, *dst = NULL;
	size_t src_size = (size_t)width * height * 2;
	size_t dst_size = (size_t)width * height * 3 / 2;
	size_t got;
	int ret = -1;

	if ((width & 3) || (height & 1) || !width || !height)
		return fprintf(stderr, "raw dimensions must be non-zero; width a multiple of 4, height even\n"), -1;
	input = fopen(input_path, "rb");
	if (!input) {
		die_errno(input_path);
		goto out;
	}
	output = fopen(output_path, "wb");
	if (!output) {
		die_errno(output_path);
		goto out;
	}
	src = malloc(src_size);
	dst = malloc(dst_size);
	if (!src || !dst) {
		fprintf(stderr, "raw conversion allocation failed (%zu + %zu bytes)\n",
			src_size, dst_size);
		goto out;
	}
	got = fread(src, 1, src_size, input);
	if (got != src_size || fgetc(input) != EOF) {
		fprintf(stderr, "%s is not exactly %zu bytes\n", input_path, src_size);
		goto out;
	}
	if (uyvy_to_nvxx(src, width, height, width * 2, dst, width, height,
			 width, nv21))
		goto out;
	if (fwrite(dst, 1, dst_size, output) != dst_size) {
		die_errno("write raw output");
		goto out;
	}
	ret = 0;
out:
	free(dst);
	free(src);
	if (output)
		fclose(output);
	if (input)
		fclose(input);
	return ret;
}

/* Host-side smoke-test entry point for the pure RAW12 converter.  It uses the
 * practical 4x mode (2560x1440 -> 640x360), and deliberately has the same
 * exact-size input contract as a tightly packed camera frame. */
static int raw12_to_nv12_file(const char *input_path, const char *output_path,
			      unsigned int width, unsigned int height)
{
	FILE *input = NULL, *output = NULL;
	uint8_t *src = NULL, *dst = NULL;
	unsigned int dst_width, dst_height;
	size_t src_stride, src_size, dst_stride, dst_size, got;
	int ret = -1;

	if (!width || !height || (width & 3) || (height & 3)) {
		fprintf(stderr, "RAW12 dimensions must be non-zero and divisible by 4\n");
		return -1;
	}
	dst_width = width / 4;
	dst_height = height / 4;
	src_stride = (size_t)width * 3 / 2;
	src_size = src_stride * height;
	dst_stride = dst_width;
	dst_size = dst_stride * dst_height * 3 / 2;
	input = fopen(input_path, "rb");
	if (!input) {
		die_errno(input_path);
		goto out;
	}
	output = fopen(output_path, "wb");
	if (!output) {
		die_errno(output_path);
		goto out;
	}
	src = malloc(src_size);
	dst = malloc(dst_size);
	if (!src || !dst) {
		fprintf(stderr, "RAW12 conversion allocation failed (%zu + %zu bytes)\n",
			src_size, dst_size);
		goto out;
	}
	got = fread(src, 1, src_size, input);
	if (got != src_size || fgetc(input) != EOF) {
		fprintf(stderr, "%s is not exactly %zu bytes\n", input_path, src_size);
		goto out;
	}
	if (raw12_to_nv12(src, width, height, (unsigned int)src_stride,
			  dst, dst_width, dst_height, dst_height,
			  (unsigned int)dst_stride))
		goto out;
	if (fwrite(dst, 1, dst_size, output) != dst_size) {
		die_errno("write raw output");
		goto out;
	}
	ret = 0;
out:
	free(dst);
	free(src);
	if (output)
		fclose(output);
	if (input)
		fclose(input);
	return ret;
}

/* ------------------------------------------------------------------ */
/* V4L2 helpers                                                        */
/* ------------------------------------------------------------------ */

struct mapped_buf {
	void *addr;
	size_t length;
	int dmabuf_fd; /* -1 for plain MMAP queues */
};

struct mapped_queue {
	struct mapped_buf *bufs;
	unsigned int count;
};

static void unmap_queue(struct mapped_queue *queue)
{
	unsigned int i;

	if (!queue->bufs)
		return;
	for (i = 0; i < queue->count; i++) {
		if (queue->bufs[i].addr && queue->bufs[i].length &&
		    queue->bufs[i].addr != MAP_FAILED)
			munmap(queue->bufs[i].addr, queue->bufs[i].length);
		if (queue->bufs[i].dmabuf_fd >= 0)
			close(queue->bufs[i].dmabuf_fd);
	}
	free(queue->bufs);
	queue->bufs = NULL;
	queue->count = 0;
}

static int map_queue(int fd, enum v4l2_buf_type type, unsigned int requested,
		     struct mapped_queue *queue)
{
	struct v4l2_requestbuffers request = {
		.count = requested,
		.type = type,
		.memory = V4L2_MEMORY_MMAP,
	};
	unsigned int i;

	if (xioctl(fd, VIDIOC_REQBUFS, &request)) {
		int saved_errno = errno;

		fprintf(stderr, "VIDIOC_REQBUFS mmap type=%u count=%u: %s\n",
			type, requested, strerror(saved_errno));
		errno = saved_errno;
		return -1;
	}
	if (request.count < 1) {
		errno = ENOBUFS;
		return -1;
	}
	queue->bufs = calloc(request.count, sizeof(*queue->bufs));
	if (!queue->bufs) {
		fprintf(stderr, "calloc V4L2 mmap queue count=%u: %s\n",
			request.count, strerror(errno));
		return -1;
	}
	queue->count = request.count;
	for (i = 0; i < queue->count; i++) {
		struct v4l2_buffer buffer = {
			.type = type,
			.memory = V4L2_MEMORY_MMAP,
			.index = i,
		};
		if (xioctl(fd, VIDIOC_QUERYBUF, &buffer)) {
			int saved_errno = errno;

			fprintf(stderr, "VIDIOC_QUERYBUF mmap type=%u index=%u: %s\n",
				type, i, strerror(saved_errno));
			errno = saved_errno;
			return -1;
		}
		queue->bufs[i].length = buffer.length;
		queue->bufs[i].dmabuf_fd = -1;
		queue->bufs[i].addr = mmap(NULL, buffer.length, PROT_READ | PROT_WRITE,
					  MAP_SHARED, fd, buffer.m.offset);
		if (queue->bufs[i].addr == MAP_FAILED) {
			int saved_errno = errno;

			queue->bufs[i].addr = NULL;
			fprintf(stderr, "mmap V4L2 buffer type=%u index=%u length=%u: %s\n",
				type, i, buffer.length, strerror(saved_errno));
			errno = saved_errno;
			return -1;
		}
	}
	return 0;
}

/* Allocate `requested` raw-frame buffers from the system dma-heap, map them
 * cached, and register them with the encoder OUTPUT queue as DMABUF. */
static int heap_queue(int heap_fd, int encoder_fd, enum v4l2_buf_type type,
		      unsigned int requested, size_t size,
		      struct mapped_queue *queue)
{
	struct v4l2_requestbuffers request = {
		.count = requested,
		.type = type,
		.memory = V4L2_MEMORY_DMABUF,
	};
	unsigned int i;

	if (xioctl(encoder_fd, VIDIOC_REQBUFS, &request))
		return -1;
	if (request.count < 1) {
		errno = ENOBUFS;
		return -1;
	}
	queue->bufs = calloc(request.count, sizeof(*queue->bufs));
	if (!queue->bufs)
		return -1;
	queue->count = request.count;
	for (i = 0; i < queue->count; i++) {
		struct dma_heap_allocation_data alloc = {
			.len = size,
			.fd_flags = O_CLOEXEC | O_RDWR,
		};

		if (xioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &alloc))
			return -1;
		queue->bufs[i].dmabuf_fd = (int)alloc.fd;
		queue->bufs[i].length = size;
		queue->bufs[i].addr = mmap(NULL, size, PROT_READ | PROT_WRITE,
					  MAP_SHARED, (int)alloc.fd, 0);
		if (queue->bufs[i].addr == MAP_FAILED) {
			queue->bufs[i].addr = NULL;
			return -1;
		}
	}
	return 0;
}

static int queue_buffer(int fd, enum v4l2_buf_type type, unsigned int index,
			unsigned int bytesused)
{
	struct v4l2_buffer buffer = {
		.type = type,
		.memory = V4L2_MEMORY_MMAP,
		.index = index,
		.bytesused = bytesused,
	};
	return xioctl(fd, VIDIOC_QBUF, &buffer);
}

static int queue_dmabuf(int fd, enum v4l2_buf_type type, unsigned int index,
			int dmabuf_fd, unsigned int bytesused)
{
	struct v4l2_buffer buffer = {
		.type = type,
		.memory = V4L2_MEMORY_DMABUF,
		.index = index,
		.bytesused = bytesused,
	};

	buffer.m.fd = dmabuf_fd;
	return xioctl(fd, VIDIOC_QBUF, &buffer);
}

static int dequeue_buffer_mem(int fd, enum v4l2_buf_type type,
			      unsigned int memory, struct v4l2_buffer *buffer)
{
	memset(buffer, 0, sizeof(*buffer));
	buffer->type = type;
	buffer->memory = memory;
	return xioctl(fd, VIDIOC_DQBUF, buffer);
}

static int dequeue_buffer(int fd, enum v4l2_buf_type type,
			  struct v4l2_buffer *buffer)
{
	return dequeue_buffer_mem(fd, type, V4L2_MEMORY_MMAP, buffer);
}

static int stream(int fd, enum v4l2_buf_type type, int on)
{
	return xioctl(fd, on ? VIDIOC_STREAMON : VIDIOC_STREAMOFF, &type);
}

static int get_format(int fd, enum v4l2_buf_type type,
		      struct v4l2_pix_format *pix)
{
	struct v4l2_format format = { .type = type };
	if (xioctl(fd, VIDIOC_G_FMT, &format))
		return -1;
	*pix = format.fmt.pix;
	return 0;
}

static int set_encoder_format(int fd, enum v4l2_buf_type type,
			      uint32_t pixel_format, unsigned int width,
			      unsigned int height, struct v4l2_pix_format *actual)
{
	struct v4l2_format format = { .type = type };

	format.fmt.pix.width = width;
	format.fmt.pix.height = height;
	format.fmt.pix.pixelformat = pixel_format;
	format.fmt.pix.field = V4L2_FIELD_NONE;
	if (pixel_format == V4L2_PIX_FMT_H264)
		format.fmt.pix.sizeimage = 1024U * 1024U;
	if (xioctl(fd, VIDIOC_S_FMT, &format))
		return -1;
	*actual = format.fmt.pix;
	return 0;
}

static int set_output_crop(int fd, unsigned int width, unsigned int height)
{
	struct v4l2_selection selection = {
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
		.target = V4L2_SEL_TGT_CROP,
		.r = {
			.width = width,
			.height = height,
		},
	};
	return xioctl(fd, VIDIOC_S_SELECTION, &selection);
}

static int set_encoder_controls(int fd, unsigned int bitrate, unsigned int gop)
{
	struct v4l2_ext_control controls[2];
	struct v4l2_ext_controls list = {
		.which = V4L2_CTRL_CLASS_CODEC,
		.count = 0,
		.controls = controls,
	};

	if (bitrate) {
		controls[list.count].id = V4L2_CID_MPEG_VIDEO_BITRATE;
		controls[list.count].value = (int32_t)bitrate;
		list.count++;
	}
	if (gop) {
		controls[list.count].id = V4L2_CID_MPEG_VIDEO_GOP_SIZE;
		controls[list.count].value = (int32_t)gop;
		list.count++;
	}
	if (!list.count)
		return 0;
	return xioctl(fd, VIDIOC_S_EXT_CTRLS, &list);
}

/* ------------------------------------------------------------------ */
/* H.264 Annex-B NAL scanning + base64                                 */
/* ------------------------------------------------------------------ */

struct nal_view {
	const uint8_t *data; /* points just past the start code */
	size_t size;
	unsigned int type;
};

/* Split one access unit into NAL views.  Returns count, or 0 when the
 * buffer does not look like Annex-B. */
static unsigned int annexb_split(const uint8_t *buf, size_t size,
				 struct nal_view *nals, unsigned int max_nals)
{
	unsigned int count = 0;
	size_t pos = 0;

	while (pos + 4 <= size && count < max_nals) {
		size_t sc = SIZE_MAX, sc_len = 0, next, i;

		for (i = pos; i + 3 <= size; i++) {
			if (buf[i] == 0 && buf[i + 1] == 0 &&
			    ((i + 3 < size && buf[i + 2] == 1) ||
			     (i + 4 <= size && buf[i + 2] == 0 && buf[i + 3] == 1))) {
				sc = i;
				sc_len = buf[i + 2] == 1 ? 3 : 4;
				break;
			}
		}
		if (sc == SIZE_MAX)
			break;
		for (next = sc + sc_len; next + 3 <= size; next++) {
			if (buf[next] == 0 && buf[next + 1] == 0 &&
			    (buf[next + 2] == 1 ||
			     (next + 3 < size && buf[next + 2] == 0 && buf[next + 3] == 1)))
				break;
		}
		if (next + 3 > size)
			next = size;
		if (sc + sc_len >= next)
			break;
		nals[count].data = buf + sc + sc_len;
		nals[count].size = next - (sc + sc_len);
		nals[count].type = nals[count].data[0] & 0x1f;
		count++;
		pos = next;
	}
	return count;
}

static const char b64_alphabet[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void base64_encode(const uint8_t *in, size_t len, char *out)
{
	size_t i, o = 0;

	for (i = 0; i + 3 <= len; i += 3) {
		uint32_t v = (uint32_t)in[i] << 16 | (uint32_t)in[i + 1] << 8 | in[i + 2];

		out[o++] = b64_alphabet[v >> 18];
		out[o++] = b64_alphabet[(v >> 12) & 63];
		out[o++] = b64_alphabet[(v >> 6) & 63];
		out[o++] = b64_alphabet[v & 63];
	}
	if (i < len) {
		uint32_t v = (uint32_t)in[i] << 16;

		if (i + 1 < len)
			v |= (uint32_t)in[i + 1] << 8;
		out[o++] = b64_alphabet[v >> 18];
		out[o++] = b64_alphabet[(v >> 12) & 63];
		out[o++] = i + 1 < len ? b64_alphabet[(v >> 6) & 63] : '=';
		out[o++] = '=';
	}
	out[o] = '\0';
}

/* ------------------------------------------------------------------ */
/* Minimal RTSP publisher (TCP interleaved, H.264 RTP packetization)   */
/* ------------------------------------------------------------------ */

struct rtsp_sink {
	char url[160];
	char host[61];
	char path[81];
	uint16_t port;
	int fd;              /* -1 when disconnected */
	uint16_t rtp_seq;
	uint32_t rtp_ssrc;
	uint32_t cseq;
	uint64_t next_retry_ms;
	/* Stashed parameter sets for the SDP offer. */
	uint8_t sps[256];
	size_t sps_len;
	uint8_t pps[256];
	size_t pps_len;
	int have_params;
};

static int rtsp_parse_url(struct rtsp_sink *sink, const char *url)
{
	const char *authority, *slash, *colon;

	if (strncmp(url, "rtsp://", 7))
		return -1;
	authority = url + 7;
	slash = strchr(authority, '/');
	if (!slash)
		return -1;
	colon = memchr(authority, ':', (size_t)(slash - authority));
	if (colon) {
		unsigned int port = 0;
		size_t host_len = (size_t)(colon - authority);
		size_t port_len = (size_t)(slash - colon - 1);
		size_t d;

		if (!host_len || host_len > 60 || !port_len || port_len > 5)
			return -1;
		for (d = 0; d < port_len; d++) {
			if (colon[1 + d] < '0' || colon[1 + d] > '9')
				return -1;
			port = port * 10 + (unsigned int)(colon[1 + d] - '0');
		}
		if (!port || port > 65535)
			return -1;
		memcpy(sink->host, authority, host_len);
		sink->host[host_len] = '\0';
		sink->port = (uint16_t)port;
	} else {
		size_t host_len = (size_t)(slash - authority);

		if (!host_len || host_len > 60)
			return -1;
		memcpy(sink->host, authority, host_len);
		sink->host[host_len] = '\0';
		sink->port = 554;
	}
	if (strlen(slash) > 80)
		return -1;
	strcpy(sink->path, slash);
	snprintf(sink->url, sizeof(sink->url), "rtsp://%s:%u%s",
		 sink->host, sink->port, sink->path);
	return 0;
}

/* Read one RTSP response (headers only); returns the status code, or -1.
 * When session_out is given, any Session: header id (before ';') is copied
 * into it for echoing back on subsequent requests. */
static int rtsp_read_reply(int fd, char *session_out, size_t session_size)
{
	char buf[1024];
	size_t used = 0;
	int status = -1;

	for (;;) {
		if (used + 1 >= sizeof(buf))
			return -1;
		ssize_t got = read(fd, buf + used, 1);

		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0)
			return -1;
		used += (size_t)got;
		buf[used] = '\0';
		if (used >= 4 && !strcmp(buf + used - 4, "\r\n\r\n"))
			break;
	}
	if (sscanf(buf, "RTSP/1.0 %d", &status) != 1)
		return -1;
	if (session_out && session_size) {
		char *line = strstr(buf, "\r\n");
		const char *end = buf + used;

		session_out[0] = '\0';
		while (line && (line += 2) < end && line[0] != '\r') {
			if (!strncasecmp(line, "Session:", 8)) {
				char *value = line + 8;
				char *stop;
				size_t len;

				while (*value == ' ')
					value++;
				stop = strchr(value, ';');
				if (!stop)
					stop = strstr(value, "\r\n");
				if (stop) {
					len = (size_t)(stop - value);
					if (len >= session_size)
						len = session_size - 1;
					memcpy(session_out, value, len);
					session_out[len] = '\0';
				}
				break;
			}
			line = strstr(line, "\r\n");
		}
	}
	return status;
}

static int rtsp_request(struct rtsp_sink *sink, const char *method,
			const char *extra_headers, const char *body)
{
	char request[2048];
	int len;

	len = snprintf(request, sizeof(request), "%s %s RTSP/1.0\r\nCSeq: %u\r\n",
		       method, sink->url, ++sink->cseq);
	if (len > 0 && extra_headers)
		len += snprintf(request + len, sizeof(request) - (size_t)len, "%s",
				extra_headers);
	if (len > 0 && body)
		len += snprintf(request + len, sizeof(request) - (size_t)len,
				"Content-Type: application/sdp\r\n"
				"Content-Length: %zu\r\n\r\n%s",
				strlen(body), body);
	else if (len > 0)
		len += snprintf(request + len, sizeof(request) - (size_t)len,
				"\r\n");
	if (len < 0 || (size_t)len >= sizeof(request))
		return -1;
	if (write_all(sink->fd, request, (size_t)len))
		return -1;
	return rtsp_read_reply(sink->fd, NULL, 0);
}

static void rtsp_close(struct rtsp_sink *sink)
{
	if (sink->fd >= 0)
		close(sink->fd);
	sink->fd = -1;
	sink->next_retry_ms = now_ms() + RTSP_RECONNECT_MS;
}

static int rtsp_connect(struct rtsp_sink *sink)
{
	char port_text[8];
	struct addrinfo hints = {
		.ai_family = AF_UNSPEC,
		.ai_socktype = SOCK_STREAM,
	};
	struct addrinfo *result = NULL, *it;
	char sps_b64[384], pps_b64[384], sdp[1200];
	int status = -1;

	if (!sink->have_params)
		return -1;
	snprintf(port_text, sizeof(port_text), "%u", sink->port);
	if (getaddrinfo(sink->host, port_text, &hints, &result))
		return -1;
	for (it = result; it; it = it->ai_next) {
		sink->fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
		if (sink->fd < 0)
			continue;
		if (!connect(sink->fd, it->ai_addr, it->ai_addrlen))
			break;
		close(sink->fd);
		sink->fd = -1;
	}
	freeaddrinfo(result);
	if (sink->fd < 0)
		return -1;

	base64_encode(sink->sps, sink->sps_len, sps_b64);
	base64_encode(sink->pps, sink->pps_len, pps_b64);
	snprintf(sdp, sizeof(sdp),
		 "v=0\r\n"
		 "o=- 0 0 IN IP4 127.0.0.1\r\n"
		 "s=sg2002-kvm\r\n"
		 "c=IN IP4 0.0.0.0\r\n"
		 "t=0 0\r\n"
		 "m=video 0 RTP/AVP/TCP 96\r\n"
		 "a=rtpmap:96 H264/90000\r\n"
		 "a=fmtp:96 packetization-mode=1;sprop-parameter-sets=%s,%s\r\n"
		 "a=control:track1\r\n",
		 sps_b64, pps_b64);

	sink->cseq = 0;
	status = rtsp_request(sink, "ANNOUNCE", NULL, sdp);
	if (status != 200)
		goto fail;
	{
		char track_url[sizeof(sink->url) + 16];
		char request[512];
		char session[80];
		int len;

		snprintf(track_url, sizeof(track_url), "%s/track1", sink->url);
		len = snprintf(request, sizeof(request),
			       "SETUP %s RTSP/1.0\r\nCSeq: %u\r\n"
			       "Transport: RTP/AVP/TCP;unicast;interleaved=0-1;mode=record\r\n\r\n",
			       track_url, ++sink->cseq);
		if (len < 0 || (size_t)len >= sizeof(request) ||
		    write_all(sink->fd, request, (size_t)len))
			goto fail;
		status = rtsp_read_reply(sink->fd, session, sizeof(session));
		if (status != 200 || !session[0])
			goto fail;
		len = snprintf(request, sizeof(request),
			       "RECORD %s RTSP/1.0\r\nCSeq: %u\r\n"
			       "Session: %s\r\nRange: npt=0.000-\r\n\r\n",
			       sink->url, ++sink->cseq, session);
		if (len < 0 || (size_t)len >= sizeof(request) ||
		    write_all(sink->fd, request, (size_t)len))
			goto fail;
		status = rtsp_read_reply(sink->fd, NULL, 0);
		if (status != 200)
			goto fail;
	}
	fprintf(stderr, "rtsp: publishing to %s\n", sink->url);
	return 0;
fail:
	rtsp_close(sink);
	return -1;
}

/* RTP-send one access unit (Annex-B).  90 kHz clock. */
static int rtsp_send_au(struct rtsp_sink *sink, const uint8_t *buf, size_t size,
			uint64_t pts_ms)
{
	struct nal_view nals[32];
	unsigned int count, n;
	uint32_t timestamp = (uint32_t)(pts_ms * 90);

	count = annexb_split(buf, size, nals, 32);
	if (!count)
		return 0;
	for (n = 0; n < count; n++) {
		const uint8_t *nal = nals[n].data;
		size_t left = nals[n].size;
		int last_nal = n == count - 1;

		if (left <= RTP_MTU) {
			uint8_t packet[12 + RTP_MTU];
			uint8_t *p = packet;

			*p++ = 0x80;
			*p++ = RTP_PT | (last_nal ? 0x80 : 0);
			*p++ = (uint8_t)(sink->rtp_seq >> 8);
			*p++ = (uint8_t)sink->rtp_seq;
			*p++ = (uint8_t)(timestamp >> 24);
			*p++ = (uint8_t)(timestamp >> 16);
			*p++ = (uint8_t)(timestamp >> 8);
			*p++ = (uint8_t)timestamp;
			*p++ = (uint8_t)(sink->rtp_ssrc >> 24);
			*p++ = (uint8_t)(sink->rtp_ssrc >> 16);
			*p++ = (uint8_t)(sink->rtp_ssrc >> 8);
			*p++ = (uint8_t)sink->rtp_ssrc;
			memcpy(p, nal, left);
			{
				uint32_t total = 12 + (uint32_t)left;
				uint8_t frame[4] = {
					'$', 0,
					(uint8_t)(total >> 8), (uint8_t)total,
				};

				if (write_all(sink->fd, frame, 4) ||
				    write_all(sink->fd, packet, total))
					return -1;
			}
			sink->rtp_seq++;
		} else {
			uint8_t fu_header = (uint8_t)(nal[0] & 0xe0); /* F/NRI */
			uint8_t nal_type = (uint8_t)(nal[0] & 0x1f);
			size_t offset = 1;

			while (offset < left) {
				size_t chunk = left - offset;
				uint8_t packet[12 + 2 + RTP_MTU];
				uint8_t *p = packet;
				int end;

				if (chunk > RTP_MTU)
					chunk = RTP_MTU;
				end = offset + chunk >= left;
				*p++ = 0x80;
				*p++ = RTP_PT | ((last_nal && end) ? 0x80 : 0);
				*p++ = (uint8_t)(sink->rtp_seq >> 8);
				*p++ = (uint8_t)sink->rtp_seq;
				*p++ = (uint8_t)(timestamp >> 24);
				*p++ = (uint8_t)(timestamp >> 16);
				*p++ = (uint8_t)(timestamp >> 8);
				*p++ = (uint8_t)timestamp;
				*p++ = (uint8_t)(sink->rtp_ssrc >> 24);
				*p++ = (uint8_t)(sink->rtp_ssrc >> 16);
				*p++ = (uint8_t)(sink->rtp_ssrc >> 8);
				*p++ = (uint8_t)sink->rtp_ssrc;
				*p++ = fu_header | 28; /* FU-A */
				*p++ = (uint8_t)((offset == 1 ? 0x80 : 0) |
						 (end ? 0x40 : 0) | nal_type);
				memcpy(p, nal + offset, chunk);
				{
					uint32_t total = 14 + (uint32_t)chunk;
					uint8_t frame[4] = {
						'$', 0,
						(uint8_t)(total >> 8), (uint8_t)total,
					};

					if (write_all(sink->fd, frame, 4) ||
					    write_all(sink->fd, packet, total))
						return -1;
				}
				sink->rtp_seq++;
				offset += chunk;
			}
		}
	}
	return 0;
}

/* Feed one coded access unit to the RTSP sink; handles (re)connect and
 * parameter-set discovery.  Never blocks the pipeline for long: failures
 * flip the sink into retry-later state and streaming continues. */
static void rtsp_offer(struct rtsp_sink *sink, const uint8_t *buf, size_t size,
		       uint64_t pts_ms)
{
	if (!sink->have_params) {
		struct nal_view nals[32];
		unsigned int count = annexb_split(buf, size, nals, 32);
		unsigned int n;

		for (n = 0; n < count; n++) {
			if (nals[n].type == 7 && nals[n].size <= sizeof(sink->sps)) {
				memcpy(sink->sps, nals[n].data, nals[n].size);
				sink->sps_len = nals[n].size;
			} else if (nals[n].type == 8 &&
				   nals[n].size <= sizeof(sink->pps)) {
				memcpy(sink->pps, nals[n].data, nals[n].size);
				sink->pps_len = nals[n].size;
			}
		}
		if (sink->sps_len && sink->pps_len) {
			sink->have_params = 1;
			fprintf(stderr, "rtsp: learned SPS (%zu) / PPS (%zu)\n",
				sink->sps_len, sink->pps_len);
		}
	}
	if (sink->fd < 0) {
		if (!sink->have_params || now_ms() < sink->next_retry_ms)
			return;
		if (rtsp_connect(sink)) {
			sink->next_retry_ms = now_ms() + RTSP_RECONNECT_MS;
			return;
		}
	}
	if (rtsp_send_au(sink, buf, size, pts_ms)) {
		fprintf(stderr, "rtsp: send failed, will retry\n");
		rtsp_close(sink);
	}
}

/* ------------------------------------------------------------------ */
/* Live bridge                                                         */
/* ------------------------------------------------------------------ */

struct bridge_options {
	const char *capture_path;
	const char *encoder_path;
	const char *scaler_path;
	const char *output_path;
	const char *rtsp_url;
	unsigned int bitrate;
	unsigned int gop;
	unsigned int mid_buffers;
	unsigned int capture_buffers;
	int half_scale;
	int use_dmabuf;
	int use_vpss;
	int mid_heap_reserved;
	uint32_t encoder_input_format; /* V4L2_PIX_FMT_NV21 or NV12 */
};

static const char *init_step;

static void die_step(void)
{
	fprintf(stderr, "live bridge at %s: %s\n", init_step ? init_step : "?",
		strerror(errno));
}

static int live_bridge(const struct bridge_options *opts)
{
	int capture_fd = -1, encoder_fd = -1, output_fd = -1, heap_fd = -1;
	struct mapped_queue capture_queue = { 0 }, encoder_out = { 0 }, encoder_cap = { 0 };
	unsigned char *output_queued = NULL;
	struct v4l2_pix_format capture_fmt, encoder_out_fmt, encoder_cap_fmt;
	unsigned int i, free_output = 0, held_capture = UINT32_MAX;
	unsigned int visible_width, visible_height, coded_height;
	uint64_t frames = 0, encoded_frames = 0, encoded_bytes = 0;
	uint64_t start_ms = 0, last_stats_ms = 0, last_stats_frames = 0;
	int capture_on = 0, encoder_out_on = 0, encoder_cap_on = 0, ret = -1;
	int use_dmabuf = opts->use_dmabuf;
	int raw12 = 0, nv21;
	uint32_t encoder_input_format;
	struct rtsp_sink rtsp;

	memset(&rtsp, 0, sizeof(rtsp));
	rtsp.fd = -1;
	if (opts->rtsp_url && rtsp_parse_url(&rtsp, opts->rtsp_url)) {
		fprintf(stderr, "bad rtsp url: %s\n", opts->rtsp_url);
		return -1;
	}
	rtsp.rtp_ssrc = 0x53324732; /* "S2G2" */

	capture_fd = open(opts->capture_path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (capture_fd < 0) {
		die_errno(opts->capture_path);
		goto out;
	}
	encoder_fd = open(opts->encoder_path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (encoder_fd < 0) {
		die_errno(opts->encoder_path);
		goto out;
	}
	if (opts->output_path) {
		if (!strcmp(opts->output_path, "-"))
			output_fd = STDOUT_FILENO;
		else
			output_fd = open(opts->output_path,
					 O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
					 0644);
		if (output_fd < 0) {
			die_errno(opts->output_path);
			goto out;
		}
	}
	if (get_format(capture_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, &capture_fmt))
		goto out_errno;
	raw12 = capture_fmt.pixelformat == V4L2_PIX_FMT_SRGGB12P;
	if ((!raw12 && capture_fmt.pixelformat != V4L2_PIX_FMT_UYVY) ||
	    capture_fmt.width < 4 || capture_fmt.height < 2 ||
	    (raw12 ? capture_fmt.bytesperline < (size_t)capture_fmt.width * 3 / 2 :
	     capture_fmt.bytesperline < (size_t)capture_fmt.width * 2)) {
		fprintf(stderr, "capture must provide packed UYVY or SRGGB12P with a valid stride\n");
		goto out;
	}
	if (raw12) {
		unsigned int raw_scale = opts->half_scale ? 2 : 4;

		if (opts->use_vpss) {
			fprintf(stderr, "SRGGB12P capture requires --scaler cpu; VPSS accepts UYVY only\n");
			goto out;
		}
		if (capture_fmt.width % raw_scale || capture_fmt.height % raw_scale) {
			fprintf(stderr, "SRGGB12P dimensions %ux%u do not support an exact %ux reduction\n",
				capture_fmt.width, capture_fmt.height, raw_scale);
			goto out;
		}
		visible_width = capture_fmt.width / raw_scale;
		visible_height = capture_fmt.height / raw_scale;
	} else {
		if ((capture_fmt.width & 3) || (opts->half_scale && (capture_fmt.height & 3))) {
			fprintf(stderr, "capture width must be a multiple of 4 (half scale: height of 4)\n");
			goto out;
		}
		visible_width = opts->half_scale ? capture_fmt.width / 2 : capture_fmt.width;
		visible_height = opts->half_scale ? capture_fmt.height / 2 : capture_fmt.height;
	}
	/* RAW12 is always converted to NV12.  Keep the existing UYVY default and
	 * --format nv21 behaviour unchanged. */
	encoder_input_format = raw12 ? V4L2_PIX_FMT_NV12 : opts->encoder_input_format;
	nv21 = encoder_input_format == V4L2_PIX_FMT_NV21;
	coded_height = (visible_height + 15U) & ~15U;
	/* Coda reads a macroblock surface even when the visible frame is 1080
	 * lines.  Negotiate that padded surface first, crop it to the source
	 * height, and only then configure CAPTURE.  Doing this in another order can
	 * leave the firmware with a 1920x1080 stride/height mismatch. */
	if (set_encoder_format(encoder_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT,
			       encoder_input_format, visible_width,
			       coded_height, &encoder_out_fmt))
		goto out_errno;
	if (set_output_crop(encoder_fd, visible_width, visible_height))
		goto out_errno;
	if (get_format(encoder_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT, &encoder_out_fmt))
		goto out_errno;
	if (set_encoder_format(encoder_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE,
			       V4L2_PIX_FMT_H264, visible_width,
			       visible_height, &encoder_cap_fmt))
		goto out_errno;
	if ((encoder_out_fmt.width & 3) || (encoder_out_fmt.height & 1) ||
	    encoder_out_fmt.bytesperline < encoder_out_fmt.width ||
	    encoder_out_fmt.sizeimage < (size_t)encoder_out_fmt.bytesperline *
					encoder_out_fmt.height * 3 / 2) {
		fprintf(stderr, "encoder did not return a usable output format\n");
		goto out;
	}
	if (encoder_cap_fmt.pixelformat != V4L2_PIX_FMT_H264)
		goto out;
	if (set_encoder_controls(encoder_fd, opts->bitrate, opts->gop))
		fprintf(stderr, "warning: encoder controls rejected, running firmware defaults\n");

	/* Encoder OUTPUT buffers: cached dma-heap import when possible. */
	if (use_dmabuf) {
		init_step = "open " DMA_HEAP_CMA;
		heap_fd = open(DMA_HEAP_CMA, O_RDONLY | O_CLOEXEC);
		if (heap_fd < 0) {
			init_step = "open " DMA_HEAP_CMA_OLD;
			heap_fd = open(DMA_HEAP_CMA_OLD, O_RDONLY | O_CLOEXEC);
		}
		if (heap_fd < 0) {
			init_step = "open " DMA_HEAP_SYSTEM;
			heap_fd = open(DMA_HEAP_SYSTEM, O_RDONLY | O_CLOEXEC);
		}
		if (heap_fd < 0) {
			fprintf(stderr, "no dma-heaps (%s), using --io mmap\n",
				strerror(errno));
			use_dmabuf = 0;
		}
	}
	init_step = "encoder output buffer allocation";
	if (use_dmabuf) {
		if (heap_queue(heap_fd, encoder_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT,
			       ENCODER_OUT_BUFFERS, encoder_out_fmt.sizeimage,
			       &encoder_out))
			goto out_errno;
	} else {
		if (map_queue(encoder_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT,
			      ENCODER_OUT_BUFFERS, &encoder_out))
			goto out_errno;
	}
	output_queued = calloc(encoder_out.count, sizeof(*output_queued));
	if (!output_queued)
		goto out_errno;
	init_step = "encoder capture buffer allocation";
	if (map_queue(encoder_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE,
		      ENCODER_CAP_BUFFERS, &encoder_cap))
		goto out_errno;
	init_step = "encoder capture QBUF";
	for (i = 0; i < encoder_cap.count; i++)
		if (queue_buffer(encoder_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, i, 0))
			goto out_errno;
	/* Prime Coda before allocating the large CSI buffers.  This gives its
	 * reconstruction surfaces first claim on the media pool.  The black frame
	 * is also a valid reference picture for the first live frame. */
	{
		size_t luma_size = (size_t)encoder_out_fmt.bytesperline *
			encoder_out_fmt.height;
		size_t frame_size = luma_size * 3 / 2;

		if (frame_size > UINT32_MAX ||
		    frame_size > encoder_out.bufs[0].length) {
			fprintf(stderr, "encoder priming buffer is too small\n");
			goto out;
		}
		memset(encoder_out.bufs[0].addr, 16, luma_size);
		memset((uint8_t *)encoder_out.bufs[0].addr + luma_size, 128,
		       frame_size - luma_size);
		if (use_dmabuf) {
			struct dma_buf_sync sync = {
				.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW,
			};

			init_step = "priming DMA_BUF_IOCTL_SYNC";
			if (xioctl(encoder_out.bufs[0].dmabuf_fd,
				   DMA_BUF_IOCTL_SYNC, &sync))
				goto out_errno;
			init_step = "priming QBUF (dmabuf import)";
			if (queue_dmabuf(encoder_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT,
					 0, encoder_out.bufs[0].dmabuf_fd,
					 (unsigned int)frame_size))
				goto out_errno;
		} else {
			if (queue_buffer(encoder_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT,
					 0, (unsigned int)frame_size))
				goto out_errno;
		}
		output_queued[0] = 1;
		free_output = encoder_out.count - 1;
	}
	init_step = "encoder capture STREAMON";
	if (stream(encoder_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, 1))
		goto out_errno;
	encoder_cap_on = 1;
	init_step = "encoder output STREAMON";
	if (stream(encoder_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT, 1))
		goto out_errno;
	encoder_out_on = 1;
	init_step = "CSI capture buffer allocation";
	if (map_queue(capture_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE,
		      opts->capture_buffers, &capture_queue))
		goto out_errno;
	init_step = "CSI capture QBUF";
	for (i = 0; i < capture_queue.count; i++)
		if (queue_buffer(capture_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, i, 0))
			goto out_errno;
	init_step = "CSI capture STREAMON";
	if (stream(capture_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, 1))
		goto out_errno;
	capture_on = 1;
	fprintf(stderr, "bridge %s (%ux%u stride %u) -> %s %s (%ux%u stride %u) io=%s bitrate=%u gop=%u\n",
		opts->capture_path, capture_fmt.width, capture_fmt.height,
		capture_fmt.bytesperline, opts->encoder_path,
		nv21 ? "NV21" : "NV12", encoder_out_fmt.width,
		encoder_out_fmt.height, encoder_out_fmt.bytesperline,
		use_dmabuf ? "dmabuf" : "mmap", opts->bitrate, opts->gop);
	start_ms = now_ms();
	last_stats_ms = start_ms;

	while (!stop_requested) {
		struct v4l2_buffer buffer;
		int progress = 0;

		/* Drain encoded CAPTURE buffers and forward to the sinks. */
		for (;;) {
			uint64_t pts_ms;

			if (dequeue_buffer(encoder_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE,
					   &buffer)) {
				if (errno == EAGAIN)
					break;
				goto out_errno;
			}
			if (buffer.index >= encoder_cap.count ||
			    buffer.bytesused > encoder_cap.bufs[buffer.index].length) {
				fprintf(stderr, "encoder returned an invalid capture buffer\n");
				goto out;
			}
			pts_ms = (uint64_t)buffer.timestamp.tv_sec * 1000ULL +
				(uint64_t)buffer.timestamp.tv_usec / 1000;
			if (!pts_ms)
				pts_ms = now_ms();
			if (output_fd >= 0 && buffer.bytesused &&
			    write_all(output_fd, encoder_cap.bufs[buffer.index].addr,
				      buffer.bytesused))
				goto out_errno;
			if (opts->rtsp_url && buffer.bytesused)
				rtsp_offer(&rtsp, encoder_cap.bufs[buffer.index].addr,
					   buffer.bytesused, pts_ms);
			encoded_frames++;
			encoded_bytes += buffer.bytesused;
			if (queue_buffer(encoder_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE,
					 buffer.index, 0))
				goto out_errno;
			progress = 1;
		}
		/* Return completed OUTPUT buffers to the free pool. */
		for (;;) {
			if (dequeue_buffer(encoder_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT,
					   &buffer)) {
				if (errno == EAGAIN)
					break;
				goto out_errno;
			}
			if (buffer.index >= encoder_out.count || !output_queued[buffer.index]) {
				fprintf(stderr, "encoder returned an unexpected output buffer\n");
				goto out;
			}
			output_queued[buffer.index] = 0;
			free_output++;
			progress = 1;
		}
		if (held_capture == UINT32_MAX) {
			if (!dequeue_buffer(capture_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE,
					    &buffer))
				held_capture = buffer.index;
			else if (errno != EAGAIN)
				goto out_errno;
		}
		if (held_capture != UINT32_MAX && free_output) {
			struct v4l2_buffer out_buffer;
			unsigned int dst_size = encoder_out_fmt.bytesperline *
					encoder_out_fmt.height * 3 / 2;
			unsigned int source_index = held_capture;
			unsigned int output_index;
			for (output_index = 0; output_index < encoder_out.count;
			     output_index++)
				if (!output_queued[output_index])
					break;
			if (output_index == encoder_out.count ||
			    dst_size > encoder_out.bufs[output_index].length) {
				fprintf(stderr, "no usable free encoder output buffer\n");
				goto out;
			}
			if (raw12 ?
			     raw12_to_nv12(capture_queue.bufs[source_index].addr,
				capture_fmt.width, capture_fmt.height,
				capture_fmt.bytesperline,
				encoder_out.bufs[output_index].addr,
				encoder_out_fmt.width, visible_height,
				encoder_out_fmt.height,
				encoder_out_fmt.bytesperline) :
			     (opts->half_scale ?
			     uyvy_to_nvxx_half(capture_queue.bufs[source_index].addr,
				capture_fmt.width, capture_fmt.height,
				capture_fmt.bytesperline,
				encoder_out.bufs[output_index].addr,
				encoder_out_fmt.width, visible_height,
				encoder_out_fmt.height,
				encoder_out_fmt.bytesperline, nv21) :
			     uyvy_to_nvxx(capture_queue.bufs[source_index].addr,
				capture_fmt.width, capture_fmt.height,
				capture_fmt.bytesperline,
				encoder_out.bufs[output_index].addr,
				encoder_out_fmt.width, encoder_out_fmt.height,
				encoder_out_fmt.bytesperline, nv21))) {
				fprintf(stderr, raw12 ?
					"SRGGB12P->NV12 conversion rejected negotiated geometry (only exact 2x/4x reductions are supported)\n" :
					"UYVY->NVxx conversion rejected negotiated geometry\n");
				goto out;
			}
			memset(&out_buffer, 0, sizeof(out_buffer));
			out_buffer.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
			out_buffer.index = output_index;
			out_buffer.bytesused = dst_size;
			if (use_dmabuf) {
				struct dma_buf_sync sync = {
					.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW,
				};

				out_buffer.memory = V4L2_MEMORY_DMABUF;
				out_buffer.m.fd = encoder_out.bufs[output_index].dmabuf_fd;
				if (xioctl(out_buffer.m.fd, DMA_BUF_IOCTL_SYNC, &sync))
					goto out_errno;
			} else {
				out_buffer.memory = V4L2_MEMORY_MMAP;
			}
			if (xioctl(encoder_fd, VIDIOC_QBUF, &out_buffer))
				goto out_errno;
			output_queued[output_index] = 1;
			free_output--;
			if (queue_buffer(capture_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE,
					 source_index, 0))
				goto out_errno;
			held_capture = UINT32_MAX;
			frames++;
			progress = 1;
		}
		{
			uint64_t now = now_ms();

			if (now - last_stats_ms >= 5000) {
				uint64_t delta = now - last_stats_ms;
				uint64_t df = encoded_frames - last_stats_frames;

				fprintf(stderr, "stats: %.1f fps encoded (%" PRIu64
					" total, %.1f kB/s)\n",
					delta ? (double)df * 1000.0 / (double)delta : 0,
					encoded_frames,
					delta ? (double)encoded_bytes / 1024.0 *
					1000.0 / (double)(now - start_ms) : 0);
				last_stats_ms = now;
				last_stats_frames = encoded_frames;
			}
		}
		if (!progress) {
			struct pollfd fds[2] = {
				{ .fd = capture_fd, .events = POLLIN },
				{ .fd = encoder_fd, .events = POLLIN | POLLOUT },
			};
			if (poll(fds, 2, 1000) < 0 && errno != EINTR)
				goto out_errno;
		}
	}
	fprintf(stderr, "stopped after %" PRIu64 " submitted frames, %" PRIu64
		" encoded frames, %" PRIu64 " encoded bytes\n",
		frames, encoded_frames, encoded_bytes);
	ret = 0;
out_errno:
	if (ret)
		die_step();
out:
	if (capture_on)
		stream(capture_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, 0);
	if (encoder_out_on)
		stream(encoder_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT, 0);
	if (encoder_cap_on)
		stream(encoder_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, 0);
	unmap_queue(&encoder_cap);
	unmap_queue(&encoder_out);
	unmap_queue(&capture_queue);
	free(output_queued);
	rtsp_close(&rtsp);
	if (heap_fd >= 0)
		close(heap_fd);
	if (encoder_fd >= 0)
		close(encoder_fd);
	if (capture_fd >= 0)
		close(capture_fd);
	if (output_fd >= 0 && output_fd != STDOUT_FILENO)
		close(output_fd);
	return ret;
}

/* ------------------------------------------------------------------ */
/* Live bridge through the VPSS scaler/CSC (zero-copy)                 */
/* ------------------------------------------------------------------ */

/* capture (UYVY, pool buffers, exported) -> VPSS OUTPUT (dmabuf import)
 * -> hardware CSC/scale -> VPSS CAPTURE (shared CMA-heap dmabuf)
 * -> encoder OUTPUT (same dmabuf imported again) -> H.264.
 *
 * The CPU touches no frame data: per frame the bridge only moves dmabuf
 * fds between the three queues.  The VPSS CAPTURE format carries the
 * encoder's macroblock-padded geometry (e.g. 1920x1088) while the
 * CAPTURE-side crop selection marks the visible image (1920x1080), so
 * the scaler DMA lands exactly in the surface layout Coda980 expects.
 */

enum mid_state { MID_FREE, MID_AT_VPSS, MID_AT_ENCODER };

static int live_bridge_vpss(const struct bridge_options *opts)
{
	int capture_fd = -1, encoder_fd = -1, scaler_fd = -1, heap_fd = -1;
	int output_fd = -1;
	struct mapped_queue capture_queue = { 0 }, mid = { 0 }, encoder_cap = { 0 };
	int *cap_fds = NULL;
	uint8_t *mid_state = NULL;
	struct v4l2_pix_format capture_fmt, scaler_in_fmt, scaler_out_fmt;
	struct v4l2_pix_format encoder_out_fmt, encoder_cap_fmt;
	unsigned int i, free_mid = 0, held_capture = UINT32_MAX;
	unsigned int visible_width, visible_height, coded_height;
	uint64_t frames = 0, encoded_frames = 0, encoded_bytes = 0;
	uint64_t start_ms = 0, last_stats_ms = 0, last_stats_frames = 0;
	int capture_on = 0, encoder_out_on = 0, encoder_cap_on = 0;
	int scaler_out_on = 0, scaler_cap_on = 0, ret = -1;
	struct rtsp_sink rtsp;

	memset(&rtsp, 0, sizeof(rtsp));
	rtsp.fd = -1;
	if (opts->rtsp_url && rtsp_parse_url(&rtsp, opts->rtsp_url)) {
		fprintf(stderr, "bad rtsp url: %s\n", opts->rtsp_url);
		return -1;
	}
	rtsp.rtp_ssrc = 0x53324732; /* "S2G2" */

	capture_fd = open(opts->capture_path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (capture_fd < 0) {
		die_errno(opts->capture_path);
		goto out;
	}
	encoder_fd = open(opts->encoder_path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (encoder_fd < 0) {
		die_errno(opts->encoder_path);
		goto out;
	}
	scaler_fd = open(opts->scaler_path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (scaler_fd < 0) {
		die_errno(opts->scaler_path);
		goto out;
	}
	if (opts->output_path) {
		if (!strcmp(opts->output_path, "-"))
			output_fd = STDOUT_FILENO;
		else
			output_fd = open(opts->output_path,
					 O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
					 0644);
		if (output_fd < 0) {
			die_errno(opts->output_path);
			goto out;
		}
	}

	init_step = "capture G_FMT";
	if (get_format(capture_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, &capture_fmt))
		goto out_errno;
	if (capture_fmt.pixelformat != V4L2_PIX_FMT_UYVY ||
	    capture_fmt.width < 4 || capture_fmt.height < 2 ||
	    capture_fmt.bytesperline < capture_fmt.width * 2) {
		fprintf(stderr, "capture must provide packed UYVY with a valid stride\n");
		goto out;
	}
	visible_width = opts->half_scale ? capture_fmt.width / 2 : capture_fmt.width;
	visible_height = opts->half_scale ? capture_fmt.height / 2 : capture_fmt.height;
	coded_height = (visible_height + 15U) & ~15U;

	/* Encoder first: its padded OUTPUT geometry dictates the VPSS
	 * CAPTURE surface layout. */
	init_step = "encoder OUTPUT S_FMT";
	if (set_encoder_format(encoder_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT,
			       opts->encoder_input_format, visible_width,
			       coded_height, &encoder_out_fmt))
		goto out_errno;
	init_step = "encoder OUTPUT crop";
	if (set_output_crop(encoder_fd, visible_width, visible_height))
		goto out_errno;
	init_step = "encoder OUTPUT G_FMT";
	if (get_format(encoder_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT, &encoder_out_fmt))
		goto out_errno;
	init_step = "encoder CAPTURE S_FMT";
	if (set_encoder_format(encoder_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE,
			       V4L2_PIX_FMT_H264, visible_width,
			       visible_height, &encoder_cap_fmt))
		goto out_errno;
	if (encoder_cap_fmt.pixelformat != V4L2_PIX_FMT_H264)
		goto out;
	if (set_encoder_controls(encoder_fd, opts->bitrate, opts->gop))
		fprintf(stderr, "warning: encoder controls rejected, running firmware defaults\n");
	fprintf(stderr, "init: encoder formats ok\n");

	/* VPSS: OUTPUT = the capture frame, CAPTURE = the encoder surface. */
	init_step = "scaler OUTPUT S_FMT";
	if (set_encoder_format(scaler_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT,
			       V4L2_PIX_FMT_UYVY, capture_fmt.width,
			       capture_fmt.height, &scaler_in_fmt))
		goto out_errno;
	if (scaler_in_fmt.bytesperline != capture_fmt.bytesperline) {
		fprintf(stderr, "capture stride %u unsupported by scaler (wants %u)\n",
			capture_fmt.bytesperline, scaler_in_fmt.bytesperline);
		goto out;
	}
	init_step = "scaler CAPTURE S_FMT";
	if (set_encoder_format(scaler_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE,
			       opts->encoder_input_format, encoder_out_fmt.width,
			       encoder_out_fmt.height, &scaler_out_fmt))
		goto out_errno;
	if (scaler_out_fmt.bytesperline != encoder_out_fmt.bytesperline ||
	    scaler_out_fmt.sizeimage > encoder_out_fmt.sizeimage) {
		fprintf(stderr, "scaler/encoder surface mismatch (%u@%u vs %u@%u)\n",
			scaler_out_fmt.bytesperline, scaler_out_fmt.sizeimage,
			encoder_out_fmt.bytesperline, encoder_out_fmt.sizeimage);
		goto out;
	}
	init_step = "scaler CAPTURE crop";
	{
		struct v4l2_selection selection = {
			.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
			.target = V4L2_SEL_TGT_CROP,
			.r = {
				.width = visible_width,
				.height = visible_height,
			},
		};

		if (xioctl(scaler_fd, VIDIOC_S_SELECTION, &selection)) {
			if (errno == EINVAL &&
			    encoder_out_fmt.height == visible_height) {
				fprintf(stderr, "warning: scaler has no CAPTURE crop; unpadded surface fits anyway\n");
			} else if (errno == EINVAL) {
				fprintf(stderr, "scaler driver lacks CAPTURE crop, cannot emit the %ux%u surface\n",
					encoder_out_fmt.width, encoder_out_fmt.height);
				goto out;
			} else {
				goto out_errno;
			}
		}
	}

	/* Middle buffers: heap-allocated, imported by both the scaler
	 * CAPTURE queue and the encoder OUTPUT queue. */
	init_step = opts->mid_heap_reserved ? "open " DMA_HEAP_RESERVED :
		"open " DMA_HEAP_CMA;
	heap_fd = open(opts->mid_heap_reserved ? DMA_HEAP_RESERVED : DMA_HEAP_CMA,
		       O_RDONLY | O_CLOEXEC);
	if (heap_fd < 0 && !opts->mid_heap_reserved) {
		init_step = "open " DMA_HEAP_CMA_OLD;
		heap_fd = open(DMA_HEAP_CMA_OLD, O_RDONLY | O_CLOEXEC);
	}
	if (heap_fd < 0 && !opts->mid_heap_reserved) {
		init_step = "open " DMA_HEAP_RESERVED;
		heap_fd = open(DMA_HEAP_RESERVED, O_RDONLY | O_CLOEXEC);
	}
	if (heap_fd < 0) {
		init_step = "open " DMA_HEAP_SYSTEM;
		heap_fd = open(DMA_HEAP_SYSTEM, O_RDONLY | O_CLOEXEC);
	}
	if (heap_fd < 0)
		goto out_errno;
	init_step = "middle buffer allocation";
	{
		struct v4l2_requestbuffers request = {
			.count = opts->mid_buffers,
			.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
			.memory = V4L2_MEMORY_DMABUF,
		};

		if (xioctl(scaler_fd, VIDIOC_REQBUFS, &request) || !request.count)
			goto out_errno;
		mid.count = request.count;
		request.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
		if (xioctl(encoder_fd, VIDIOC_REQBUFS, &request) ||
		    request.count < mid.count)
			goto out_errno;
		mid.bufs = calloc(mid.count, sizeof(*mid.bufs));
		if (!mid.bufs)
			goto out_errno;
		for (i = 0; i < mid.count; i++) {
			struct dma_heap_allocation_data alloc = {
				.len = encoder_out_fmt.sizeimage,
				.fd_flags = O_CLOEXEC | O_RDWR,
			};

			if (xioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &alloc))
				goto out_errno;
			mid.bufs[i].dmabuf_fd = (int)alloc.fd;
			mid.bufs[i].length = encoder_out_fmt.sizeimage;
			mid.bufs[i].addr = mmap(NULL, encoder_out_fmt.sizeimage,
						PROT_READ | PROT_WRITE, MAP_SHARED,
						(int)alloc.fd, 0);
			if (mid.bufs[i].addr == MAP_FAILED) {
				mid.bufs[i].addr = NULL;
				goto out_errno;
			}
		}
	}
	mid_state = calloc(mid.count, sizeof(*mid_state));
	if (!mid_state)
		goto out_errno;
	fprintf(stderr, "init: scaler formats + %u middle buffers ok\n", mid.count);

	/* Encoder CAPTURE queue + priming frame (first claim on the media
	 * pool, and a valid reference picture for the first live frame). */
	init_step = "encoder CAPTURE buffers";
	if (map_queue(encoder_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE,
		      ENCODER_CAP_BUFFERS, &encoder_cap))
		goto out_errno;
	for (i = 0; i < encoder_cap.count; i++)
		if (queue_buffer(encoder_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, i, 0))
			goto out_errno;
	init_step = "encoder priming";
	{
		size_t luma_size = (size_t)encoder_out_fmt.bytesperline *
			encoder_out_fmt.height;
		size_t frame_size = luma_size * 3 / 2;
		struct dma_buf_sync sync = {
			.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW,
		};

		if (frame_size > UINT32_MAX || frame_size > mid.bufs[0].length)
			goto out;
		memset(mid.bufs[0].addr, 16, luma_size);
		memset((uint8_t *)mid.bufs[0].addr + luma_size, 128,
		       frame_size - luma_size);
		if (xioctl(mid.bufs[0].dmabuf_fd, DMA_BUF_IOCTL_SYNC, &sync))
			goto out_errno;
		if (queue_dmabuf(encoder_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT,
				 0, mid.bufs[0].dmabuf_fd, (unsigned int)frame_size))
			goto out_errno;
		mid_state[0] = MID_AT_ENCODER;
	}
	free_mid = mid.count - 1;
	fprintf(stderr, "init: encoder primed\n");

	init_step = "encoder STREAMON";
	if (stream(encoder_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, 1))
		goto out_errno;
	encoder_cap_on = 1;
	if (stream(encoder_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT, 1))
		goto out_errno;
	encoder_out_on = 1;

	/* Capture buffers: driver-owned (media pool), exported for the
	 * scaler OUTPUT queue. */
	init_step = "capture buffers";
	if (map_queue(capture_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE,
		      opts->capture_buffers, &capture_queue))
		goto out_errno;
	cap_fds = calloc(capture_queue.count, sizeof(*cap_fds));
	if (!cap_fds)
		goto out_errno;
	for (i = 0; i < capture_queue.count; i++) {
		struct v4l2_exportbuffer export = {
			.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
			.index = i,
			.flags = O_CLOEXEC | O_RDWR,
		};

		init_step = "capture EXPBUF (scaler must not be pool-bound)";
		if (xioctl(capture_fd, VIDIOC_EXPBUF, &export))
			goto out_errno;
		cap_fds[i] = export.fd;
	}

	for (i = 0; i < capture_queue.count; i++)
		if (queue_buffer(capture_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, i, 0))
			goto out_errno;
	/* Capture first: while the CSI driver streams, the VIP fabric
	 * clocks are on and VPSS register access is safe (the 6-clock DT
	 * leaves the fabric clocks to the capture driver; with them the
	 * order is merely hygienic). */
	init_step = "capture STREAMON";
	if (stream(capture_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, 1))
		goto out_errno;
	capture_on = 1;
	fprintf(stderr, "init: capture streaming\n");

	/* Scaler OUTPUT imports the capture buffers 1:1 by index. */
	init_step = "scaler OUTPUT REQBUFS";
	{
		struct v4l2_requestbuffers request = {
			.count = capture_queue.count,
			.type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
			.memory = V4L2_MEMORY_DMABUF,
		};

		if (xioctl(scaler_fd, VIDIOC_REQBUFS, &request) ||
		    request.count < capture_queue.count) {
			fprintf(stderr, "scaler OUTPUT queue too shallow (%u < %u)\n",
				request.count, capture_queue.count);
			goto out;
		}
	}
	if (stream(scaler_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT, 1))
		goto out_errno;
	scaler_out_on = 1;
	if (stream(scaler_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, 1))
		goto out_errno;
	scaler_cap_on = 1;

	fprintf(stderr, "bridge %s (%ux%u) -> %s (%s %ux%u crop %ux%u) -> %s %s io=vpss-dmabuf bitrate=%u gop=%u\n",
		opts->capture_path, capture_fmt.width, capture_fmt.height,
		opts->scaler_path,
		opts->encoder_input_format == V4L2_PIX_FMT_NV21 ? "NV21" : "NV12",
		scaler_out_fmt.width, scaler_out_fmt.height,
		visible_width, visible_height,
		opts->encoder_path, encoder_cap_fmt.pixelformat == V4L2_PIX_FMT_H264 ? "h264" : "?",
		opts->bitrate, opts->gop);
	start_ms = now_ms();
	last_stats_ms = start_ms;

	while (!stop_requested) {
		struct v4l2_buffer buffer;
		int progress = 0;

		/* Drain encoded CAPTURE buffers and forward to the sinks. */
		for (;;) {
			uint64_t pts_ms;

			if (dequeue_buffer(encoder_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE,
					   &buffer)) {
				if (errno == EAGAIN)
					break;
				goto out_errno;
			}
			if (buffer.index >= encoder_cap.count ||
			    buffer.bytesused > encoder_cap.bufs[buffer.index].length) {
				fprintf(stderr, "encoder returned an invalid capture buffer\n");
				goto out;
			}
			pts_ms = (uint64_t)buffer.timestamp.tv_sec * 1000ULL +
				(uint64_t)buffer.timestamp.tv_usec / 1000;
			if (!pts_ms)
				pts_ms = now_ms();
			if (output_fd >= 0 && buffer.bytesused &&
			    write_all(output_fd, encoder_cap.bufs[buffer.index].addr,
				      buffer.bytesused))
				goto out_errno;
			if (opts->rtsp_url && buffer.bytesused)
				rtsp_offer(&rtsp, encoder_cap.bufs[buffer.index].addr,
					   buffer.bytesused, pts_ms);
			encoded_frames++;
			encoded_bytes += buffer.bytesused;
			if (queue_buffer(encoder_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE,
					 buffer.index, 0))
				goto out_errno;
			progress = 1;
		}
		/* Encoder OUTPUT done -> middle buffer back to free. */
		for (;;) {
			if (dequeue_buffer_mem(encoder_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT,
					       V4L2_MEMORY_DMABUF, &buffer)) {
				if (errno == EAGAIN)
					break;
				goto out_errno;
			}
			if (buffer.index >= mid.count || mid_state[buffer.index] != MID_AT_ENCODER) {
				fprintf(stderr, "encoder returned an unexpected output buffer\n");
				goto out;
			}
			mid_state[buffer.index] = MID_FREE;
			free_mid++;
			progress = 1;
		}
		/* Scaler CAPTURE done -> filled middle buffer to the encoder. */
		for (;;) {
			if (dequeue_buffer_mem(scaler_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE,
					       V4L2_MEMORY_DMABUF, &buffer)) {
				if (errno == EAGAIN)
					break;
				goto out_errno;
			}
			if (buffer.index >= mid.count || mid_state[buffer.index] != MID_AT_VPSS) {
				fprintf(stderr, "scaler returned an unexpected capture buffer\n");
				goto out;
			}
			if (queue_dmabuf(encoder_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT,
					 buffer.index, mid.bufs[buffer.index].dmabuf_fd,
					 encoder_out_fmt.sizeimage))
				goto out_errno;
			mid_state[buffer.index] = MID_AT_ENCODER;
			progress = 1;
		}
		/* Scaler OUTPUT done -> capture buffer back to the CSI queue. */
		for (;;) {
			if (dequeue_buffer_mem(scaler_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT,
					       V4L2_MEMORY_DMABUF, &buffer)) {
				if (errno == EAGAIN)
					break;
				goto out_errno;
			}
			if (buffer.index >= capture_queue.count) {
				fprintf(stderr, "scaler returned an invalid output buffer\n");
				goto out;
			}
			if (queue_buffer(capture_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE,
					 buffer.index, 0))
				goto out_errno;
			progress = 1;
		}
		if (held_capture == UINT32_MAX) {
			if (!dequeue_buffer(capture_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE,
					    &buffer))
				held_capture = buffer.index;
			else if (errno != EAGAIN)
				goto out_errno;
		}
		if (held_capture != UINT32_MAX && free_mid) {
			unsigned int mid_index;

			for (mid_index = 0; mid_index < mid.count; mid_index++)
				if (mid_state[mid_index] == MID_FREE)
					break;
			if (mid_index == mid.count) {
				fprintf(stderr, "no usable free middle buffer\n");
				goto out;
			}
			if (queue_dmabuf(scaler_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT,
					 held_capture, cap_fds[held_capture],
					 capture_fmt.sizeimage))
				goto out_errno;
			if (queue_dmabuf(scaler_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE,
					 mid_index, mid.bufs[mid_index].dmabuf_fd, 0))
				goto out_errno;
			mid_state[mid_index] = MID_AT_VPSS;
			free_mid--;
			held_capture = UINT32_MAX;
			frames++;
			progress = 1;
		}
		{
			uint64_t now = now_ms();

			if (now - last_stats_ms >= 5000) {
				uint64_t delta = now - last_stats_ms;
				uint64_t df = encoded_frames - last_stats_frames;

				fprintf(stderr, "stats: %.1f fps encoded (%" PRIu64
					" total, %.1f kB/s)\n",
					delta ? (double)df * 1000.0 / (double)delta : 0,
					encoded_frames,
					delta ? (double)encoded_bytes / 1024.0 *
					1000.0 / (double)(now - start_ms) : 0);
				last_stats_ms = now;
				last_stats_frames = encoded_frames;
			}
		}
		if (!progress) {
			struct pollfd fds[3] = {
				{ .fd = capture_fd, .events = POLLIN },
				{ .fd = scaler_fd, .events = POLLIN | POLLOUT },
				{ .fd = encoder_fd, .events = POLLIN | POLLOUT },
			};
			if (poll(fds, 3, 1000) < 0 && errno != EINTR)
				goto out_errno;
		}
	}
	fprintf(stderr, "stopped after %" PRIu64 " scaled frames, %" PRIu64
		" encoded frames, %" PRIu64 " encoded bytes\n",
		frames, encoded_frames, encoded_bytes);
	ret = 0;
out_errno:
	if (ret)
		die_step();
out:
	if (scaler_out_on)
		stream(scaler_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT, 0);
	if (scaler_cap_on)
		stream(scaler_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, 0);
	if (capture_on)
		stream(capture_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, 0);
	if (encoder_out_on)
		stream(encoder_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT, 0);
	if (encoder_cap_on)
		stream(encoder_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, 0);
	unmap_queue(&encoder_cap);
	unmap_queue(&mid);
	unmap_queue(&capture_queue);
	if (cap_fds) {
		for (i = 0; i < capture_queue.count; i++)
			if (cap_fds[i] > 0)
				close(cap_fds[i]);
		free(cap_fds);
	}
	free(mid_state);
	rtsp_close(&rtsp);
	if (heap_fd >= 0)
		close(heap_fd);
	if (scaler_fd >= 0)
		close(scaler_fd);
	if (encoder_fd >= 0)
		close(encoder_fd);
	if (capture_fd >= 0)
		close(capture_fd);
	if (output_fd >= 0 && output_fd != STDOUT_FILENO)
		close(output_fd);
	return ret;
}

static void usage(const char *program)
{
	fprintf(stderr,
		"usage: %s [capture-node] [encoder-node] [options]\n"
		"       %s [capture-node] [encoder-node] [h264-output|-] [full|half]   (legacy)\n"
		"       %s --raw nv12|nv21|srggb12 width height input.raw output.raw\n"
		"\n"
		"options:\n"
		"  --output PATH|-      write the Annex-B stream (repeatable with --rtsp)\n"
		"  --rtsp URL           publish via RTSP (rtsp://host[:8554]/hdmi)\n"
		"  --size full|half     UYVY: half = 2x downscale (default full);\n"
		"                       SRGGB12P: default = 4x to 640x360, half = 2x\n"
		"  --format nv21|nv12   encoder input format (default nv21 staged; nv12\n"
		"                       needs a kernel with fixed direct input; SRGGB12P\n"
		"                       always uses NV12)\n"
		"  --io dmabuf|mmap     raw-frame buffers: cached dma-heap (default) or vb2 mmap\n"
		"  --scaler cpu|vpss    cpu = software UYVY->NVxx (default); vpss = hardware\n"
		"                       scaler/CSC via the mem2mem node, zero-copy dmabuf chain\n"
		"  --scaler-node PATH   VPSS mem2mem node (default " DEFAULT_SCALER ")\n"
		"  --mid-buffers N      vpss mode: shared scaler/encoder buffers (default 4)\n"
		"  --heap auto|reserved vpss mode: middle-buffer heap (default auto: CMA,\n"
		"                       then the reserved media pool, then system)\n"
		"  --capture-buffers N  CSI queue depth (default 4; 2 fits the camera's\n"
		"                       shared capture/Coda media-pool budget)\n"
		"  --bitrate N          encoder bitrate bit/s (default 4000000)\n"
		"  --gop N              encoder GOP size (default 30)\n",
		program, program, program);
}

int main(int argc, char **argv)
{
	struct bridge_options opts = {
		.capture_path = DEFAULT_CAPTURE,
		.encoder_path = DEFAULT_ENCODER,
		.scaler_path = DEFAULT_SCALER,
		.output_path = NULL,
		.rtsp_url = NULL,
		.bitrate = 4000000,
		.gop = 30,
		.mid_buffers = SCALER_MID_BUFFERS,
		.capture_buffers = CAPTURE_BUFFERS,
		.half_scale = 0,
		.use_dmabuf = 1,
		.use_vpss = 0,
		.encoder_input_format = V4L2_PIX_FMT_NV21,
	};
	struct sigaction action = { .sa_handler = on_signal };
	unsigned int width, height;
	int nv21;
	int i;
	int positional = 0;

	if (argc >= 2 && !strcmp(argv[1], "--raw")) {
		if (argc != 7 || (strcmp(argv[2], "nv12") && strcmp(argv[2], "nv21") &&
			   strcmp(argv[2], "srggb12")) ||
		    parse_u32(argv[3], &width) || parse_u32(argv[4], &height)) {
			usage(argv[0]);
			return EXIT_FAILURE;
		}
		if (!strcmp(argv[2], "srggb12"))
			return raw12_to_nv12_file(argv[5], argv[6], width, height) ?
				EXIT_FAILURE : EXIT_SUCCESS;
		nv21 = !strcmp(argv[2], "nv21");
		return uyvy_to_nvxx_file(argv[5], argv[6], width, height, nv21) ?
			EXIT_FAILURE : EXIT_SUCCESS;
	}
	for (i = 1; i < argc; i++) {
		const char *arg = argv[i];

		if (!strcmp(arg, "--output") && i + 1 < argc)
			opts.output_path = argv[++i];
		else if (!strcmp(arg, "--rtsp") && i + 1 < argc)
			opts.rtsp_url = argv[++i];
		else if (!strcmp(arg, "--size") && i + 1 < argc) {
			if (strcmp(argv[++i], "half") == 0)
				opts.half_scale = 1;
			else if (strcmp(argv[i], "full"))
				goto bad_usage;
		} else if (!strcmp(arg, "--format") && i + 1 < argc) {
			if (!strcmp(argv[++i], "nv12"))
				opts.encoder_input_format = V4L2_PIX_FMT_NV12;
			else if (strcmp(argv[i], "nv21"))
				goto bad_usage;
		} else if (!strcmp(arg, "--io") && i + 1 < argc) {
			if (!strcmp(argv[++i], "mmap"))
				opts.use_dmabuf = 0;
			else if (strcmp(argv[i], "dmabuf"))
				goto bad_usage;
		} else if (!strcmp(arg, "--scaler") && i + 1 < argc) {
			if (!strcmp(argv[++i], "vpss"))
				opts.use_vpss = 1;
			else if (strcmp(argv[i], "cpu"))
				goto bad_usage;
		} else if (!strcmp(arg, "--scaler-node") && i + 1 < argc) {
			opts.scaler_path = argv[++i];
		} else if (!strcmp(arg, "--mid-buffers") && i + 1 < argc) {
			if (parse_u32(argv[++i], &opts.mid_buffers) ||
			    opts.mid_buffers < 2 || opts.mid_buffers > 16)
				goto bad_usage;
		} else if (!strcmp(arg, "--heap") && i + 1 < argc) {
			if (!strcmp(argv[++i], "reserved"))
				opts.mid_heap_reserved = 1;
			else if (strcmp(argv[i], "auto"))
				goto bad_usage;
		} else if (!strcmp(arg, "--capture-buffers") && i + 1 < argc) {
			if (parse_u32(argv[++i], &opts.capture_buffers) ||
			    opts.capture_buffers < 2 || opts.capture_buffers > 16)
				goto bad_usage;
		} else if (!strcmp(arg, "--bitrate") && i + 1 < argc) {
			if (parse_u32(argv[++i], &opts.bitrate))
				goto bad_usage;
		} else if (!strcmp(arg, "--gop") && i + 1 < argc) {
			if (parse_u32(argv[++i], &opts.gop))
				goto bad_usage;
		} else if (arg[0] != '-' && positional == 0) {
			opts.capture_path = arg;
			positional++;
		} else if (arg[0] != '-' && positional == 1) {
			opts.encoder_path = arg;
			positional++;
		} else if (arg[0] != '-' && positional == 2) {
			opts.output_path = arg; /* legacy positional output */
			positional++;
		} else if (arg[0] != '-' && positional == 3 &&
			   (!strcmp(arg, "full") || !strcmp(arg, "half"))) {
			opts.half_scale = !strcmp(arg, "half");
			positional++;
		} else {
			goto bad_usage;
		}
	}
	if (!opts.output_path && !opts.rtsp_url) {
		fprintf(stderr, "nothing to do: pass --output and/or --rtsp\n");
		goto bad_usage;
	}
	if (sigemptyset(&action.sa_mask) || sigaction(SIGINT, &action, NULL) ||
	    sigaction(SIGTERM, &action, NULL))
		return EXIT_FAILURE;
	if (opts.use_vpss)
		return live_bridge_vpss(&opts) ? EXIT_FAILURE : EXIT_SUCCESS;
	return live_bridge(&opts) ? EXIT_FAILURE : EXIT_SUCCESS;

bad_usage:
	usage(argv[0]);
	return EXIT_FAILURE;
}
