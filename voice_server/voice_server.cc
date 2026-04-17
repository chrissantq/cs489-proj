#include "voice_server.hh"
#include <asm-generic/socket.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/socket.h>
#include <algorithm>

struct wav_header {
    char riff[4] = {'R', 'I', 'F', 'F'};
    int32_t size = 0;
    char wave[4] = {'W', 'A', 'V', 'E'};
    char fmt[4] = {'f', 'm', 't', ' '};
    int32_t fmt_size = 16;
    int16_t format = 1; // PCM
    int16_t channels = 1;
    int32_t sample_rate = 16000;
    int32_t byte_rate = 32000;
    int16_t align = 2;
    int16_t bits = 16;
    char data[4] = {'d', 'a', 't', 'a'};
    int32_t data_size = 0;
};

void write_wav_file(const char* filename, const std::vector<float>& pcmf) {
    FILE* f = fopen(filename, "wb");
    if (!f) return;

    wav_header header;
    header.data_size = pcmf.size() * 2;
    header.size = 36 + header.data_size;
    
    fwrite(&header, sizeof(header), 1, f);
    for (float v : pcmf) {
        int16_t s = (int16_t)std::max(-32768.0f, std::min(32767.0f, v * 32768.0f));
        fwrite(&s, 2, 1, f);
    }
    fclose(f);
}

int main() {

  int sockfd;
  struct sockaddr_in servaddr, cliaddr;
  socklen_t len = sizeof(cliaddr);

  std::vector<int16_t> pcm16_buffer(PACKET_SAMPLES); // store packet on arrive
  std::vector<float> audio_buffer; // build up audio buffer to feed whisper
  uint8_t recv_buf[BUFFER_SIZE]; // init packet arrival point

  // open debug file
  FILE* debug_f = fopen("debug.wav", "wb");
  if (debug_f) {
    wav_header dummy_header;
    dummy_header.size = 0x7FFFFFFF; // dummy large size
    dummy_header.data_size = 0x7FFFFFFF;
    fwrite(&dummy_header, sizeof(dummy_header), 1, debug_f);
  } else {
    perror("failed to open debug.wav");
  }

  // udp socket
  if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
    perror("failed socket creation");
    return -1;
  }

  memset(&servaddr, 0, sizeof(servaddr));
  memset(&cliaddr, 0, sizeof(cliaddr));

  // server info
  servaddr.sin_family = AF_INET;
  servaddr.sin_addr.s_addr = INADDR_ANY;
  servaddr.sin_port = htons(PORT);


  // bind socket to server addr
  if (bind(sockfd, (const struct sockaddr *)&servaddr, 
           sizeof(servaddr)) < 0) {
    perror("bind fail");
    close(sockfd);
    return -1;
  }

  // flush old packets
  int opt = 1;
  setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  char drain[BUFFER_SIZE];
  while (recvfrom(sockfd, drain, BUFFER_SIZE, MSG_DONTWAIT,
                  (struct sockaddr*)&cliaddr, &len) > 0);


  // init whisper model
  const char * model_loc = "../whisper.cpp/models/ggml-base.en.bin";

  // params for building
  whisper_context_params cparams = whisper_context_default_params();
  whisper_context * ctx = whisper_init_from_file_with_params(model_loc, cparams);

  // params for running
  whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_BEAM_SEARCH);
  params.print_progress = false;
  params.print_special = false;
  params.print_realtime = false;
  params.print_timestamps = false;
  params.translate = false;
  params.language = "en";
  params.temperature = 0.0f;
  params.temperature_inc = 0.0f;

  if (!ctx) {
    fprintf(stderr, "Failed to load whisper model\n");
    return 1;
  }

  printf("Server listening on port %d...\n", PORT);

  while (true) {

    // recv audio packets from arduino
    int n = recvfrom(sockfd, (char*)recv_buf, BUFFER_SIZE, MSG_WAITALL,
              (struct sockaddr*)&cliaddr, &len);

    if (n != BUFFER_SIZE) {
      fprintf(stderr, "wrong size: got %d expected %d\n", n, BUFFER_SIZE);
      continue;
    };

    // parse and validate header
    uint16_t magic, seq, n_samples, checksum;
    std::memcpy(&magic, recv_buf + 0, 2);
    std::memcpy(&seq, recv_buf + 2, 2);
    std::memcpy(&n_samples, recv_buf + 4, 2);
    std::memcpy(&checksum, recv_buf + 6, 2);

    if (magic != MAGIC) {
      fprintf(stderr, "bad magic: 0x%04X (expected 0x%04X)\n", magic, MAGIC);
      continue;
    }
    if (n_samples != PACKET_SAMPLES) {
      fprintf(stderr, "bad n_samples: %d\n", n_samples);
      continue;
    }

    // verify checksum
    uint8_t computed = 0;
    for (int i = HEADER_SIZE; i < BUFFER_SIZE; i++) computed ^= recv_buf[i];
    if (computed != (uint8_t) checksum) {
      fprintf(stderr, "bad checksum: got 0x%04X computed 0x%04X (seq %d)\n", checksum, computed, seq);
      continue;
    }

    // debug: print first few samples
    if (seq % 100 == 0) {
        printf("[seq=%d] First samples: ", seq);
        for (int i = 0; i < 5; i++) {
            int16_t sample;
            std::memcpy(&sample, recv_buf + HEADER_SIZE + i * 2, 2);
            printf("%d ", sample);
        }
        printf("\n");
    }

    // save raw audio to debug file
    if (debug_f) {
      fwrite(recv_buf + HEADER_SIZE, 1, PACKET_SAMPLES * 2, debug_f);
      static int flush_count = 0;
      if (++flush_count % 100 == 0) fflush(debug_f);
    }

    // decode audio
    std::vector<float> pcmf(PACKET_SAMPLES); // ea sample 2 bytes
    for (size_t i = 0; i < PACKET_SAMPLES; i++) {
      int16_t sample;
      std::memcpy(&sample, recv_buf + HEADER_SIZE + i * 2, sizeof(int16_t));
      pcmf[i] = (float)sample / 32768.0f;
    }

    // silence filter
    float max = 0.0f;
    for (float v : pcmf) max = std::max(max, fabsf(v));
    // printf("[seq=%5d] Max: %.4f\n", seq, max);

    if (max < 0.010f) { // slightly lower threshold (was 0.015)
      // empty buffer if silent for a bit
      if (!audio_buffer.empty()) {
        static int silent_pkts = 0;
        if (++silent_pkts > 20) {
          // ~.4 seconds of silence -> end of talking
          silent_pkts = 0;
          if (audio_buffer.size() >= (size_t)(16000 * 1)) {
            // 1 second is enough to process it 
            std::string res;
            process_audio(ctx, params, audio_buffer, res);
          }
          audio_buffer.clear();
        }
      }
      continue;
    }

    audio_buffer.insert(audio_buffer.end(), pcmf.begin(), pcmf.end());

    // store a few seconds of audio
    // wait >= 3 seconds and for noise
    if (audio_buffer.size() >= 16000 * MIN_AUDIO_SEC) {
      std::string res;
      process_audio(ctx, params, audio_buffer, res);

      // keep last .5 sec for continuity
      size_t keep = 16000 / 2;
      if (audio_buffer.size() > keep) {
        audio_buffer.erase(audio_buffer.begin(),
                           audio_buffer.end() - keep);
      }
    }
  }

  if (debug_f) fclose(debug_f);
  close(sockfd);
  return 0;

}

// process the incoming audio stream with whisper.cpp
int process_audio(whisper_context * ctx, whisper_full_params params, std::vector<float>& audio_buffer, std::string& res) {

    static int chunk_count = 0;
    char chunk_name[64];
    snprintf(chunk_name, sizeof(chunk_name), "whisper_chunk_%d.wav", ++chunk_count);
    printf("Processing %zu samples (%.2f seconds) -> saving to %s...\n", 
           audio_buffer.size(), (float)audio_buffer.size() / 16000.0f, chunk_name);
    
    write_wav_file(chunk_name, audio_buffer);

    // pass to whisper for processing

    // if fail return err code 1 to continue loop
    if (whisper_full(ctx, params, audio_buffer.data(), audio_buffer.size())) {
      fprintf(stderr, "Whisper failed\n");
      return 1;
    }


    // get result
    res.clear();
    int n_segments = whisper_full_n_segments(ctx);
    for (int i = 0; i < n_segments; ++i) {
      res += whisper_full_get_segment_text(ctx, i);
    }

    // temp see what is outputted
    if (!res.empty()) {
        printf("Heard: %s\n", res.c_str());
    } else {
        printf("Heard: [nothing]\n");
    }

    return 0;

};
