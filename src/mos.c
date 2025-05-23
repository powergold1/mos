// TODO:
// move directories. show directories in view
// volume control
// quick jump to predefined directories
// toggle to sort all entries by name or mtime
// mouse wheel up and down to scroll the list
// mouse click to play track
// show length of files in list. maybe lazily.
// load playlist from files
#include "def.h"

#include <SDL3/SDL_keycode.h>
#include <time.h>

#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_oldnames.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_render.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <libavcodec/avcodec.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/file.h>
#include <libavutil/mem.h>

typedef enum {
	Ok,
	ErrNoAudioStream,
	ErrNoCodecFound,
	ErrAllocFailed,
	ErrFfmpeg,
} ResultTag;

typedef enum {
	InputDefault,
	InputFilter,
} InputMode;

typedef struct {
	ResultTag tag;
	int rc;
} Result;

#define ffmpegerr(rc) (Result){.tag=ErrFfmpeg, .rc = rc}
#define R(x) (Result){.tag=x}
#define okp(r) (r.tag == Ok)

typedef struct {
	i32 start;
	i32 len;
} Sub;

typedef struct {
	SDL_Texture *texture;
	float w;
	float h;
	int advance;
} Glyph;

typedef struct {
	const u8 *base;
	const u8 *cur;
	const u8 *end;
} ByteRange;

#define extensions_def\
	X(wav)\
	X(mp3)\
	X(opus)\
	X(ogg)\
	X(m4a)\

#if 0
	X(xm)\
	X(mod)\
	X(it)
#endif

typedef enum ExtensionId {
#define X(A) ext_##A,
	extensions_def
#undef X
	ExtIdCount,
} ExtensionId;

static const Slice accepted_extensions[] = {
#define X(A) S(#A),
	extensions_def
#undef X
};

typedef struct {
	Sub path;
	i32 name_offset;
	ExtensionId ext;
	i64 mtime;
} MusicEntry;

typedef struct {
	char *data;
	i32 count;
	i32 cap;
} CharList;

typedef struct {
	i32 *data;
	i32 count;
	i32 cap;
} I32List;

typedef struct {
	MusicEntry *data;
	i32 count;
	i32 cap;
} MusicEntryList;

typedef struct {
	Sub base_name;
	MusicEntryList entries;
	CharList names;
} Playlist;

typedef struct { u64 state; u64 inc; } Pcg32;

static u32 pcg32_random(Pcg32* rng)
{
	u64 oldstate = rng->state;
	rng->state = oldstate * 6364136223846793005ULL + (rng->inc|1);
	u32 xorshifted = ((oldstate >> 18u) ^ oldstate) >> 27u;
	u32 rot = oldstate >> 59u;
	return (xorshifted >> rot) | (xorshifted << ((-rot) & 31));
}

static void pcg32_seed(Pcg32* rng, uint64_t initstate, uint64_t initseq)
{
	rng->state = 0U;
	rng->inc = (initseq << 1u) | 1u;
	pcg32_random(rng);
	rng->state += initstate;
	pcg32_random(rng);
}

static u32 pcg32_boundedrand(Pcg32* rng, u32 bound)
{
	u32 threshold = -bound % bound;
	while(1){
		u32 r = pcg32_random(rng);
		if (r >= threshold)
			return r % bound;
	}
}

typedef struct {
	bool want_to_quit;

	SDL_Window *window;

	f32 window_height;
	f32 playlist_height;
	f32 window_width;
	f32 max_progress_bar_width;
	Glyph ascii_glyphs[128];
	f32 font_line_skip;

	Playlist playlist;
	i32 previous_selected_idx;
	i32 playlist_selected_idx;
	i32 playlist_top;
	i32 playlist_playing_idx;

	SDL_AudioDeviceID audio_device_id;
	SDL_AudioSpec dst_audio_spec;
	SDL_AudioStream *current_audio_stream;

	SDL_Mutex *avmutex;
	AVFormatContext *format_context;
	AVStream *stream;
	AVCodecContext *codec_context;
	AVCodec *codec;
	i32 sample_size;
	i32 audio_stream_idx;
	bool eof;
	bool paused;
	bool auto_next;
	bool shuffle;
	bool seeking;

	AVPacket *current_packet;
	AVFrame *current_frame;
	i32 current_frame_sample;
	f32 last_relative_duration;

	Pcg32 rng;
	InputMode input_mode;
	CharList filter_prompt;
	i32 filter_prompt_cursor;
	I32List matching_items;

	I32List history;
	i32 history_cursor;
} Player;

constexpr u8 font_bytes[] = {
#embed "golos-ui.ttf"
};

static void log_err(Result r){
	switch(r.tag){
		case ErrFfmpeg:
			const char tmp[4] = { (char)(-r.rc), (char)(-r.rc >> 8), (char)(-r.rc >> 16), (char)(-r.rc >> 24), };
			bool printable_tag = 1;
			for(int i = 0; i < 4; ++i){
				printable_tag &= (tmp[i] >= 32 && tmp[i] <= 126);
			}
			eprint("err: ", r.tag, " (ffmpeg), return code: ", r.rc);
			if(printable_tag){
				Slice tmp2 = {tmp, sizeof(tmp)};
				eprintln(", tag: ", tmp2);
			} else {
				eprint("\n");
			}
			break;
		default:
			eprintln("err: ", r.tag, " ", r.rc);
			break;
	}
}

// funny that the order of parameters in SDL_qsort_r is different from the C stdlib qsort_r
static int compare_sub(void *arg, const void *pa, const void *pb){
	const CharList *strdata = arg;
	const Sub *a = pa;
	const Sub *b = pb;
	i32 minlen = MIN(a->len, b->len);
	i32 d = memcmp(strdata->data + a->start, strdata->data + b->start, minlen);
	if(d != 0)
		return d;
	if(a->len < b->len)
		return -1;
	if(a->len > b->len)
		return 1;
	return 0;
}

static void push_string(CharList *l, const char *str, i32 len){
	if(l->count + len > l->cap){
		do {
			l->cap *= 2;
		} while(l->count + len > l->cap);
		l->data = realloc(l->data, l->cap * sizeof(l->data[0]));
	}
	memcpy(l->data + l->count, str, len);
	l->count += len;
}

static void delete_chars(CharList *l, i32 at, i32 n){
	assert(at + n <= l->count);
	i32 r = l->count - at + n;
	memmove(l->data + at, l->data + at + n, r);
	l->count -= n;
}

static void insert_string(CharList *l, i32 at, const char *str, i32 len){
	if(l->count + len > l->cap){
		do {
			l->cap *= 2;
		} while(l->count + len > l->cap);
		l->data = realloc(l->data, sizeof(l->data[0]) * l->cap);
	}
	memmove(l->data + at + len, l->data + at, len);
	memcpy(l->data + at, str, len);
	l->count += len;
}

static void push_entry(MusicEntryList *l, const MusicEntry *entry){
	if(l->count >= l->cap){
		l->cap *= 2;
		l->data = realloc(l->data, sizeof(l->data[0]) * l->cap);
	}
	l->data[l->count] = *entry;
	l->count += 1;
}

static CharList make_charlist(void){
	i32 cap = 64;
	CharList l;
	l.data = malloc(cap * sizeof(l.data[0]));
	l.count = 0;
	l.cap = cap;
	return l;
}

static I32List make_i32list(void){
	i32 cap = 64;
	I32List l;
	l.data = malloc(cap * sizeof(l.data[0]));
	l.count = 0;
	l.cap = cap;
	return l;
}

static void push_i32(I32List *l, i32 x){
	if(l->count >= l->cap){
		l->cap *= 2;
		l->data = realloc(l->data, sizeof(l->data[0]) * l->cap);
	}
	l->data[l->count] = x;
	l->count += 1;
}

static MusicEntryList make_entrylist(void){
	i32 cap = 64;
	MusicEntryList l;
	l.data = malloc(cap * sizeof(l.data[0]));
	l.count = 0;
	l.cap = cap;
	return l;
}

static Sub get_extension(const CharList *l){
	i32 i = l->count - 1;
	while(i >= 0){
		if(l->data[i] == '.'){
			++i;
			break;
		}
		--i;
	}
	if(i < 0){
		return (Sub){-1,-1};
	} else {
		return (Sub){i, l->count - 1 - i};
	}
}

static i32 get_extension_id(CharList *base, Sub s, const Slice cands[], i32 cand_count){
	for(i32 i = 0; i < cand_count; ++i){
		if(cands[i].len == s.len && 0 == memcmp(cands[i].str, base->data + s.start, s.len)){
			return i;
		}
	}
	return -1;
}

static Playlist make_playlist_from_directory(Slice directory){
	AVIODirContext *dirp;
	int rc = avio_open_dir(&dirp, directory.str, NULL);
	Playlist pl = {};
	if(rc < 0){
		return pl;
	}
	CharList fullpath = make_charlist();
	push_string(&fullpath, directory.str, directory.len);
	assert(fullpath.count > 0);
	if(fullpath.data[fullpath.count-1] != '/'){
		push_string(&fullpath, "/", 1);
	}
	i32 baselen = fullpath.count;
	CharList names = make_charlist();
	push_string(&names, fullpath.data, fullpath.count);
	pl.base_name.start = 0;
	pl.base_name.len = baselen;
	MusicEntryList entries = make_entrylist();

	while(1){
		AVIODirEntry *directory_entry;
		rc = avio_read_dir(dirp, &directory_entry);
		if(!directory_entry){
			break;
		}
		i64 mtime = directory_entry->modification_timestamp;
		fullpath.count = baselen;
		i32 namelen = strlen(directory_entry->name);
		push_string(&fullpath, directory_entry->name, namelen);
		push_string(&fullpath, "\0", 1);
		i64 filemode = directory_entry->type;
		avio_free_directory_entry(&directory_entry);
		// TODO: AVIO_ENTRY_SYMBOLIC_LINK
		// TODO: AVIO_ENTRY_DIRECTORY
		if(filemode != AVIO_ENTRY_FILE){
			continue;
		}
		Sub ext = get_extension(&fullpath);
		if(ext.start < 0)
			continue;
		i32 ext_id = get_extension_id(&fullpath, ext, accepted_extensions, countof(accepted_extensions));
		if(ext_id < 0)
			continue;
		assert(ext_id < ExtIdCount);
		MusicEntry music_entry;
		music_entry.path.start = names.count;
		music_entry.name_offset = baselen;
		music_entry.path.len = fullpath.count;
		music_entry.ext = ext_id;
		music_entry.mtime = mtime;
		push_string(&names, fullpath.data, fullpath.count);
		push_entry(&entries, &music_entry);
	}
	free(fullpath.data);
	avio_close_dir(&dirp);
	SDL_qsort_r(entries.data, entries.count, sizeof(entries.data[0]), compare_sub, &names);
	pl.names = names;
	pl.entries = entries;
	return pl;
}

static void print_playlist(const Playlist *pl){
	for(i32 i = 0; i < pl->entries.count; ++i){
		Slice tmp = {pl->names.data + pl->entries.data[i].path.start, pl->entries.data[i].path.len};
		println(tmp);
	}
}

static void free_playlist(Playlist *pl){
	free(pl->entries.data);
	pl->entries.data = NULL;
	pl->entries.count =0;
	pl->entries.cap = 0;
	free(pl->names.data);
	pl->names.data = NULL;
	pl->names.count = 0;
	pl->names.cap = 0;
}

static void free_player(Player *player){
	assert(player != NULL);
	free_playlist(&player->playlist);
	free(player->matching_items.data);
	player->matching_items.data = NULL;
	player->matching_items.count = 0;
	player->matching_items.cap = 0;
	free(player->filter_prompt.data);
	player->filter_prompt.data = NULL;
	player->filter_prompt.count = 0;
	player->filter_prompt.cap = 0;
	if(player->current_audio_stream){
		SDL_DestroyAudioStream(player->current_audio_stream);
	}
}

static void fill_silence(SDL_AudioStream *stream, int amount){
	char buf[4*4096];
	memset(buf, 0, MIN(amount, (int)sizeof(buf)));
	while(amount > 0){
		int now = MIN(amount, (int)sizeof(buf));
		SDL_PutAudioStreamData(stream, buf, now);
		amount -= now;
	}
}

static void audio_stream_callback(void *userdata, SDL_AudioStream *stream, int additional_amount, int total_amount)
{
	Player *player = (Player*)userdata;

	if(player->paused){
		fill_silence(stream, additional_amount);
		return;
	}

	SDL_LockMutex(player->avmutex);

	while(additional_amount > 0){
		// get a new packet if we need one
		if(NULL == player->current_packet){
			player->current_frame = NULL;
			player->current_frame_sample = 0;
			AVPacket *packet = av_packet_alloc();
			while(1){
				// get a packet
				packet->stream_index = player->stream->index;
				if(0 == player->eof){
					int rc = av_read_frame(player->format_context, packet);
					if(rc >= 0){
						if(packet->stream_index != player->stream->index){
							continue;
						} else {
							break;
						}
					} else {
						// eof
						if(rc == AVERROR_EOF){
							player->eof = 1;
						}
						// eof
						if(player->format_context->pb && player->format_context->pb->eof_reached){
							player->eof = 1;
						}
						// actual error
						if(rc < 0 && 0 == player->eof){
							av_packet_free(&packet);
							packet = NULL;
						}
					}
				}
				if(player->eof){
					av_packet_free(&packet);
					packet = NULL;
					break;
				}
				if(NULL == packet){
					break;
				}
			}
			if(packet != NULL){
				player->current_packet = packet;
				int rc = avcodec_send_packet(player->codec_context, packet);
				if(rc < 0){
					log_err(ffmpegerr(rc));
				}
				assert(rc >= 0);
			}
		}
		if(player->eof){
			fill_silence(stream, additional_amount);
			break;
		}
		// just for now
		assert(player->current_packet != NULL);
		assert(player->current_packet->size > 0);

		// get a new frame if we need one. this may require us to get a new packet.
		if(NULL == player->current_frame){
			AVFrame *decoded_frame = av_frame_alloc();
			int rc = avcodec_receive_frame(player->codec_context, decoded_frame);
			if(rc == AVERROR(EAGAIN) || rc == AVERROR_EOF){
				av_frame_free(&decoded_frame);
				av_packet_free(&player->current_packet);
				player->current_packet = NULL;
			} else {
				assert(rc >= 0);
				player->current_frame = decoded_frame;
			}
		}
		if(NULL == player->current_frame){
			assert(player->current_packet == NULL);
			continue;
		}

		const i32 channel_count = player->codec_context->ch_layout.nb_channels;

		const int sample_size = av_get_bytes_per_sample(player->codec_context->sample_fmt);
		bool is_planar = 0;
		switch(player->codec_context->sample_fmt){
		case AV_SAMPLE_FMT_U8:
		case AV_SAMPLE_FMT_U8P:
		case AV_SAMPLE_FMT_S16P:
		case AV_SAMPLE_FMT_S32P:
		case AV_SAMPLE_FMT_FLTP:
			is_planar = 1;
			break;
		default:
			is_planar = 0;
			break;
		}

		i32 current_sample = player->current_frame_sample;
		// TODO: i think i should use linesize here, because for packed audio i don't count the number of samples in each channel, but for all channels.
		const i32 frame_sample_count = is_planar ? player->current_frame->nb_samples : channel_count * player->current_frame->nb_samples;
		assert(channel_count < 8);
		if(is_planar){
			int how_many_samples = frame_sample_count - current_sample;
			int how_many_bytes = how_many_samples * (channel_count * sample_size);
			if(how_many_bytes > additional_amount){
				how_many_samples = additional_amount / (channel_count * sample_size);
			}
			u8 const *ptrs[8];
			for(int ch = 0; ch < channel_count; ++ch){
				ptrs[ch] = player->current_frame->data[ch] + current_sample * sample_size;
			}
			SDL_PutAudioStreamPlanarData(stream, (const void * const*)ptrs, channel_count, how_many_samples);
			additional_amount -= how_many_samples * (channel_count * sample_size);
			current_sample += how_many_samples;
		} else {
			int how_many_samples = frame_sample_count - current_sample;
			int how_many_bytes = how_many_samples * channel_count;
			if(how_many_bytes > additional_amount){
				how_many_bytes = additional_amount;
				how_many_samples = how_many_bytes / channel_count;
			}
			SDL_PutAudioStreamData(stream, player->current_frame->data[0] + current_sample * sample_size, how_many_bytes);
			additional_amount -= how_many_bytes;
			current_sample += how_many_samples;
		}
		// Should have exhausted the current frame or provided enough data to the sdl audio stream (or both).
		assert(current_sample == frame_sample_count || 0 == additional_amount);

		player->last_relative_duration = (player->current_frame->pts + (player->current_frame_sample / (f32)player->current_frame->nb_samples) * player->current_frame->duration) / player->stream->duration;

		player->current_frame_sample = current_sample;
		if(player->current_frame_sample == frame_sample_count){
			av_frame_free(&player->current_frame);
			player->current_frame = NULL;
			player->current_frame_sample = 0;
		}

	}

	SDL_UnlockMutex(player->avmutex);
}

// TODO: iterate utf8 codepoints, draw unicode text. don't care about shaping
// everything correctly, but we should at least be able to draw more than
// ascii.
static void draw_text(SDL_Renderer *renderer, Glyph *ascii_glyphs, Slice text, float x, float y, float max_w){
	f32 cur_w = 0.0f;
	for(int i = 0; i < text.len; ++i){
		u8 c = (u8)text.str[i];
		if(c >= 0x20 && c < 127){
			if(ascii_glyphs[c].texture != NULL){
				SDL_FRect dstrect = {
					.x = x,
					.y = y,
					.w = ascii_glyphs[c].w,
					.h = ascii_glyphs[c].h,
				};
				// println(dstrect.x, dstrect.y, dstrect.w, dstrect.h);
				bool ok = SDL_RenderTexture(renderer, ascii_glyphs[c].texture, NULL, &dstrect);
				if(!ok){
					const char *err = SDL_GetError();
					eprintln("failed to render glyph ", err);
				}
			}
			x += ascii_glyphs[c].advance;
			cur_w += ascii_glyphs[c].advance;
			if(cur_w >= max_w)
				break;
		}
	}
}

static void draw_text_colored(SDL_Renderer *renderer, Glyph *ascii_glyphs, Slice text, float x, float y, float max_w, f32 h, SDL_Color bg){
	f32 cur_w = 0.0f;
	f32 begin_x = x;
	for(int i = 0; i < text.len; ++i){
		u8 c = (u8)text.str[i];
		if(c >= 0x20 && c < 127){
			x += ascii_glyphs[c].advance;
			cur_w += ascii_glyphs[c].advance;
			if(cur_w >= max_w)
				break;
		}
	}
	SDL_FRect rect = { .x = begin_x, .y = y, .w = cur_w, .h = h };
	SDL_Color back;
	SDL_GetRenderDrawColor(renderer, &back.r, &back.g, &back.b, &back.a);
	SDL_SetRenderDrawColor(renderer, bg.r, bg.g, bg.b, bg.a);
	SDL_RenderFillRect(renderer, &rect);
	SDL_SetRenderDrawColor(renderer, back.r, back.g, back.b, back.a);
	draw_text(renderer, ascii_glyphs, text, begin_x, y, max_w);
}


static Slice playlist_entry_name(Player *player, i32 i, bool fullpath){
	assert(i >= 0);
	assert(i < player->playlist.entries.count);
	const char *name_base = player->playlist.names.data;
	const MusicEntry *entry = &player->playlist.entries.data[i];
	const Slice name = {
		name_base + entry->path.start + (fullpath ? 0 : entry->name_offset),
		fullpath ? entry->path.len : entry->path.len - entry->name_offset
	};
	return name;
}

static f32 measure_text_advance(Glyph *ascii_glyphs, Slice text){
	f32 res = 0.0;
	for(i32 i = 0; i < text.len; ++i){
		res += ascii_glyphs[(u32)text.str[i]].advance;
	}
	return res;
}

static void draw_playlist(SDL_Renderer *renderer, Player *player, f32 x, f32 y){
	if(player->playlist.entries.count <= 0){
		return;
	}

	if(player->input_mode == InputDefault){
		draw_text(renderer, player->ascii_glyphs, (Slice){player->playlist.names.data + player->playlist.base_name.start, player->playlist.base_name.len}, x, y, player->window_width);
		y += player->font_line_skip;
	} else if(player->input_mode == InputFilter){
		draw_text(renderer, player->ascii_glyphs, S("Search: "), x, y, player->window_width);
		f32 x2 = measure_text_advance(player->ascii_glyphs, S("Search: "));
		Slice filter_query_text = {player->filter_prompt.data, player->filter_prompt.count};
		draw_text(renderer, player->ascii_glyphs, filter_query_text, x2, y, player->window_width);
		y += player->font_line_skip;
	}

	if(player->playlist_selected_idx < player->playlist_top){
		player->playlist_top = player->playlist_selected_idx;
	}
	// because of the header line, we do height - font_line_skip.
	const int num_visible_entries = (int)((player->playlist_height - player->font_line_skip)/ player->font_line_skip);
	int bottom = player->playlist_top + num_visible_entries;
	if(player->playlist_selected_idx >= bottom){
		player->playlist_top = player->playlist_selected_idx - num_visible_entries + 1;
	}
	const int relative_playlist_selected_idx = player->playlist_selected_idx - player->playlist_top;
	assertm(relative_playlist_selected_idx >= 0, relative_playlist_selected_idx, " ", player->playlist_top, " ", num_visible_entries);
	assertm(relative_playlist_selected_idx <= num_visible_entries, relative_playlist_selected_idx, " ", player->playlist_top, " ", num_visible_entries);
	int i = player->playlist_top;
	int max_i = player->input_mode == InputDefault ? player->playlist.entries.count : player->matching_items.count;
	while(1){
		if(i >= max_i)
			break;
		const i32 j = player->input_mode == InputDefault ? i : player->matching_items.data[i];
		Slice name = playlist_entry_name(player, j, false);
		if(i == player->playlist_selected_idx){
			draw_text_colored(renderer, player->ascii_glyphs, name, x, y, player->window_width, player->font_line_skip, (SDL_Color){.r=0x80, .g=0x80, .b=0x80, .a=0x80});
		} else {
			draw_text(renderer, player->ascii_glyphs, name, x, y, player->window_width);
		}
		y += player->font_line_skip;
		if(y >= player->playlist_height){
			break;
		}
		i++;
	}
}


static void draw_progress_bar(SDL_Renderer *renderer, Player *player, f32 x, f32 y){
	if(player->playlist_playing_idx < 0)
		return;

	// Technically this is a race condition. last_relative_duration is set by
	// the audio callback thread, but this function is called from the main
	// thread.  But I don't mind. Worst that can happen is that we read a stale
	// value for the progress bar.
	f32 w = player->last_relative_duration * player->max_progress_bar_width;
	SDL_SetRenderDrawColor(renderer, 0xff, 0xff, 0xff, 0xff);
	SDL_FRect rect = {.x=0, .y = y, .w = w, .h = player->font_line_skip, };
	SDL_RenderFillRect(renderer, &rect);
}

static void draw_ui_indicators(SDL_Renderer *renderer, Player *player, f32 x, f32 y){
	SDL_FRect rect = {.x = x-2, .y = y, .w = 2, .h = player->font_line_skip, };
	SDL_RenderFillRect(renderer, &rect);
	SDL_Color shuffle_bg = player->shuffle ? (SDL_Color){0x60, 0x60, 0x60, 0x60} : (SDL_Color){};
	SDL_Color auto_next_bg = player->auto_next ? (SDL_Color){0x60, 0x60, 0x60, 0x60} : (SDL_Color){};
	draw_text_colored(renderer, player->ascii_glyphs, S("S"), x, y, player->window_width - x, player->font_line_skip, shuffle_bg);
	x += player->ascii_glyphs['S'].advance;
	draw_text_colored(renderer, player->ascii_glyphs, S("X"), x, y, player->window_width - x, player->font_line_skip, auto_next_bg);
}

static void draw_currently_playing(SDL_Renderer *renderer, Player *player, f32 x, f32 y){
	if(player->playlist_playing_idx < 0)
		return;
	Slice name = playlist_entry_name(player, player->playlist_playing_idx, false);
	draw_text(renderer, player->ascii_glyphs, name, x, y, player->window_width);
}

//static void libavcodec_log_callback(void*,int,const char*, va_list){
//}


static Result player_load_audio(Player *player, Slice path)
{
	SDL_PauseAudioDevice(player->audio_device_id);

	SDL_LockMutex(player->avmutex);

	// free all existing decoding state.
	if(player->format_context){
		avformat_free_context(player->format_context);
		player->format_context = NULL;
	}
	if(player->stream){
		player->stream = NULL;
	}
	if(player->codec_context){
		avcodec_free_context(&player->codec_context);
		player->codec_context = NULL;
	}
	if(player->codec){
		player->codec = NULL;
	}
	player->sample_size = 0;
	player->audio_stream_idx = 0;
	player->eof = 0;
	if(player->current_packet){
		av_packet_free(&player->current_packet);
		player->current_packet = NULL;
	}
	if(player->current_frame){
		av_frame_free(&player->current_frame);
		player->current_frame = NULL;
		player->current_frame_sample = 0;
	}
	player->last_relative_duration = 0.0f;

	SDL_UnlockMutex(player->avmutex);

	AVFormatContext *format_ctx = avformat_alloc_context();
	if(format_ctx == NULL){
		return R(ErrAllocFailed);
	}

	int rc = avformat_open_input(&format_ctx, path.str, NULL, NULL);
	if(rc < 0){
		return ffmpegerr(rc);
	}
	rc = avformat_find_stream_info(format_ctx, NULL);
	if(rc < 0){
		return ffmpegerr(rc);
	}

	u32 audio_stream_idx = 0;
	for(; audio_stream_idx < format_ctx->nb_streams; audio_stream_idx += 1){
		if(format_ctx->streams[audio_stream_idx]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO){
			break;
		}
	}
	if(audio_stream_idx == format_ctx->nb_streams){
		return R(ErrNoAudioStream);
	}
	AVStream *stream = format_ctx->streams[audio_stream_idx];
	const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
	if(codec == NULL){
		return R(ErrNoCodecFound);
	}
	AVCodecContext *codec_context = avcodec_alloc_context3(codec);
	if(codec_context == NULL){
		return R(ErrAllocFailed);
	}
	avcodec_parameters_to_context(codec_context, stream->codecpar);
	AVDictionary **codec_options = NULL;
	rc = avcodec_open2(codec_context, codec, codec_options);
	if(rc < 0){
		return ffmpegerr(rc);
	}

	SDL_AudioSpec src_spec;
	src_spec.channels = codec_context->ch_layout.nb_channels;
	src_spec.freq = codec_context->sample_rate;
	int sample_size;
	switch(codec_context->sample_fmt){
		case AV_SAMPLE_FMT_U8:
		case AV_SAMPLE_FMT_U8P:
			src_spec.format = SDL_AUDIO_U8;
			sample_size = 1;
			break;
		case AV_SAMPLE_FMT_S16:
		case AV_SAMPLE_FMT_S16P:
			src_spec.format = SDL_AUDIO_S16;
			sample_size = 2;
			break;
		case AV_SAMPLE_FMT_S32:
		case AV_SAMPLE_FMT_S32P:
			src_spec.format = SDL_AUDIO_S32;
			sample_size = 4;
			break;
		case AV_SAMPLE_FMT_FLT:
		case AV_SAMPLE_FMT_FLTP:
			src_spec.format = SDL_AUDIO_F32;
			sample_size = 4;
			break;
		default:
			sample_size = 0;
			break;
	}
	// assertm(sample_size == 2, sample_size);
	// println("src spec ", src_spec.format,  ", ", src_spec.channels, ", ", src_spec.freq);
	// println("dst spec ", player->dst_audio_spec.format,  ", ", player->dst_audio_spec.channels, ", ", player->dst_audio_spec.freq);

	if(player->current_audio_stream){
		SDL_UnbindAudioStream(player->current_audio_stream);
		SDL_DestroyAudioStream(player->current_audio_stream);
		player->current_audio_stream = NULL;
	}
	SDL_AudioStream *audio_stream = SDL_CreateAudioStream(&src_spec, &player->dst_audio_spec);
	void *audio_callback_userdata = player;
	bool ok;
	ok = SDL_SetAudioStreamGetCallback(audio_stream, audio_stream_callback, audio_callback_userdata);
	assert(ok);
	ok = SDL_BindAudioStream(player->audio_device_id, audio_stream);
	assert(ok);
	player->current_audio_stream = audio_stream;

	player->format_context = format_ctx;
	player->stream = stream;
	player->codec_context = codec_context;
	player->audio_stream_idx = audio_stream_idx;
	player->sample_size = sample_size;
	SDL_ResumeAudioDevice(player->audio_device_id);

	return R(Ok);
}


static void update_window_height(Player *player, f32 w, f32 h){
	player->window_width = w;
	player->window_height = h;
	int row_count = (int)(w / player->font_line_skip);
	float bottom_pad = player->font_line_skip * 2;
	player->playlist_height = h - bottom_pad;
	player->max_progress_bar_width = w - player->ascii_glyphs['S'].advance - player->ascii_glyphs['X'].advance - 2;
}

static bool is_alpha(char c){
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static void update_playlist_filter(Player *player){
	// TODO: be smarter about resetting the selected index. try to keep the same track. otherwise take the closest idx that passes the filter.
	player->matching_items.count = 0;
	player->playlist_selected_idx = 0;
	player->playlist_top = 0;
	for(i32 i = 0; i < player->playlist.entries.count; ++i){
		i32 j = 0;
		i32 k = 0;
		bool ok = 1;
		Slice name = playlist_entry_name(player, i, false);
		// TODO: better fuzzy match with multiple separate words maybe?
		while(k < player->filter_prompt.count){
			if(j >= name.len){
				ok = 0;
				break;
			}
			const char a = name.str[j];
			const char b = player->filter_prompt.data[k];
			if(a == b || (is_alpha(a) && is_alpha(b) && (a | 32) == (b | 32))) {
				j++;
				k++;
			} else {
				j++;
				k = 0;
			}
		}
		if(ok){
			push_i32(&player->matching_items, i);
		}
	}
}


static bool point_in_box(f32 x, f32 y, f32 left, f32 top, f32 right, f32 bottom)
{
	return x >= left && x < right && y >= top && y < bottom;
}

static void set_next_track_to_play(Player *player){
	if(player->shuffle){
		// TODO: better random with some distribution guarantees? e.g.
		// maybe ensure we don't repeat songs before at least half of the
		// others in the playlist have played.
		if(player->history_cursor >= player->history.count){
			player->playlist_playing_idx = pcg32_boundedrand(&player->rng, player->playlist.entries.count);
			push_i32(&player->history, player->playlist_playing_idx);
		} else {
			player->playlist_playing_idx = player->history.data[player->history_cursor];
		}
		player->history_cursor += 1;
	} else {
		player->playlist_playing_idx = (player->playlist_playing_idx + 1) % player->playlist.entries.count;
	}
}


static void set_previous_track_to_play(Player *player){
	if(player->shuffle){
		if(player->history_cursor > 0){
			player->history_cursor -= 1;
			player->playlist_playing_idx = player->history.data[player->history_cursor];
		} else {
			player->playlist_playing_idx = -1;
		}
	} else {
		if(player->playlist_playing_idx > 0){
			player->playlist_playing_idx -= 1;
		} else {
			player->playlist_playing_idx = player->playlist.entries.count - 1;
		}
	}
}


static void load_and_play(Player *player){
	Slice path = playlist_entry_name(player, player->playlist_playing_idx, true);
	Result rc = player_load_audio(player, path);
	if(!okp(rc)){
		log_err(rc);
	} else {
		// TODO: I'm not sure if I always want this, but most of the time I think I want this.
		player->playlist_selected_idx = player->playlist_playing_idx;
	}
}

static void handle_text_input(Player *player, const SDL_TextInputEvent *ev){
	if(player->input_mode == InputFilter){
		i32 len = strlen(ev->text);
		insert_string(&player->filter_prompt, player->filter_prompt_cursor, ev->text, len);
		player->filter_prompt_cursor += len;
		update_playlist_filter(player);
	}
}

static void handle_key_event(Player *player, const SDL_KeyboardEvent *ev, bool is_down){
	if(is_down){
		if(player->input_mode == InputDefault){
			if(ev->key == SDLK_ESCAPE || ev->key == SDLK_Q){
				player->want_to_quit = 1;
			}
			if(player->playlist_playing_idx >= 0 && ev->key == SDLK_SPACE){
				player->paused = !player->paused;
				if(player->paused){
					SDL_PauseAudioDevice(player->audio_device_id);
				} else {
					SDL_ResumeAudioDevice(player->audio_device_id);
				}
			}
			if(player->playlist.entries.count > 0){
				if(ev->key == SDLK_DOWN){
					player->playlist_selected_idx = (player->playlist_selected_idx + 1) % player->playlist.entries.count;
				}
				if(ev->key == SDLK_UP){
					player->playlist_selected_idx = (player->playlist_selected_idx - 1);
					if(player->playlist_selected_idx < 0){
						player->playlist_selected_idx = player->playlist.entries.count - 1;
					}
				}
				// TODO: if the entry is a directory. change directory, make new playlist
				if(ev->key == SDLK_RETURN){
					player->playlist_playing_idx = player->playlist_selected_idx;
					if(player->shuffle){
						push_i32(&player->history, player->playlist_playing_idx);
						player->history_cursor += 1;
					}
					load_and_play(player);
				}
			}


			if(ev->key == SDLK_X){
				player->auto_next = !player->auto_next;
			}
			if(ev->key == SDLK_S){
				player->shuffle = !player->shuffle;
			}

			if(ev->key == SDLK_G){
				if(player->playlist_playing_idx >= 0){
					player->playlist_selected_idx = player->playlist_playing_idx;
				}
			}

			if(ev->key == SDLK_F && ((ev->mod & SDL_KMOD_CTRL) != 0)){
				SDL_StartTextInput(player->window);
				player->previous_selected_idx = player->playlist_selected_idx;
				player->input_mode = InputFilter;
				update_playlist_filter(player);
			}

			if(ev->key == SDLK_N){
				set_next_track_to_play(player);
				load_and_play(player);
			}
			if(ev->key == SDLK_B){
				set_previous_track_to_play(player);
				if(player->playlist_playing_idx >= 0){
					load_and_play(player);
				} else {
					player->eof = 1;
				}
			}
		} else if(player->input_mode == InputFilter){
			if(player->filter_prompt_cursor > 0 && ev->key == SDLK_LEFT){
				player->filter_prompt_cursor -= 1;
			}
			if(player->filter_prompt_cursor < player->filter_prompt.count && ev->key == SDLK_RIGHT){
				player->filter_prompt_cursor += 1;
			}
			if(player->filter_prompt_cursor > 0 && ev->key == SDLK_BACKSPACE){
				player->filter_prompt_cursor -= 1;
				delete_chars(&player->filter_prompt, player->filter_prompt_cursor, 1);
				update_playlist_filter(player);
			}
			if(ev->key == SDLK_UP){
				player->playlist_selected_idx -= 1;
				if(player->playlist_selected_idx < 0){
					player->playlist_selected_idx = player->matching_items.count - 1;
				}
			}
			if(ev->key == SDLK_DOWN){
				player->playlist_selected_idx = (player->playlist_selected_idx + 1) % player->matching_items.count;
			}
			if(ev->key == SDLK_RETURN){
				// TODO: should we keep the history and add this track to the list?
				player->history.count = 0;
				player->history_cursor = 0;
				player->playlist_selected_idx = player->matching_items.data[player->playlist_selected_idx];
				player->playlist_playing_idx = player->playlist_selected_idx;
				if(player->shuffle){
					push_i32(&player->history, player->playlist_playing_idx);
					player->history_cursor += 1;
				}
				load_and_play(player);
				player->input_mode = InputDefault;
				player->filter_prompt.count = 0;
				player->filter_prompt_cursor = 0;
			}
			if(ev->key == SDLK_ESCAPE){
				player->input_mode = InputDefault;
				player->playlist_selected_idx = player->previous_selected_idx;
				player->filter_prompt.count = 0;
				player->filter_prompt_cursor = 0;
			}
		}
	}
}

static void seek_to_mouse_cursor(Player *player, f32 x){
	const f32 progress_bar_y_start = player->playlist_height;
	const f32 progress_bar_y_end = player->playlist_height + player->font_line_skip;
	const f32 progress_bar_x_start = 0.0f;
	const f32 progress_bar_x_end = player->max_progress_bar_width;
	f32 relative = (x - progress_bar_x_start) / (progress_bar_x_end - progress_bar_x_start);
	int flags = 0;
	if(fabsf(relative - player->last_relative_duration) > 5e-3){
		if(relative < player->last_relative_duration){
			flags |= AVSEEK_FLAG_BACKWARD;
		}
		i64 timestamp_to_seek = (f32)player->stream->duration * relative;
		SDL_LockMutex(player->avmutex);
		av_seek_frame(player->format_context, player->audio_stream_idx, timestamp_to_seek, flags);
		player->last_relative_duration = relative;
		if(player->current_frame){
			av_frame_free(&player->current_frame);
			player->current_frame = NULL;
		}
		player->current_frame_sample = 0;
		if(player->current_packet){
			av_packet_free(&player->current_packet);
			player->current_packet = NULL;
		}
		SDL_UnlockMutex(player->avmutex);
	}
}

static void handle_mouse_motion_event(Player *player, const SDL_MouseMotionEvent *ev){
	if(player->seeking){
		seek_to_mouse_cursor(player, ev->x);
	}
}

static void handle_mouse_button_event(Player *player, const SDL_MouseButtonEvent *ev){
	if(ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN){
		const f32 progress_bar_y_start = player->playlist_height;
		const f32 progress_bar_y_end = player->playlist_height + player->font_line_skip;
		const f32 progress_bar_x_start = 0.0f;
		const f32 progress_bar_x_end = player->max_progress_bar_width;
		if(point_in_box(ev->x, ev->y, progress_bar_x_start, progress_bar_y_start, progress_bar_x_end, progress_bar_y_end)){
			player->seeking = 1;
			seek_to_mouse_cursor(player, ev->x);
		}
	} else {
		assert(ev->type == SDL_EVENT_MOUSE_BUTTON_UP);
		player->seeking = 0;
	}
}

int main(void){
	Player player = {};
	player.matching_items = make_i32list();
	player.history = make_i32list();
	player.filter_prompt = make_charlist();
	{
		struct timespec ts;
		timespec_get(&ts, TIME_UTC);
		pcg32_seed(&player.rng, (u64)ts.tv_sec, (u64)ts.tv_nsec);
	}
	player.playlist_playing_idx = -1;
	player.playlist = make_playlist_from_directory(S("/home/aru/Music"));
	//av_log_set_callback(libavcodec_log_callback);
	av_log_set_level(AV_LOG_QUIET);

	// SDL_SetHint(SDL_HINT_SHUTDOWN_DBUS_ON_QUIT, "1");
	SDL_Init(SDL_INIT_AUDIO);
	player.avmutex = SDL_CreateMutex();
	TTF_Init();
	player.dst_audio_spec = (SDL_AudioSpec){
		.format = SDL_AUDIO_S16,
		.channels = 2,
		.freq = 48000,
	};
	player.audio_device_id = SDL_OpenAudioDevice(
		SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &player.dst_audio_spec
	);
	if(player.audio_device_id == 0){
		eprintln("failed to open audio device");
		return 1;
	}
	SDL_PauseAudioDevice(player.audio_device_id);

	TTF_Font *font = NULL;
	{
		// The raw font bytes can only be freed once we close the font.  While
		// we're still rendering glyphs, we need to keep the memory around.
		// We embed the font statically, so it doesn't matter.
		SDL_IOStream *fontio = SDL_IOFromConstMem(font_bytes, sizeof(font_bytes));
		font = TTF_OpenFontIO(fontio, true, 16.0f);
		if(!font){
			const char *err = SDL_GetError();
			eprintln("failed to open font ", err);
		}
		assert(font != NULL);
	}

	player.window = SDL_CreateWindow("mos", 640, 480, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN);
	// the vulkan renderer implementation has weird glitches when rendering
	// quads and then rendering text above those quads.
	SDL_Renderer *renderer = SDL_CreateRenderer(player.window, "opengl,opengles2,vulkan,gpu,software");
	{
		int w,h;
		SDL_GetRenderOutputSize(renderer, &w, &h);
		player.window_width = (f32)w;
		player.window_height = (f32)h;
		player.playlist_height = (f32)h;
	}
	int numdrivers = SDL_GetNumRenderDrivers();
	SDL_SetRenderVSync(renderer, 1);

	player.font_line_skip = TTF_GetFontLineSkip(font);
	constexpr SDL_Color fontfg = {0xff, 0xff, 0xff, 0xff};
	for(u32 i = 0x20; i < 128; ++i){
		TTF_GetGlyphMetrics(font, i, NULL, NULL, NULL, NULL, &player.ascii_glyphs[i].advance);
		// skip space. don't need it
		if(i != 0x20){
			SDL_Surface *surface = TTF_RenderGlyph_Blended(font, i, fontfg);
			if(surface != NULL){
				player.ascii_glyphs[i].texture = SDL_CreateTextureFromSurface(renderer, surface);
				if(player.ascii_glyphs[i].texture != NULL){
					SDL_DestroySurface(surface);
					SDL_GetTextureSize(player.ascii_glyphs[i].texture, &player.ascii_glyphs[i].w, &player.ascii_glyphs[i].h);
				} else {
					const char *err = SDL_GetError();
					assertm(0, "failed to create texture for glyph ", i, ", ", err);
				}
			} else {
				const char *err = SDL_GetError();
				assertm(0, "failed to render glyph ", i, " to surface ", ", ", err);
			}
		}
	}
	TTF_CloseFont(font);

	update_window_height(&player, player.window_width, player.window_height);
	SDL_ShowWindow(player.window);

	while(!player.want_to_quit){
		SDL_Event ev;
		while(SDL_PollEvent(&ev)){
			switch(ev.type){
				case SDL_EVENT_TEXT_INPUT:
					handle_text_input(&player, &ev.text);
					break;
				case SDL_EVENT_KEY_DOWN:
				case SDL_EVENT_KEY_UP:
					handle_key_event(&player, &ev.key, ev.type == SDL_EVENT_KEY_DOWN);
					break;
				case SDL_EVENT_WINDOW_RESIZED:
					int w = ev.window.data1;
					int h = ev.window.data2;
					update_window_height(&player, (f32)w, (f32)h);
					break;
				case SDL_EVENT_QUIT:
					player.want_to_quit = 1;
					break;
				case SDL_EVENT_MOUSE_MOTION:
					handle_mouse_motion_event(&player, &ev.motion);
					break;
				case SDL_EVENT_MOUSE_BUTTON_UP:
				case SDL_EVENT_MOUSE_BUTTON_DOWN:
					handle_mouse_button_event(&player, &ev.button);
					break;
			}
		}

		if(player.eof && player.auto_next){
			set_next_track_to_play(&player);
			load_and_play(&player);
		}

		SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
		SDL_RenderClear(renderer);
		draw_playlist(renderer, &player, 0.0f, 0.0f);
		draw_progress_bar(renderer, &player, 0.0f, player.playlist_height);
		draw_ui_indicators(renderer, &player, player.max_progress_bar_width, player.playlist_height);
		draw_currently_playing(renderer, &player, 0.0f, player.playlist_height + player.font_line_skip);
		SDL_RenderPresent(renderer);
	}

	free_player(&player);
	TTF_Quit();
	SDL_CloseAudioDevice(player.audio_device_id);
	SDL_DestroyRenderer(renderer);
	SDL_DestroyWindow(player.window);
	SDL_Quit();
	return 0;
}
