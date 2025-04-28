#include "def.h"

#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_oldnames.h>
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

void qsort_r(
	void *base,
	size_t n,
	size_t size,
	typeof(int (const void *, const void *, void *)) *compar, 
	void *arg
);

typedef enum {
	Ok,
	ErrNoAudioStream,
	ErrNoCodecFound,
	ErrAllocFailed,
	ErrFfmpeg,
} ResultTag;

typedef enum {
	KeyNone,
	KeyEsc,
	KeySpace,
	KeyUp,
	KeyDown,
	KeyEnter,
	KeyCount,
} KeyId;

typedef struct {
	bool down;
	int changes;
} KeyPresses;

typedef struct {
	KeyPresses keys[KeyCount];
} InputState;;

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
	// int x, y;
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
	X(xm)\
	X(mod)\
	X(it)\

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
	ExtensionId ext;
	i64 mtime;
} MusicEntry;

typedef struct {
	char *data;
	i32 count;
	i32 cap;
} CharList;

typedef struct {
	MusicEntry *data;
	i32 count;
	i32 cap;
} MusicEntryList;

typedef struct {
	MusicEntryList entries;
	CharList names;
} Playlist;

typedef struct {
	f32 window_height;
	f32 window_width;
	Glyph ascii_glyphs[128];
	f32 font_line_skip;

	Playlist playlist;
	i32 playlist_selected_idx;
	i32 playlist_top;
	i32 playlist_playing_idx;

	SDL_AudioDeviceID audio_device_id;
	SDL_AudioSpec dst_audio_spec;
	SDL_AudioStream *current_audio_stream;

	ByteRange encoded_data;

	AVFormatContext *format_context;
	AVIOContext *avio_context;
	AVStream *stream;
	AVCodecContext *codec_context;
	AVCodec *codec;
	i32 sample_size;
	i32 audio_stream_idx;
	bool eof;
	bool paused;

	AVPacket *current_packet;
	AVFrame *current_frame;
	i32 current_frame_sample;
} Player;

constexpr u8 font_bytes[] = {
#embed "golos-ui.ttf"
};

static void log_err(Result r){
	switch(r.tag){
		case ErrFfmpeg:
			eprintln("err: ", r.tag, "(ffmpeg), return code: ", r.rc);
			break;
		default:
			eprintln("err: ", r.tag, " ", r.rc);
			break;
	}
}

static int compare_sub(const void *pa, const void *pb, void *arg){
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
		l->data = realloc(l->data, sizeof(l->data[0]) * l->cap);
	}
	memcpy(l->data + l->count, str, len);
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
		music_entry.path.len = fullpath.count;
		music_entry.ext = ext_id;
		music_entry.mtime = mtime;
		push_string(&names, fullpath.data, fullpath.count);
		push_entry(&entries, &music_entry);
	}
	free(fullpath.data);
	avio_close_dir(&dirp);
	qsort_r(entries.data, entries.count, sizeof(entries.data[0]), compare_sub, &names);
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
	if(player->current_audio_stream){
		SDL_DestroyAudioStream(player->current_audio_stream);
	}
}

static void audio_stream_callback(void *userdata, SDL_AudioStream *stream, int additional_amount, int total_amount)
{
#if 0
	println("callback ", additional_amount, " ", total_amount);
	char buf[4096];
	memset(buf, 0, sizeof(buf));
	int loop = 1;
	while(additional_amount > 0){
		int now = MIN(additional_amount, (int)sizeof(buf));
		SDL_PutAudioStreamData(stream, buf, now);
		additional_amount -= now;
	}
#endif

	Player *player = (Player*)userdata;
	char buf[4*4096];
	memset(buf, 0, sizeof(buf));

	if(player->paused){
		while(additional_amount > 0){
			int now = MIN(additional_amount, (int)sizeof(buf));
			SDL_PutAudioStreamData(stream, buf, now);
			additional_amount -= now;
		}
		return;
	}

	int put_count = 0;
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
						break;
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
							// TODO: error, so stop playback?
							av_packet_free(&packet);
							packet = NULL;
						}
					}
				}
				if(NULL == packet){
					break;
				}
				if(packet->stream_index != player->stream->index){
					continue;
				}
			}
			if(packet != NULL){
				player->current_packet = packet;
				int rc = avcodec_send_packet(player->codec_context, packet);
				assert(rc >= 0);
			}
		}
		if(player->eof){
			memset(buf, 0, sizeof(buf));
			while(additional_amount > 0){
				int now = MIN(additional_amount, (int)sizeof(buf));
				SDL_PutAudioStreamData(stream, buf, additional_amount);
				additional_amount -= sizeof(buf);
			}
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
		// just for now
		i32 channel_count = player->codec_context->ch_layout.nb_channels;
		assert(channel_count == 2);

		int sample_size = av_get_bytes_per_sample(player->codec_context->sample_fmt);
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
		while(current_sample < frame_sample_count){
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
			if(0 == additional_amount){
				break;
			}
		}
		player->current_frame_sample = current_sample;
		if(player->current_frame_sample == frame_sample_count){
			av_frame_free(&player->current_frame);
			player->current_frame = NULL;
			player->current_frame_sample = 0;
		}
	}
}

static int read_packet_callback(void *opaque, uint8_t *buf, int _buf_size)
{
	ByteRange *byte_range = (ByteRange *)opaque;
	assert(_buf_size >= 0);
	size_t buf_size = _buf_size;
	size_t to_copy = buf_size;
	size_t remaining = byte_range->end - byte_range->cur;
	if(to_copy > remaining)
		to_copy = remaining;
	
	if(0 == to_copy)
		return AVERROR_EOF;
	// printf("cur: %zu, remaining: %zu\n", byte_range->cur - byte_range->base, remaining);
	
	memcpy(buf, byte_range->cur, to_copy);
	byte_range->cur += to_copy;
	
	return _buf_size;
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
					println("failed to render glyph ", err);
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


static Slice playlist_entry_name(Player *player, i32 i){
	assert(i >= 0);
	assert(i < player->playlist.entries.count);
	const char *name_base = player->playlist.names.data;
	const MusicEntry *entry = &player->playlist.entries.data[i];
	const Slice name = { name_base + entry->path.start, entry->path.len };
	return name;
}


static void draw_playlist(SDL_Renderer *renderer, Player *player, f32 x, f32 y){
	if(player->playlist.entries.count <= 0){
		return;
	}

	if(player->playlist_selected_idx < player->playlist_top){
		player->playlist_top = player->playlist_selected_idx;
	}
	const int num_visible_entries = (int)(player->window_height / player->font_line_skip);
	int bottom = player->playlist_top + num_visible_entries;
	if(player->playlist_selected_idx >= bottom){
		player->playlist_top = player->playlist_selected_idx - num_visible_entries + 1;
	}
	const int relative_playlist_selected_idx = player->playlist_selected_idx - player->playlist_top;
	assertm(relative_playlist_selected_idx >= 0, relative_playlist_selected_idx, " ", player->playlist_top, " ", num_visible_entries);
	assertm(relative_playlist_selected_idx <= num_visible_entries, relative_playlist_selected_idx, " ", player->playlist_top, " ", num_visible_entries);
	int i = player->playlist_top;
	while(1){
		Slice name = playlist_entry_name(player, i);
		if(i == player->playlist_selected_idx){
			draw_text_colored(renderer, player->ascii_glyphs, name, x, y, player->window_width, player->font_line_skip, (SDL_Color){.r=0x80, .g=0x80, .b=0x80, .a=0x80});
		} else {
			draw_text(renderer, player->ascii_glyphs, name, x, y, player->window_width);
		}
		y += player->font_line_skip;
		if(y >= player->window_height){
			break;
		}
		i++;
	}
}

static Result player_load_audio(Player *player, Slice path)
{
	// TODO: i'm not sure if there's still a race condition where another thread
	// can be in the audio callback trying to produce new audio while we're here
	// trying to load new audio.  I don't think so, but if we ever get a weird
	// crash like that, we're gonna need a mutex.
	SDL_PauseAudioDevice(player->audio_device_id);

	// free all existing decoding state.
	if(player->format_context){
		avformat_free_context(player->format_context);
		player->format_context = NULL;
	}
	if(player->avio_context){
		avio_context_free(&player->avio_context);
		player->avio_context = NULL;
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
	}

	if(player->encoded_data.base){
		av_file_unmap((u8*)player->encoded_data.base, player->encoded_data.end - player->encoded_data.base);
		player->encoded_data.base = NULL;
		player->encoded_data.cur = NULL;
		player->encoded_data.end = NULL;
	}

	{
		size_t encoded_len;
		int rc = av_file_map(path.str, (u8**)&player->encoded_data.base, &encoded_len, 0, NULL);
		player->encoded_data.cur = player->encoded_data.base;
		player->encoded_data.end = player->encoded_data.base + encoded_len;
		if(rc < 0){
			return ffmpegerr(rc);
		}
	}

	AVFormatContext *format_ctx = avformat_alloc_context();
	if(format_ctx == NULL){
		return R(ErrAllocFailed);
	}
	constexpr size_t avio_ctx_buffer_size = 4096;
	u8 *avio_ctx_buffer = av_malloc(avio_ctx_buffer_size);
	AVIOContext *avio_ctx = avio_alloc_context(avio_ctx_buffer, avio_ctx_buffer_size, 0, &player->encoded_data, &read_packet_callback, NULL, NULL);
	if(avio_ctx == NULL){
		return R(ErrAllocFailed);
	}
	format_ctx->pb = avio_ctx;
	int rc = avformat_open_input(&format_ctx, NULL, NULL, NULL);
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
	println("src spec ", src_spec.format,  ", ", src_spec.channels, ", ", src_spec.freq);
	println("dst spec ", player->dst_audio_spec.format,  ", ", player->dst_audio_spec.channels, ", ", player->dst_audio_spec.freq);

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
	player->avio_context = avio_ctx;
	player->stream = stream;
	player->codec_context = codec_context;
	player->audio_stream_idx = audio_stream_idx;
	player->sample_size = sample_size;
	SDL_ResumeAudioDevice(player->audio_device_id);

	return R(Ok);
}

static bool key_was_just_pressed(const InputState *input_state, KeyId key){
	return input_state->keys[key].changes >= 2 
		|| (input_state->keys[key].down && input_state->keys[key].changes == 1);
}

int main(void){
	Player player = {};
	player.playlist_playing_idx = -1;
	InputState input_state = {};
	player.playlist = make_playlist_from_directory(S("/home/aru/Music/"));

	// SDL_SetHint(SDL_HINT_SHUTDOWN_DBUS_ON_QUIT, "1");
	SDL_Init(SDL_INIT_AUDIO);
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

	TTF_Font *font = NULL;
	{
		// The raw font bytes can only be freed once we close the font.  While
		// we're still rendering glyphs, we need to keep the memory around.
		// We embed the font statically, so it doesn't matter.
		SDL_IOStream *fontio = SDL_IOFromConstMem(font_bytes, sizeof(font_bytes));
		font = TTF_OpenFontIO(fontio, true, 16.0f);
		if(!font){
			const char *err = SDL_GetError();
			println("failed to open font ", err);
		}
		assert(font != NULL);
	}

	SDL_Window *window = SDL_CreateWindow("mos", 320, 240, 0);
	// the vulkan renderer implementation has weird glitches when rendering
	// quads and then rendering text above those quads.
	SDL_Renderer *renderer = SDL_CreateRenderer(window, "opengl,opengles2,vulkan,gpu,software");
	{
		int w,h;
		SDL_GetRenderOutputSize(renderer, &w, &h);
		player.window_width = (f32)w;
		player.window_height = (f32)h;
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

	SDL_PauseAudioDevice(player.audio_device_id);

	bool running = 1;
	while(running){
		for(int i = 0; i < KeyCount; ++i){
			input_state.keys[i].changes = 0;
		}

		SDL_Event ev;
		while(SDL_PollEvent(&ev)){
			KeyId key = KeyNone;
			switch(ev.type){
				case SDL_EVENT_KEY_DOWN:
				case SDL_EVENT_KEY_UP:
					switch(ev.key.key){
						case SDLK_ESCAPE: key = KeyEsc; break;
						case SDLK_SPACE: key = KeySpace; break;
						case SDLK_DOWN: key = KeyDown; break;
						case SDLK_UP: key = KeyUp; break;
						case SDLK_RETURN: key = KeyEnter; break;
						default: break;
					}
					if(key != KeyNone){
						assert(key < KeyCount);
						input_state.keys[key].down = ev.type == SDL_EVENT_KEY_DOWN;
						input_state.keys[key].changes++;
					}
					break;
				case SDL_EVENT_QUIT:
					running = 0;
					break;
			}
		}

		if(key_was_just_pressed(&input_state, KeyEsc)){
			running = 0;
		}
		if(player.playlist_playing_idx >= 0 && key_was_just_pressed(&input_state, KeySpace)){
			player.paused = !player.paused;
			if(player.paused){
				SDL_PauseAudioDevice(player.audio_device_id);
			} else {
				SDL_ResumeAudioDevice(player.audio_device_id);
			}
		}
		if(player.playlist.entries.count > 0){
			if(key_was_just_pressed(&input_state, KeyDown)){
				player.playlist_selected_idx = (player.playlist_selected_idx + 1) % player.playlist.entries.count;
			}
			if(key_was_just_pressed(&input_state, KeyUp)){
				player.playlist_selected_idx = (player.playlist_selected_idx - 1);
				if(player.playlist_selected_idx < 0){
					player.playlist_selected_idx = player.playlist.entries.count - 1;
				}
			}
			// TODO: if the entry is a directory. change directory, make new playlist
			if(key_was_just_pressed(&input_state, KeyEnter)){
				player.playlist_playing_idx = player.playlist_selected_idx;
				Slice path = playlist_entry_name(&player, player.playlist_selected_idx);
				Result rc = player_load_audio(&player, path);
				if(!okp(rc)){
					log_err(rc);
				}
			}
		}

		SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
		SDL_RenderClear(renderer);
		draw_playlist(renderer, &player, 0.0f, 0.0f);
		// draw_text(renderer, player.ascii_glyphs, S("hello world"), 0.0f, 0.0f);
		SDL_RenderPresent(renderer);
	}

	free_player(&player);
	TTF_Quit();
	SDL_CloseAudioDevice(player.audio_device_id);
	SDL_DestroyRenderer(renderer);
	SDL_DestroyWindow(window);
	SDL_Quit();
	return 0;
}
