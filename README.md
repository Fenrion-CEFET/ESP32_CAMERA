ESP32_CAMERA (Equipe FENRION)

Código da câmera Wi-Fi do drone, baseada na placa ESP32-CAM (AI-Thinker). A câmera cria um servidor de vídeo na rede Wi-Fi, e o notebook da estação de solo lê as imagens pelo navegador ou pelo Python (OpenCV).

Este guia foi escrito para quem acabou de instalar a Arduino IDE 2.x e nunca gravou uma ESP32-CAM.

1. Preparar a Arduino IDE (só na primeira vez)
1.1 Instalar o pacote das placas ESP32

A biblioteca da câmera (esp_camera.h) já vem dentro do pacote das placas ESP32. Não é preciso instalar nenhuma biblioteca separada.

Abra a Arduino IDE.
No menu lateral esquerdo, clique no ícone de placas (Boards Manager), ou vá em Tools > Board > Boards Manager.
Pesquise por esp32.
Instale o pacote esp32 by Espressif Systems. A instalação demora alguns minutos.

Se o pacote não aparecer na pesquisa:

Vá em File > Preferences.
No campo Additional boards manager URLs, cole:
   https://espressif.github.io/arduino-esp32/package_esp32_index.json
Clique em OK e repita a pesquisa no Boards Manager.
1.2 Baixar o código deste repositório
Nesta página do GitHub, clique no botão verde Code e depois em Download ZIP.
Descompacte o arquivo.
A Arduino IDE exige que o arquivo .ino fique dentro de uma pasta com o mesmo nome do arquivo. Exemplo: o arquivo esp32_camera.ino precisa estar dentro da pasta esp32_camera. Se a IDE perguntar se pode criar a pasta ao abrir, clique em OK.
Abra o arquivo .ino na IDE (File > Open).
2. Ligar a ESP32-CAM no computador

A ESP32-CAM não tem porta USB própria. Há duas formas de ligar:

Opção A: com a base ESP32-CAM-MB (recomendada)

A base MB é a plaquinha com conector micro-USB onde a ESP32-CAM encaixa por cima.

Encaixe a ESP32-CAM na base MB (a câmera fica do lado oposto ao conector USB).
Ligue o cabo USB no computador. Use um cabo de dados; alguns cabos só carregam e não funcionam.
Se o Windows não reconhecer a placa, instale o driver CH340 (pesquise "driver CH340" no site do fabricante WCH).
Opção B: com conversor USB-serial (FTDI)
FTDI	ESP32-CAM
5V	5V
GND	GND
TX	U0R (RX)
RX	U0T (TX)

Para gravar, ligue também o pino IO0 no GND da própria ESP32-CAM. Depois da gravação, retire esse fio e aperte o botão RST para o programa rodar.

3. Configurar a placa na IDE

Em Tools (ferramentas), ajuste:

Opção	Valor
Board	AI Thinker ESP32-CAM (em esp32)
Port	a porta COM que apareceu ao ligar a placa
Partition Scheme	Huge APP (3MB No OTA/1MB SPIFFS)
PSRAM	Enabled (se a opção aparecer)

Para descobrir qual é a porta COM: veja a lista em Tools > Port com a placa desligada, ligue a placa e veja qual porta apareceu.

4. Colocar a rede Wi-Fi e gravar
No código, procure as linhas:
cpp
   const char* ssid = "...";
   const char* password = "...";
Coloque o nome e a senha da rede Wi-Fi que o notebook da estação de solo vai usar. Nunca envie o código para o GitHub com a senha preenchida. Volte para "SUA_REDE" e "SUA_SENHA" antes de fazer upload.
Clique em Upload (seta para a direita).
Se aparecer Connecting....____ e travar: com a base MB, segure o botão IO0 (ou BOOT) da base, aperte e solte RST, e solte IO0. Com FTDI, confira o fio IO0 no GND.
Quando terminar (Hard resetting via RTS pin...), aperte RST na placa.

5. Ver a imagem
Abra o Serial Monitor (ícone de lupa no canto superior direito) e ajuste a velocidade para 115200 baud.
Aperte RST na placa. Vai aparecer algo como:
   WiFi connected
   Camera Ready! Use 'http://192.168.0.105' to connect
Com o notebook na mesma rede Wi-Fi, abra esse endereço no navegador e clique em Start Stream.

Para ler o vídeo no Python (OpenCV), o endereço do stream costuma ser a porta 81:

python
import cv2
cap = cv2.VideoCapture("http://192.168.0.105:81/stream")  # troque pelo IP da sua placa
while True:
    ok, frame = cap.read()
    if not ok:
        break
    cv2.imshow("ESP32-CAM", frame)
    if cv2.waitKey(1) == 27:  # tecla ESC fecha
        break
cap.release()
cv2.destroyAllWindows()


6. Alterar as opções da câmera

Há dois jeitos: pela página no navegador (temporário, volta ao padrão quando a placa reinicia) ou no código (permanente).

6.1 Pela página no navegador (para testar)

Na página aberta no item 5 há controles para resolução, qualidade, brilho, contraste, espelhar e virar a imagem. Use para descobrir a melhor configuração e depois passe os valores para o código.


6.2 No código (permanente)

Resolução e qualidade: procure no código as linhas de config:

cpp
config.frame_size = FRAMESIZE_VGA;  // resolução
config.jpeg_quality = 12;           // qualidade JPEG: 10 a 63 (menor = melhor imagem, mais pesada)
config.fb_count = 2;                // buffers de imagem (2 com PSRAM)

Resoluções mais usadas:

Valor	Pixels	Uso sugerido
FRAMESIZE_QVGA	320 x 240	menor atraso, imagem pobre
FRAMESIZE_VGA	640 x 480	bom ponto de partida para a competição
FRAMESIZE_SVGA	800 x 600	mais detalhe, um pouco mais de atraso
FRAMESIZE_XGA	1024 x 768	foto nítida (útil para a foto do manômetro na M3)
FRAMESIZE_UXGA	1600 x 1200	máxima, atraso alto no vídeo

Atenção: quanto maior a resolução e melhor a qualidade, maior o atraso do vídeo pelo Wi-Fi. Para o pouso de precisão, prefira atraso baixo (VGA ou menor).

Brilho, contraste, espelhamento: logo depois de esp_camera_init(&config), o código costuma ter um bloco com sensor_t * s = esp_camera_sensor_get();. Ali dá para ajustar:

cpp
sensor_t * s = esp_camera_sensor_get();
s->set_framesize(s, FRAMESIZE_VGA); // resolução (também pode trocar aqui)
s->set_quality(s, 12);              // qualidade JPEG (10 a 63)
s->set_brightness(s, 0);            // brilho: -2 a 2
s->set_contrast(s, 0);              // contraste: -2 a 2
s->set_saturation(s, 0);            // saturação: -2 a 2
s->set_vflip(s, 1);                 // 1 = vira a imagem de cabeça para baixo
s->set_hmirror(s, 0);               // 1 = espelha na horizontal
s->set_exposure_ctrl(s, 1);         // 1 = exposição automática (bom ao ar livre)
s->set_whitebal(s, 1);              // 1 = balanço de branco automático

Se o bloco não existir no código, pode criar logo depois da linha esp_camera_init(&config).

Depois de alterar, grave de novo (item 4) e confira a imagem (item 5).

7. Problemas comuns
Sintoma	O que fazer
Porta COM não aparece	Trocar o cabo USB (precisa ser de dados); instalar driver CH340
Failed to connect to ESP32	Colocar a placa em modo de gravação (IO0 no GND ou botão IO0 + RST)
Camera init failed	Conferir se a placa escolhida é AI Thinker ESP32-CAM; verificar o cabo flat da câmera
Reinicia sozinha (Brownout detector)	Fonte fraca: usar fonte de 5 V com pelo menos 2 A, ou outra porta USB
Não conecta no Wi-Fi	Conferir nome e senha; a ESP32 só funciona em redes 2,4 GHz
Vídeo com muito atraso	Baixar a resolução e aumentar jpeg_quality (ex.: 15 a 20)
Regras da equipe para este repositório
Nunca enviar senha de Wi-Fi no código.
Toda alteração enviada precisa ter uma descrição clara no commit (o que mudou e por quê).
Configurações testadas e aprovadas em voo devem ser anotadas aqui no README.
