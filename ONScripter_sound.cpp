/* -*- C++ -*-
 *
 *  ONScripter_sound.cpp - Methods for playing sound
 *
 *  Copyright (c) 2001-2020 Ogapee. All rights reserved.
 *
 *  ogapee@aqua.dti2.ne.jp
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "ONScripter.h"
#include <new>
#if defined(LINUX)
#include <signal.h>
#endif

#ifdef ANDROID
extern "C" void playVideoAndroid(const char* filename);
#endif

#if defined(IOS)
extern "C" void playVideoIOS(const char* filename, bool click_flag, bool loop_flag);
#endif

#if defined(USE_AVIFILE)
#include "AVIWrapper.h"
#endif

#if defined(USE_SMPEG)
extern "C" void mp3callback(void* userdata, Uint8* stream, int len)
{
    SMPEG_playAudio((SMPEG*)userdata, stream, len);
}
#endif

extern bool ext_music_play_once_flag;

extern "C" {
extern void musicFinishCallback();
extern Uint32 SDLCALL cdaudioCallback(Uint32 interval, void* param);
}
extern void midiCallback(int sig);
extern SDL_TimerID timer_cdaudio_id;

extern SDL_TimerID timer_bgmfade_id;
extern "C" Uint32 SDLCALL bgmfadeCallback(Uint32 interval, void* param);

#define TMP_MUSIC_FILE "tmp.mus"

int ONScripter::playSound(const char* filename, int format, bool loop_flag, int channel)
{
    if (!audio_open_flag) return SOUND_NONE;

    long length = script_h.cBR->getFileLength(filename);
    if (length == 0) return SOUND_NONE;

    unsigned char* buffer;

    if (format & SOUND_MUSIC &&
        length == music_buffer_length &&
        music_buffer) {
        buffer = music_buffer;
    }
    else {
        buffer = new (std::nothrow) unsigned char[length];
        if (buffer == NULL) {
            fprintf(stderr, "failed to load [%s] because file size [%lu] is too large.\n", filename, length);
            return SOUND_NONE;
        }
        script_h.cBR->getFile(filename, buffer);
    }

    if (format & SOUND_MUSIC) {
#if SDL_MIXER_MAJOR_VERSION >= 2
        music_info = Mix_LoadMUS_RW(SDL_RWFromMem(buffer, length), 0);
#else
        music_info = Mix_LoadMUS_RW(SDL_RWFromMem(buffer, length));
#endif
        Mix_VolumeMusic(music_volume);
        Mix_HookMusicFinished(musicFinishCallback);
        if (Mix_PlayMusic(music_info, (music_play_loop_flag && music_loopback_offset == 0.0) ? -1 : 0) == 0) {
            music_buffer = buffer;
            music_buffer_length = length;
            return SOUND_MUSIC;
        }
    }

    if (format & SOUND_CHUNK) {
        Mix_Chunk* chunk = Mix_LoadWAV_RW(SDL_RWFromMem(buffer, length), 1);
        if (playWave(chunk, format, loop_flag, channel) == 0) {
            delete[] buffer;
            return SOUND_CHUNK;
        }
    }

    /* check WMA */
    if (buffer[0] == 0x30 && buffer[1] == 0x26 &&
        buffer[2] == 0xb2 && buffer[3] == 0x75) {
        delete[] buffer;
        return SOUND_OTHER;
    }

    if (format & SOUND_MIDI) {
        FILE* fp;
        if ((fp = fopen(TMP_MUSIC_FILE, "wb", true)) == NULL) {
            fprintf(stderr, "can't open temporaly MIDI file %s\n", TMP_MUSIC_FILE);
        }
        else {
            fwrite(buffer, 1, length, fp);
            fclose(fp);
            ext_music_play_once_flag = !loop_flag;
            if (playMIDI(loop_flag) == 0) {
                delete[] buffer;
                return SOUND_MIDI;
            }
        }
    }

    delete[] buffer;

    return SOUND_OTHER;
}

void ONScripter::playCDAudio()
{
    if (cdaudio_flag) {
#ifdef USE_CDROM
        if (cdrom_info) {
            int length = cdrom_info->track[current_cd_track - 1].length / 75;
            SDL_CDPlayTracks(cdrom_info, current_cd_track - 1, 0, 1, 0);
            timer_cdaudio_id = SDL_AddTimer(length * 1000, cdaudioCallback, NULL);
        }
#endif
    }
    else {
        char filename[256];
        sprintf(filename, "cd\\track%2.2d.mp3", current_cd_track);
        int ret = playSound(filename, SOUND_MUSIC, cd_play_loop_flag);
        if (ret == SOUND_MUSIC) return;

        sprintf(filename, "cd\\track%2.2d.ogg", current_cd_track);
        ret = playSound(filename, SOUND_MUSIC, cd_play_loop_flag);
        if (ret == SOUND_MUSIC) return;

        sprintf(filename, "cd\\track%2.2d.wav", current_cd_track);
        ret = playSound(filename, SOUND_MUSIC | SOUND_CHUNK, cd_play_loop_flag, MIX_BGM_CHANNEL);
    }
}

int ONScripter::playWave(Mix_Chunk* chunk, int format, bool loop_flag, int channel)
{
    if (!chunk) return -1;

    Mix_Pause(channel);
    if (wave_sample[channel]) Mix_FreeChunk(wave_sample[channel]);
    wave_sample[channel] = chunk;

    if (channel == 0)
        Mix_Volume(channel, voice_volume * MIX_MAX_VOLUME / 100);
    else if (channel == MIX_BGM_CHANNEL)
        Mix_Volume(channel, music_volume * MIX_MAX_VOLUME / 100);
    else
        Mix_Volume(channel, se_volume * MIX_MAX_VOLUME / 100);

    if (!(format & SOUND_PRELOAD))
        Mix_PlayChannel(channel, wave_sample[channel], loop_flag ? -1 : 0);

    return 0;
}

int ONScripter::playMIDI(bool loop_flag)
{
    Mix_SetMusicCMD(midi_cmd);

    char midi_filename[256];
    sprintf(midi_filename, "%s%s", save_dir ? save_dir : archive_path, TMP_MUSIC_FILE);
    if ((midi_info = Mix_LoadMUS(midi_filename)) == NULL) return -1;

    int midi_looping = loop_flag ? -1 : 0;

#if defined(LINUX)
    signal(SIGCHLD, midiCallback);
    if (midi_cmd) midi_looping = 0;
#endif

    Mix_VolumeMusic(music_volume);
    Mix_PlayMusic(midi_info, midi_looping);
    current_cd_track = -2;

    return 0;
}

#if defined(USE_SMPEG)
#if defined(USE_SDL_RENDERER)
#if SDL_VERSION_ATLEAST(2, 0, 0)
typedef struct
{
    SMPEG_Frame* frame;
    SDL_mutex* mutex;
} update_context;
static void smpeg_update_callback(void* data, SMPEG_Frame* frame)
{
    update_context* c = (update_context*)data;
    c->frame = frame;
}
#else
struct OverlayInfo {
    SDL_Overlay overlay;
    SDL_mutex* mutex;
};
static void smpeg_filter_callback(SDL_Overlay* dst, SDL_Overlay* src, SDL_Rect* region, SMPEG_FilterInfo* filter_info, void* data)
{
    if (dst) {
        dst->w = 0;
        dst->h = 0;
    }

    OverlayInfo* oi = (OverlayInfo*)data;

    SDL_mutexP(oi->mutex);
    memcpy(oi->overlay.pixels[0], src->pixels[0],
           oi->overlay.w * oi->overlay.h + (oi->overlay.w / 2) * (oi->overlay.h / 2) * 2);
    SDL_mutexV(oi->mutex);
}
static void smpeg_filter_destroy(struct SMPEG_Filter* filter)
{
}
#endif
#elif defined(ANDROID)
static void smpeg_filter_callback(SDL_Overlay* dst, SDL_Overlay* src, SDL_Rect* region, SMPEG_FilterInfo* filter_info, void* data)
{
    if (dst) {
        dst->w = 0;
        dst->h = 0;
    }

    ONScripter* ons = (ONScripter*)data;
    AnimationInfo* ai = ons->getSMPEGInfo();
    ai->convertFromYUV(src);
}
static void smpeg_filter_destroy(struct SMPEG_Filter* filter)
{
}
#endif
#endif

int ONScripter::playMPEG(const char* filename, bool click_flag, bool loop_flag, bool nosound_flag)
{
    unsigned long length = script_h.cBR->getFileLength(filename);
    if (length == 0) {
        fprintf(stderr, " *** can't find file [%s] ***\n", filename);
        return 0;
    }

#ifdef IOS
    char* absolute_filename = new char[strlen(archive_path) + strlen(filename) + 1];
    sprintf(absolute_filename, "%s%s", archive_path, filename);
    playVideoIOS(absolute_filename, click_flag, loop_flag);
    delete[] absolute_filename;
#endif

    int ret = 0;
#if defined(USE_SMPEG)
    stopSMPEG();
    layer_smpeg_buffer = new unsigned char[length];
    script_h.cBR->getFile(filename, layer_smpeg_buffer);
    SMPEG_Info info;
#if SDL_VERSION_ATLEAST(2, 0, 0)
    layer_smpeg_sample = SMPEG_new_rwops(SDL_RWFromMem(layer_smpeg_buffer, length), &info, 0, 0);
#else
    layer_smpeg_sample = SMPEG_new_rwops(SDL_RWFromMem(layer_smpeg_buffer, length), &info, 0);
#endif
    unsigned char packet_code[4] = {0x00, 0x00, 0x01, 0xba};
    if (SMPEG_error(layer_smpeg_sample) ||
        layer_smpeg_buffer[0] != packet_code[0] ||
        layer_smpeg_buffer[1] != packet_code[1] ||
        layer_smpeg_buffer[2] != packet_code[2] ||
        layer_smpeg_buffer[3] != packet_code[3] ||
        (layer_smpeg_buffer[4] & 0xf0) != 0x20) {
        stopSMPEG();
#ifdef ANDROID
        playVideoAndroid(filename);
#endif
        return ret;
    }

    SMPEG_enableaudio(layer_smpeg_sample, 0);
    if (audio_open_flag && !nosound_flag) {
        int mpegversion, frequency, layer, bitrate;
        char mode[10];
        sscanf(info.audio_string,
               "MPEG-%d Layer %d %dkbit/s %dHz %s",
               &mpegversion, &layer, &bitrate, &frequency, mode);
        printf("MPEG-%d Layer %d %dkbit/s %dHz %s\n",
               mpegversion, layer, bitrate, frequency, mode);

        openAudio(frequency);

        SMPEG_actualSpec(layer_smpeg_sample, &audio_format);
        SMPEG_enableaudio(layer_smpeg_sample, 1);
    }
    SMPEG_enablevideo(layer_smpeg_sample, 1);

#if defined(USE_SDL_RENDERER)
#if SDL_VERSION_ATLEAST(2, 0, 0)
    update_context c;
    c.mutex = SDL_CreateMutex();
    SMPEG_setdisplay(layer_smpeg_sample, smpeg_update_callback, &c, c.mutex);

    int texture_width = (info.width + 15) & ~15;
    int texture_height = (info.height + 15) & ~15;
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_YV12, SDL_TEXTUREACCESS_STREAMING, texture_width, texture_height);
#else
    SMPEG_setdisplay(layer_smpeg_sample, accumulation_surface, NULL, NULL);

    OverlayInfo oi;
    Uint16 pitches[3];
    Uint8* pixels[3];
    oi.overlay.format = SDL_YV12_OVERLAY;
    oi.overlay.w = info.width;
    oi.overlay.h = info.height;
    oi.overlay.planes = 3;
    pitches[0] = info.width;
    pitches[1] = info.width / 2;
    pitches[2] = info.width / 2;
    oi.overlay.pitches = pitches;
    Uint8* pixel_buf = new Uint8[info.width * info.height + (info.width / 2) * (info.height / 2) * 2];
    pixels[0] = pixel_buf;
    pixels[1] = pixel_buf + info.width * info.height;
    pixels[2] = pixel_buf + info.width * info.height + (info.width / 2) * (info.height / 2);
    oi.overlay.pixels = pixels;
    oi.mutex = SDL_CreateMutex();

    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_YV12, SDL_TEXTUREACCESS_TARGET, info.width, info.height);

    layer_smpeg_filter.data = &oi;
    layer_smpeg_filter.callback = smpeg_filter_callback;
    layer_smpeg_filter.destroy = smpeg_filter_destroy;
    SMPEG_filter(layer_smpeg_sample, &layer_smpeg_filter);
#endif
#elif defined(ANDROID)
    SMPEG_setdisplay(layer_smpeg_sample, screen_surface, NULL, NULL);
    AnimationInfo* smpeg_info_back = smpeg_info;
    smpeg_info = new AnimationInfo();
    smpeg_info->image_surface = accumulation_surface;
    layer_smpeg_filter.data = this;
    layer_smpeg_filter.callback = smpeg_filter_callback;
    layer_smpeg_filter.destroy = smpeg_filter_destroy;
    SMPEG_filter(layer_smpeg_sample, &layer_smpeg_filter);
#else
    SMPEG_setdisplay(layer_smpeg_sample, screen_surface, NULL, NULL);
#endif
    if (!nosound_flag) {
        SMPEG_setvolume(layer_smpeg_sample, music_volume);
        if (info.has_audio) Mix_HookMusic(mp3callback, layer_smpeg_sample);
    }

    SMPEG_loop(layer_smpeg_sample, loop_flag ? 1 : 0);
    SMPEG_play(layer_smpeg_sample);

    bool done_flag = false;
    while (!(done_flag & click_flag) && SMPEG_status(layer_smpeg_sample) == SMPEG_PLAYING) {
        SDL_Event event;

        while (SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_KEYUP:
                if (((SDL_KeyboardEvent*)&event)->keysym.sym == SDLK_RETURN ||
                    ((SDL_KeyboardEvent*)&event)->keysym.sym == SDLK_SPACE ||
                    ((SDL_KeyboardEvent*)&event)->keysym.sym == SDLK_ESCAPE)
                    done_flag = true;
                if (((SDL_KeyboardEvent*)&event)->keysym.sym == SDLK_RCTRL)
                    ctrl_pressed_status &= ~0x01;

                if (((SDL_KeyboardEvent*)&event)->keysym.sym == SDLK_LCTRL)
                    ctrl_pressed_status &= ~0x02;
                break;
            case SDL_QUIT:
                ret = 1;
            case SDL_MOUSEBUTTONUP:
                done_flag = true;
                break;
            default:
                break;
            }
        }

#if defined(USE_SDL_RENDERER) && SDL_VERSION_ATLEAST(2, 0, 0)
        bool updated_frame = false;
        SDL_mutexP(c.mutex);
        if (c.frame) {
            SDL_UpdateTexture(texture, NULL, c.frame->image, c.frame->image_width);
            updated_frame = true;
        }
        SDL_mutexV(c.mutex);
        if (updated_frame) {
            SDL_Rect r;
            r.x = 0;
            r.y = 0;
            r.w = info.width;
            r.h = info.height;
            SDL_RenderCopy(renderer, texture, &r, NULL);
            SDL_RenderPresent(renderer);
        }
#elif defined(USE_SDL_RENDERER) && !SDL_VERSION_ATLEAST(2, 0, 0)
        SDL_mutexP(oi.mutex);
        flushDirectYUV(&oi.overlay);
        SDL_mutexV(oi.mutex);
#elif defined(ANDROID)
        SDL_mutexP(smpeg_info->mutex);
        flushDirect(screen_rect, REFRESH_NONE_MODE);
        SDL_mutexV(smpeg_info->mutex);
#endif
        SDL_Delay(1);
    }

    if (!nosound_flag)
        Mix_HookMusic(NULL, NULL);
    stopSMPEG();
    if (!nosound_flag)
        openAudio();
#if defined(USE_SDL_RENDERER) && SDL_VERSION_ATLEAST(2, 0, 0)
    SDL_DestroyMutex(c.mutex);
    SDL_DestroyTexture(texture);
    texture = SDL_CreateTextureFromSurface(renderer, accumulation_surface);
#elif defined(USE_SDL_RENDERER) && !SDL_VERSION_ATLEAST(2, 0, 0)
    delete[] pixel_buf;
    SDL_DestroyMutex(oi.mutex);
    texture = SDL_CreateTextureFromSurface(renderer, accumulation_surface);
#elif defined(ANDROID)
    smpeg_info->image_surface = NULL;
    delete smpeg_info;
    smpeg_info = smpeg_info_back;
#endif
#elif !defined(IOS)
    fprintf(stderr, "mpegplay command is disabled.\n");
#endif

    return ret;
}

int ONScripter::playAVI(const char* filename, bool click_flag)
{
    unsigned long length = script_h.cBR->getFileLength(filename);
    if (length == 0) {
        fprintf(stderr, " *** can't find file [%s] ***\n", filename);
        return 0;
    }

#ifdef ANDROID
    playVideoAndroid(filename);
    return 0;
#endif

#if defined(USE_AVIFILE) && !defined(USE_SDL_RENDERER)
    char* absolute_filename = new char[strlen(archive_path) + strlen(filename) + 1];
    sprintf(absolute_filename, "%s%s", archive_path, filename);
    for (unsigned int i = 0; i < strlen(absolute_filename); i++)
        if (absolute_filename[i] == '/' ||
            absolute_filename[i] == '\\')
            absolute_filename[i] = DELIMITER;

    if (audio_open_flag) Mix_CloseAudio();

    AVIWrapper* avi = new AVIWrapper();
    if (avi->init(absolute_filename, false) == 0 &&
        avi->initAV(screen_surface, audio_open_flag) == 0) {
        if (avi->play(click_flag)) return 1;
    }
    delete avi;
    delete[] absolute_filename;

    if (audio_open_flag) {
        Mix_CloseAudio();
        openAudio();
    }
#else
    fprintf(stderr, "avi command is disabled.\n");
#endif

    return 0;
}

void ONScripter::stopBGM(bool continue_flag)
{
    removeBGMFadeEvent();
    if (timer_bgmfade_id) SDL_RemoveTimer(timer_bgmfade_id);
    timer_bgmfade_id = NULL;
    mp3fadeout_duration_internal = 0;

#ifdef USE_CDROM
    if (cdaudio_flag && cdrom_info) {
        extern SDL_TimerID timer_cdaudio_id;

        if (timer_cdaudio_id) {
            SDL_RemoveTimer(timer_cdaudio_id);
            timer_cdaudio_id = NULL;
        }
        if (SDL_CDStatus(cdrom_info) >= CD_PLAYING)
            SDL_CDStop(cdrom_info);
    }
#endif

    if (wave_sample[MIX_BGM_CHANNEL]) {
        Mix_Pause(MIX_BGM_CHANNEL);
        Mix_FreeChunk(wave_sample[MIX_BGM_CHANNEL]);
        wave_sample[MIX_BGM_CHANNEL] = NULL;
    }

    if (music_info) {
        ext_music_play_once_flag = true;
        Mix_HaltMusic();
        Mix_FreeMusic(music_info);
        music_info = NULL;
    }

    if (midi_info) {
        ext_music_play_once_flag = true;
        Mix_HaltMusic();
        Mix_FreeMusic(midi_info);
        midi_info = NULL;
    }

    if (!continue_flag) {
        setStr(&music_file_name, NULL);
        music_play_loop_flag = false;
        if (music_buffer) {
            delete[] music_buffer;
            music_buffer = NULL;
        }

        setStr(&midi_file_name, NULL);
        midi_play_loop_flag = false;

        current_cd_track = -1;
    }
}

void ONScripter::stopAllDWAVE()
{
    for (int ch = 0; ch < ONS_MIX_CHANNELS; ch++)
        if (wave_sample[ch]) {
            Mix_Pause(ch);
            Mix_FreeChunk(wave_sample[ch]);
            wave_sample[ch] = NULL;
        }
}

void ONScripter::playClickVoice()
{
    if (clickstr_state == CLICK_NEWPAGE) {
        if (clickvoice_file_name[CLICKVOICE_NEWPAGE])
            playSound(clickvoice_file_name[CLICKVOICE_NEWPAGE],
                      SOUND_CHUNK, false, MIX_WAVE_CHANNEL);
    }
    else if (clickstr_state == CLICK_WAIT) {
        if (clickvoice_file_name[CLICKVOICE_NORMAL])
            playSound(clickvoice_file_name[CLICKVOICE_NORMAL],
                      SOUND_CHUNK, false, MIX_WAVE_CHANNEL);
    }
}
