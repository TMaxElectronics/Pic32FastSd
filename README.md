# Pic32FastSd
An SD Card driver compatible with FatFs. Originally made by ChaN with edits from A. Morrison for PIC32.
Requires FreeRTOS as well as my SPI, DMA and DLL libraries to function.

# changes over the base version
Contains a FS management task to manage connection and removal of sd cards. Should eventually be thread safe (not yet lol, like not at all).

Also supports the new readList command with extra fast DMA reads. That together with the improvements of readFast gives a performance boost of around 4-5x over the default implementation, getting to only around 4% overhead with large reads (>10kB) while only using 10% cpu during the read.
Tested with a 32gb class 10 card aswell as with a cheap 8gb class {something bad} card, both getting to an average read speed of 6MBytes/sec with 50MHz SPI Speed (of the theoretical maximum of 6.25MB/s) on a 50kB file.

Implementation is however not really a library yet, and contains quite a few static statements (such as the cardAvailable function)
