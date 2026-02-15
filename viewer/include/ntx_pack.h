#ifndef NTX_PACK_H
#define NTX_PACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct
{
	uint16_t note_id;
	uint16_t first_part_id;
	uint16_t part_count;
	uint16_t total_chunks;
	uint32_t total_text_bytes;
	char* title;
} NtxNoteEntry;

typedef struct
{
	uint16_t count;
	NtxNoteEntry* entries;
} NtxIndex;

bool ntx_load_index(NtxIndex* out, char* err, size_t err_len);
void ntx_free_index(NtxIndex* index);
void ntx_part_name_from_id(uint16_t id, char out_name[9]);
bool ntx_load_chunk_text(const NtxNoteEntry* note, uint16_t global_chunk_index, char** out_text, uint16_t* out_len,
                         uint8_t* out_split_kind, char* err, size_t err_len);

#endif
