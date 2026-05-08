## esptool Upload Error

Some users may run into this upload error related to esptool:

esptool.py v4.5.1
Serial port COM33
Connecting....

A serial exception error occurred: Write timeout
Note: This error originates from pySerial. It is likely not a problem with esptool, but with the hardware connection or drivers.
For troubleshooting steps visit: https://docs.espressif.com/projects/esptool/en/latest/troubleshooting.html
the selected serial port For troubleshooting steps visit: https://docs.espressif.com/projects/esptool/en/latest/troubleshooting.html
does not exist or your board is not connected

This error means the board is not entering ROM bootloader mode automatically, causing the upload to hang and fail. We found this relates specifically to esptool.py v4.5.1 in the current stable release of the ESP32 Boards Package (2.0.11). Users can work around this error with one of the following three fixes: 
    
1. Use the alpha release (3.0.0 alpha3) of the ESP32 Boards Package. This release updates to esptool.py v4.6.
2. Download the latest release of esptool.py from the [GitHub repository](https://github.com/espressif/esptool/releases) releases and then manually replace it in the espressif boards package folder. 
3. If you want to keep using the current full release and encounter this error, force the board to enter the bootloader by holding the BOOT button down, clicking the RESET button, and then releasing the BOOT button.

### Digital Reset

Some users may run into an issue when uploading completes where the board does not digitally reset properly. If you notice your code is not running after upload finishes, just press the RESET button on the board and it should start running.

## General Troubleshooting

!!! warning "Need Help?"
    If you need technical assistance or more information on a product that is not working as you expected, we recommend heading on over to the [SparkFun Technical Assistance](https://www.sparkfun.com/technical_assistanc) page for some initial troubleshooting.

    <center>
    [SparkFun Technical Assistance Page](https://www.sparkfun.com/technical_assistance){ .md-button .md-button--primary }
    </center>
    
    If you can't find what you need there, the [SparkFun Forums](https://forum.sparkfun.com/index.php) is a great place to search product forums and ask questions.
    
    !!! info "Account Registration Required"
        If this is your first visit to our forum, you'll need to create a [Forum Account](https://forum.sparkfun.com/ucp.php?mode=register) to post questions.