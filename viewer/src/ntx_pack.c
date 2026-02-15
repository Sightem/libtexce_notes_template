#include "ntx_pack.h"

#include <fileioc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NTX_INDEX_NAME "NTXIDX"
#define NTX_MAGIC_IDX "NTXI"
#define NTX_MAGIC_PART "NTXP"

#define NTX_PART_HEADER_SIZE 24U
#define NTX_PART_ENTRY_SIZE 8U

static void set_err(char* err, size_t err_len, const char* msg)
{
	if (!err || err_len == 0)
		return;
	snprintf(err, err_len, "%s", msg ? msg : "error");
}

static void set_err_name(char* err, size_t err_len, const char* prefix, const char* name)
{
	if (!err || err_len == 0)
		return;
	snprintf(err, err_len, "%s%s", prefix ? prefix : "", name ? name : "");
}

static uint16_t read_u16_le(const uint8_t* p)
{
	return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t read_u32_le(const uint8_t* p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8U) | ((uint32_t)p[2] << 16U) | ((uint32_t)p[3] << 24U);
}

static bool read_appvar_bytes(const char* name, uint8_t** out_buf, uint16_t* out_len, char* err, size_t err_len)
{
	if (!out_buf || !out_len)
		return false;
	*out_buf = NULL;
	*out_len = 0;

	uint8_t h = ti_Open(name, "r");
	if (!h)
	{
		set_err_name(err, err_len, "open fail: ", name);
		return false;
	}

	uint16_t sz = ti_GetSize(h);
	if (sz == 0)
	{
		ti_Close(h);
		set_err_name(err, err_len, "empty var: ", name);
		return false;
	}

	uint8_t* buf = (uint8_t*)malloc(sz);
	if (!buf)
	{
		ti_Close(h);
		set_err_name(err, err_len, "oom reading ", name);
		return false;
	}

	ti_Rewind(h);
	size_t got = ti_Read(buf, 1, sz, h);
	ti_Close(h);

	if (got != sz)
	{
		free(buf);
		set_err_name(err, err_len, "short read: ", name);
		return false;
	}

	*out_buf = buf;
	*out_len = sz;
	return true;
}

bool ntx_load_index(NtxIndex* out, char* err, size_t err_len)
{
	if (!out)
		return false;
	memset(out, 0, sizeof(*out));

	uint8_t* buf = NULL;
	uint16_t len = 0;
	if (!read_appvar_bytes(NTX_INDEX_NAME, &buf, &len, err, err_len))
		return false;

	if (len < 16)
	{
		free(buf);
		set_err(err, err_len, "index too small");
		return false;
	}

	if (memcmp(buf, NTX_MAGIC_IDX, 4) != 0)
	{
		free(buf);
		set_err(err, err_len, "bad index magic");
		return false;
	}

	uint16_t version = read_u16_le(buf + 4);
	uint16_t hdr_size = read_u16_le(buf + 6);
	uint16_t note_count = read_u16_le(buf + 8);

	if (version != 1 || hdr_size != 16)
	{
		free(buf);
		set_err(err, err_len, "index version mismatch");
		return false;
	}

	if (note_count == 0)
	{
		free(buf);
		return true;
	}

	NtxNoteEntry* entries = (NtxNoteEntry*)calloc(note_count, sizeof(NtxNoteEntry));
	if (!entries)
	{
		free(buf);
		set_err(err, err_len, "oom entries");
		return false;
	}

	size_t pos = hdr_size;
	for (uint16_t i = 0; i < note_count; ++i)
	{
		if (pos + 14 > len)
		{
			free(entries);
			free(buf);
			set_err(err, err_len, "truncated index");
			return false;
		}

		entries[i].note_id = read_u16_le(buf + pos + 0);
		entries[i].first_part_id = read_u16_le(buf + pos + 2);
		entries[i].part_count = read_u16_le(buf + pos + 4);
		entries[i].total_chunks = read_u16_le(buf + pos + 6);
		entries[i].total_text_bytes = read_u32_le(buf + pos + 8);
		uint8_t title_len = buf[pos + 12];
		pos += 14;

		if (pos + title_len > len)
		{
			for (uint16_t j = 0; j < i; ++j)
				free(entries[j].title);
			free(entries);
			free(buf);
			set_err(err, err_len, "truncated title");
			return false;
		}

		entries[i].title = (char*)malloc((size_t)title_len + 1U);
		if (!entries[i].title)
		{
			for (uint16_t j = 0; j < i; ++j)
				free(entries[j].title);
			free(entries);
			free(buf);
			set_err(err, err_len, "oom title");
			return false;
		}
		memcpy(entries[i].title, buf + pos, title_len);
		entries[i].title[title_len] = '\0';
		pos += title_len;
	}

	out->count = note_count;
	out->entries = entries;
	free(buf);
	return true;
}

void ntx_free_index(NtxIndex* index)
{
	if (!index || !index->entries)
		return;
	for (uint16_t i = 0; i < index->count; ++i)
		free(index->entries[i].title);
	free(index->entries);
	index->entries = NULL;
	index->count = 0;
}

void ntx_part_name_from_id(uint16_t id, char out_name[9])
{
	if (!out_name)
		return;
	snprintf(out_name, 9, "NTX%04u", (unsigned int)id);
}

bool ntx_load_chunk_text(const NtxNoteEntry* note, uint16_t global_chunk_index, char** out_text, uint16_t* out_len,
                         uint8_t* out_split_kind, char* err, size_t err_len)
{
	if (!note || !out_text || !out_len)
	{
		set_err(err, err_len, "bad args");
		return false;
	}
	*out_text = NULL;
	*out_len = 0;
	if (out_split_kind)
		*out_split_kind = 0;

	if (global_chunk_index >= note->total_chunks)
	{
		set_err(err, err_len, "chunk out of range");
		return false;
	}

	for (uint16_t p = 0; p < note->part_count; ++p)
	{
		char name[9] = { 0 };
		ntx_part_name_from_id((uint16_t)(note->first_part_id + p), name);

		uint8_t* buf = NULL;
		uint16_t len = 0;
		if (!read_appvar_bytes(name, &buf, &len, err, err_len))
			return false;

		if (len < NTX_PART_HEADER_SIZE || memcmp(buf, NTX_MAGIC_PART, 4) != 0)
		{
			free(buf);
			set_err(err, err_len, "bad part header");
			return false;
		}

		uint16_t version = read_u16_le(buf + 4);
		uint16_t header_size = read_u16_le(buf + 6);
		uint16_t chunk_count = read_u16_le(buf + 14);
		uint16_t chunk_table_off = read_u16_le(buf + 16);
		uint16_t payload_off = read_u16_le(buf + 18);
		uint16_t payload_size = read_u16_le(buf + 20);

		if (version != 1 || header_size != NTX_PART_HEADER_SIZE)
		{
			free(buf);
			set_err(err, err_len, "part version mismatch");
			return false;
		}
		if ((size_t)payload_off + payload_size > len)
		{
			free(buf);
			set_err(err, err_len, "part payload out of bounds");
			return false;
		}
		if ((size_t)chunk_table_off + ((size_t)chunk_count * NTX_PART_ENTRY_SIZE) > len)
		{
			free(buf);
			set_err(err, err_len, "part chunk table out of bounds");
			return false;
		}

		for (uint16_t c = 0; c < chunk_count; ++c)
		{
			const uint8_t* ent = buf + chunk_table_off + ((size_t)c * NTX_PART_ENTRY_SIZE);
			uint16_t rel = read_u16_le(ent + 0);
			uint16_t clen = read_u16_le(ent + 2);
			uint8_t split_kind = ent[4];
			uint16_t gidx = read_u16_le(ent + 6);

			if (gidx != global_chunk_index)
				continue;

			if ((size_t)rel + clen > payload_size)
			{
				free(buf);
				set_err(err, err_len, "chunk payload out of bounds");
				return false;
			}

			char* text = (char*)malloc((size_t)clen + 1U);
			if (!text)
			{
				free(buf);
				set_err(err, err_len, "oom chunk");
				return false;
			}

			memcpy(text, buf + payload_off + rel, clen);
			text[clen] = '\0';
			free(buf);

			*out_text = text;
			*out_len = clen;
			if (out_split_kind)
				*out_split_kind = split_kind;
			return true;
		}

		free(buf);
	}

	set_err(err, err_len, "chunk not found");
	return false;
}
