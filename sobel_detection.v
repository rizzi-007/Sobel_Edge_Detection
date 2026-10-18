`timescale 1ns / 1ps

module axis_sobel_256x256 #(
    parameter IMG_WIDTH    = 256,
    parameter IMG_HEIGHT   = 256,
    parameter FRAME_PIXELS = 65536
)(
    input  wire        aclk,
    input  wire        aresetn,

    // AXI4-Stream Slave Interface (Connected to AXI DMA MM2S)
    input  wire [7:0]  s_axis_tdata,
    input  wire        s_axis_tvalid,
    output wire        s_axis_tready,
    input  wire        s_axis_tlast,

    // AXI4-Stream Master Interface (Connected to AXI DMA S2MM)
    output reg  [7:0]  m_axis_tdata,
    output reg         m_axis_tvalid,
    input  wire        m_axis_tready,
    output reg         m_axis_tlast
);

    // Flow control: enable data flow when valid input and ready for output
    wire pipe_en;
    assign s_axis_tready = m_axis_tready || !m_axis_tvalid;
    assign pipe_en       = s_axis_tvalid && s_axis_tready;

    // Two 256-byte circular line buffers (synthesizes to BRAM/Distributed RAM)
    reg [7:0] line_buf0 [0:IMG_WIDTH-1];
    reg [7:0] line_buf1 [0:IMG_WIDTH-1];
    reg [7:0] col_ptr;

    // 16-bit pixel counter to precisely frame 65,536 bytes
    reg [15:0] pixel_count;

    // 3x3 Sliding Window Matrix Registers
    // [ p00  p01  p02 ]
    // [ p10  p11  p12 ]
    // [ p20  p21  p22 ]
    reg [7:0] p00, p01, p02;
    reg [7:0] p10, p11, p12;
    reg [7:0] p20, p21, p22;

    // Gradient calculations (11-bit signed math to prevent overflow)
    wire signed [10:0] gx;
    wire signed [10:0] gy;
    wire        [10:0] abs_gx;
    wire        [10:0] abs_gy;
    wire        [10:0] mag;

    // Gx Convolution:
    // [ -1  0  +1 ]
    // [ -2  0  +2 ]
    // [ -1  0  +1 ]
    assign gx = ($signed({3'b0, p02}) + ($signed({3'b0, p12}) <<< 1) + $signed({3'b0, p22})) -
                ($signed({3'b0, p00}) + ($signed({3'b0, p10}) <<< 1) + $signed({3'b0, p20}));

    // Gy Convolution:
    // [ -1  -2  -1 ]
    // [  0   0   0 ]
    // [ +1  +2  +1 ]
    assign gy = ($signed({3'b0, p20}) + ($signed({3'b0, p21}) <<< 1) + $signed({3'b0, p22})) -
                ($signed({3'b0, p00}) + ($signed({3'b0, p01}) <<< 1) + $signed({3'b0, p02}));

    // Absolute value approximations
    assign abs_gx = (gx < 0) ? -gx : gx;
    assign abs_gy = (gy < 0) ? -gy : gy;

    // L1-norm gradient magnitude (|Gx| + |Gy|)
    assign mag = abs_gx + abs_gy;

    // Simulation memory initialization
    integer i;
    initial begin
        for (i = 0; i < IMG_WIDTH; i = i + 1) begin
            line_buf0[i] = 8'd0;
            line_buf1[i] = 8'd0;
        end
    end

    // Sequential Processing and Window Shift Pipeline
    always @(posedge aclk) begin
        if (!aresetn) begin
            col_ptr       <= 8'd0;
            pixel_count   <= 16'd0;
            m_axis_tvalid <= 1'b0;
            m_axis_tdata  <= 8'd0;
            m_axis_tlast  <= 1'b0;
            p00 <= 8'd0; p01 <= 8'd0; p02 <= 8'd0;
            p10 <= 8'd0; p11 <= 8'd0; p12 <= 8'd0;
            p20 <= 8'd0; p21 <= 8'd0; p22 <= 8'd0;
        end else if (pipe_en) begin
            // 1. Shift the 3x3 window horizontally
            p00 <= p01;
            p01 <= p02;
            p02 <= line_buf1[col_ptr];

            p10 <= p11;
            p11 <= p12;
            p12 <= line_buf0[col_ptr];

            p20 <= p21;
            p21 <= p22;
            p22 <= s_axis_tdata;

            // 2. Shift pixel data into vertical line buffers
            line_buf1[col_ptr] <= line_buf0[col_ptr];
            line_buf0[col_ptr] <= s_axis_tdata;

            // 3. Increment column pointer (wraps back to 0 at 256)
            col_ptr <= col_ptr + 8'd1;

            // 4. Output the Sobel edge pixel (saturated to 255)
            m_axis_tvalid <= 1'b1;
            m_axis_tdata  <= (mag > 11'd255) ? 8'd255 : mag[7:0];

            // 5. Assert TLAST on the 65,536th pixel (pixel_count == 65535)
            if (pixel_count == FRAME_PIXELS - 1) begin
                m_axis_tlast <= 1'b1;
                pixel_count  <= 16'd0;
            end else begin
                m_axis_tlast <= 1'b0;
                pixel_count  <= pixel_count + 16'd1;
            end
        end else if (m_axis_tready) begin
            // Drop valid and last when no new data is being processed
            m_axis_tvalid <= 1'b0;
            m_axis_tlast  <= 1'b0;
        end
    end

endmodule
