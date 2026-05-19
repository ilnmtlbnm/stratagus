//       _________ __                 __
//      /   _____//  |_____________ _/  |______     ____  __ __  ______
//      \_____  \\   __\_  __ \__  \\   __\__  \   / ___\|  |  \/  ___/
//      /        \|  |  |  | \// __ \|  |  / __ \_/ /_/  >  |  /\___ |
//     /_______  /|__|  |__|  (____  /__| (____  /\___  /|____//____  >
//             \/                  \/          \//_____/            \/
//  ______________________                           ______________________
//                        T H E   W A R   B E G I N S
//         Stratagus - A free fantasy real time strategy game engine
//
/**@name sound_server.cpp - The sound server (hardware layer and so on) */
//
//      (c) Copyright 1998-2006 by Lutz Sammer, Fabrice Rossi, and
//                                 Jimmy Salmon
//
//      This program is free software; you can redistribute it and/or modify
//      it under the terms of the GNU General Public License as published by
//      the Free Software Foundation; only version 2 of the License.
//
//      This program is distributed in the hope that it will be useful,
//      but WITHOUT ANY WARRANTY; without even the implied warranty of
//      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//      GNU General Public License for more details.
//
//      You should have received a copy of the GNU General Public License
//      along with this program; if not, write to the Free Software
//      Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
//      02111-1307, USA.
//


//@{

/*----------------------------------------------------------------------------
--  Includes
----------------------------------------------------------------------------*/

#include <numeric>

#include "stratagus.h"

#include "dusty_log.h"
#include "sound_server.h"

#ifdef USE_FLUIDSYNTH
#include "fluidsynth.h"
#endif

#include "iolib.h"
#include "unit.h"

#include <SDL.h>
#include <SDL_mixer.h>

#ifdef _MSC_VER
# include <Shlobj.h>
#endif

#ifdef __EMSCRIPTEN__
#include <unordered_map>
#include <vector>
#include <atomic>

// dusty-bytes SFX bridge state (Phase 5.5 remesh).
// We bypass Mix_OpenAudio entirely so EMSCRIPTENAUDIO_OpenDevice never installs
// its scriptProcessorNode/silenceTimer dynCall("vp") callbacks.
// Channel bookkeeping is managed here rather than via SDL_mixer.

static int ems_next_chunk_id = 1;  // monotonic, never 0
// Maps Mix_Chunk* → chunkId for bridge calls. chunkId is passed to JS.
static std::unordered_map<Mix_Chunk*, int> ems_chunk_id_map;
// Per-channel tracking: which chunk is playing, for SampleIsPlaying / GetChannelSample.
static Mix_Chunk* ems_ch_sample[64] = {};  // MaxChannels=64
static std::atomic<bool> ems_ch_playing[64];  // true while JS source is active
static bool ems_audio_opened = false;
#endif

/*----------------------------------------------------------------------------
--  Variables
----------------------------------------------------------------------------*/

static const int NotYetLoadedMagic = static_cast<int>(0xcafebeef);

uint32_t SDL_SOUND_FINISHED;

static bool SoundInitialized;    /// is sound initialized
static bool MusicEnabled = true;
static bool EffectsEnabled = true;
static double VolumeScale = 1.0;
static int MusicVolume = 0;

#ifdef USE_WIN32
static bool externalPlayerIsPlaying = false;

static HANDLE g_hStatusThread;
static HANDLE g_hDebugThread;
static HANDLE g_hChildStd_IN_Wr;
static PROCESS_INFORMATION pi;

static DWORD WINAPI StatusThreadFunction(LPVOID lpParam) {
	CHAR chStatus;
	DWORD dwRead = 1;
	while (1) {
		if (!ReadFile((HANDLE)lpParam, &chStatus, 1, &dwRead, nullptr) || dwRead == 0) {
			CloseHandle((HANDLE)lpParam);
			break;
		}
		// any write means we finished
		if (externalPlayerIsPlaying) {
			externalPlayerIsPlaying = false;
			CallbackMusicTrigger();
		}
	}
	return 0;
}

static DWORD WINAPI DebugThreadFunction(LPVOID lpParam) {
	DWORD dwRead = 1;
	while (1) {
		char chStatus[1024] = {'\0'};
		if (!ReadFile((HANDLE)lpParam, chStatus, 1024, &dwRead, nullptr) || dwRead == 0) {
			CloseHandle((HANDLE)lpParam);
			break;
		}
		DebugPrint("%s", chStatus);
	}
	return 0;
}

static void KillPlayingProcess() {
	externalPlayerIsPlaying = false;
	if (g_hChildStd_IN_Wr) {
		TerminateProcess(pi.hProcess, 0);
		CloseHandle(g_hChildStd_IN_Wr);
		g_hChildStd_IN_Wr = nullptr;
		WaitForSingleObject(StatusThreadFunction, 0);
		WaitForSingleObject(DebugThreadFunction, 0);
	}
}

static bool External_Play(const std::string &file) {
	static bool useNativeMidi = !Mix_HasMusicDecoder("TIMIDITY") || SDL_GetHintBoolean("SDL_NATIVE_MUSIC", SDL_FALSE);
	if (!useNativeMidi) {
		return false;
	}
	if (fs::path(file).extension() == ".mid") {
		// midi file, use external player, since windows vista+ does not allow midi volume control independent of process volume
		fs::path full_filename = LibraryFileName(file);

		// try to communicate with the running midiplayer if we can
		if (g_hChildStd_IN_Wr != nullptr) {
			// already playing, just send the new song
			// XXX: timfel: disabled, since the midiplayer behaves weirdly when it receives the next file, just kill and restart
			KillPlayingProcess();
			/*
			// negative value signals a new filename
			int fileSize = full_filename.size() & 0xffff;
			char loSize = fileSize & 0xff;
			char hiSize = (fileSize >> 8) & 0xff;
			char buf[2] = {loSize, hiSize};
			externalPlayerIsPlaying = true;
			if (!WriteFile(g_hChildStd_IN_Wr, buf, 2, nullptr, nullptr)) {
				KillPlayingProcess();
			} else {
				// then write the filename
				if (!WriteFile(g_hChildStd_IN_Wr, full_filename.c_str(), fileSize, nullptr, nullptr)) {
					KillPlayingProcess();
				} else {
					return true;
				}
			}
			*/
		}
		// need to start an external player first

		// setup pipes to player
		HANDLE hChildStd_IN_Rd = nullptr;
		HANDLE hChildStd_OUT_Rd = nullptr;
		HANDLE hChildStd_OUT_Wr = nullptr;
		HANDLE hChildStd_ERR_Rd = nullptr;
		HANDLE hChildStd_ERR_Wr = nullptr;
		SECURITY_ATTRIBUTES saAttr;
		saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
		saAttr.bInheritHandle = TRUE;
		saAttr.lpSecurityDescriptor = nullptr;
		CreatePipe(&hChildStd_OUT_Rd, &hChildStd_OUT_Wr, &saAttr, 0);
		CreatePipe(&hChildStd_ERR_Rd, &hChildStd_ERR_Wr, &saAttr, 0);
		CreatePipe(&hChildStd_IN_Rd, &g_hChildStd_IN_Wr, &saAttr, 0);
		SetHandleInformation(g_hChildStd_IN_Wr, HANDLE_FLAG_INHERIT, 0);

		// start the process
#if defined(WIN32) && defined(UNICODE)
		auto volumeStr = std::to_wstring(std::min(MusicVolume, 127));
		auto full_filenameStr = full_filename.wstring();
#else
		auto volumeStr = std::to_string(std::min(MusicVolume, 127));
		auto full_filenameStr = full_filename.string();
#endif
		auto args = QuoteArguments({L("stratagus-midiplayer.exe"), volumeStr, full_filenameStr});
		auto cmd = std::accumulate(std::next(args.begin()), args.end(), args[0], [](const auto& lhs, const auto& rhs) { return lhs + L(" ") + rhs; });
		DebugPrint("Using external command to play midi on windows: %s\n", cmd.c_str());
		STARTUPINFO si;
		ZeroMemory(&si, sizeof(si));
		si.cb = sizeof(si);
		si.hStdError = hChildStd_ERR_Wr;
		si.hStdOutput = hChildStd_OUT_Wr;
		si.hStdInput = hChildStd_IN_Rd;
		si.dwFlags |= STARTF_USESTDHANDLES;
		ZeroMemory(&pi, sizeof(pi));
		bool result = true;
		TCHAR cmdline[4096]{};
		std::copy(cmd.begin(), cmd.end(), cmdline);
		if (CreateProcess(nullptr, cmdline, nullptr, nullptr, TRUE, /* Handles are inherited */ CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
			CloseHandle(hChildStd_OUT_Wr);
			CloseHandle(hChildStd_ERR_Wr);
			CloseHandle(hChildStd_IN_Rd);
			externalPlayerIsPlaying = true;
			g_hStatusThread = CreateThread(nullptr, 0, StatusThreadFunction, hChildStd_OUT_Rd, 0, nullptr);
			g_hDebugThread = CreateThread(nullptr, 0, DebugThreadFunction, hChildStd_ERR_Rd, 0, nullptr);
		} else {
			result = false;
			ErrorPrint("CreateProcess failed (%d).\n", GetLastError());
		}
		return result;
	}
	KillPlayingProcess();
	return false;
}

static bool External_IsPlaying() {
	return externalPlayerIsPlaying;
}

static bool External_Stop() {
	if (External_IsPlaying()) {
		KillPlayingProcess();
		return true;
	}
	return false;
}

static bool External_Volume(int volume, int oldVolume) {
	if (External_IsPlaying()) {
		char buf[2] = {0, volume & 0xFF};
		if (!WriteFile(g_hChildStd_IN_Wr, buf, 2, nullptr, nullptr)) {
			External_Stop();
			return false;
		}
		return true;
	}
	return false;
}
#elif defined(__EMSCRIPTEN__)
#include <emscripten.h>

static bool ems_music_playing = false;

// dusty-bytes: External_Play reads the OGG from MEMFS via FS.readFile
// (a synchronous JS call on the main thread) and hands encoded bytes to
// dusty.audio.playMusic. Uses MAIN_THREAD_EM_ASM_INT so that when the engine
// main loop runs on a pthread (Phase 5), the call is proxied to the Emscripten
// main thread where FS and self.dusty.audio are defined.
// Returns true so PlayMusic skips Mix_PlayMusic (avoids SDL_mixer OGG trap).
// Only handles long-form compressed music (.ogg/.opus/.mp3). SFX voice clips
// (.wav/.snd/.acm) and MIDI fall through (return false) to SDL_mixer's
// pre-decoded path, which is already async-clean.
static bool External_Play(const std::string &file) {
	// Only bridge long-form compressed music. SFX voice clips (.wav/.snd/.acm)
	// and MIDI fall through (return false) to SDL_mixer's pre-decoded path.
	// Use a lambda so no extra wasm function-table entry is created (adding a
	// new static function shifts ASYNCIFY's indirect-call table indices).
	auto has_music_ext = [](const std::string &s) -> bool {
		static const char *exts[] = { ".ogg", ".opus", ".mp3", nullptr };
		for (const char **e = exts; *e; ++e) {
			size_t n = strlen(*e);
			if (s.size() >= n && s.compare(s.size() - n, n, *e) == 0) return true;
		}
		return false;
	};
	if (!has_music_ext(file)) {
		return false;
	}
	fs::path full = LibraryFileName(file.c_str());
	int ok = MAIN_THREAD_EM_ASM_INT({
		try {
			var path = UTF8ToString($0);
			var name = UTF8ToString($1);
			if (typeof self === 'undefined' || !self.dusty || !self.dusty.audio || !self.dusty.audio.playMusic) {
				return 0;
			}
			var bytes = FS.readFile(path);
			if (!bytes || bytes.length === 0) {
				return 0;
			}
			var copy = new Uint8Array(bytes);
			self.dusty.audio.playMusic(name, copy, { loop: true });
			return 1;
		} catch(e) {
			return 0;
		}
	}, full.c_str(), file.c_str());
	if (ok) {
		ems_music_playing = true;
		DUSTY_LOG("audio", "warn", "[dusty.audio] external-play ok path=%s", full.c_str());
	} else {
		DUSTY_LOG("audio", "warn", "[dusty.audio] external-play file-not-found path=%s", full.c_str());
	}
	return ok != 0;
}

static bool External_IsPlaying() {
	return ems_music_playing;
}

static bool External_Stop() {
	if (!ems_music_playing) return false;
	MAIN_THREAD_EM_ASM({
		if (typeof self !== 'undefined' && self.dusty && self.dusty.audio && self.dusty.audio.stopMusic) {
			self.dusty.audio.stopMusic();
		}
	});
	ems_music_playing = false;
	return true;
}

static bool External_Volume(int volume, int /*oldVolume*/) {
	MAIN_THREAD_EM_ASM({
		if (typeof self !== 'undefined' && self.dusty && self.dusty.audio && self.dusty.audio.setMusicVolume) {
			self.dusty.audio.setMusicVolume($0 / 128.0);
		}
	}, volume);
	return true;
}

#else
static bool External_Play(const std::string & /*file*/) {
	return false;
}
static bool External_IsPlaying() {
	return false;
}
static bool External_Stop() {
	return false;
}
static bool External_Volume(int /*volume*/, int /*oldVolume*/) {
	return false;
}
#endif

/// Channels for sound effects and unit speech
struct SoundChannel {
	std::unique_ptr<Origin> Unit;          /// pointer to unit, who plays the sound, if any
	void (*FinishedCallback)(int channel) = nullptr; /// Callback for when a sample finishes playing
};

#define MaxChannels 64     /// How many channels are supported

static SoundChannel Channels[MaxChannels];

void ChannelFinished(int channel);

/**
**  Check if this sound is already playing
*/
bool SampleIsPlaying(Mix_Chunk *sample)
{
#ifdef __EMSCRIPTEN__
	for (int i = 0; i < MaxChannels; ++i) {
		if (ems_ch_sample[i] == sample && ems_ch_playing[i].load()) {
			return true;
		}
	}
	return false;
#else
	for (int i = 0; i < MaxChannels; ++i) {
		if (Mix_GetChunk(i) == sample && Mix_Playing(i)) {
			return true;
		}
	}
	return false;
#endif
}

bool UnitSoundIsPlaying(const Origin &origin)
{
	if (origin.Id == 0) {
		return false;
	}
#ifdef __EMSCRIPTEN__
	for (int i = 0; i < MaxChannels; ++i) {
		if (Channels[i].Unit && origin.Id == Channels[i].Unit->Id && ems_ch_playing[i].load()) {
			return true;
		}
	}
	return false;
#else
	for (int i = 0; i < MaxChannels; ++i) {
		if (Channels[i].Unit && origin.Id == Channels[i].Unit->Id && Mix_Playing(i)) {
			return true;
		}
	}
	return false;
#endif
}

/**
**  A channel is finished playing
*/
__attribute__((noinline,used)) void ChannelFinished(int channel)
{
	if (channel < 0 || channel >= MaxChannels) {
		ErrorPrint("ERROR: Out of bounds channel (how?)\n");
		return;
	}
	if (Channels[channel].FinishedCallback != nullptr) {
		SDL_Event event;
		SDL_zero(event);
		event.type = SDL_SOUND_FINISHED;
		event.user.code = channel;
		event.user.data1 = (void*) Channels[channel].FinishedCallback;
		SDL_PeepEvents(&event, 1, SDL_ADDEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT);
	}
	Channels[channel].Unit = nullptr;
}

__attribute__((noinline,used)) void HandleSoundEvent(SDL_Event &event)
{
	((void (*)(int channel))(event.user.data1))((int) event.user.code);
}

/**
**  Set the channel volume
**
**  @param channel  Channel to set
**  @param volume   New volume 0-255, <0 will not set the volume
**
**  @return         Current volume of the channel, -1 for error
*/
int SetChannelVolume(int channel, int volume)
{
#ifdef __EMSCRIPTEN__
	int sdl_vol = static_cast<int>(volume * VolumeScale);
	MAIN_THREAD_EM_ASM({
		if (typeof self !== 'undefined' && self.dusty && self.dusty.audio && self.dusty.audio.setSfxVolume) {
			self.dusty.audio.setSfxVolume($0, $1);
		}
	}, channel, sdl_vol);
	return sdl_vol;
#else
	return Mix_Volume(channel, volume * VolumeScale);
#endif
}

/**
**  Set the channel stereo
**
**  @param channel  Channel to set
**  @param stereo   -128 to 127, out of range will not set the stereo
**
**  @return         Current stereo of the channel, -1 for error
*/
void SetChannelStereo(int channel, int stereo)
{
#ifdef __EMSCRIPTEN__
	// Panning not implemented in the SFX bridge; stereo is ignored.
	(void)channel; (void)stereo;
#else
	if (Preference.StereoSound == false) {
		Mix_SetPanning(channel, 255, 255);
	} else {
		int left, right;
		if (stereo > 0) {
			left = 255 - stereo;
			right = 255;
		} else {
			left = 255;
			right = 255 + stereo;
		}
		Mix_SetPanning(channel, left, right);
	}
#endif
}

/**
**  Get the sample playing on a channel
*/
Mix_Chunk *GetChannelSample(int channel)
{
#ifdef __EMSCRIPTEN__
	if (channel >= 0 && channel < MaxChannels && ems_ch_playing[channel].load()) {
		return ems_ch_sample[channel];
	}
	return nullptr;
#else
	if (Mix_Playing(channel)) {
		return Mix_GetChunk(channel);
	}
	return nullptr;
#endif
}

static void ems_halt_channel(int channel);

/**
**  Stop a channel
**
**  @param channel  Channel to stop
*/
void StopChannel(int channel)
{
#ifdef __EMSCRIPTEN__
	ems_halt_channel(channel);
#else
	Mix_HaltChannel(channel);
#endif
}

/**
**  Stop all channels
*/
void StopAllChannels()
{
#ifdef __EMSCRIPTEN__
	ems_halt_channel(-1);
#else
	Mix_HaltChannel(-1);
#endif
}

static Mix_Music *currentMusic = nullptr;

static Mix_Music *LoadMusic(const char *name)
{
	if (currentMusic) {
		StopMusic();
		Mix_FreeMusic(currentMusic);
	}
	currentMusic = Mix_LoadMUS(name);
	if (currentMusic) {
		return currentMusic;
	}

	auto f = std::make_unique<CFile>();
	if (f->open(name, CL_OPEN_READ) == -1) {
		printf("Can't open file '%s'\n", name);
		return nullptr;
	}
	currentMusic = Mix_LoadMUS_RW(CFile::to_SDL_RWops(std::move(f)), 1);
	return currentMusic;
}

static sdl2::ChunkPtr ForceLoadSample(const char *name)
{
#ifdef __EMSCRIPTEN__
	// dusty-bytes SFX bridge (Phase 5.5): read file bytes via CFile (uses MEMFS),
	// allocate a real Mix_Chunk so the engine can read ->allocated / ->volume safely,
	// assign a monotonic chunkId, and store the pointer→chunkId mapping for bridge calls.
	// We do NOT call Mix_LoadWAV because there is no open audio device (Mix_OpenAudio
	// is skipped to prevent EMSCRIPTENAUDIO_OpenDevice installing dynCall("vp") handlers).
	auto f = std::make_unique<CFile>();
	if (f->open(name, CL_OPEN_READ) == -1) {
		DUSTY_LOG("audio", "warn", "[dusty.audio] sfx file-not-found path=%s", name);
		return nullptr;
	}
	// Read file bytes
	f->seek(0, SEEK_END);
	long sz = f->tell();
	f->seek(0, SEEK_SET);
	if (sz <= 0) {
		DUSTY_LOG("audio", "warn", "[dusty.audio] sfx empty path=%s", name);
		return nullptr;
	}
	Uint8 *buf = static_cast<Uint8 *>(SDL_malloc(static_cast<size_t>(sz)));
	if (!buf) return nullptr;
	if (f->read(buf, static_cast<size_t>(sz)) != static_cast<int>(sz)) {
		SDL_free(buf);
		DUSTY_LOG("audio", "warn", "[dusty.audio] sfx read-fail path=%s", name);
		return nullptr;
	}

	// Allocate a real Mix_Chunk so field accesses work; abuf holds the raw bytes.
	Mix_Chunk *chunk = static_cast<Mix_Chunk *>(SDL_calloc(sizeof(Mix_Chunk), 1));
	if (!chunk) { SDL_free(buf); return nullptr; }
	chunk->allocated = 1;    // FreeSample checks this to call Mix_FreeChunk
	chunk->abuf = buf;
	chunk->alen = static_cast<Uint32>(sz);
	chunk->volume = MIX_MAX_VOLUME;

	// Register chunk → chunkId
	int cid = ems_next_chunk_id++;
	ems_chunk_id_map[chunk] = cid;
	DUSTY_LOG("audio", "warn", "[dusty.audio] sfx loaded path=%s chunkId=%d bytes=%ld", name, cid, sz);
	return sdl2::ChunkPtr{chunk};
#else
	sdl2::ChunkPtr r{Mix_LoadWAV(name)};
	if (r) {
		return r;
	}
	auto f = std::make_unique<CFile>();
	if (f->open(name, CL_OPEN_READ) == -1) {
		printf("Can't open file '%s'\n", name);
		return nullptr;
	}
	return sdl2::ChunkPtr{Mix_LoadWAV_RW(CFile::to_SDL_RWops(std::move(f)), 1)};
#endif
}

static sdl2::ChunkPtr LoadSample(const char *name)
{
#ifdef DYNAMIC_LOAD
	sdl2::ChunkPtr r{(Mix_Chunk *) SDL_calloc(sizeof(Mix_Chunk), 1)};
	r->allocated = NotYetLoadedMagic;
	r->abuf = (Uint8 *)(strdup(name));
	return r;
#else
	return ForceLoadSample(name);
#endif
}

/**
**  Load a music file
**
**  @param name  File name
**
**  @return      Mix_Music pointer
*/
Mix_Music *LoadMusic(const std::string &name)
{
	const fs::path filename = LibraryFileName(name);
	Mix_Music *music = LoadMusic(filename.string().c_str());

	if (music == nullptr) {
		ErrorPrint("Can't load the music '%s'\n", name.c_str());
	}
	return music;
}

/**
**  Load a sample
**
**  @param name  File name of sample (short version).
**
**  @return      General sample loaded from file into memory.
**
**  @todo  Add streaming, caching support.
*/
sdl2::ChunkPtr LoadSample(const std::string &name)
{
	const fs::path filename = LibraryFileName(name);
	auto sample = LoadSample(filename.string().c_str());

	if (sample == nullptr) {
		ErrorPrint("Can't load the sound '%s': %s\n", name.c_str(), Mix_GetError());
	}
	return sample;
}

/**
 * Free a sample loaded with LoadSample.
 *
 * @param sample
 */
void FreeSample(Mix_Chunk *sample)
{
	if (sample == nullptr) {
		return;
	}
#ifdef DYNAMIC_LOAD
	if (sample->allocated == NotYetLoadedMagic) {
		free(sample->abuf);
		SDL_free(sample);
		return;
	}
#endif
#ifdef __EMSCRIPTEN__
	// Notify JS to drop cached AudioBuffer, then free the C-side allocation.
	auto it = ems_chunk_id_map.find(sample);
	if (it != ems_chunk_id_map.end()) {
		int cid = it->second;
		ems_chunk_id_map.erase(it);
		MAIN_THREAD_EM_ASM({
			if (typeof self !== 'undefined' && self.dusty && self.dusty.audio && self.dusty.audio.freeSfx) {
				self.dusty.audio.freeSfx($0);
			}
		}, cid);
	}
	SDL_free(sample->abuf);
	SDL_free(sample);
#else
	if (sample->allocated == 1) {
		Mix_FreeChunk(sample);
	}
#endif
}

/**
**  Play a sound sample
**
**  @param sample  Sample to play
**
**  @return        Channel number, -1 for error
*/
#ifdef __EMSCRIPTEN__
// Halt a channel (or all channels if channel==-1) via the bridge + fire ChannelFinished.
static void ems_halt_channel(int channel)
{
	auto do_halt = [](int ch) {
		if (ch < 0 || ch >= MaxChannels) return;
		if (!ems_ch_playing[ch].load()) return;
		ems_ch_playing[ch].store(false);
		ems_ch_sample[ch] = nullptr;
		// Invoke ChannelFinished to keep engine bookkeeping coherent.
		ChannelFinished(ch);
	};
	if (channel == -1) {
		for (int i = 0; i < MaxChannels; ++i) do_halt(i);
		MAIN_THREAD_EM_ASM({
			if (typeof self !== 'undefined' && self.dusty && self.dusty.audio && self.dusty.audio.stopSfx) {
				self.dusty.audio.stopSfx(-1);
			}
		});
	} else {
		do_halt(channel);
		MAIN_THREAD_EM_ASM({
			if (typeof self !== 'undefined' && self.dusty && self.dusty.audio && self.dusty.audio.stopSfx) {
				self.dusty.audio.stopSfx($0);
			}
		}, channel);
	}
}

// Find a free channel on Emscripten (no SDL_mixer channel manager available).
static int ems_find_free_channel()
{
	for (int i = 0; i < MaxChannels; ++i) {
		if (!ems_ch_playing[i].load() && !Channels[i].Unit) {
			return i;
		}
	}
	// All channels busy — steal channel 0.
	ems_halt_channel(0);
	return 0;
}
#endif

static int PlaySample(Mix_Chunk *sample, Origin *origin, void (*callback)(int channel))
{
	int channel = -1;
	if (SoundEnabled() && EffectsEnabled && sample) {
		DebugPrint("play sample %d\n", sample->volume);
#ifdef DYNAMIC_LOAD
		if (sample->allocated == NotYetLoadedMagic) {
			char *name = (char*)(sample->abuf);
			if (auto loadedSample = ForceLoadSample(name)) {
#ifdef __EMSCRIPTEN__
				// Save the ForceLoadSample ptr (has the chunkId) before swap.
				Mix_Chunk *loaded_ptr = loadedSample.get();
				int saved_cid = 0;
				{
					auto it = ems_chunk_id_map.find(loaded_ptr);
					if (it != ems_chunk_id_map.end()) {
						saved_cid = it->second;
						ems_chunk_id_map.erase(it);
					}
				}
				std::swap(*sample, *loadedSample);
				// Now re-register sample (original ptr) with the saved chunkId.
				if (saved_cid != 0) {
					ems_chunk_id_map[sample] = saved_cid;
				} else {
					ems_chunk_id_map[sample] = ems_next_chunk_id++;
				}
#else
				std::swap(*sample, *loadedSample);
#endif
			} else {
				return -1;
			}
		}
#endif

#ifdef __EMSCRIPTEN__
		// Bridge path: find a free channel, set up bookkeeping, send bytes to JS.
		channel = ems_find_free_channel();
		if (channel >= 0 && channel < MaxChannels) {
			Channels[channel].FinishedCallback = callback;
			if (origin && origin->Base) {
				auto src = std::make_unique<Origin>();
				src->Base = origin->Base;
				src->Id = origin->Id;
				Channels[channel].Unit = std::move(src);
			}
			ems_ch_sample[channel] = sample;
			ems_ch_playing[channel].store(true);

			int cid = 0;
			auto it = ems_chunk_id_map.find(sample);
			if (it != ems_chunk_id_map.end()) cid = it->second;
			int sdl_vol = static_cast<int>(sample->volume * VolumeScale);
			// Pass bytes from abuf to JS bridge via MAIN_THREAD_EM_ASM.
			// abuf is valid until FreeSample; JS decodes asynchronously and caches.
			MAIN_THREAD_EM_ASM({
				if (typeof self !== 'undefined' && self.dusty && self.dusty.audio && self.dusty.audio.playSfx) {
					var bytes = new Uint8Array(HEAPU8.buffer, $2, $3);
					self.dusty.audio.playSfx($0, $1, 0, $4, bytes);
				}
			}, cid, channel, (uintptr_t)sample->abuf, (int)sample->alen, sdl_vol);

			// Fire ChannelFinished immediately: no async audio completion signal from
			// the bridge back to C, and the engine uses ChannelFinished to reset
			// bookkeeping (Unit ptr) and allow FreeSample. The JS bridge plays async;
			// we mark C-side "not playing" immediately to avoid stale channel locks.
			// This mirrors the iter-3 Mix_HaltChannel approach.
			ems_ch_playing[channel].store(false);
			ems_ch_sample[channel] = nullptr;
			ChannelFinished(channel);
			// Keep Channels[channel].Unit = nullptr (ChannelFinished clears it).
		}
#else
		channel = Mix_PlayChannel(-1, sample, 0);
		if (channel >= 0 && channel < MaxChannels) {
			Channels[channel].FinishedCallback = callback;
			if (origin && origin->Base) {
				auto source = std::make_unique<Origin>();
				source->Base = origin->Base;
				source->Id = origin->Id;
				Channels[channel].Unit = std::move(source);
			}
		}
#endif
	}
	return channel;
}

int PlaySample(Mix_Chunk *sample, Origin *origin)
{
	return PlaySample(sample, origin, nullptr);
}

int PlaySample(Mix_Chunk *sample, void (*callback)(int channel))
{
	return PlaySample(sample, nullptr, callback);
}

/**
**  Set the global sound volume.
**
**  @param volume  the sound volume 0-255
*/
void SetEffectsVolume(int volume)
{
	VolumeScale = (volume * 1.0) / 255.0;
}

/**
**  Get effects volume
*/
int GetEffectsVolume()
{
	return VolumeScale * 255;
}

/**
**  Set effects enabled
*/
void SetEffectsEnabled(bool enabled)
{
	EffectsEnabled = enabled;
}

/**
**  Check if effects are enabled
*/
bool IsEffectsEnabled()
{
	return EffectsEnabled;
}

/*----------------------------------------------------------------------------
--  Music
----------------------------------------------------------------------------*/

/**
**  Set the music finished callback
*/
void SetMusicFinishedCallback(void (*callback)())
{
#ifndef __EMSCRIPTEN__
	// On Emscripten: Mix_OpenAudio is skipped (audio_opened==0); music is handled
	// by External_Play / dusty.audio.playMusic. No SDL_mixer music callback needed.
	Mix_HookMusicFinished(callback);
#else
	(void)callback;
#endif
}

/**
**  Play a music file. This also enables the music finished callback.
**
**  @param file  Name of music file, format is automatically detected.
**
**  @return      0 if music is playing, -1 if not.
*/
int PlayMusic(const std::string &file)
{
	if (!SoundEnabled() || !IsMusicEnabled()) {
		return -1;
	}
	DebugPrint("play music %s\n", file.c_str());
	CallbackMusicEnable();

	if (External_Play(file)) {
		return 0;
	}

#ifdef __EMSCRIPTEN__
	// On Emscripten: Mix_OpenAudio was skipped; Mix_LoadMUS / Mix_PlayMusic would
	// crash without audio_opened. Non-OGG music (e.g. MIDI) is unsupported — skip.
	ErrorPrint("PlayMusic: unsupported format on Emscripten '%s'\n", file.c_str());
	return -1;
#else
	Mix_Music *music = LoadMusic(file);
	if (music) {
		Mix_PlayMusic(music, 0);
		return 0;
	} else {
		ErrorPrint("Could not play '%s'\n", file.c_str());
		return -1;
	}
#endif
}

/**
**  Stop the current playing music. This does not trigger the music finished callback.
*/
__attribute__((noinline, used)) void StopMusic()
{
	CallbackMusicDisable();
	if (!External_Stop()) {
		Mix_HaltMusic();
	}
	CallbackMusicEnable();
}

/**
**  Set the music volume.
**
**  @param volume  the music volume 0-255
*/
__attribute__((noinline, used)) void SetMusicVolume(int volume)
{
	// due to left-right separation, sound effect volume is effectively halfed,
	// so we adjust the music
	int oldVolume = MusicVolume;
	MusicVolume = volume;
#ifndef __EMSCRIPTEN__
	Mix_VolumeMusic(MusicVolume);
#endif
	External_Volume(MusicVolume, oldVolume);
}

/**
**  Get music volume
*/
int GetMusicVolume()
{
	return MusicVolume;
}

/**
**  Set music enabled
*/
void SetMusicEnabled(bool enabled)
{
	if (enabled) {
		MusicEnabled = true;
	} else {
		MusicEnabled = false;
		StopMusic();
	}
}

/**
**  Check if music is enabled
*/
bool IsMusicEnabled()
{
	return MusicEnabled;
}

/**
**  Check if music is playing
*/
__attribute__((noinline, used)) bool IsMusicPlaying()
{
#ifdef __EMSCRIPTEN__
	return External_IsPlaying();
#else
	return Mix_PlayingMusic() || External_IsPlaying();
#endif
}

/*----------------------------------------------------------------------------
--  Init
----------------------------------------------------------------------------*/

/**
**  Check if sound is enabled
*/
bool SoundEnabled()
{
	return SoundInitialized;
}

/**
**  Initialize sound card hardware part with SDL.
**
**  @param freq  Sample frequency (44100,22050,11025 hz).
**  @param size  Sample size (8bit, 16bit)
**
**  @return      false if failure, true if everything ok.
*/
static bool InitSdlSound()
{
#ifdef __EMSCRIPTEN__
	// dusty-bytes Phase 5.5 (SFX bridge remesh): Skip Mix_OpenAudio entirely on
	// Emscripten. Without Mix_OpenAudio, EMSCRIPTENAUDIO_OpenDevice never runs,
	// so no ScriptProcessorNode / onaudioprocess / silenceTimer dynCall("vp") path
	// is installed — the entire crash category is removed at the source.
	//
	// SDL_mixer functions that require audio_opened (Mix_PlayChannel, Mix_HaltChannel,
	// Mix_Playing, Mix_GetChunk, Mix_SetPanning, etc.) are bypassed by our #ifdef
	// __EMSCRIPTEN__ shims above. Mix_LoadWAV is replaced by ForceLoadSample's
	// EMSCRIPTEN branch which reads from MEMFS via CFile directly.
	//
	// Mix_AllocateChannels and Mix_ChannelFinished require audio_opened; we skip
	// them too — channel bookkeeping lives in ems_ch_playing[] / ems_ch_sample[].
	// ChannelFinished() is called synchronously from ems_halt_channel().
	//
	// Music continues through dusty.audio.playMusic (External_Play branch, unchanged).
	DUSTY_LOG("audio", "warn", "[dusty.audio] SFX bridge init — Mix_OpenAudio skipped");
	ems_audio_opened = true;
	return true;
#else
	fs::path timidityCfg(StratagusLibPath);
	timidityCfg = timidityCfg / "timidity" / "timidity.cfg";
#ifndef BUILD_VENDORED_SDL
	const char *cfg = SDL_getenv("TIMIDITY_CFG");
	if (!cfg && fs::exists(timidityCfg)) {
		SDL_setenv("TIMIDITY_CFG", timidityCfg.generic_u8string().c_str(), 0);
	} else {
		SDL_setenv("TIMIDITY_CFG", (GetExecutablePath().parent_path() / "freepats" / "crude.cfg").generic_u8string().c_str(), 0);
	}
#else
	if (fs::exists(timidityCfg)) {
		Mix_SetTimidityCfg(timidityCfg.generic_u8string().c_str());
	} else {
		Mix_SetTimidityCfg((GetExecutablePath().parent_path() / "freepats" / "crude.cfg").generic_u8string().c_str());
	}
#endif
	// just activate everything we can by setting all bits
	Mix_Init(std::numeric_limits<unsigned int>::max());
	if (Mix_OpenAudio(MIX_DEFAULT_FREQUENCY, MIX_DEFAULT_FORMAT, MIX_DEFAULT_CHANNELS, 1024)) {
		ErrorPrint("Couldn't open audio: %s\n", SDL_GetError());
		return false;
	} else {
		printf("Supported sound decoders:");
		for (int i = 0; i < Mix_GetNumChunkDecoders(); i++) {
			printf(" %s", Mix_GetChunkDecoder(i));
		}
		printf("\nSupported music decoders:");
		for (int i = 0; i < Mix_GetNumMusicDecoders(); i++) {
			printf(" %s", Mix_GetMusicDecoder(i));
		}
		printf("\n");
	}
	return true;
#endif
}

/**
**  Initialize sound card.
**
**  @return  false if failure, true if everything ok.
*/
bool InitSound()
{
	//
	// Open sound device, 8bit samples, stereo.
	//
	if (!InitSdlSound()) {
		SoundInitialized = false;
		return false;
	}
	SDL_SOUND_FINISHED = SDL_RegisterEvents(1);
	SoundInitialized = true;
#ifndef __EMSCRIPTEN__
	// On Emscripten: Mix_OpenAudio is skipped, so audio_opened==0 and
	// Mix_AllocateChannels / Mix_ChannelFinished / Mix_ResumeMusic / Mix_Resume
	// would fail. Channel management uses ems_ch_playing[] / ems_ch_sample[] instead.
	Mix_AllocateChannels(MaxChannels);
	Mix_ChannelFinished(ChannelFinished);
	// Now we're ready for the callback to run
	Mix_ResumeMusic();
	Mix_Resume(-1);
#endif
	return true;
}

/**
**  Cleanup sound server.
*/
void QuitSound()
{
#ifndef __EMSCRIPTEN__
	Mix_CloseAudio();
	Mix_Quit();
#endif
	SoundInitialized = false;
}

//@}
