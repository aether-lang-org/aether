#ifndef AETHER_LZF_H
#define AETHER_LZF_H

int lzf_max_compressed_size(int length);

int lzf_try_compress(const char* data, int length);
const char* lzf_get_compress_bytes(void);
int lzf_get_compress_length(void);
void lzf_release_compress(void);

int lzf_try_decompress(const char* data, int length, int output_length);
const char* lzf_get_decompress_bytes(void);
int lzf_get_decompress_length(void);
void lzf_release_decompress(void);

#endif
