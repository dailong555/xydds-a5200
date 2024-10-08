/*
 * compfile.c - File I/O and ZLIB compression
 *
 * Copyright (C) 1995-1998 David Firth
 * Copyright (C) 1998-2006 Atari800 development team (see DOC/CREDITS)
 *
 * This file is part of the Atari800 emulator project which emulates
 * the Atari 400, 800, 800XL, 130XE, and 5200 8-bit computers.
 *
 * Atari800 is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Atari800 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Atari800; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include "config.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#ifdef HAVE_LIBZ
#include <zlib.h>
#endif
#ifdef _WIN32
#include <windows.h> /* ULONG */
#endif

#include "atari.h"
#include "compfile.h"
#include "util.h"

/* GZ decompression ------------------------------------------------------ */

/* Opens a GZIP compressed file and decompresses its contents to outfp.
   Returns TRUE on success. */
int CompressedFile_ExtractGZ(const char *infilename, FILE *outfp)
{
#ifndef HAVE_LIBZ
	return FALSE;
#else
	/* TODO: replace gz* with low-level light-weight ZLIB functions. */
	gzFile gzf = gzopen(infilename, "rb");
	void *buf;
	int result;
	if (gzf == NULL) {
		return FALSE;
	}
#define UNCOMPRESS_BUFFER_SIZE 32768
	buf = Util_malloc(UNCOMPRESS_BUFFER_SIZE);
	do {
		result = gzread(gzf, buf, UNCOMPRESS_BUFFER_SIZE);
		if (result > 0) {
			if ((int) fwrite(buf, 1, result, outfp) != result)
				result = -1;
		}
	} while (result == UNCOMPRESS_BUFFER_SIZE);
	free(buf);
	gzclose(gzf);
	return result >= 0;
#endif	/* HAVE_LIBZ */
}


/* DCM decompression ----------------------------------------------------- */

static int fgetw(FILE *fp)
{
	int low;
	int high;
	low = fgetc(fp);
	if (low == EOF)
		return -1;
	high = fgetc(fp);
	if (high == EOF)
		return -1;
	return low + (high << 8);
}

static int fload(void *buf, int size, FILE *fp)
{
	return (int) fread(buf, 1, size, fp) == size;
}

static int fsave(void *buf, int size, FILE *fp)
{
	return (int) fwrite(buf, 1, size, fp) == size;
}

typedef struct {
	FILE *fp;
	int sectorcount;
	int sectorsize;
	int current_sector;
} ATR_Info;

static int write_atr_header(const ATR_Info *pai)
{
	int sectorcount;
	int sectorsize;
	ULONG paras;
	struct ATR_Header header;
	sectorcount = pai->sectorcount;
	sectorsize = pai->sectorsize;
	paras = (sectorsize != 256 || sectorcount <= 3)
		? (sectorcount << 3) /* single density or only boot sectors: sectorcount * 128 / 16 */
		: (sectorcount << 4) - 0x18; /* double density with 128-byte boot sectors: (sectorcount * 256 - 3 * 128) / 16 */
	memset(&header, 0, sizeof(header));
	header.magic1 = MAGIC1;
	header.magic2 = MAGIC2;
	header.secsizelo = (UBYTE) sectorsize;
	header.secsizehi = (UBYTE) (sectorsize >> 8);
	header.seccountlo = (UBYTE) paras;
	header.seccounthi = (UBYTE) (paras >> 8);
	header.hiseccountlo = (UBYTE) (paras >> 16);
	header.hiseccounthi = (UBYTE) (paras >> 24);
	return fsave(&header, sizeof(header), pai->fp);
}

static int write_atr_sector(ATR_Info *pai, UBYTE *buf)
{
	return fsave(buf, pai->current_sector++ <= 3 ? 128 : pai->sectorsize, pai->fp);
}

static int pad_till_sector(ATR_Info *pai, int till_sector)
{
	UBYTE zero_buf[256];
	memset(zero_buf, 0, sizeof(zero_buf));
	while (pai->current_sector < till_sector)
		if (!write_atr_sector(pai, zero_buf))
			return FALSE;
	return TRUE;
}

static int dcm_pass(FILE *infp, ATR_Info *pai)
{
	UBYTE sector_buf[256];
	memset(sector_buf, 0, sizeof(sector_buf));
	for (;;) {
		/* sector group */
		int sector_no;
		int sector_type;
		sector_no = fgetw(infp);
		sector_type = fgetc(infp);
		if (sector_type == 0x45)
			return TRUE;
		if (sector_no < pai->current_sector) {
			return FALSE;
		}
		if (!pad_till_sector(pai, sector_no))
			return FALSE;
		for (;;) {
			/* sector */
			int i;
			switch (sector_type & 0x7f) {
			case 0x41:
				i = fgetc(infp);
				if (i == EOF)
					return FALSE;
				do {
					int b = fgetc(infp);
					if (b == EOF)
						return FALSE;
					sector_buf[i] = (UBYTE) b;
				} while (i-- != 0);
				break;
			case 0x42:
				if (!fload(sector_buf + 123, 5, infp))
					return FALSE;
				memset(sector_buf, sector_buf[123], 123);
				break;
			case 0x43:
				i = 0;
				do {
					int j;
					int c;
					j = fgetc(infp);
					if (j < i) {
						if (j != 0)
							return FALSE;
						j = 256;
					}
					if (i < j && !fload(sector_buf + i, j - i, infp))
						return FALSE;
					if (j >= pai->sectorsize)
						break;
					i = fgetc(infp);
					if (i < j) {
						if (i != 0)
							return FALSE;
						i = 256;
					}
					c = fgetc(infp);
					if (c == EOF)
						return FALSE;
					memset(sector_buf + j, c, i - j);
				} while (i < pai->sectorsize);
				break;
			case 0x44:
				i = fgetc(infp);
				if (i == EOF || i >= pai->sectorsize)
					return FALSE;
				if (!fload(sector_buf + i, pai->sectorsize - i, infp))
					return FALSE;
				break;
			case 0x46:
				break;
			case 0x47:
				if (!fload(sector_buf, pai->sectorsize, infp))
					return FALSE;
				break;
			default:
				return FALSE;
			}
			if (!write_atr_sector(pai, sector_buf))
				return FALSE;
			if (!(sector_type & 0x80))
				break; /* goto sector group */
			sector_type = fgetc(infp);
			if (sector_type == 0x45)
				return TRUE;
		}
	}
}

int CompressedFile_DCMtoATR(FILE *infp, FILE *outfp)
{
	int archive_type;
	int archive_flags;
	ATR_Info ai;
	int pass_flags;
	int last_sector;
	archive_type = fgetc(infp);
	if (archive_type != 0xf9 && archive_type != 0xfa) {
		return FALSE;
	}
	archive_flags = fgetc(infp);
	if ((archive_flags & 0x1f) != 1) {
		return FALSE;
	}
	ai.fp = outfp;
	ai.current_sector = 1;
	switch ((archive_flags >> 5) & 3) {
	case 0:
		ai.sectorcount = 720;
		ai.sectorsize = 128;
		break;
	case 1:
		ai.sectorcount = 720;
		ai.sectorsize = 256;
		break;
	case 2:
		ai.sectorcount = 1040;
		ai.sectorsize = 128;
		break;
	default:
		return FALSE;
	}
	if (!write_atr_header(&ai))
		return FALSE;
	pass_flags = archive_flags;
	for (;;) {
		/* pass */
		int block_type;
		if (!dcm_pass(infp, &ai))
			return FALSE;
		if (pass_flags & 0x80)
			break;
		block_type = fgetc(infp);
		if (block_type != archive_type) {
			return FALSE;
		}
		pass_flags = fgetc(infp);
		if ((pass_flags ^ archive_flags) & 0x60) {
			return FALSE;
		}
		/* TODO: check pass number, this is tricky for >31 */
	}
	last_sector = ai.current_sector - 1;
	if (last_sector <= ai.sectorcount)
		return pad_till_sector(&ai, ai.sectorcount + 1);
	/* more sectors written: update ATR header */
	ai.sectorcount = last_sector;
	Util_rewind(outfp);
	return write_atr_header(&ai);
}
