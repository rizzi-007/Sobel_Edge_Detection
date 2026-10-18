# Hardware-Accelerated Sobel Edge Detection on Xilinx Zynq-7000 SoC over Gigabit Ethernet

An end-to-end **Hardware-Software Co-Design** vision pipeline implemented on the **Xilinx Zynq-7000 ZedBoard (XC7Z020-CLG484-1)**. 

This project bridges a Windows host PC and an FPGA System-on-Chip over a direct, point-to-point **1000 Mbps Gigabit Ethernet** link using the **bare-metal lwIP (Lightweight IP) RAW TCP/IP stack**. Live webcam frames are captured via Python OpenCV, converted into $256 \times 256$ 8-bit grayscale matrices ($65,536\text{ bytes}$), and streamed into external physical **Micron DDR3 RAM (`0x10000000`)**. The **ARM Cortex-A9 Processing System (PS)** enforces L1/L2 hardware cache coherency and triggers a high-speed **AXI Direct Memory Access (AXI DMA)** transfer over the `S_AXI_HP0` interconnect into a custom **Verilog AXI4-Stream Sobel Edge Detection Accelerator** in the **Artix-7 Programmable Logic (PL)**. The processed edge map is written back to DDR3 (`0x10100000`) and returned over TCP to the host PC in milliseconds.

---

## 1. System Architecture & End-to-End Data Flow

The complete hardware/software pipeline operates across four distinct hardware domains:
1. **Host Application Layer:** Windows PC running Python (`OpenCV`, `NumPy`, `Winsock` TCP Client).
2. **Zynq Processing System (PS):** Dual-core ARM Cortex-A9 @ 667 MHz, Gigabit Ethernet Controller (`GEM0`), DDR3 Memory Controller, and L1/L2 Cache Controller.
3. **External Physical Memory:** 512 MB Micron DDR3 SDRAM partitioned into isolated Input (`0x10000000`) and Output (`0x10100000`) frame buffers.
4. **Zynq Programmable Logic (PL):** Xilinx AXI DMA IP Core (Simple Register Mode) and Custom Verilog $3 \times 3$ Sobel Spatial Convolution IP Core operating on AXI4-Stream interfaces.

<pre>
┌────────────────────────────────────────────────────────────────────────────┐
│ WINDOWS HOST PC (192.168.1.20) — sobel.py                                  │
│  1. Captures live webcam frame via OpenCV (cv2.VideoCapture)               │
│  2. Converts BGR to 8-bit Grayscale & resizes to 256x256 (65,536 bytes)    │
│  3. Stage 1: Streams 64 KB raw pixel payload over TCP Port 7 (1 KB chunks) │
└─────────────────────────────────────┬──────────────────────────────────────┘
                                      │ Physical RJ45 CAT6 Cable
                                      │ (1000 Mbps Full Duplex)
                                      ▼
┌────────────────────────────────────────────────────────────────────────────┐
│ ZYNQ-7000 PROCESSING SYSTEM (PS — ARM Cortex-A9 @ 192.168.1.10)            │
│  4. Marvell 88E1518 PHY (MDIO/RGMII) -> Zynq GEM0 MAC -> Ethernet DMA      │
│  5. xemacif_input() strips Ethernet/IPv4/TCP headers -> fires recv_callback│
│  6. Accumulates incoming TCP segments sequentially into DDR3 Input Buffer: │
│     memcpy((void *)(0x10000000 + rx_bytes), p->payload, p->len)            │
│  7. Once rx_bytes == 65,536:                                               │
│     • Xil_DCacheFlushRange(0x10000000, 65536) -> Pushes L1/L2 cache to RAM │
│     • Xil_DCacheInvalidateRange(0x10100000, 65536) -> Clears output cache  │
│  8. Programs AXI DMA registers over M_AXI_GP0 (AXI4-Lite Control Bus):     │
│     • S2MM Channel (Write): Destination = 0x10100000, Length = 65,536 B    │
│     • MM2S Channel (Read) : Source      = 0x10000000, Length = 65,536 B    │
└─────────────────────────────────────┬──────────────────────────────────────┘
                                      │ S_AXI_HP0 (High-Performance AXI4)
                                      ▼
┌────────────────────────────────────────────────────────────────────────────┐
│ ZYNQ-7000 PROGRAMMABLE LOGIC (PL — Artix-7 FPGA Fabric @ 100 MHz)          │
│                                                                            │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │ Xilinx AXI DMA Engine (MM2S Read Channel)                            │  │
│  │ Bursts 65,536 bytes from DDR3 (0x10000000) and converts memory words │  │
│  │ into a continuous byte stream (M_AXIS_MM2S: TDATA, TVALID, TREADY)   │  │
│  └──────────────────────────────────┬───────────────────────────────────┘  │
│                                     │ AXI4-Stream (8-bit Grayscale Pixels) │
│                                     ▼                                      │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │ Custom Verilog Sobel Edge Detection Core (axis_sobel_256x256.v)      │  │
│  │ • Buffers 2 full image rows (2 x 256-byte shift registers) to form   │  │
│  │   a 3x3 sliding pixel window on every single clock cycle             │  │
│  │ • Computes horizontal (Gx) and vertical (Gy) spatial derivatives     │  │
│  │ • Calculates gradient magnitude |G| = |Gx| + |Gy|, clamped to [0,255]│  │
│  │ • Asserts m_axis_tlast = 1 on the 65,536th output pixel              │  │
│  └──────────────────────────────────┬───────────────────────────────────┘  │
│                                     │ AXI4-Stream (8-bit Sobel Edges)      │
│                                     ▼                                      │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │ Xilinx AXI DMA Engine (S2MM Write Channel)                           │  │
│  │ Captures processed pixel stream and writes 65,536 bytes directly to  │  │
│  │ DDR3 Output Buffer (0x10100000). Drops Busy flag upon TLAST pulse    │  │
│  └──────────────────────────────────┬───────────────────────────────────┘  │
└─────────────────────────────────────┼──────────────────────────────────────┘
                                      │
                                      ▼
┌────────────────────────────────────────────────────────────────────────────┐
│ STAGE 2 READBACK & RECONSTRUCTION (Zynq PS -> Windows Host PC)             │
│  9. ARM CPU exits while(XAxiDma_Busy()) loop                               │
│ 10. Executes Xil_DCacheInvalidateRange(0x10100000, 65536) to force physical│
│     DDR3 readback of the newly written FPGA output                         │
│ 11. Streams 65,536 bytes from 0x10100000 over TCP in 4 KB chunks           │
│ 12. Python reshapes buffer into (256, 256) matrix & displays side-by-side  │
└────────────────────────────────────────────────────────────────────────────┘
</pre>

---

## 2. Hardware, Network, & Memory Specifications

| Parameter | ZedBoard (Zynq-7000 Target) | Windows Host PC (Client) |
| :--- | :--- | :--- |
| **Device / Processor** | Xilinx Zynq-7000 (`XC7Z020`), Dual ARM Cortex-A9 | Windows 10/11 (x86_64) |
| **IPv4 Address** | `192.168.1.10` (Static, DHCP Disabled) | `192.168.1.20` (Static) |
| **Subnet Mask** | `255.255.255.0` (`/24`) | `255.255.255.0` (`/24`) |
| **Default Gateway** | `192.168.1.1` | None (Direct Point-to-Point Link) |
| **Hardware MAC Address** | `00:0a:35:00:01:02` (Xilinx OUI) | Host NIC Hardware Default |
| **Physical Link Speed** | `1000 Mbps` Full Duplex (Marvell 88E1518 PHY) | Auto-Negotiation (Power Saving Disabled) |
| **Transport Protocol** | Bare-Metal lwIP `v2.1.3` RAW API (TCP Port `7`) | Python `socket` (`AF_INET`, `SOCK_STREAM`, Port `7`) |
| **Image Resolution** | $256 \times 256$ pixels, 8-bit Grayscale (`65,536` bytes) | Resized via OpenCV `cv2.resize()` |
| **DDR3 Input Buffer** | `0x10000000` (`DDR_RX_ADDR`, 256 MB offset) | Stage 1 Upload (`1024-byte` chunks) |
| **DDR3 Output Buffer** | `0x10100000` (`DDR_TX_ADDR`, 257 MB offset) | Stage 2 Download (`4096-byte` chunks) |
| **DMA Architecture** | Xilinx AXI DMA (`S_AXI_HP0` 64-bit High-Performance Port) | Controlled via `XAxiDma_SimpleTransfer` |

---

## 3. Mathematical & Hardware Design of the Sobel Core

### 3.1 Grayscale Luminance Conversion (Host Preprocessing)
Before transmission, the host converts the 24-bit BGR webcam frame into an 8-bit single-channel grayscale intensity matrix $I(x,y) \in [0, 255]$ using standard ITU-R BT.601 luminance weights:

$$Y = 0.299R + 0.587G + 0.114B$$

### 3.2 2D Spatial Convolution in FPGA Hardware
For every pixel coordinate $(x, y)$ in the $256 \times 256$ frame, the FPGA Sobel core forms a $3 \times 3$ local neighborhood window $\mathbf{W}$:

$$\mathbf{W} = \begin{bmatrix} P_{0,0} & P_{0,1} & P_{0,2} \\ P_{1,0} & P_{1,1} & P_{1,2} \\ P_{2,0} & P_{2,1} & P_{2,2} \end{bmatrix}$$

The horizontal gradient ($G_x$) and vertical gradient ($G_y$) are computed simultaneously in a single clock cycle by convolving $\mathbf{W}$ with the $3 \times 3$ Sobel operators:

$$G_x = \begin{bmatrix} -1 & 0 & +1 \\ -2 & 0 & +2 \\ -1 & 0 & +1 \end{bmatrix} * \mathbf{W} = (P_{0,2} + 2P_{1,2} + P_{2,2}) - (P_{0,0} + 2P_{1,0} + P_{2,0})$$

$$G_y = \begin{bmatrix} -1 & -2 & -1 \\ 0 & 0 & 0 \\ +1 & +2 & +1 \end{bmatrix} * \mathbf{W} = (P_{2,0} + 2P_{2,1} + P_{2,2}) - (P_{0,0} + 2P_{0,1} + P_{0,2})$$

* **Multiplier-Free Hardware Optimization:** Multiplication by $2$ is implemented in Verilog using a 1-bit hardware left-shift (`<<< 1`), requiring zero DSP48 slices for the kernel weights.
* **Gradient Magnitude & Clamping:** To avoid costly square-root hardware ($\sqrt{G_x^2 + G_y^2}$), the L1-norm magnitude approximation is computed and saturated to 8 bits:

$$\vert{}G\vert{} = \vert{}G_x\vert{} + \vert{}G_y\vert{}, \quad P_{\text{out}} = \min(\vert{}G\vert{}, 255)$$

### 3.3 Streaming Line Buffer & `TLAST` Architecture
Because AXI4-Stream delivers one pixel per clock cycle in raster-scan order, the Verilog core implements **two 256-byte circular line buffers** (`line_buf0` and `line_buf1`) feeding nine 8-bit pipeline registers (`p00` through `p22`).
* **Throughput:** 1 pixel per clock cycle ($65,536\text{ cycles} = 655.36\ \mu\text{s}$ per frame at $100\text{ MHz}$).
* **AXI DMA Packet Framing (`TLAST`):** The AXI DMA `S2MM` (Stream-to-Memory-Mapped) state machine requires the `m_axis_tlast` signal to assert high on the exact final byte of the transfer. A 16-bit hardware counter (`pixel_count`) tracks valid handshakes (`s_axis_tvalid && s_axis_tready`) and pulses `m_axis_tlast = 1'b1` when `pixel_count == 65535`.

---

## 4. Repository File Structure & Code Responsibilities

### 4.1 `main.c` — Bare-Metal Platform, PHY Link, & Superloop
* **Static IP Override (`#undef LWIP_DHCP` / `#define LWIP_DHCP 0`):** Overrides the Vitis BSP `lwipopts.h` DHCP setting so the board does not hang waiting for a router on a direct PC-to-board cable connection.
* **Physical Layer Initialization (`xemac_add`):** Configures Zynq `GEM0` at `0xE000B000`, sets up Scatter-Gather Ethernet DMA descriptors, and communicates over the MDIO bus with the onboard Marvell 88E1518 PHY to auto-negotiate a `1000 Mbps` full-duplex link.
* **Event Superloop (`while(1)`):** Services the 250 ms (`TcpFastTmrFlag`) and 500 ms (`TcpSlowTmrFlag`) hardware timer interrupts for TCP acknowledgments/retransmissions and continuously polls `xemacif_input(echo_netif)` to pull incoming Ethernet frames from DMA into lwIP.

### 4.2 `echo.c` — TCP Server, Cache Coherency, & AXI DMA Controller
* **`init_dma()`:** Looks up `XPAR_AXIDMA_0_DEVICE_ID`, initializes the `XAxiDma` driver instance, resets the FPGA DMA engine, and disables DMA interrupts for deterministic polling mode.
* **`recv_callback()`:**
  1. Accumulates incoming TCP segments (`p->payload`) into physical DDR3 RAM starting at `0x10000000 + rx_bytes`.
  2. When `rx_bytes == 65536`, calls `Xil_DCacheFlushRange(0x10000000, 65536)` so the ARM Cortex-A9 pushes all cached pixel lines out to external physical DDR3 prior to FPGA access.
  3. Calls `XAxiDma_SimpleTransfer()` on both `XAXIDMA_DEVICE_TO_DMA` (S2MM destination `0x10100000`) and `XAXIDMA_DMA_TO_DEVICE` (MM2S source `0x10000000`).
  4. Polls `XAxiDma_Busy()` until both channels complete.
  5. Calls `Xil_DCacheInvalidateRange(0x10100000, 65536)` to discard stale ARM cache lines and force the CPU to read fresh FPGA output directly from physical DDR3.
* **`send_next_chunk()` & `sent_callback()`:** Streams the 65,536-byte output buffer from `0x10100000` in safe `4096-byte` slices as TCP `ACK` packets arrive from the host PC, preventing bare-metal `tcp_sndbuf` exhaustion.

### 4.3 `sobel.py` & `image.py` — Host Client Scripts
* **`image.py` (Phase 1 DDR3 Memory Loopback Verifier):** Captures a webcam frame, compresses it to JPEG (~26–31 KB), and streams it in `1024-byte` lockstep chunks to `0x10000000` and back to verify bit-accurate physical DDR3 read/write integrity.
* **`sobel.py` (Phase 2 Full-Frame FPGA Sobel Pipeline):** Captures a webcam frame, converts it to a $256 \times 256$ grayscale array (`65,536` bytes), uploads the full frame to `0x10000000` (Stage 1), downloads the 64 KB FPGA-processed frame from `0x10100000` (Stage 2), and displays the original and Sobel edge-detected frames side-by-side (`np.hstack`).

---

## 5. Key Engineering Challenges & Debugging Solutions

| Challenge / Error Encountered | Root Technical Cause | Engineering Fix Implemented |
| :--- | :--- | :--- |
| **1. Firmware Hang on Boot (No IP Printed)** | Vitis BSP sets `LWIP_DHCP = 1` by default. On a direct cable with no router, `dhcp_start()` traps the ARM CPU in an infinite polling loop waiting for a DHCP Offer. | Forced `#undef LWIP_DHCP` and `#define LWIP_DHCP 0` in `main.c` and hardcoded `192.168.1.10`. |
| **2. `Auto negotiation error` / `Phy setup failure init_emacps`** | Zynq `GEM0` timed out polling the Marvell PHY over MDIO because Windows Power Management put the laptop's Ethernet transceiver into low-power sleep when idle. | Disabled NIC power saving & Energy Efficient Ethernet in Windows Device Manager; locked link at `1000 Mbps`. |
| **3. Subnet Hijack (`172.22.18.61`) & APIPA (`169.254.126.20`)** | Windows Ethernet defaulted to DHCP, assigning a `169.254.x.x` APIPA address and routing pings to a virtual WSL/Hyper-V switch (`172.22.18.61`). | Forced static IP `192.168.1.20` (`255.255.255.0`) on the physical Ethernet adapter via Admin `netsh`. |
| **4. ARP Resolution Failure (`Reply from 192.168.1.20: Unreachable`)** | Resetting the Windows NIC momentarily dropped the board's PHY state, causing ARP broadcasts for `192.168.1.10` to go unanswered. | Bound `192.168.1.10` to MAC `00-0a-35-00-01-02` permanently in the Windows ARP table (`netsh add neighbors`). |
| **5. TCP Deadlock on Multi-Kilobyte Frame Burst** | Sending 30–64 KB in a single burst overflowed the Zynq's 8 KB bare-metal `tcp_sndbuf`, dropping packets and freezing Python's `recv()` loop. | Implemented `1024-byte` chunked uploads in Python and `4096-byte` ACK-driven `sent_callback` streaming in C. |
| **6. Non-Coherent DMA Stale Memory Reads** | The Zynq `S_AXI_HP0` port connects directly to the DDR3 controller and cannot snoop the ARM L1/L2 caches. | Added `Xil_DCacheFlushRange()` before DMA start and `Xil_DCacheInvalidateRange()` after DMA completion. |

---

## 6. Step-by-Step Guide to Reproduce & Run

### Step 1: Vivado & Vitis Hardware/Firmware Setup
1. Open the hardware project in **Xilinx Vivado**, synthesize the block design (containing Zynq7 Processing System, AXI DMA in Simple Mode connected to `S_AXI_HP0`, and the `axis_sobel_256x256` core), generate the bitstream, and export the `.xsa` file.
2. Open **Xilinx Vitis IDE**, create a bare-metal application project from your `.xsa` platform, and enable the **`lwip213`** library in the Board Support Package (BSP) settings.
3. Replace `main.c` and `echo.c` in the application `src/` directory with the files from this repository.
4. Build the project, power on the ZedBoard with the JTAG, UART (`COM4` @ `115200` baud), and RJ45 Ethernet cables connected, and click **Run As -> Launch Hardware**.
5. Confirm the Vitis Serial Terminal prints:
<pre>
--- FPGA Sobel Hardware Accelerator Server ---
Start PHY autonegotiation
Waiting for PHY to complete autonegotiation.
autonegotiation complete
Using default Speed from design
link speed for phy address 0: 1000
Board IP: 192.168.1.10
Netmask : 255.255.255.0
Gateway : 192.168.1.1
FPGA Sobel Server listening @ port 7
</pre>

### Step 2: Configure Windows Host Network (Run in Administrator CMD)
Open Command Prompt as **Administrator** and configure the static IPv4 route and static ARP binding:
<pre>
netsh interface ipv4 set address name="Ethernet" static 192.168.1.20 255.255.255.0
netsh interface ipv4 add neighbors "Ethernet" 192.168.1.10 00-0a-35-00-01-02
ping 192.168.1.10
</pre>
Verify that `ping 192.168.1.10` replies in `1ms` with `0% loss`.

### Step 3: Execute the Python Sobel Pipeline
Install the required Python packages and launch `sobel.py`:
<pre>
pip install opencv-python numpy
py sobel.py
</pre>

**Expected Host Terminal Output:**
<pre>
1. Opening webcam...
2. Prepared 65536-byte grayscale frame (256x256).
3. Connecting to ZedBoard at 192.168.1.10:7...
Connected!
-> Stage 1: Uploading 64 KB grayscale image to DDR3 (0x10000000)...
-> Stage 2: Downloading 64 KB FPGA Sobel output from DDR3 (0x10100000)...
Transfer + FPGA Execution Time: 0.0482 seconds
Success! Press any key on the image window to exit.
</pre>

<img width="1315" height="711" alt="Screenshot 2026-10-14 191529" src="https://github.com/user-attachments/assets/d502f76b-812b-4fbf-a288-0c8f023b0a7b" />

