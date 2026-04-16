#include "voice_server.hh"
#include <asm-generic/socket.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/socket.h>

int main() {

  int sockfd;
  struct sockaddr_in servaddr, cliaddr;
  socklen_t len = sizeof(cliaddr);

  std::vector<int16_t> pcm16_buffer(PACKET_SAMPLES); // store packet on arrive
  std::vector<float> audio_buffer; // build up audio buffer to feed whisper
  uint8_t recv_buf[BUFFER_SIZE]; // init packet arrival point

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
    printf("[seq=%5d] Max: %.4f\n", seq, max);

    if (max < SILENCE_THRESH) {
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

  close(sockfd);
  return 0;

}

// process the incoming audio stream with whisper.cpp
int process_audio(whisper_context * ctx, whisper_full_params params, std::vector<float>& audio_buffer, std::string& res) {

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
    if (!res.empty()) printf("Heard: %s\n", res.c_str());

    return 0;

};
