# Hardware-Accelerated Sobel Edge Detection on ZedBoard (Zynq-7000)

## Project Overview
This project implements a Hardware-Software Co-Design pipeline between a Windows PC and a Xilinx Zynq-7000 ZedBoard over a 1000 Mbps Gigabit Ethernet link. 

A Python OpenCV script captures a live webcam frame, converts it to a 256x256 grayscale image (65,536 bytes), and sends it over TCP/IP (Port 7) to the ZedBoard. The ARM Cortex-A9 processor stores the image in DDR3 RAM (`0x10000000`), flushes the cache, and uses an AXI Direct Memory Access (AXI DMA) controller to stream the pixels through a custom Verilog Sobel Edge Detection IP core in the FPGA fabric. The processed output is written to DDR3 (`0x10100000`) and sent back over Ethernet to the PC for display.

## System Specifications
* **Board:** Xilinx Zynq-7000 ZedBoard (`XC7Z020`)
* **ZedBoard Static IP:** `192.168.1.10` (Port `7`, MAC `00:0a:35:00:01:02`)
* **Host PC Static IP:** `192.168.1.20` (Subnet Mask `255.255.255.0`)
* **DDR3 Input Buffer:** `0x10000000` (64 KB)
* **DDR3 Output Buffer:** `0x10100000` (64 KB)

## Files in This Repository
* `main.c` - Initializes the Zynq Processing System, Gigabit Ethernet PHY (1000 Mbps), static IP (`192.168.1.10`), and the bare-metal lwIP event loop.
* `echo.c` - Receives the 64 KB image over TCP, manages ARM L1/L2 Data Cache coherency (`Xil_DCacheFlushRange` / `Xil_DCacheInvalidateRange`), triggers the AXI DMA transfer (`MM2S` and `S2MM`), and streams the processed Sobel output back to the PC.
* `sobel.py` - Python host script that captures a webcam frame, resizes it to 256x256 grayscale, uploads it to `0x10000000`, downloads the FPGA Sobel output from `0x10100000`, and displays both images side-by-side.
* `image.py` - Initial DDR3 memory loopback test script using 1 KB lockstep chunking.

## How to Run
1. Build `main.c` and `echo.c` in Vitis IDE with the `lwip213` library enabled, program the FPGA with the bitstream, and launch the application on the ZedBoard.
2. On Windows (Administrator Command Prompt), set the static IP and ARP entry:
   `netsh interface ipv4 set address name="Ethernet" static 192.168.1.20 255.255.255.0`
   `netsh interface ipv4 add neighbors "Ethernet" 192.168.1.10 00-0a-35-00-01-02`
3. Run the Python script:
   `py sobel.py`
