---
icon: material/cog
---

Now that we're familiar with the hardware on this Thing Plus board, it's time to connect it to our computer or battery power.

## Basic USB Assembly

Basic assembly of the Thing Plus ESP32-S3 only requires a USB-C cable connecting the board to a computer. Just plug the cable into the USB-C connector and move on to the Software Setup section. When powered up for the first time, the RGB LED on the "top-right" of the board should be cycling through a rainbow color similar to the video below:

<figure markdown>
[![Short video showing RGB led cycling.](./assets/img/WRL-24408-ESP-32-S3-Action-GIF.gif){ width="600"}](./assets/img/WRL-24408-ESP-32-S3-Action-GIF.gif "Click to enlarge")
</figure>

!!! note
    Your computer may not recognize the board as a known USB device if you have not installed the espressif boards package in Arduino and/or installed the espressif IDF.

## Battery Assembly

If you prefer a battery-powered application, plug a single-cell LiPo battery into the 2-pin JST connector on the underside of the board like the photo below. Remember, the MCP73831 only charges the battery when V_USB has voltage present either from the USB-C connector or through the V_USB PTH pin.

For tips on the proper use of a LiPo battery and the 2-pin JST connector, please read through our [Single Cell LiPo Battery Care](https://learn.sparkfun.com/tutorials/single-cell-lipo-battery-care) tutorial.

## Soldered Assembly

Those who prefer a traditional soldered assembly should solder wires or header pins to the PTH header on the side of the board. If you're not familiar with through-hole soldering or would like a refresher, take a look at our Through-Hole Soldering Tutorial:

<div class="grid cards" markdown align="center">

-   <a href="https://learn.sparkfun.com/tutorials/5">
    <figure markdown>
    ![How to Solder: Through-Hole Soldering](https://cdn.sparkfun.com/c/264-148/assets/e/3/9/9/4/51d9fbe1ce395f7a2a000000.jpg)
    </figure>
    </a>
    <a href="https://learn.sparkfun.com/tutorials/5">**How to Solder: Through-Hole Soldering**
    </a>

</div>