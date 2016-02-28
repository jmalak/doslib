
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef O_BINARY
#define O_BINARY (0)
#endif

#pragma pack(push,1)
struct vrl_header {
	uint8_t			vrl_sig[4];		// +0x00  "VRL1"
	uint8_t			fmt_sig[4];		// +0x04  "VGAX"
	uint8_t			height;			// +0x08  Sprite height
	uint8_t			width;			// +0x09  Sprite width
	int8_t			hotspot_x;		// +0x0A  Hotspot offset (X) for programmer's reference
	int8_t			hotspot_y;		// +0x0B  Hotspot offset (Y) for programmer's reference
};							// =0x0C
#pragma pack(pop)

#pragma pack(push,1)
struct pcx_header {
	uint8_t			manufacturer;		// +0x00  always 0x0A
	uint8_t			version;		// +0x01  0, 2, 3, or 5
	uint8_t			encoding;		// +0x02  always 0x01 for RLE
	uint8_t			bitsPerPlane;		// +0x03  bits per pixel in each color plane (1, 2, 4, 8, 24)
	uint16_t		Xmin,Ymin,Xmax,Ymax;	// +0x04  window (image dimensions). Pixel count in each dimension is Xmin <= x <= Xmax, Ymin <= y <= Ymax i.e. INCLUSIVE
	uint16_t		VertDPI,HorzDPI;	// +0x0C  vertical/horizontal resolution in DPI
	uint8_t			palette[48];		// +0x10  16-color or less color palette
	uint8_t			reserved;		// +0x40  reserved, set to zero
	uint8_t			colorPlanes;		// +0x41  number of color planes
	uint16_t		bytesPerPlaneLine;	// +0x42  number of bytes to read for a single plane's scanline (uncompressed, apparently)
	uint16_t		palType;		// +0x44  palette type (1 = color)
	uint16_t		hScrSize,vScrSize;	// +0x46  scrolling?
	uint8_t			pad[54];		// +0x4A  padding
};							// =0x80
#pragma pack(pop)

static unsigned char		transparent_color = 0;

static unsigned char*		src_pcx = NULL;
static unsigned char		*src_pcx_start,*src_pcx_end;
static unsigned int		src_pcx_size = 0;
static unsigned int		src_pcx_stride = 0;
static unsigned int		src_pcx_width = 0;
static unsigned int		src_pcx_height = 0;

static unsigned char		out_strip[(256*3)+16];
static unsigned int		out_strip_height = 0;
static unsigned int		out_strips = 0;

static void help() {
	fprintf(stderr,"PCX2VRL VGA Mode X sprite compiler (C) 2016 Jonathan Campbell\n");
	fprintf(stderr,"PCX file must be 256-color format with VGA palette.\n");
	fprintf(stderr,"\n");
	fprintf(stderr,"pcx2vrl [options]\n");
	fprintf(stderr,"  -o <filename>                Write VRL sprite to file\n");
	fprintf(stderr,"  -i <filename>                Read image from PCX file\n");
	fprintf(stderr,"  -tc <index>                  Specify transparency color\n");
	fprintf(stderr,"  -p <filename>                Write PCX palette to file\n");
}

int main(int argc,char **argv) {
	const char *src_file = NULL,*dst_file = NULL,*pal_file = NULL;
	unsigned int x,y,runcount,skipcount;
	unsigned char *s,*d,*dfence;
	const char *a;
	int i,fd;

	for (i=1;i < argc;) {
		a = argv[i++];
		if (*a == '-') {
			do { a++; } while (*a == '-');

			if (!strcmp(a,"h") || !strcmp(a,"help")) {
				help();
				return 1;
			}
			else if (!strcmp(a,"i")) {
				src_file = argv[i++];
			}
			else if (!strcmp(a,"o")) {
				dst_file = argv[i++];
			}
			else if (!strcmp(a,"p")) {
				pal_file = argv[i++];
			}
			else if (!strcmp(a,"tc")) {
				transparent_color = (unsigned char)strtoul(argv[i++],NULL,0);
			}
			else {
				fprintf(stderr,"Unknown switch '%s'. Use --help\n",a);
				return 1;
			}
		}
		else {
			fprintf(stderr,"Unknown param %s\n",a);
			return 1;
		}
	}

	if (src_file == NULL || dst_file == NULL) {
		help();
		return 1;
	}

	/* load the PCX into memory */
	/* WARNING: 16-bit DOS builds of this code cannot load a PCX that decodes to more than 64K - 800 bytes */
	fd = open(src_file,O_RDONLY|O_BINARY);
	if (fd < 0) {
		fprintf(stderr,"Cannot open source file '%s', %s\n",src_file,strerror(errno));
		return 1;
	}
	{
		unsigned long sz = lseek(fd,0,SEEK_END);
		if (sz < (128+769)) {
			fprintf(stderr,"File is too small to be PCX\n");
			return 1;
		}
		if (sizeof(unsigned int) == 2 && sz > 65530UL) {
			fprintf(stderr,"File is too large to load into memory\n");
			return 1;
		}

		src_pcx_size = (unsigned int)sz;
		src_pcx = malloc(src_pcx_size);
		if (src_pcx == NULL) {
			fprintf(stderr,"Cannot malloc for source PCX\n");
			return 1;
		}
	}
	lseek(fd,0,SEEK_SET);
	if ((unsigned int)read(fd,src_pcx,src_pcx_size) != src_pcx_size) {
		fprintf(stderr,"Cannot read PCX\n");
		return 1;
	}
	close(fd);
	src_pcx_start = src_pcx + 128;
	src_pcx_end = src_pcx + src_pcx_size;

	/* parse header */
	{
		struct pcx_header *hdr = (struct pcx_header*)src_pcx;

		if (hdr->manufacturer != 0xA || hdr->encoding != 1 || hdr->bitsPerPlane != 8 ||
			hdr->colorPlanes != 1 || hdr->Xmin >= hdr->Xmax || hdr->Ymin >= hdr->Ymax) {
			fprintf(stderr,"PCX format not supported\n");
			return 1;
		}
		src_pcx_stride = hdr->bytesPerPlaneLine;
		src_pcx_width = hdr->Xmax + 1 - hdr->Xmin;
		src_pcx_height = hdr->Ymax + 1 - hdr->Ymin;
		if (src_pcx_width >= 512 || src_pcx_height >= 256) {
			fprintf(stderr,"PCX too big\n");
			return 1;
		}
		if (src_pcx_stride < src_pcx_width) {
			fprintf(stderr,"PCX stride < width\n");
			return 1;
		}

		out_strip_height = src_pcx_height;
		out_strips = src_pcx_width;
	}

	/* identify and load palette */
	if (src_pcx_size > 769) {
		s = src_pcx + src_pcx_size - 769;
		if (*s == 0x0C) {
			src_pcx_end = s++;
			if (pal_file != NULL) {
				fd = open(pal_file,O_WRONLY|O_CREAT|O_TRUNC,0644);
				if (fd < 0) {
					fprintf(stderr,"Cannot create file '%s', %s\n",pal_file,strerror(errno));
					return 1;
				}
				write(fd,s,768);
				close(fd);
			}
		}
	}

	/* decode the PCX */
	{
		unsigned char b,run;
		unsigned char *tmp = malloc(src_pcx_stride * src_pcx_height);
		if (tmp == NULL) {
			fprintf(stderr,"Cannot allocate decode buffer\n");
			return 1;
		}

		d = tmp;
		dfence = tmp + (src_pcx_stride * src_pcx_height);
		s = src_pcx_start;
		while (s < src_pcx_end && d < dfence) {
			b = *s++;
			if ((b & 0xC0) == 0xC0) {
				run = b & 0x3F;
				if (s >= src_pcx_end) break;
				b = *s++;
				while (run > 0) {
					*d++ = b;
					run--;
					if (d >= dfence) break;
				}
			}
			else {
				*d++ = b;
			}
		}

		/* discard source PCX data */
		free(src_pcx);
		src_pcx = tmp;
	}

	/* write out */
	fd = open(dst_file,O_WRONLY|O_CREAT|O_TRUNC,0644);
	if (fd < 0) {
		fprintf(stderr,"Cannot create file '%s', %s\n",dst_file,strerror(errno));
		return 1;
	}
	{
		struct vrl_header hdr;
		memset(&hdr,0,sizeof(hdr));
		memcpy(hdr.vrl_sig,"VRL1",4); // Vertical Run Length v1
		memcpy(hdr.fmt_sig,"VGAX",4); // VGA mode X
		hdr.height = out_strip_height;
		hdr.width = out_strips;
		write(fd,&hdr,sizeof(hdr));

		for (x=0;x < out_strips;x++) {
			y = 0;
			d = out_strip;
			s = src_pcx + x;
			dfence = out_strip + sizeof(out_strip);
			while (y < out_strip_height) {
				unsigned char *stripstart = d;
				unsigned char color_run = 0;

				d += 2; // patch bytes later
				runcount = 0;
				skipcount = 0;
				while (y < out_strip_height && *s == transparent_color) {
					y++;
					s += src_pcx_stride;
					if ((++skipcount) == 255) break;
				}

				// check: can we do a run length of one color?
				if (y < out_strip_height && *s != transparent_color) {
					unsigned char first_color = *s;
					unsigned char *scan_s = s;
					unsigned int scan_y = y;

					scan_s += src_pcx_stride;
					color_run = 1;
					scan_y++;

					while (scan_y < out_strip_height) {
						if (*scan_s != first_color) {
							if (color_run < 8) color_run = 0;
							break;
						}
						scan_y++;
						scan_s += src_pcx_stride;
						if ((++color_run) == 127) break;
					}

					if (color_run == 0) {
						unsigned char ppixel,same_count = 0;

						scan_s = s;
						scan_y = y;
						while (scan_y < out_strip_height && *scan_s != transparent_color) {
							if (*scan_s == ppixel) {
								if (same_count >= 8) {
									d -= same_count;
									scan_y -= same_count;
									scan_s -= same_count * src_pcx_stride;
									runcount -= same_count;
									break;
								}
								same_count++;
							}
							else {
								same_count=0;
							}

							scan_y++;
							*d++ = ppixel = *scan_s;
							scan_s += src_pcx_stride;
							if ((++runcount) == 127) break;
						}
					}
					else {
						runcount = color_run;
					}

					y = scan_y;
					s = scan_s;
				}

				if (runcount == 0 && skipcount == 0) {
					d = stripstart;
				}
				// overwrite the first byte with run + skip count
				else if (color_run != 0 && runcount > 3) {
					stripstart[0] = runcount + 0x80; // it's a run of one color
					d = stripstart + 3; // it becomes <runcount+0x80> <skipcount> <color to repeat>
				}
				else {
					stripstart[0] = runcount; // <runcount> <skipcount> [run]
				}
				stripstart[1] = skipcount;
			}

			// final byte
			*d++ = 0x00;
			*d++ = 0x00;
			assert(d <= dfence);
			write(fd,out_strip,(int)(d - out_strip));
		}
	}
	close(fd);
	return 0;
}

