
#include <stdio.h>
#include <conio.h> /* this is where Open Watcom hides the outp() etc. functions */
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <malloc.h>
#include <direct.h>
#include <ctype.h>
#include <fcntl.h>
#include <dos.h>

#include <hw/dos/dos.h>
#include <hw/8237/8237.h>		/* 8237 DMA */
#include <hw/8254/8254.h>		/* 8254 timer */
#include <hw/8259/8259.h>		/* 8259 PIC interrupts */
#include <hw/sndsb/sndsb.h>
#include <hw/dos/doswin.h>
#include <hw/dos/tgusmega.h>
#include <hw/dos/tgussbos.h>
#include <hw/dos/tgusumid.h>
#include <hw/isapnp/isapnp.h>
#include <hw/sndsb/sndsbpnp.h>

static struct dma_8237_allocation *sb_dma = NULL; /* DMA buffer */

static struct sndsb_ctx*	sb_card = NULL;

/*============================TODO: move to library=============================*/
static int vector_is_iret(const unsigned char vector) {
	const unsigned char far *p;
	uint32_t rvector;

#if TARGET_MSDOS == 32
	rvector = ((uint32_t*)0)[vector];
	if (rvector == 0) return 0;
	p = (const unsigned char*)(((rvector >> 16UL) << 4UL) + (rvector & 0xFFFFUL));
#else
	rvector = *((uint32_t far*)MK_FP(0,(vector*4)));
	if (rvector == 0) return 0;
	p = (const unsigned char far*)MK_FP(rvector>>16UL,rvector&0xFFFFUL);
#endif

	if (*p == 0xCF) {
		// IRET. Yep.
		return 1;
	}
	else if (p[0] == 0xFE && p[1] == 0x38) {
		// DOSBox callback. Probably not going to ACK the interrupt.
		return 1;
	}

	return 0;
}
/*==============================================================================*/

static int			wav_fd = -1;
static char			wav_file[130] = {0};
static unsigned char		wav_stereo = 0,wav_16bit = 0,wav_bytes_per_sample = 1;
static unsigned long		wav_data_offset = 44,wav_data_length = 0,wav_sample_rate = 8000,wav_position = 0,wav_buffer_filepos = 0;
static unsigned char		wav_playing = 0;

static void stop_play();

/* WARNING!!! This interrupt handler calls subroutines. To avoid system
 * instability in the event the IRQ fires while, say, executing a routine
 * in the DOS kernel, you must compile this code with the -zu flag in
 * 16-bit real mode Large and Compact memory models! Without -zu, minor
 * memory corruption in the DOS kernel will result and the system will
 * hang and/or crash. */
unsigned char old_irq_masked = 0;
static void (interrupt *old_irq)() = NULL;
static void interrupt sb_irq() {
	unsigned char c;

	sb_card->irq_counter++;

	/* ack soundblaster DSP if DSP was the cause of the interrupt */
	/* NTS: Experience says if you ack the wrong event on DSP 4.xx it
	   will just re-fire the IRQ until you ack it correctly...
	   or until your program crashes from stack overflow, whichever
	   comes first */
	c = sndsb_interrupt_reason(sb_card);
	sndsb_interrupt_ack(sb_card,c);

	/* FIXME: The sndsb library should NOT do anything in
	   send_buffer_again() if it knows playback has not started! */
	/* for non-auto-init modes, start another buffer */
	if (wav_playing) sndsb_irq_continue(sb_card,c);

	/* NTS: we assume that if the IRQ was masked when we took it, that we must not
	 *      chain to the previous IRQ handler. This is very important considering
	 *      that on most DOS systems an IRQ is masked for a very good reason---the
	 *      interrupt handler doesn't exist! In fact, the IRQ vector could easily
	 *      be unitialized or 0000:0000 for it! CALLing to that address is obviously
	 *      not advised! */
	if (old_irq_masked || old_irq == NULL) {
		/* ack the interrupt ourself, do not chain */
		if (sb_card->irq >= 8) p8259_OCW2(8,P8259_OCW2_NON_SPECIFIC_EOI);
		p8259_OCW2(0,P8259_OCW2_NON_SPECIFIC_EOI);
	}
	else {
		/* chain to the previous IRQ, who will acknowledge the interrupt */
		old_irq();
	}
}

static void load_audio(struct sndsb_ctx *cx,uint32_t up_to,uint32_t min,uint32_t max,uint8_t initial) { /* load audio up to point or max */
	unsigned char FAR *buffer = sb_dma->lin;
	int rd,i,bufe=0;
	uint32_t how;

	/* caller should be rounding! */
	assert((up_to & 3UL) == 0UL);
	if (up_to >= cx->buffer_size) return;
	if (cx->buffer_size < 32) return;
	if (cx->buffer_last_io == up_to) return;

	if (sb_card->dsp_adpcm > 0 && (wav_16bit || wav_stereo)) return;
	if (max == 0) max = cx->buffer_size/4;
	if (max < 16) return;
	lseek(wav_fd,wav_data_offset + (wav_position * (unsigned long)wav_bytes_per_sample),SEEK_SET);

	if (cx->buffer_last_io == 0)
		wav_buffer_filepos = wav_position;

	while (max > 0UL) {
		if (cx->backwards) {
			if (up_to > cx->buffer_last_io) {
				how = cx->buffer_last_io;
				if (how == 0) how = cx->buffer_size - up_to;
				bufe = 1;
			}
			else {
				how = (cx->buffer_last_io - up_to);
				bufe = 0;
			}
		}
		else {
			if (up_to < cx->buffer_last_io) {
				how = (cx->buffer_size - cx->buffer_last_io); /* from last IO to end of buffer */
				bufe = 1;
			}
			else {
				how = (up_to - cx->buffer_last_io); /* from last IO to up_to */
				bufe = 0;
			}
		}

		if (how > 16384UL)
			how = 16384UL;

		if (how == 0UL)
			break;
		else if (how > max)
			how = max;
		else if (!bufe && how < min)
			break;

		if (cx->buffer_last_io == 0)
			wav_buffer_filepos = wav_position;

        {
            uint32_t oa,adj;

			oa = cx->buffer_last_io;
			if (cx->backwards) {
				if (cx->buffer_last_io == 0) {
					cx->buffer_last_io = cx->buffer_size - how;
				}
				else if (cx->buffer_last_io >= how) {
					cx->buffer_last_io -= how;
				}
				else {
					abort();
				}

				adj = (uint32_t)how / wav_bytes_per_sample;
				if (wav_position >= adj) wav_position -= adj;
				else if (wav_position != 0UL) wav_position = 0;
				else {
					wav_position = lseek(wav_fd,0,SEEK_END);
					if (wav_position >= adj) wav_position -= adj;
					else if (wav_position != 0UL) wav_position = 0;
					wav_position /= wav_bytes_per_sample;
				}

				lseek(wav_fd,wav_data_offset + (wav_position * (unsigned long)wav_bytes_per_sample),SEEK_SET);
			}

			assert(cx->buffer_last_io <= cx->buffer_size);
#if TARGET_MSDOS == 32
			rd = _dos_xread(wav_fd,buffer + cx->buffer_last_io,how);
#else
            {
                uint32_t o;

                o  = (uint32_t)FP_SEG(buffer) << 4UL;
                o += (uint32_t)FP_OFF(buffer);
                o += cx->buffer_last_io;
                rd = _dos_xread(wav_fd,MK_FP(o >> 4UL,o & 0xFUL),how);
            }
#endif
			if (rd == 0 || rd == -1) {
				if (!cx->backwards) {
					wav_position = 0;
					lseek(wav_fd,wav_data_offset + (wav_position * (unsigned long)wav_bytes_per_sample),SEEK_SET);
					rd = _dos_xread(wav_fd,buffer + cx->buffer_last_io,how);
					if (rd == 0 || rd == -1) {
						/* hmph, fine */
#if TARGET_MSDOS == 32
						memset(buffer+cx->buffer_last_io,128,how);
#else
						_fmemset(buffer+cx->buffer_last_io,128,how);
#endif
						rd = (int)how;
					}
				}
				else {
					rd = (int)how;
				}
			}

			assert((cx->buffer_last_io+((uint32_t)rd)) <= cx->buffer_size);
			if (sb_card->audio_data_flipped_sign) {
				if (wav_16bit)
					for (i=0;i < (rd-1);i += 2) buffer[cx->buffer_last_io+i+1] ^= 0x80;
				else
					for (i=0;i < rd;i++) buffer[cx->buffer_last_io+i] ^= 0x80;
			}

			if (!cx->backwards) {
				cx->buffer_last_io += (uint32_t)rd;
				wav_position += (uint32_t)rd / wav_bytes_per_sample;
			}
		}

		assert(cx->buffer_last_io <= cx->buffer_size);
		if (!cx->backwards) {
			if (cx->buffer_last_io == cx->buffer_size) cx->buffer_last_io = 0;
		}
		max -= (uint32_t)rd;
	}

	if (cx->buffer_last_io == 0)
		wav_buffer_filepos = wav_position;
}

#define DMA_WRAP_DEBUG

static void wav_idle() {
	const unsigned int leeway = sb_card->buffer_size / 100;
	uint32_t pos;
#ifdef DMA_WRAP_DEBUG
	uint32_t pos2;
#endif

	if (!wav_playing || wav_fd < 0)
		return;

	/* if we're playing without an IRQ handler, then we'll want this function
	 * to poll the sound card's IRQ status and handle it directly so playback
	 * continues to work. if we don't, playback will halt on actual Creative
	 * Sound Blaster 16 hardware until it gets the I/O read to ack the IRQ */
	sndsb_main_idle(sb_card);

	_cli();
#ifdef DMA_WRAP_DEBUG
	pos2 = sndsb_read_dma_buffer_position(sb_card);
#endif
	pos = sndsb_read_dma_buffer_position(sb_card);
#ifdef DMA_WRAP_DEBUG
	if (sb_card->backwards) {
		if (pos2 < 0x1000 && pos >= (sb_card->buffer_size-0x1000)) {
			/* normal DMA wrap-around, no problem */
		}
		else {
			if (pos > pos2)	fprintf(stderr,"DMA glitch! 0x%04lx 0x%04lx\n",pos,pos2);
			else		pos = max(pos,pos2);
		}

		pos += leeway;
		if (pos >= sb_card->buffer_size) pos -= sb_card->buffer_size;
	}
	else {
		if (pos < 0x1000 && pos2 >= (sb_card->buffer_size-0x1000)) {
			/* normal DMA wrap-around, no problem */
		}
		else {
			if (pos < pos2)	fprintf(stderr,"DMA glitch! 0x%04lx 0x%04lx\n",pos,pos2);
			else		pos = min(pos,pos2);
		}

		if (pos < leeway) pos += sb_card->buffer_size - leeway;
		else pos -= leeway;
	}
#endif
	pos &= (~3UL); /* round down */
	_sti();

    /* load from disk */
    load_audio(sb_card,pos,min(wav_sample_rate/8,4096)/*min*/,
        sb_card->buffer_size/4/*max*/,0/*first block*/);
}

static void update_cfg();

static void close_wav() {
	if (wav_fd >= 0) {
		close(wav_fd);
		wav_fd = -1;
	}
}

static void open_wav() {
	char tmp[64];

	if (wav_fd < 0) {
        uint32_t riff_length,scan,len;

	    wav_position = 0;
		wav_data_offset = 0;
		wav_data_length = 0;
        wav_sample_rate = 0;
        wav_bytes_per_sample = 0;
		if (strlen(wav_file) < 1) return;

		wav_fd = open(wav_file,O_RDONLY|O_BINARY);
		if (wav_fd < 0) return;

        if (lseek(wav_fd,0,SEEK_SET) != 0) goto fail;

        /* first, the RIFF:WAVE chunk */
        /* 3 DWORDS: 'RIFF' <length> 'WAVE' */
        if (read(wav_fd,tmp,12) != 12) goto fail;
        if (memcmp(tmp+0,"RIFF",4) || memcmp(tmp+8,"WAVE",4)) goto fail;

        scan = 12;
        riff_length = *((uint32_t*)(tmp+4));
        if (riff_length <= 44) goto fail;
        riff_length -= 4; /* the length includes the 'WAVE' marker */

        while ((scan+8UL) <= riff_length) {
            /* RIFF chunks */
            /* 2 WORDS: <fourcc> <length> */
            if (lseek(wav_fd,scan,SEEK_SET) != scan) goto fail;
            if (read(wav_fd,tmp,8) != 8) goto fail;
            len = *((uint32_t*)(tmp+4));

            /* process! */
            if (!memcmp(tmp,"fmt ",4)) {
                if (len >= 16 && len <= sizeof(tmp)) {
                    if (read(wav_fd,tmp,len) == len) {
#if 0
                        typedef struct tWAVEFORMATEX {
                            WORD  wFormatTag;               /* +0 */
                            WORD  nChannels;                /* +2 */
                            DWORD nSamplesPerSec;           /* +4 */
                            DWORD nAvgBytesPerSec;          /* +8 */
                            WORD  nBlockAlign;              /* +12 */
                            WORD  wBitsPerSample;           /* +14 */
                        } WAVEFORMATEX;
#endif
                        wav_sample_rate = *((uint32_t*)(tmp + 4));
                        wav_stereo = *((uint16_t*)(tmp + 2)) > 1;
                        wav_16bit = *((uint16_t*)(tmp + 14)) > 8;
                        wav_bytes_per_sample = (wav_stereo ? 2 : 1) * (wav_16bit ? 2 : 1);
                    }
                }
            }
            else if (!memcmp(tmp,"data",4)) {
                wav_data_offset = scan + 8UL;
                wav_data_length = len;
            }

            /* next! */
            scan += len + 8UL;
        }

        if (wav_sample_rate == 0UL || wav_data_length == 0UL) goto fail;
	}

	update_cfg();
    return;
fail:
    /* assume wav_fd >= 0 */
    close(wav_fd);
    wav_fd = -1;
}

static void free_dma_buffer() {
    if (sb_dma != NULL) {
        dma_8237_free_buffer(sb_dma);
        sb_dma = NULL;
    }
}

static void realloc_dma_buffer() {
    uint32_t choice;
    int8_t ch;

    free_dma_buffer();

    ch = sndsb_dsp_playback_will_use_dma_channel(sb_card,wav_sample_rate,wav_stereo,wav_16bit);

    if (ch >= 4)
        choice = sndsb_recommended_16bit_dma_buffer_size(sb_card,0);
    else
        choice = sndsb_recommended_dma_buffer_size(sb_card,0);

    do {
        if (ch >= 4)
            sb_dma = dma_8237_alloc_buffer_dw(choice,16);
        else
            sb_dma = dma_8237_alloc_buffer_dw(choice,8);

        if (sb_dma == NULL) choice -= 4096UL;
    } while (sb_dma == NULL && choice > 4096UL);

    if (!sndsb_assign_dma_buffer(sb_card,sb_dma))
        return;
    if (sb_dma == NULL)
        return;
}

static void begin_play() {
	unsigned long choice_rate;

	if (wav_playing)
		return;

	if (sb_card->dsp_play_method == SNDSB_DSPOUTMETHOD_DIRECT)
        return;

    if (sb_dma == NULL)
        realloc_dma_buffer();

    {
        int8_t ch = sndsb_dsp_playback_will_use_dma_channel(sb_card,wav_sample_rate,wav_stereo,wav_16bit);
        if (ch >= 0) {
            if (sb_dma->dma_width != (ch >= 4 ? 16 : 8))
                realloc_dma_buffer();
            if (sb_dma == NULL)
                return;
        }
    }

    if (sb_dma != NULL) {
        if (!sndsb_assign_dma_buffer(sb_card,sb_dma))
            return;
    }

	if (wav_fd < 0)
		return;

	choice_rate = wav_sample_rate;

	update_cfg();
	if (!sndsb_prepare_dsp_playback(sb_card,choice_rate,wav_stereo,wav_16bit))
		return;

	sndsb_setup_dma(sb_card);

    load_audio(sb_card,sb_card->buffer_size/2,0/*min*/,0/*max*/,1/*first block*/);

	/* make sure the IRQ is acked */
	if (sb_card->irq >= 8) {
		p8259_OCW2(8,P8259_OCW2_SPECIFIC_EOI | (sb_card->irq & 7));
		p8259_OCW2(0,P8259_OCW2_SPECIFIC_EOI | 2);
	}
	else if (sb_card->irq >= 0) {
		p8259_OCW2(0,P8259_OCW2_SPECIFIC_EOI | sb_card->irq);
	}
	if (sb_card->irq >= 0)
		p8259_unmask(sb_card->irq);

	if (!sndsb_begin_dsp_playback(sb_card))
		return;

	_cli();
	wav_playing = 1;
	_sti();
}

static void stop_play() {
	if (!wav_playing) return;

	_cli();
	sndsb_stop_dsp_playback(sb_card);
	wav_playing = 0;
	_sti();
}

static void update_cfg() {
    sb_card->dsp_adpcm = 0;
    sb_card->buffer_irq_interval = sb_card->buffer_size / wav_bytes_per_sample;
}

static void help() {
    printf("dosamp [options] <file>\n");
    printf(" /h /help             This help\n");
}

static int parse_argv(int argc,char **argv) {
    int i;

	for (i=1;i < argc;) {
		char *a = argv[i++];

		if (*a == '-' || *a == '/') {
			unsigned char m = *a++;
			while (*a == m) a++;

			if (!strcmp(a,"h") || !strcmp(a,"help")) {
				help();
				return 0;
			}
			else {
                return 0;
            }
        }
        else {
            size_t l = strlen(a);
            if (l >= sizeof(wav_file)) return 0;
            strcpy(wav_file,a);
        }
	}

    if (wav_file[0] == 0) {
        printf("You must specify a file to play\n");
        return 0;
    }

    return 1;
}

int main(int argc,char **argv) {
	int i,loop,redraw,bkgndredraw;

    if (!parse_argv(argc,argv))
        return 1;

	probe_8237();
	if (!probe_8259()) {
		printf("Cannot init 8259 PIC\n");
		return 1;
	}
	if (!probe_8254()) {
		printf("Cannot init 8254 timer\n");
		return 1;
	}
	if (!init_sndsb()) {
		printf("Cannot init library\n");
		return 1;
	}
	if (!init_isa_pnp_bios()) {
		printf("Cannot init ISA PnP\n");
		return 1;
	}

    /* we want to know if certain emulation TSRs exist */
    gravis_mega_em_detect(&megaem_info);
    gravis_sbos_detect();

	/* it's up to us now to tell it certain minor things */
	sndsb_detect_virtualbox();		// whether or not we're running in VirtualBox
	/* sndsb now allows us to keep the EXE small by not referring to extra sound card support */
	sndsb_enable_sb16_support();		// SB16 support
	sndsb_enable_sc400_support();		// SC400 support
	sndsb_enable_ess_audiodrive_support();	// ESS AudioDrive support

    /* Plug & Play scan */
    if (find_isa_pnp_bios()) {
        const unsigned int devnode_raw_sz = 4096U;
        unsigned char *devnode_raw = malloc(devnode_raw_sz);

        if (devnode_raw != NULL) {
            unsigned char csn,node=0,numnodes=0xFF,data[192];
            unsigned int j,nodesize=0;
            const char *whatis = NULL;

            memset(data,0,sizeof(data));
            if (isa_pnp_bios_get_pnp_isa_cfg(data) == 0) {
                struct isapnp_pnp_isa_cfg *nfo = (struct isapnp_pnp_isa_cfg*)data;
                isapnp_probe_next_csn = nfo->total_csn;
                isapnp_read_data = nfo->isa_pnp_port;
            }

            /* enumerate device nodes reported by the BIOS */
            if (isa_pnp_bios_number_of_sysdev_nodes(&numnodes,&nodesize) == 0 && numnodes != 0xFF && nodesize <= devnode_raw_sz) {
                for (node=0;node != 0xFF;) {
                    struct isa_pnp_device_node far *devn;
                    unsigned char this_node;

                    /* apparently, start with 0. call updates node to
                     * next node number, or 0xFF to signify end */
                    this_node = node;
                    if (isa_pnp_bios_get_sysdev_node(&node,devnode_raw,ISA_PNP_BIOS_GET_SYSDEV_NODE_CTRL_NOW) != 0) break;

                    devn = (struct isa_pnp_device_node far*)devnode_raw;
                    if (isa_pnp_is_sound_blaster_compatible_id(devn->product_id,&whatis)) {
                        if (sndsb_try_isa_pnp_bios(devn->product_id,this_node,devn,devnode_raw_sz) > 0)
                            printf("PnP: Found %s\n",whatis);
                    }
                }
            }

            /* enumerate the ISA bus directly */
            if (isapnp_read_data != 0) {
                for (csn=1;csn < 255;csn++) {
                    isa_pnp_init_key();
                    isa_pnp_wake_csn(csn);

                    isa_pnp_write_address(0x06); /* CSN */
                    if (isa_pnp_read_data() == csn) {
                        /* apparently doing this lets us read back the serial and vendor ID in addition to resource data */
                        /* if we don't, then we only read back the resource data */
                        isa_pnp_init_key();
                        isa_pnp_wake_csn(csn);

                        for (j=0;j < 9;j++) data[j] = isa_pnp_read_config();

                        if (isa_pnp_is_sound_blaster_compatible_id(*((uint32_t*)data),&whatis)) {
                            if (sndsb_try_isa_pnp(*((uint32_t*)data),csn) > 0)
                                printf("PnP: Found %s\n",whatis);
                        }
                    }

                    /* return back to "wait for key" state */
                    isa_pnp_write_data_register(0x02,0x02);	/* bit 1: set -> return to Wait For Key state (or else a Pentium Pro system I own eventually locks up and hangs) */
                }
            }

            free(devnode_raw);
        }
    }

    /* Non-plug & play scan */
	if (sndsb_try_blaster_var() != NULL) {
		if (!sndsb_init_card(sndsb_card_blaster))
			sndsb_free_card(sndsb_card_blaster);
	}

    /* Most SB cards exist at 220h or 240h */
    sndsb_try_base(0x220);
    sndsb_try_base(0x240);

    /* now let the user choose */
	for (i=0;i < SNDSB_MAX_CARDS;i++) {
		struct sndsb_ctx *cx = sndsb_index_to_ctx(i);
		if (cx->baseio == 0) continue;

		if (cx->irq < 0)
			sndsb_probe_irq_F2(cx);
		if (cx->irq < 0)
			sndsb_probe_irq_80(cx);
		if (cx->dma8 < 0)
			sndsb_probe_dma8_E2(cx);
		if (cx->dma8 < 0)
			sndsb_probe_dma8_14(cx);

		// having IRQ and DMA changes the ideal playback method and capabilities
		sndsb_update_capabilities(cx);
		sndsb_determine_ideal_dsp_play_method(cx);
	}

	{
		unsigned char count = 0;
        int sc_idx = -1;

		for (i=0;i < SNDSB_MAX_CARDS;i++) {
			struct sndsb_ctx *cx = sndsb_index_to_ctx(i);
			if (cx->baseio == 0) continue;

			printf("  [%u] base=%X dma=%d dma16=%d irq=%d DSPv=%u.%u\n",
					i+1,cx->baseio,cx->dma8,cx->dma16,cx->irq,(unsigned int)cx->dsp_vmaj,(unsigned int)cx->dsp_vmin);

			count++;
		}

		if (count == 0) {
			printf("No cards found.\n");
			return 1;
		}
        else if (count > 1) {
            printf("-----------\n");
            printf("Which card?: "); fflush(stdout);

            i = getch();
            printf("\n");
            if (i == 27) return 0;
            if (i == 13 || i == 10) i = '1';
            sc_idx = i - '0';

            if (sc_idx < 1 || sc_idx > SNDSB_MAX_CARDS) {
                printf("Sound card index out of range\n");
                return 1;
            }
        }
        else { /* count == 1 */
            sc_idx = 1;
        }

        sb_card = &sndsb_card[sc_idx-1];
        if (sb_card->baseio == 0)
            return 1;
    }

    realloc_dma_buffer();

	if (!sndsb_assign_dma_buffer(sb_card,sb_dma)) {
		printf("Cannot assign DMA buffer\n");
		return 1;
	}

	if (sb_card->irq != -1) {
		old_irq_masked = p8259_is_masked(sb_card->irq);
		if (vector_is_iret(irq2int(sb_card->irq)))
			old_irq_masked = 1;

		old_irq = _dos_getvect(irq2int(sb_card->irq));
		_dos_setvect(irq2int(sb_card->irq),sb_irq);
		p8259_unmask(sb_card->irq);
	}

	loop=1;
	redraw=1;
	bkgndredraw=1;

    open_wav();
    begin_play();

	while (loop) {
        wav_idle();

		if (kbhit()) {
			i = getch();
			if (i == 0) i = getch() << 8;

			if (i == 27) {
                loop = 0;
                break;
            }
			else if (i == ' ') {
                if (wav_playing) stop_play();
                else begin_play();
            }
		}
	}

	_sti();
	stop_play();
	close_wav();
    free_dma_buffer();

	if (sb_card->irq >= 0 && old_irq_masked)
		p8259_mask(sb_card->irq);

	if (sb_card->irq != -1)
		_dos_setvect(irq2int(sb_card->irq),old_irq);

	sndsb_free_card(sb_card);
	free_sndsb(); /* will also de-ref/unhook the NMI reflection */
	return 0;
}

