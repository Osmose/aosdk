/*
	Audio Overload SDK - main driver.  for demonstration only, not user friendly!

	Copyright (c) 2007-2009 R. Belmont and Richard Bannister.

	All rights reserved.

	Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

	* Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
	* Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.
	* Neither the names of R. Belmont and Richard Bannister nor the names of its contributors may be used to endorse or promote products derived from this software without specific prior written permission.

	THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
	"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
	LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
	A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
	CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
	EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
	PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
	PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
	LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
	NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
	SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "argparse/argparse.h"
#include "ao.h"
#include "eng_protos.h"
#include "m1sdr.h"
#include "wavedump.h"

/* file types */
static uint32 type;
static wavedump_t song_dump;
volatile ao_bool ao_song_done;

static struct
{
	uint32 sig;
	char *name;
	int32 (*start)(uint8 *, uint32);
	int32 (*sample)(stereo_sample_t *);
	int32 (*frame)(void);
	int32 (*stop)(void);
	int32 (*command)(int32, int32);
	uint32 rate;
	int32 (*fillinfo)(ao_display_info *);
} types[] =
{
	{ 0x50534641, "Capcom QSound (.qsf)", qsf_start, qsf_sample, qsf_frame, qsf_stop, qsf_command, 60, qsf_fill_info },
	{ 0x50534611, "Sega Saturn (.ssf)", ssf_start, ssf_sample, ssf_frame, ssf_stop, ssf_command, 60, ssf_fill_info },
	{ 0x50534601, "Sony PlayStation (.psf)", psf_start, psf_sample, psf_frame, psf_stop, psf_command, 60, psf_fill_info },
	{ 0x53505500, "Sony PlayStation (.spu)", spu_start, spu_sample, spu_frame, spu_stop, spu_command, 60, spu_fill_info },
	{ 0x50534602, "Sony PlayStation 2 (.psf2)", psf2_start, psf2_sample, psf2_frame, psf2_stop, psf2_command, 60, psf2_fill_info },
	{ 0x50534612, "Sega Dreamcast (.dsf)", dsf_start, dsf_sample, dsf_frame, dsf_stop, dsf_command, 60, dsf_fill_info },

	{ 0xffffffff, "", NULL, NULL, NULL, NULL, NULL, 0, NULL }
};

/* redirect stubs to interface the Z80 core to the QSF engine */
uint8 memory_read(uint16 addr)
{
	return qsf_memory_read(addr);
}

uint8 memory_readop(uint16 addr)
{
	return memory_read(addr);
}

uint8 memory_readport(uint16 addr)
{
	return qsf_memory_readport(addr);
}

void memory_write(uint16 addr, uint8 byte)
{
	qsf_memory_write(addr, byte);
}

void memory_writeport(uint16 addr, uint8 byte)
{
	qsf_memory_writeport(addr, byte);
}

/* ao_get_lib: called to load secondary files */
int ao_get_lib(const char *filename, uint8 **buffer, uint64 *length)
{
	uint8 *filebuf;
	uint32 size;
	FILE *auxfile;

	auxfile = ao_fopen(filename, "rb");
	if (!auxfile)
	{
		printf("Unable to find auxiliary file %s\n", filename);
		return AO_FAIL;
	}

	fseek(auxfile, 0, SEEK_END);
	size = ftell(auxfile);
	fseek(auxfile, 0, SEEK_SET);

	filebuf = malloc(size);

	if (!filebuf)
	{
		fclose(auxfile);
		printf("ERROR: could not allocate %d bytes of memory\n", size);
		return AO_FAIL;
	}

	fread(filebuf, size, 1, auxfile);
	fclose(auxfile);

	*buffer = filebuf;
	*length = (uint64)size;

	return AO_SUCCESS;
}

static void do_frame(unsigned long sample_count, stereo_sample_t *buffer)
{
	unsigned long i;
	stereo_sample_t *p = buffer;
	for (i = 0; i < sample_count; i++)
	{
		(*types[type].sample)(p);
		p++;
	}
	wavedump_stream_append(&song_dump, sample_count, buffer);
	(*types[type].frame)();
}

static void intr_handler(int sig)
{
	ao_song_done = 1;
}

int main(int argc, const char *argv[])
{
	FILE *file;
	uint8 *buffer;
	uint32 size, filesig;
	int noplay = false;
	int nowave = false;

	const char *const usages[] =
	{
		"aosdk filename",
		NULL
	};

	struct argparse_option options[] =
	{
		OPT_HELP(),
		OPT_BOOLEAN('p', "noplay", &noplay, "don't play back the song"),
		OPT_BOOLEAN('w', "nowave", &nowave, "don't dump the song to a .wav file"),
		OPT_END()
	};

	struct argparse argparse;
	argparse_init(&argparse, options, usages, 0);
	argparse_describe(&argparse,
		"\n"
		"AOSDK test program v1.0 by R. Belmont [AOSDK release 1.4.8]\n"
		"Copyright (c) 2007-2009 R. Belmont and Richard Bannister - please read license.txt for license details",
		NULL
	);

	argc = argparse_parse(&argparse, argc, argv);

	// check if an argument was given
	if (argc < 1)
	{
		argparse_usage(&argparse);
		return -1;
	}

	file = ao_fopen(argv[0], "rb");

	if (!file)
	{
		printf("ERROR: could not open file %s\n", argv[0]);
		return -1;
	}

	// get the length of the file by seeking to the end then reading the current position
	fseek(file, 0, SEEK_END);
	size = ftell(file);
	// reset the pointer
	fseek(file, 0, SEEK_SET);

	buffer = malloc(size);

	if (!buffer)
	{
		fclose(file);
		printf("ERROR: could not allocate %d bytes of memory\n", size);
		return -1;
	}

	// read the file
	fread(buffer, size, 1, file);
	fclose(file);

	// now try to identify the file
	type = 0;
	filesig = buffer[0]<<24 | buffer[1]<<16 | buffer[2]<<8 | buffer[3];
	while (types[type].sig != 0xffffffff)
	{
		if (filesig == types[type].sig)
		{
			break;
		}
		else
		{
			type++;
		}
	}

	// now did we identify it above or just fall through?
	if (types[type].sig != 0xffffffff)
	{
		printf("File identified as %s\n", types[type].name);
	}
	else
	{
		printf("ERROR: File is unknown, signature bytes are %02x %02x %02x %02x\n", buffer[0], buffer[1], buffer[2], buffer[3]);
		free(buffer);
		return -1;
	}

	if ((*types[type].start)(buffer, size) != AO_SUCCESS)
	{
		free(buffer);
		printf("ERROR: Engine rejected file!\n");
		return -1;
	}

	if(!nowave && wavedump_stream_open(&song_dump, argv[0]))
	{
		printf("Dumping to %s%s.\n", argv[0], ".wav");
	}

	signal(SIGINT, intr_handler);
	if(!noplay)
	{
		m1sdr_Init(44100);
		m1sdr_SetCallback(do_frame);
		m1sdr_PlayStart();
		printf("Playing.  ");
	}
	printf("Press CTRL-C to stop.\n");

	while (!ao_song_done)
	{
		if(!noplay)
		{
			m1sdr_TimeCheck();
		}
		else
		{
			stereo_sample_t buffer[44100 / 60];
			do_frame(sizeof(buffer) / sizeof(stereo_sample_t), buffer);
		}
	}

	signal(SIGINT, SIG_IGN);
	wavedump_stream_finish(&song_dump, 44100);

	free(buffer);

	return 1;
}

// stub for MAME stuff
int change_pc(int foo)
{
}
