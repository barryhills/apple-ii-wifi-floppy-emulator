# Third-party code

This project (MIT, see `LICENSE`) includes or derives from the following.
Each keeps its own licence; the notices are also in the files themselves.

| What | Where | From | Licence |
|---|---|---|---|
| Disk II read-data PIO program (program body unchanged) | `firmware/src/disk2.pio` | [rezafouladian/RP2350-Floppy-Emulator](https://github.com/rezafouladian/RP2350-Floppy-Emulator), Copyright (c) Reza Fouladian | MIT OR Apache-2.0, used under MIT |
| DHCP server for setup mode (unmodified) | `firmware/src/third_party/dhcpserver/` | MicroPython / pico-examples, Copyright (c) 2013-2022 Damien P. George | MIT (`LICENSE` in that folder) |
| 5x7 font (characters 32-126) | `firmware/src/font5x7.h` | [Adafruit-GFX-Library](https://github.com/adafruit/Adafruit-GFX-Library) `glcdfont.c`, Copyright (c) 2012 Adafruit Industries | BSD (text in the file) |
| OLED init sequence and I2C framing | `firmware/src/oled.c` | [raspberrypi/pico-examples](https://github.com/raspberrypi/pico-examples) `i2c/ssd1306_i2c`, Copyright (c) 2020 Raspberry Pi (Trading) Ltd. | BSD-3-Clause |

The firmware is built with the [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk)
(BSD-3-Clause), which brings lwIP (BSD), the CYW43 driver (Infineon/Raspberry
Pi licence for RP2040/RP2350 use) and other components under their own
licences. They are not included in this repository.

## MIT licence text (RP2350-Floppy-Emulator)

Copyright (c) Reza Fouladian

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## BSD-3-Clause licence text (pico-examples)

Copyright 2020 (c) 2020 Raspberry Pi (Trading) Ltd.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.
3. Neither the name of the copyright holder nor the names of its contributors
   may be used to endorse or promote products derived from this software
   without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
