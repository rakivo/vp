// std-style library I made out of this library: https://github.com/yanjiesang/pcm2wav

#ifndef __WAV_H__
#define __WAV_H__

#define LITTLE_ENDIAN_BYTE_ORDER 0
#define BIG_ENDIAN_BYTE_ORDER 1
#define PCM_BYTE_ORDER LITTLE_ENDIAN_BYTE_ORDER

#if PCM_BYTE_ORDER == LITTLE_ENDIAN_BYTE_ORDER
#define COMPOSE_ID(a, b, c, d) ((a) | ((b) << 8) | ((c) << 16) | ((d) << 24))
#define LE_SHORT(v) (v)
#define LE_INT(v) (v)
#define BE_SHORT(v) bswap_16(v)
#define BE_INT(v) bswap_32(v)
#elif PCM_BYTE_ORDER == BIG_ENDIAN_BYTE_ORDER
#define COMPOSE_ID(a, b, c, d) ((d) | ((c) << 8) | ((b) << 16) | ((a) << 24))
#define LE_SHORT(v) bswap_16(v)
#define LE_INT(v) bswap_32(v)
#define BE_SHORT(v) (v)
#define BE_INT(v) (v)
#else
#error "Wrong endian"
#endif

#define WAV_RIFF COMPOSE_ID('R', 'I', 'F', 'F')
#define WAV_WAVE COMPOSE_ID('W', 'A', 'V', 'E')
#define WAV_FMT COMPOSE_ID('f', 'm', 't', ' ')
#define WAV_DATA COMPOSE_ID('d', 'a', 't', 'a')

/* WAVE fmt block constants from Microsoft mmreg.h header */
#define WAV_FMT_PCM 0x0001
#define WAV_FMT_IEEE_FLOAT 0x0003
#define WAV_FMT_DOLBY_AC3_SPDIF 0x0092
#define WAV_FMT_EXTENSIBLE 0xfffe

typedef struct {
  unsigned int magic;  /* 'RIFF' */
  unsigned int length; /* filelen */
  unsigned int type;   /* 'WAVE' */
} WavHeader;

typedef struct {
  unsigned int magic;    /* 'FMT '*/
  unsigned int fmt_size; /* 16 or 18 */
  unsigned short format; /* see WAV_FMT_* */
  unsigned short channels;
  unsigned int sample_rate; /* frequence of sample */
  unsigned int bytes_p_second;
  unsigned short blocks_align;  /* samplesize; 1 or 2 bytes */
  unsigned short sample_length; /* 8, 12 or 16 bit */
} WavFmt;

typedef struct {
  unsigned int type;   /* 'data' */
  unsigned int length; /* samplecount */
} WavChunkHeader;

typedef struct {
  WavHeader header;
  WavFmt format;
  WavChunkHeader chunk;
} WavContainer, *PWavContainer;

typedef struct {
  unsigned char *data;
  size_t size;
} Data_And_Size;

Data_And_Size *Pcm2Wav(const unsigned char *pcm_data,
											 size_t pcm_size,
                       unsigned short channels,
											 unsigned short sample_rate,
                       unsigned short sample_length);

#ifdef PCM2WAV_IMPLEMENTATION
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static PWavContainer CreatWavContainer(void) {
  PWavContainer pWavContainer = NULL;
  pWavContainer = (PWavContainer)calloc(1, sizeof(WavContainer));
  return pWavContainer;
}

static void DestoryWavContainer(PWavContainer pWavContainer) {
  if (pWavContainer) {
    free(pWavContainer);
    pWavContainer = NULL;
  }
}

static int WavHeaderFill(const PWavContainer pWavContainer,
                         unsigned short channels, unsigned short sample_rate,
                         unsigned short sample_length,
                         unsigned int chunk_data_length) {
  if (pWavContainer == NULL) {
    return -1;
  }

  pWavContainer->header.magic = WAV_RIFF;
  pWavContainer->header.type = WAV_WAVE;

  pWavContainer->format.magic = WAV_FMT;
  pWavContainer->format.fmt_size = LE_INT(sizeof(WavFmt) - 8);
  pWavContainer->format.format = LE_SHORT(WAV_FMT_PCM);
  pWavContainer->format.channels = LE_SHORT(channels);
  pWavContainer->format.sample_rate = LE_INT(sample_rate);
  pWavContainer->format.blocks_align = LE_SHORT(channels * sample_length / 8);
  pWavContainer->format.bytes_p_second =
      LE_INT(pWavContainer->format.blocks_align * sample_rate);
  pWavContainer->format.sample_length = LE_SHORT(sample_length);

  pWavContainer->chunk.type = WAV_DATA;
  pWavContainer->chunk.length = LE_INT(chunk_data_length);

  pWavContainer->header.length =
      LE_INT((unsigned int)(pWavContainer->chunk.length) +
             sizeof(pWavContainer->chunk) + sizeof(pWavContainer->format) +
             sizeof(pWavContainer->header) - 8);

  return 0;
}

Data_And_Size *Pcm2Wav(const unsigned char *pcm_data,
											 size_t pcm_size,
                       unsigned short channels,
											 unsigned short sample_rate,
                       unsigned short sample_length)
{
  PWavContainer pWavContainer = NULL;
  unsigned int wav_header_size = sizeof(WavContainer);
  size_t total_size = wav_header_size + pcm_size;

  if (pcm_data == NULL || pcm_size == 0) {
    printf("  [error] pcm data is NULL or size is 0\n");
    return NULL;
  }

  if (channels != 1 && channels != 2) {
    printf("  [error] channel set %d, should set 1(Mono) or 2(Stereo)\n",
           channels);
    return NULL;
  }

  // Create WAV container
  pWavContainer = CreatWavContainer();
  if (pWavContainer == NULL) {
    printf("  [error] create wav container failed\n");
    return NULL;
  }

  // Fill in the WAV header with appropriate information
  WavHeaderFill(pWavContainer, channels, sample_rate, sample_length, pcm_size);

  // Allocate memory for the resulting WAV file (header + PCM data)
  Data_And_Size *output = (Data_And_Size *)malloc(sizeof(Data_And_Size));
  if (output == NULL) {
    printf("  [error] memory allocation for Data_And_Size failed\n");
    goto CleanUp;
  }

  // Allocate memory for the WAV data (header + PCM data)
  output->data = (unsigned char *)malloc(total_size);
  if (output->data == NULL) {
    printf("  [error] memory allocation for wav data failed\n");
    free(output);
    goto CleanUp;
  }

  // Copy WAV header to the beginning of the output data
  memmove(output->data, pWavContainer, wav_header_size);

  // Copy PCM data to the output data after the header
  memmove(output->data + wav_header_size, pcm_data, pcm_size);

  // Set the size of the returned data
  output->size = total_size;

CleanUp:
  if (pWavContainer) {
    DestoryWavContainer(pWavContainer);
  }

  // Return the pointer to the output Data_And_Size structure
  return output;
}
#endif // PCM2WAV_IMPLEMENTATION
#endif // __WAV_H__
