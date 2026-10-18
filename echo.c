#include <stdio.h>
#include <string.h>
#include "lwip/err.h"
#include "lwip/tcp.h"
#include "xil_printf.h"
#include "xil_cache.h"
#include "xparameters.h"
#include "xaxidma.h"

#define DDR_RX_ADDR 0x10000000
#define DDR_TX_ADDR 0x10100000
#define FRAME_SIZE  65536
#define TX_CHUNK    4096

XAxiDma AxiDma;
static u32 rx_bytes = 0;
static u32 tx_bytes_sent = 0;

int transfer_data() {
    return 0;
}

void print_app_header() {
    xil_printf("\n\r--- FPGA Sobel Hardware Accelerator Server ---\n\r");
}

int init_dma() {
    XAxiDma_Config *CfgPtr;
    int status;

    CfgPtr = XAxiDma_LookupConfig(XPAR_AXIDMA_0_DEVICE_ID);
    if (!CfgPtr) {
        xil_printf("Error: No config found for AXI DMA\n\r");
        return XST_FAILURE;
    }

    status = XAxiDma_CfgInitialize(&AxiDma, CfgPtr);
    if (status != XST_SUCCESS) {
        xil_printf("Error: AXI DMA Initialization failed\n\r");
        return XST_FAILURE;
    }

    XAxiDma_IntrDisable(&AxiDma, XAXIDMA_IRQ_ALL_MASK, XAXIDMA_DEVICE_TO_DMA);
    XAxiDma_IntrDisable(&AxiDma, XAXIDMA_IRQ_ALL_MASK, XAXIDMA_DMA_TO_DEVICE);

    return XST_SUCCESS;
}

void send_next_chunk(struct tcp_pcb *tpcb) {
    while (tx_bytes_sent < FRAME_SIZE && tcp_sndbuf(tpcb) >= TX_CHUNK) {
        u32 remaining = FRAME_SIZE - tx_bytes_sent;
        u32 len = (remaining < TX_CHUNK) ? remaining : TX_CHUNK;
        err_t err = tcp_write(tpcb, (void *)(DDR_TX_ADDR + tx_bytes_sent), len, 1);
        if (err != ERR_OK) {
            break;
        }
        tx_bytes_sent += len;
    }
    tcp_output(tpcb);
}

err_t sent_callback(void *arg, struct tcp_pcb *tpcb, u16_t len) {
    if (tx_bytes_sent < FRAME_SIZE) {
        send_next_chunk(tpcb);
    }
    return ERR_OK;
}

err_t recv_callback(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
    if (!p) {
        tcp_close(tpcb);
        tcp_recv(tpcb, NULL);
        rx_bytes = 0;
        tx_bytes_sent = 0;
        return ERR_OK;
    }

    tcp_recved(tpcb, p->len);

    if (rx_bytes + p->len <= FRAME_SIZE) {
        memcpy((void *)(DDR_RX_ADDR + rx_bytes), p->payload, p->len);
    }
    rx_bytes += p->len;
    pbuf_free(p);

    if (rx_bytes >= FRAME_SIZE) {
        xil_printf("Stage 1 Complete: 64 KB at 0x10000000. Starting DMA...\n\r");

        Xil_DCacheFlushRange((INTPTR)DDR_RX_ADDR, FRAME_SIZE);
        Xil_DCacheInvalidateRange((INTPTR)DDR_TX_ADDR, FRAME_SIZE);

        XAxiDma_SimpleTransfer(&AxiDma, (UINTPTR)DDR_TX_ADDR, FRAME_SIZE, XAXIDMA_DEVICE_TO_DMA);
        XAxiDma_SimpleTransfer(&AxiDma, (UINTPTR)DDR_RX_ADDR, FRAME_SIZE, XAXIDMA_DMA_TO_DEVICE);

        while (XAxiDma_Busy(&AxiDma, XAXIDMA_DMA_TO_DEVICE) ||
               XAxiDma_Busy(&AxiDma, XAXIDMA_DEVICE_TO_DMA)) {
        }

        Xil_DCacheInvalidateRange((INTPTR)DDR_TX_ADDR, FRAME_SIZE);
        xil_printf("Stage 2 Complete: Sobel at 0x10100000. Streaming to PC...\n\r");

        rx_bytes = 0;
        tx_bytes_sent = 0;
        send_next_chunk(tpcb);
    }

    return ERR_OK;
}

err_t accept_callback(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    rx_bytes = 0;
    tx_bytes_sent = 0;
    tcp_recv(newpcb, recv_callback);
    tcp_sent(newpcb, sent_callback);
    return ERR_OK;
}

int start_application()
{
    struct tcp_pcb *pcb;
    err_t err;
    unsigned port = 7;

    if (init_dma() != XST_SUCCESS) {
        xil_printf("CRITICAL ERROR: AXI DMA init failed.\n\r");
        return -1;
    }

    pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (!pcb) {
        xil_printf("Error creating PCB.\n\r");
        return -1;
    }

    err = tcp_bind(pcb, IP_ANY_TYPE, port);
    if (err != ERR_OK) {
        xil_printf("Unable to bind to port %d\n\r", port);
        return -2;
    }

    tcp_arg(pcb, NULL);
    pcb = tcp_listen(pcb);
    if (!pcb) {
        xil_printf("Out of memory while tcp_listen\n\r");
        return -3;
    }

    tcp_accept(pcb, accept_callback);
    xil_printf("FPGA Sobel Server listening @ port %d\n\r", port);
    return 0;
}
