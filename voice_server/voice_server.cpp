#include <string>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <vector>

//#include "whisper.h"

#define PORT 1223
#define PACKET_SAMPLES 320
#define BUFFER_SIZE (PACKET_SAMPLES * 2) // 640 bytes for int16_t


int main() {

  int sockfd;
  struct sockaddr_in servaddr, cliaddr;

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

  std::vector<int16_t> pcm16_buffer(PACKET_SAMPLES);
  uint8_t recv_buf[BUFFER_SIZE];
  socklen_t len = sizeof(cliaddr);

  while (true) {

    // recv audio packets from arduino
    int n = recvfrom(sockfd, (char*)recv_buf, BUFFER_SIZE, MSG_WAITALL,
              (struct sockaddr*)&cliaddr, &len);

    // if packet data:
    if (n > 0) {
      // cpy raw to 16-bit pcm buffer, convert to float
      std::memcpy(pcm16_buffer.data(), recv_buf, n);
      std::vector<float> pcmf(n/2); // ea sample 2 bytes
      for (size_t i = 0; i < pcmf.size(); ++i) {
        // (in range -32768 -> 32767, whisper requires -1.0 to 1.0)
        pcmf[i] = (float) pcm16_buffer[i] / 32768.0f;
        printf("%f\n", pcmf[i]);
      }

      // TODO: pass to whisper for processing

      

    }

  }

  // whisper_context * ctx = whisper_init_from_file("ggml-base.en.bin");

  close(sockfd);
  return 0;

}
