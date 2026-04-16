#include <cstdio>
#include <cmath>
#include <string>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <vector>

#include "whisper.h"

#define PORT 1223
#define PACKET_SAMPLES 320
#define HEADER_SIZE 8
#define BUFFER_SIZE (PACKET_SAMPLES * 2 + HEADER_SIZE) // 648
#define MAGIC 0xA5B4
#define SILENCE_THRESH 0.015
#define RMS_THRESH 0.012f 
#define MIN_AUDIO_SEC 2

int process_audio(whisper_context*, whisper_full_params, std::vector<float>&, std::string&);
