#include <Arduino.h>
#include "buffer.h"

#include <WiFi.h>
#include <WiFiAP.h>
#include <WebServer.h>

#include "FS.h"
#include "SPIFFS.h"
#define FORMAT_SPIFFS_IF_FAILED true

const char *ssid = "GBPrinter";
const char *password = "20141111";
const IPAddress ip(192, 168, 4, 1);
const IPAddress subnet(255, 255, 255, 0);
WebServer server(80);

void handleRoot()
{
  // server.send(200, "text/plain", "hello from esp8266!");
  fs::FS fs = SPIFFS;
  File file = fs.open("/test.bmp", "r");
  server.streamFile(file, "image/bmp");
  file.close();
}

#define GPIOP_SCK GPIO_NUM_19
#define GPIOP_SIN GPIO_NUM_22
#define GPIOP_SOUT GPIO_NUM_23

#define ESP_INTR_FLAG_DEFAULT 0

#define WHITE     0xFF
#define LIGHTGREY 0xA5
#define DARKGREY  0x5A
#define BLACK     0x00

const uint8_t PALETTE_WHITE[3]     = {0xFF, 0xFF, 0xFF};
const uint8_t PALETTE_LIGHTGREY[3] = {0xA5, 0xA5, 0xA5};
const uint8_t PALETTE_DARKGREY[3]  = {0x5A, 0x5A, 0x5A};
const uint8_t PALETTE_BLACK[3]     = {0x00, 0x00, 0x00};

// 変換後の画像データの設定
// 初期値は何でもいい
#define PICTURE_WIDTH 160
#define PICTURE_HEIGHT 144
uint8_t bmp[PICTURE_WIDTH * PICTURE_HEIGHT] = {BLACK};
uint8_t print_start = 0;

const char printer_magic[] = {0x88, 0x33};

// 受信データを貯めておく領域の設定
#define MAX_DATA_LENGTH 5984
static unsigned char data[6000] = {0};
static unsigned int data_ptr = 0;

enum printer_state
{
  PS_MAGIC0,
  PS_MAGIC1,
  PS_CMD,
  PS_ARG0,
  PS_LEN_LOW,
  PS_LEN_HIGH,
  PS_DATA,
  PS_CHECKSUM0,
  PS_CHECKSUM1,
  PS_ACK,
  PS_STATUS
};
enum printer_state printer_state;
enum printer_state printer_state_prev;
uint16_t printer_data_len;

volatile uint8_t gb_sin, gb_sout;
volatile uint8_t gb_bit;

struct circular_buf recv_buf;

static void printer_state_reset()
{
  printer_data_len = 0;
  printer_state = PS_MAGIC0;
  printer_state_prev = printer_state;
}

static void printer_state_update(uint8_t b)
{
  printer_state_prev = printer_state;
  switch (printer_state)
  {
  case PS_MAGIC0:
    if (b == printer_magic[0])
    {
      printer_state = PS_MAGIC1;
    }
    break;
  case PS_MAGIC1:
    if (b == printer_magic[1])
    {
      printer_state = PS_CMD;
    }
    else
    {
      printer_state = PS_MAGIC0;
    }
    break;
  case PS_CMD:
    if (b == 0x02)
    {
      print_start = 1;
    }
    printer_state = PS_ARG0;
    break;
  case PS_ARG0:
    printer_state = PS_LEN_LOW;
    break;
  case PS_LEN_LOW:
    printer_data_len = b;
    printer_state = PS_LEN_HIGH;
    break;
  case PS_LEN_HIGH:
    printer_data_len |= b << 8;
    if (printer_data_len != 0)
    {
      printer_state = PS_DATA;
    }
    else
    {
      printer_state = PS_CHECKSUM0;
    }
    break;
  case PS_DATA:
    printer_data_len--;
    printer_state = (printer_data_len == 0) ? PS_CHECKSUM0 : PS_DATA;
    break;
  case PS_CHECKSUM0:
    printer_state = PS_CHECKSUM1;
    break;
  case PS_CHECKSUM1:
    printer_state = PS_ACK;
    break;
  case PS_ACK:
    printer_state = PS_STATUS;
    break;
  case PS_STATUS:
    printer_state = PS_MAGIC0;
    break;
  default:
    break;
  }
}

inline static void IRAM_ATTR gpio_isr_handler(void *arg)
{
  if (gpio_get_level(GPIOP_SCK) == 0)
  { // FALLING
    gb_sout |= gpio_get_level(GPIOP_SOUT) ? 1 : 0;
    gb_bit++;
    if (gb_bit == 8)
    {
      // 1バイトがまとまったので送信する
      //usart_send_blocking(USART2, gb_sout);
      data[data_ptr++] = gb_sout;
      printer_state_update(gb_sout);
      switch (printer_state)
      {
      case PS_ACK:
        buf_push(&recv_buf, 0x81);
        break;
      case PS_STATUS:
        buf_push(&recv_buf, 0x00);
        break;
      default:
        break;
      }

      // Reset state
      gb_bit = 0;
      gb_sout = 0;

      // Prepare next gb_sin
      if (buf_empty(&recv_buf))
      {
        gb_sin = 0x00;
      }
      else
      {
        gb_sin = buf_pop(&recv_buf);
      }
    }
    else
    {
      gb_sin <<= 1;
      gb_sout <<= 1;
    }
  }
  else
  { // RISING
    (gb_sin & 0x80) ? gpio_set_level(GPIOP_SIN, 1) : gpio_set_level(GPIOP_SIN, 0);
  }
}

static void gblink_slave_gpio_setup()
{
  gpio_config_t io_conf;
  //disable interrupt
  io_conf.intr_type = GPIO_INTR_DISABLE;
  //set as output mode
  io_conf.mode = GPIO_MODE_OUTPUT;
  //bit mask of the pins that you want to set,e.g.GPIO18/19
  io_conf.pin_bit_mask = (1ULL << GPIOP_SIN);
  //disable pull-down mode
  io_conf.pull_down_en = GPIO_PULLDOWN_ENABLE;
  //disable pull-up mode
  io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
  //configure GPIO with the given settings
  gpio_config(&io_conf);
  gpio_set_level(GPIOP_SIN, 0);

  io_conf.intr_type = GPIO_INTR_DISABLE;
  io_conf.mode = GPIO_MODE_INPUT;
  io_conf.pin_bit_mask = (1ULL << GPIOP_SOUT);
  io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
  io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
  gpio_config(&io_conf);

  io_conf.intr_type = GPIO_INTR_ANYEDGE;
  io_conf.mode = GPIO_MODE_INPUT;
  io_conf.pin_bit_mask = (1ULL << GPIOP_SCK);
  io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
  io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
  gpio_config(&io_conf);

  gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
  gpio_isr_handler_add(GPIOP_SCK, gpio_isr_handler, (void *)GPIOP_SCK);
}

inline uint8_t convertColor(uint8_t dot){
    switch(dot) {
        case 0x00:
            return WHITE;
            break;
        case 0x01:
            return LIGHTGREY;
            break;
        case 0x02:
            return DARKGREY;
            break;
        case 0x03:
            return BLACK;
            break;
        default:
            return 0x55;
            break;
    }
    return 0xAA;
}

static void saveBMPdataToFile(){
  unsigned char bitsPerPixel = 24;
  int filesize = 54 + (bitsPerPixel/8) * PICTURE_WIDTH * PICTURE_HEIGHT;      //  w is image width, h is image height  

  // create file headers (also taken from above example)
  unsigned char bmpFileHeader[14] = {
    'B','M', 0,0,0,0, 0,0, 0,0, 54,0,0,0             };
  unsigned char bmpInfoHeader[40] = {
    40,0,0,0, 0,0,0,0, 0,0,0,0, 1,0, bitsPerPixel,0  ,0,0,0,0,  0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0 };
  unsigned char bmpColorPalette[4*16] = {0};
  unsigned char bmpPad[3] = {
    0,0,0             };

  bmpFileHeader[ 2] = (unsigned char)(filesize          );
  bmpFileHeader[ 3] = (unsigned char)(filesize      >> 8);
  bmpFileHeader[ 4] = (unsigned char)(filesize      >>16);
  bmpFileHeader[ 5] = (unsigned char)(filesize      >>24);

  bmpInfoHeader[ 4] = (unsigned char)(PICTURE_WIDTH     );
  bmpInfoHeader[ 5] = (unsigned char)(PICTURE_WIDTH >> 8);
  bmpInfoHeader[ 6] = (unsigned char)(PICTURE_WIDTH >>16);
  bmpInfoHeader[ 7] = (unsigned char)(PICTURE_WIDTH >>24);
  bmpInfoHeader[ 8] = (unsigned char)(PICTURE_HEIGHT    );
  bmpInfoHeader[ 9] = (unsigned char)(PICTURE_HEIGHT>> 8);
  bmpInfoHeader[10] = (unsigned char)(PICTURE_HEIGHT>>16);
  bmpInfoHeader[11] = (unsigned char)(PICTURE_HEIGHT>>24);

  // write the file!
  // this is a combination of the bmp example above and
  // one from the SdFat library (it doesn't create a usable
  // bmp file, though)...
  fs::FS fs = SPIFFS;
  fs.remove("/test.bmp");
  File file = fs.open("/test.bmp", FILE_WRITE);
  file.write(bmpFileHeader, sizeof(bmpFileHeader));    // write file header
  file.write(bmpInfoHeader, sizeof(bmpInfoHeader));    // " info header
  // file.write(bmpColorPalette, sizeof(bmpColorPalette));    // " info header

  /*
  // sizeof(img) is always 2
  for (int i=0; i<sizeof(img); i++) {                  // iterate image array
    file.write(img+(w*(h-i-1)*3), w);                  // write px data
    file.write(bmpPad, (4-(w*3)%4)%4);                 // and padding as needed
  }
  */
  int n = PICTURE_WIDTH * PICTURE_HEIGHT;
  for (int i = PICTURE_HEIGHT-1; i >= 0; i--) {
    unsigned int base = i * PICTURE_WIDTH;
    for (int j = 0; j < PICTURE_WIDTH; j++){
      switch (bmp[base+j])
      {
      case 0x00:
        file.write(PALETTE_WHITE,     sizeof(PALETTE_WHITE));
        break;
      case 0x01:
        file.write(PALETTE_LIGHTGREY, sizeof(PALETTE_LIGHTGREY));
        break;
      case 0x02:
        file.write(PALETTE_DARKGREY,  sizeof(PALETTE_DARKGREY));
        break;
      case 0x03:
        file.write(PALETTE_BLACK,     sizeof(PALETTE_BLACK));
        break;
      default:
        file.write(PALETTE_WHITE,     sizeof(PALETTE_WHITE));
        break;
      }
    }
  }
  file.close();
}

void setup()
{
  // put your setup code here, to run once:
  if (!SPIFFS.begin(FORMAT_SPIFFS_IF_FAILED))
  {
    printf("SPIFFS Mount Failed\n");
    return;
  }
  WiFi.mode(WIFI_MODE_AP);
  WiFi.softAP(ssid);
  delay(100); // 追記：このdelayを入れないと失敗する場合がある
  WiFi.softAPConfig(ip, ip, subnet);
  IPAddress myIP = WiFi.softAPIP();

  server.on("/", handleRoot);

  server.on("/inline", []()
            { server.send(200, "text/plain", "this works as well"); });

  server.begin();
  printf("HTTP server started\n");

  printer_state_reset();
  gblink_slave_gpio_setup();
}

void loop()
{
  // put your main code here, to run repeatedly:
  server.handleClient();

  if (print_start)
  {
    printf("Print\n");

    /* Print information that the timer reported an event */
    uint8_t rows = 0;

    // ここで受信データから画像部分を取り出し、ビットマップに変換したあと、
    // M5Stackの画像表示を一気に呼ぶ
    for (int i = 0; i < MAX_DATA_LENGTH - 15; i++)
    {
      // 受信データがif文の条件の順で並んでいるとき、それ以降の640バイトがドットデータとなる。
      // https://dhole.github.io/post/gameboy_serial_2/
      if (data[i] == 0x88 && data[i + 1] == 0x33 && data[i + 2] == 0x04 && data[i + 3] == 0x00 && data[i + 4] == 0x80 && data[i + 5] == 0x02)
      {
        // 640バイトで160*16ドットを表現
        // 8x8ドットごとに並べていく
        // 1ドットあたり2ビット :: 2バイトで8ドット
        uint8_t *payload = data + i + 6;

        for (uint8_t cols = 0; cols < 20; cols++)
        {
          // タイル位置データのコピーを実施
          uint8_t *tile;
          tile = payload + cols * 16;
          uint8_t tile_x, tile_y;
          for (tile_y = 0; tile_y < 16; tile_y += 2)
          {
            for (tile_x = 0; tile_x < 8; tile_x++)
            {
              uint8_t mask = 0x01 << (7 - tile_x);
              uint8_t dot = 0;
              dot = (tile[tile_y] & mask) ? 1 : 0;
              dot += (tile[tile_y + 1] & mask) ? 2 : 0;
              // bmp[(rows * 8 + tile_y / 2) * 160 + (cols * 8 + tile_x)] = convertColor(dot);
              bmp[(rows * 8 + tile_y / 2) * 160 + (cols * 8 + tile_x)] = dot;
            }
          }
        }

        rows++;

        payload = data + i + 6 + 320;

        for (uint8_t cols = 0; cols < 20; cols++)
        {
          // タイル位置データのコピーを実施
          uint8_t *tile;
          tile = payload + cols * 16;
          uint16_t tile_x, tile_y;
          for (tile_y = 0; tile_y < 16; tile_y += 2)
          {
            for (tile_x = 0; tile_x < 8; tile_x++)
            {
              uint8_t mask = 0x01 << (7 - tile_x);
              uint8_t dot = 0;
              dot = (tile[tile_y] & mask) ? 1 : 0;
              dot += (tile[tile_y + 1] & mask) ? 2 : 0;
              // bmp[(rows * 8 + tile_y / 2) * 160 + (cols * 8 + tile_x)] = convertColor(dot);
              bmp[(rows * 8 + tile_y / 2) * 160 + (cols * 8 + tile_x)] = dot;
            }
          }
        }
        rows++;
      }
    }

    saveBMPdataToFile();
    // M5.Lcd.drawBitmap(0, 0, PICTURE_WIDTH, PICTURE_HEIGHT, (uint16_t*)bmp); 画像表示が可能になる箇所

    printer_state_reset();
    print_start = 0;
    data_ptr = 0;
  }
}