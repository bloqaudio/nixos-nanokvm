/*
 * SG2002 V4L2 H.264 bridge:
 *
 *   /dev/video0 (UYVY capture) -> CPU UYVY->NV12/NV21 -> /dev/video1 (Coda)
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
#define DEFAULT_CAPTURE "/dev/video0"
#define DEFAULT_ENCODER "/dev/video1"
#define DMA_HEAP_SYSTEM "/dev/dma_heap/system"

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

	if (xioctl(fd, VIDIOC_REQBUFS, &request))
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
		struct v4l2_buffer buffer = {
			.type = type,
			.memory = V4L2_MEMORY_MMAP,
			.index = i,
		};
		if (xioctl(fd, VIDIOC_QUERYBUF, &buffer))
			return -1;
		queue->bufs[i].length = buffer.length;
		queue->bufs[i].dmabuf_fd = -1;
		queue->bufs[i].addr = mmap(NULL, buffer.length, PROT_READ | PROT_WRITE,
					  MAP_SHARED, fd, buffer.m.offset);
		if (queue->bufs[i].addr == MAP_FAILED) {
			queue->bufs[i].addr = NULL;
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

static int dequeue_buffer(int fd, enum v4l2_buf_type type,
			  struct v4l2_buffer *buffer)
{
	memset(buffer, 0, sizeof(*buffer));
	buffer->type = type;
	buffer->memory = V4L2_MEMORY_MMAP;
	return xioctl(fd, VIDIOC_DQBUF, buffer);
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
	const char *output_path;
	const char *rtsp_url;
	unsigned int bitrate;
	unsigned int gop;
	int half_scale;
	int use_dmabuf;
	uint32_t encoder_input_format; /* V4L2_PIX_FMT_NV21 or NV12 */
};

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
	int nv21 = opts->encoder_input_format == V4L2_PIX_FMT_NV21;
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
	if (capture_fmt.pixelformat != V4L2_PIX_FMT_UYVY ||
	    capture_fmt.width < 4 || capture_fmt.height < 2 ||
	    capture_fmt.bytesperline < capture_fmt.width * 2) {
		fprintf(stderr, "capture must provide packed UYVY with a valid stride\n");
		goto out;
	}
	if ((capture_fmt.width & 3) || (opts->half_scale && (capture_fmt.height & 3))) {
		fprintf(stderr, "capture width must be a multiple of 4 (half scale: height of 4)\n");
		goto out;
	}
	visible_width = opts->half_scale ? capture_fmt.width / 2 : capture_fmt.width;
	visible_height = opts->half_scale ? capture_fmt.height / 2 : capture_fmt.height;
	coded_height = (visible_height + 15U) & ~15U;
	/* Coda reads a macroblock surface even when the visible frame is 1080
	 * lines.  Negotiate that padded surface first, crop it to the source
	 * height, and only then configure CAPTURE.  Doing this in another order can
	 * leave the firmware with a 1920x1080 stride/height mismatch. */
	if (set_encoder_format(encoder_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT,
			       opts->encoder_input_format, visible_width,
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
		heap_fd = open(DMA_HEAP_SYSTEM, O_RDONLY | O_CLOEXEC);
		if (heap_fd < 0) {
			fprintf(stderr, "no " DMA_HEAP_SYSTEM " (%s), using --io mmap\n",
				strerror(errno));
			use_dmabuf = 0;
		}
	}
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
	if (map_queue(encoder_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE,
		      ENCODER_CAP_BUFFERS, &encoder_cap))
		goto out_errno;
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

			if (xioctl(encoder_out.bufs[0].dmabuf_fd,
				   DMA_BUF_IOCTL_SYNC, &sync))
				goto out_errno;
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
	if (stream(encoder_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, 1))
		goto out_errno;
	encoder_cap_on = 1;
	if (stream(encoder_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT, 1))
		goto out_errno;
	encoder_out_on = 1;
	if (map_queue(capture_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE,
		      CAPTURE_BUFFERS, &capture_queue))
		goto out_errno;
	for (i = 0; i < capture_queue.count; i++)
		if (queue_buffer(capture_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, i, 0))
			goto out_errno;
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
			if ((opts->half_scale ?
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
				fprintf(stderr, "UYVY->NVxx conversion rejected negotiated geometry\n");
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
	if (ret && errno)
		die_errno("live bridge");
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

static void usage(const char *program)
{
	fprintf(stderr,
		"usage: %s [capture-node] [encoder-node] [options]\n"
		"       %s [capture-node] [encoder-node] [h264-output|-] [full|half]   (legacy)\n"
		"       %s --raw nv12|nv21 width height input.uyvy output.raw\n"
		"\n"
		"options:\n"
		"  --output PATH|-      write the Annex-B stream (repeatable with --rtsp)\n"
		"  --rtsp URL           publish via RTSP (rtsp://host[:8554]/hdmi)\n"
		"  --size full|half     half = 2x box-filter downscale (default full)\n"
		"  --format nv21|nv12   encoder input format (default nv21 staged; nv12\n"
		"                       needs a kernel with fixed direct input)\n"
		"  --io dmabuf|mmap     raw-frame buffers: cached dma-heap (default) or vb2 mmap\n"
		"  --bitrate N          encoder bitrate bit/s (default 4000000)\n"
		"  --gop N              encoder GOP size (default 30)\n",
		program, program, program);
}

int main(int argc, char **argv)
{
	struct bridge_options opts = {
		.capture_path = DEFAULT_CAPTURE,
		.encoder_path = DEFAULT_ENCODER,
		.output_path = NULL,
		.rtsp_url = NULL,
		.bitrate = 4000000,
		.gop = 30,
		.half_scale = 0,
		.use_dmabuf = 1,
		.encoder_input_format = V4L2_PIX_FMT_NV21,
	};
	struct sigaction action = { .sa_handler = on_signal };
	unsigned int width, height;
	int nv21;
	int i;
	int positional = 0;

	if (argc >= 2 && !strcmp(argv[1], "--raw")) {
		if (argc != 7 || (strcmp(argv[2], "nv12") && strcmp(argv[2], "nv21")) ||
		    parse_u32(argv[3], &width) || parse_u32(argv[4], &height)) {
			usage(argv[0]);
			return EXIT_FAILURE;
		}
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
	return live_bridge(&opts) ? EXIT_FAILURE : EXIT_SUCCESS;

bad_usage:
	usage(argv[0]);
	return EXIT_FAILURE;
}
