# ESP32P4-Jet

This is an application of CubeCoders' Jet (https://github.com/CubeCoders/Jet) 3D renderring library.

## Hardware

This is desinged for Waveshare's ESP32-P4-WIFI6-POE-ETH board & 5-DSI-TOUCH-A LCD display:

- https://docs.waveshare.com/ESP32-P4-WIFI6-POE-ETH
- https://www.waveshare.com/wiki/5-DSI-TOUCH-A?srsltid=AfmBOoqJHMFsEj88ozwSaozcvow9fhw7FGydK1QIuFCG_sB9le9H1Off

But you can select other products from Waveshare.

## Features

- 3D objects(plane, cube, sphere) are rotating.
- The camera viewpoint can be moved using touch and drag.
- Object magnification can be changed using 2 finger touch.


## Instruction

This is tested with ESP-IDF v5.5.3 .

```
git clone --recursive https://github.com/Sunyoup/FNK104N_Jet.git
cd FNK104N_Jet
rm -rf components/Jet/src/JetConfig.example.hpp
cp JetConfig.hpp components/Jet/src/
idf.py menuconfig
- Go to "Component config" -> "Board Support Package(ESP32-P4)" -> "Display" -> "Select LCD type" -> "Waveshare 5-DSI-TOUCH-A Display"

(If your ESP32-P4 chip revision is below 3.x, do following:)
- Go to "Component config" -> "Hardware Settings" -> "Chip revision" -> "[*] Select ESP32-P4 revisions <3.0 (No >=3.x Support)" : Chek.
- Go to "Component config" -> "ESP System Settings" -> "[ ] Force 400MHz on revision < 3.0 (EXPERIMENTAL)" : Uncheck.

(Save sdkconfig)

idf.py flash monitor
```

