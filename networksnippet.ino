#define INSTR_TYPE_ON 0
#define INSTR_TYPE_OFF 1
#define INSTR_TYPE_COLOR 2

IPAddress bulbIP(192, 168, 1, 70); // SET PROPERLY SOMEWHERE

void updateNetworkDevice(int instrType, int onOff, int r, int g, int b) {

    String msg;

    switch (instrType) {

      case INSTR_TYPE_ON:
        msg = "{\"method\":\"setPilot\",\"params\":{\"state\":true}}";
        break;

      case INSTR_TYPE_OFF;
        msg = "{\"method\":\"setPilot\",\"params\":{\"state\":false}}";
        break;

      case INSTR_TYPE_COLOR:
        msg = "{\"method\":\"setPilot\",\"params\":{";
        msg += "\"r\":" + String(r);
        msg += ",\"g\":" + String(g);
        msg += ",\"b\":" + String(b);
        msg += ",\"dimming\":100}}";
        break;

      default:
        return;
    }

    udp.beginPacket(bulbIP, 38899);
    udp.write((const uint8_t*)msg.c_str(), msg.length());
    udp.endPacket();
}
