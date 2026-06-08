#define _DEFAULT_SOURCE
#define _GNU_SOURCE

#include <stdio.h>
#include <ncurses.h>
#include <locale.h> 
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h> 
#include <alsa/asoundlib.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>

#define P 512

char dir[P], **files = NULL, buf[P*2], playing[P], err[P];
int n = 0, max_files = 0, sel = 0, st = 0;
volatile int stop, alive, paused;
volatile int loop_track = 0; 
volatile double seek_offset = 0; 
volatile int seek_req = 0;        
volatile int seek_busy = 0; 

void c(int y, const char *s) { int x = (COLS - strlen(s)) / 2; mvprintw(y, x < 0 ? 0 : x, "%s", s); }

int audio(const char *f) {
    const char *e[] = {".mp3",".flac",".wav",".ogg",".m4a",0};
    const char *d = strrchr(f, '.');
    if (!d) return 0;
    for (int i = 0; e[i]; i++) if (!strcasecmp(d, e[i])) return 1;
    return 0;
}

void clear_files() {
    while (n) free(files[--n]);
    free(files);
    files = NULL;
    max_files = 0;
    sel = st = 0;
}

void scan_rec(const char *base_path) {
    DIR *d = opendir(base_path);
    if (!d) return;

    struct dirent *e;
    char path[P * 2];

    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue; 

        int dl = strlen(base_path);
        snprintf(path, sizeof(path), "%s%s%s", base_path, (dl && base_path[dl-1] == '/') ? "" : "/", e->d_name);

        if (e->d_type == DT_DIR) {
            scan_rec(path);
        } else if (e->d_type == DT_REG && audio(e->d_name)) {
            if (n >= max_files) {
                max_files = max_files == 0 ? 1024 : max_files * 2;
                files = realloc(files, max_files * sizeof(char *));
            }
            files[n++] = strdup(path); 
        }
    }
    closedir(d);
}

void scan() {
    scan_rec(dir);
}

void draw() {
    erase();
    int y = 0;
    
    int divider_y = LINES - 5; 
    int vis = divider_y - 2; 

    c(y++, dir); y++;
    if (sel < st) st = sel;
    if (sel >= st + vis) st = sel - vis + 1;

    for (int i = st; i < n && y < divider_y; i++) {
        if (i == sel) attron(A_REVERSE);
        const char *filename = strrchr(files[i], '/');
        c(y++, filename ? filename + 1 : files[i]);
        if (i == sel) attroff(A_REVERSE);
    }

    mvhline(divider_y, 0, ACS_HLINE, COLS);

    if (alive && playing[0]) { 
        const char *filename = strrchr(playing, '/');
        snprintf(buf, sizeof(buf), "%s %s %s", 
                 paused ? "[PAUSED]" : ">", 
                 filename ? filename + 1 : playing,
                 loop_track ? "[LOOPING]" : ""); 
        c(LINES - 4, buf); 
    }
    if (err[0]) { attron(A_BOLD); c(LINES - 4, err); attroff(A_BOLD); }

    c(LINES - 3, "q - exit | r - toggle loop | space - pause/play");
    c(LINES - 2, "h/l - rewind/forward 10s | j/k - down/up");
    c(LINES - 1, "enter - play selected");
    refresh();
}

void prompt() {
    const char *pr = "dir: ";
    int pl = strlen(pr), p = strlen(dir);
    curs_set(1);
    nodelay(stdscr, FALSE);
    while (1) {
        erase();
        mvprintw(LINES / 2, (COLS - pl - p) / 2, "%s%s", pr, dir);
        move(LINES / 2, (COLS - pl - p) / 2 + pl + p);
        refresh();
        int ch = getch();
        if (ch == '\n') break;
        if (ch == 127 || ch == '\b' || ch == KEY_BACKSPACE) { if (p) dir[--p] = 0; }
        else if (p < P - 1 && ch >= 32 && ch < 127) { dir[p++] = ch; dir[p] = 0; }
        else if (ch == KEY_RESIZE) { }
    }
    curs_set(0);
    nodelay(stdscr, TRUE);
    FILE *out = fopen(".m", "w");
    if (out) { fprintf(out, "%s\n", dir); fclose(out); }
}

void load() {
    FILE *in = fopen(".m", "r");
    if (in) { fgets(dir, P, in); fclose(in); dir[strcspn(dir, "\n")] = 0; }
}

void stop_play() {
    if (!alive) return;
    stop = 1;
    while (alive) usleep(10000);
}

void *play_thread(void *arg) {
    char *path = arg;
    av_log_set_level(AV_LOG_QUIET);
    err[0] = 0;
    alive = 1;
    stop = 0;
    paused = 0;
    seek_req = 0;
    seek_offset = 0;
    seek_busy = 0;
    
    AVFormatContext *fmt = NULL;
    AVCodecContext *ctx = NULL;
    snd_pcm_t *pcm = NULL;
    SwrContext *swr = NULL;
    AVPacket *pkt = NULL;
    AVFrame *frame = NULL;
    int16_t *out = NULL;

    if (avformat_open_input(&fmt, path, NULL, NULL) < 0) { snprintf(err, sizeof(err), "open"); goto clean; }
    if (avformat_find_stream_info(fmt, NULL) < 0) { snprintf(err, sizeof(err), "info"); goto clean; }
    int si = -1;
    for (unsigned i = 0; i < fmt->nb_streams; i++)
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) { si = i; break; }
    if (si < 0) { snprintf(err, sizeof(err), "noaudio"); goto clean; }
    AVCodecParameters *par = fmt->streams[si]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(par->codec_id);
    if (!codec) { snprintf(err, sizeof(err), "codec"); goto clean; }
    ctx = avcodec_alloc_context3(codec);
    if (!ctx || avcodec_parameters_to_context(ctx, par) < 0 || avcodec_open2(ctx, codec, NULL) < 0) {
        snprintf(err, sizeof(err), "ctx"); goto clean;
    }
    int rate = par->sample_rate;
    int ich = par->ch_layout.nb_channels ? par->ch_layout.nb_channels : 2;
    
    if (snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0) { snprintf(err, sizeof(err), "alsa"); goto clean; }
    snd_pcm_hw_params_t *hw;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(pcm, hw);
    snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16);
    snd_pcm_hw_params_set_channels(pcm, hw, 2);
    unsigned int r = rate;
    snd_pcm_hw_params_set_rate_near(pcm, hw, &r, 0);
    if (snd_pcm_hw_params(pcm, hw) < 0) { snprintf(err, sizeof(err), "aparams"); goto clean; }

    AVChannelLayout in_layout, out_layout;
    av_channel_layout_default(&in_layout, ich);
    av_channel_layout_default(&out_layout, 2);

    int swr_err = swr_alloc_set_opts2(&swr, 
                                      &out_layout, AV_SAMPLE_FMT_S16, (int)r, 
                                      &in_layout, ctx->sample_fmt, rate, 
                                      0, NULL);
    
    av_channel_layout_uninit(&in_layout);
    av_channel_layout_uninit(&out_layout);

    if (swr_err < 0 || !swr || swr_init(swr) < 0) { snprintf(err, sizeof(err), "swr"); goto clean; }

    pkt = av_packet_alloc();
    frame = av_frame_alloc();
    out = malloc(48000 * 4);

    double current_seconds = 0.0;
    double total_seconds = (double)fmt->duration / AV_TIME_BASE; 
    AVRational tb = fmt->streams[si]->time_base;
    int alsa_hardware_paused = 0;

    while (1) {
        if (stop) goto clean;
        
        if (paused) {
            if (!alsa_hardware_paused) {
                snd_pcm_pause(pcm, 1); 
                alsa_hardware_paused = 1;
            }
            usleep(10000); 
            continue; 
        } else if (alsa_hardware_paused) {
            snd_pcm_pause(pcm, 0); 
            alsa_hardware_paused = 0;
        }

        if (seek_req) {
            double target_seconds = current_seconds + seek_offset;
            
            if (target_seconds < 0.0) target_seconds = 0.0;
            if (target_seconds > total_seconds) target_seconds = total_seconds - 1.0; 
            
            int64_t target_pts = av_rescale_q((int64_t)(target_seconds * AV_TIME_BASE), AV_TIME_BASE_Q, tb);
            
            if (av_seek_frame(fmt, si, target_pts, AVSEEK_FLAG_ANY) >= 0) {
                avcodec_flush_buffers(ctx);
                snd_pcm_drop(pcm);    
                snd_pcm_prepare(pcm); 
                current_seconds = target_seconds;
            }
            seek_req = 0; 
            seek_offset = 0;
            seek_busy = 0; 
        }

        if (av_read_frame(fmt, pkt) < 0) break;

        if (pkt->stream_index == si) {
            if (pkt->pts != AV_NOPTS_VALUE) {
                current_seconds = pkt->pts * av_q2d(tb); 
            }
            
            if (avcodec_send_packet(ctx, pkt) >= 0) {
                while (avcodec_receive_frame(ctx, frame) >= 0) {
                    if (stop) goto clean;

                    uint8_t *op = (uint8_t*)out;
                    int max_out_samples = swr_get_out_samples(swr, frame->nb_samples);
                    int conv = swr_convert(swr, &op, max_out_samples, (const uint8_t**)frame->data, frame->nb_samples);
                    if (conv > 0) {
                        int offset = 0;
                        while (offset < conv && !stop) {
                            if (paused || seek_req) break; 
                            
                            snd_pcm_sframes_t w = snd_pcm_writei(pcm, out + (offset * 2), conv - offset);
                            if (w == -EPIPE) {
                                snd_pcm_prepare(pcm);
                            } else if (w < 0) {
                                break;
                            } else {
                                offset += w;
                            }
                        }
                    }
                }
            }
        }
        av_packet_unref(pkt);
    }

    avcodec_send_packet(ctx, NULL);
    while (avcodec_receive_frame(ctx, frame) >= 0) {
        if (stop) goto clean;
        uint8_t *op = (uint8_t*)out;
        int conv = swr_convert(swr, &op, 48000, (const uint8_t**)frame->data, frame->nb_samples);
        if (conv > 0) snd_pcm_writei(pcm, out, conv);
    }
    uint8_t *op = (uint8_t*)out;
    swr_convert(swr, &op, 48000, NULL, 0);
    snd_pcm_drain(pcm);

    if (!stop) seek_offset = -999.0; 

clean:
    free(out);
    av_packet_free(&pkt);
    av_frame_free(&frame);
    swr_free(&swr);
    if (pcm) snd_pcm_close(pcm);
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);
    free(path);
    alive = 0;
    return NULL;
}

void start_track() {
    stop_play();
    char *path = strdup(files[sel]); 
    snprintf(playing, sizeof(playing), "%s", files[sel]);
    pthread_t tid;
    pthread_create(&tid, NULL, play_thread, path);
    pthread_detach(tid);
}

long long get_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int main() {
    setlocale(LC_ALL, ""); 
    initscr(); cbreak(); noecho(); keypad(stdscr, 1); curs_set(0);
    nodelay(stdscr, TRUE);
    load();
    prompt();
    scan();
    
    long long last_seek_time = 0;
    long long debounce_threshold = 150; 
    
    while (1) {
        draw();
        int ch = getch();
        
        if (ch == 'q' || ch == 'Q') { stop_play(); break; }
        if (ch == 'r' || ch == 'R') { loop_track = !loop_track; }
        
        if ((ch == 'k' || ch == 'K') && sel > 0) sel--;
        if ((ch == 'j' || ch == 'J') && sel < n - 1) sel++;
        
        if (ch == ' ') { 
            if (alive) paused = !paused; 
        }
        
        if (ch == 'l' || ch == 'L') { 
            if (alive && !seek_busy) { 
                long long now = get_ms();
                if (now - last_seek_time > debounce_threshold) {
                    seek_busy = 1;
                    seek_offset = 10.0;
                    seek_req = 1;
                    last_seek_time = now;
                }
            } 
        }
        if (ch == 'h' || ch == 'H') { 
            if (alive && !seek_busy) { 
                long long now = get_ms();
                if (now - last_seek_time > debounce_threshold) {
                    seek_busy = 1;
                    seek_offset = -10.0;
                    seek_req = 1;
                    last_seek_time = now;
                }
            } 
        }
        
        if (ch == '\n' && n > 0) start_track();

        if (seek_offset == -999.0) {
            seek_offset = 0;
            seek_req = 0;
            
            if (loop_track) {
                start_track();
            } else if (sel < n - 1) {
                sel++;
                start_track();
            } else {
                playing[0] = 0;
            }
        }
        usleep(20000); 
    }
    
    clear_files();
    endwin();
    return 0;
}
