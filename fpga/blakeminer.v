/* blakeminer.v copyright kramble 2013
 * Based on https://github.com/teknohog/Open-Source-FPGA-Bitcoin-Miner/tree/master/projects/Xilinx_cluster_cgminer
 * Hub code for a cluster of miners using async links
 * by teknohog
 */

module blakeminer (CLOCK, RxD, TxD, LED 

`ifdef HEX_INDICATOR
	,HEX0, HEX1, HEX2, HEX3, HEX4, HEX5
`endif
				);

	function integer clog2;		// Courtesy of razorfishsl, replaces $clog2()
		input integer value;
		begin
		value = value-1;
		for (clog2=0; value>0; clog2=clog2+1)
		value = value>>1;
		end
	endfunction

`ifdef SPEED_MHZ
	parameter SPEED_MHZ = `SPEED_MHZ;
`else
	parameter SPEED_MHZ = 50;						// Default to slow, use dynamic config to ramp up in realtime
`endif

`ifdef SPEED_LIMIT
	parameter SPEED_LIMIT = `SPEED_LIMIT;			// Fastest speed accepted by dyn_pll config
`else
	parameter SPEED_LIMIT = 250;
`endif

`ifdef SPEED_MIN
	parameter SPEED_MIN = `SPEED_MIN;				// Slowest speed accepted by dyn_pll config (CARE can lock up if too low)
`else
	parameter SPEED_MIN = 10;
`endif

`ifdef SERIAL_CLK
	parameter comm_clk_frequency = `SERIAL_CLK;
`else
	parameter comm_clk_frequency = 12_500_000;		// 100MHz divide 8
`endif

`ifdef BAUD_RATE
	parameter BAUD_RATE = `BAUD_RATE;
`else
	parameter BAUD_RATE = 115_200;
`endif

// Miners on the same FPGA with this hub
//look at project settings
//cyclone_v revision for DE10 board has 3 local miners
//MAX10 revision of marsohod3 board has 1 miner
`ifdef LOCAL_MINERS
	parameter LOCAL_MINERS = `LOCAL_MINERS;
`else
	parameter LOCAL_MINERS = 4; // One to four cores
`endif

	localparam SLAVES = LOCAL_MINERS;

	input wire CLOCK; wire osc_clk; assign osc_clk = CLOCK;
	input	wire RxD;
	output wire TxD;
	output wire [7:0]LED;

`ifdef HEX_INDICATOR
	output wire [6:0]HEX0;
	output wire [6:0]HEX1;
	output wire [6:0]HEX2;
	output wire [6:0]HEX3;
	output wire [6:0]HEX4;
	output wire [6:0]HEX5;
`endif
	
	wire hash_clk, uart_clk, unused_clk;

`ifndef SIM
	`ifdef MAX10
		m10pll mpll_inst (
			.areset(1'b0),
			.inclk0(osc_clk),
			.c0(hash_clk),
			.c1(uart_clk),
			.locked() );
	`else
		mpll mpll_inst ( .refclk(osc_clk), .rst(1'b0), .outclk_0(hash_clk), .outclk_1(uart_clk));
	`endif
		assign unused_clk = uart_clk;
`else
	assign hash_clk = osc_clk;
	assign uart_clk = osc_clk;
	assign unused_clk = osc_clk;
`endif
  
	wire reset, nonce_chip;
	assign reset = 1'b0;
	assign nonce_chip = 1'b0;

	// Results from the input buffers (in serial_hub.v) of each slave
	wire [SLAVES*32-1:0]	slave_nonces;
	wire [SLAVES-1:0]		new_nonces;

	// Using the same transmission code as individual miners from serial.v
	wire		serial_send;
	wire		serial_busy;
	wire [31:0]	golden_nonce;
	
	serial_transmit #(.comm_clk_frequency(comm_clk_frequency), .baud_rate(BAUD_RATE)) sertx (.clk(uart_clk), .TxD(TxD), .send(serial_send), .busy(serial_busy), .word(golden_nonce));
	hub_core #(.SLAVES(SLAVES)) hc (.uart_clk(uart_clk), .new_nonces(new_nonces), .golden_nonce(golden_nonce), .serial_send(serial_send), .serial_busy(serial_busy), .slave_nonces(slave_nonces));

	// Common workdata input for local miners
	wire [255:0]	data1;			// midstate
	wire [127:0]	data2;
	wire [31:0]		target;
	reg  [31:0]		targetreg = 32'h000007ff;	// NB Target is only use to set clock speed in BLAKE
	wire			rx_done;		// Signals hashcore to reset the nonce
									// NB in my implementation, it loads the nonce from data2 which should be fine as
									// this should be zero, but also supports testing using non-zero nonces.

	// Synchronise across clock domains from uart_clk to hash_clk
	// This probably looks amateurish (mea maxima culpa, novice verilogger at work), but should be OK
	reg rx_done_toggle = 1'b0;		// uart_clk domain
	always @ (posedge uart_clk)
		rx_done_toggle <= rx_done_toggle ^ rx_done;

	reg rx_done_toggle_d1 = 1'b0;	// hash_clk domain
	reg rx_done_toggle_d2 = 1'b0;
	reg rx_done_toggle_d3 = 1'b0;
	
	wire loadnonce;
	assign loadnonce = rx_done_toggle_d3 ^ rx_done_toggle_d2;

	always @ (posedge hash_clk)
	begin
		rx_done_toggle_d1 <= rx_done_toggle;
		rx_done_toggle_d2 <= rx_done_toggle_d1;
		rx_done_toggle_d3 <= rx_done_toggle_d2;
		if (loadnonce)
			targetreg <= target;
	end
	// End of clock domain sync
	
	serial_receive #(.comm_clk_frequency(comm_clk_frequency), .baud_rate(BAUD_RATE)) serrx (.clk(uart_clk), .RxD(RxD), .data1(data1),
						.data2(data2), .target(target), .rx_done(rx_done));

	reg [255:0]	data1sr;			// midstate
	reg [127:0]	data2sr;
	wire din = data1sr[255];
	reg shift = 0;
	reg [11:0] shift_count = 0;
	reg [15:0] allones;				// Fudge to ensure ISE does NOT optimise the shift registers re-creating the huge global
									// buses that are unroutable. Its probably not needed, but I just want to be sure
	always @ (posedge hash_clk)
	begin
		shift <= (shift_count != 0);
		if (shift_count != 0)
			shift_count <= shift_count + 1;
		if (loadnonce)
		begin
			data1sr <= data1;
			data2sr <= data2;
			shift_count <= shift_count + 1;
		end
		else if (shift)
		begin
			data1sr <= { data1sr[254:0], data2sr[127] };
			data2sr <= { data2sr[126:0], 1'b0 };
		end
		if (shift_count == 384)
			shift_count <= 0;
		allones <= { allones[14:0], targetreg[31] | ~targetreg[30] | targetreg[23] | ~targetreg[22] };	// Fudge
	end
	
	// Local miners now directly connected
	generate
		genvar i;
		for (i = 0; i < LOCAL_MINERS; i = i + 1)
		begin: miners
			wire [31:0] nonce_out;	// Not used
			wire [1:0] nonce_core = i;
			wire gn_match;

`ifdef SIMX
			hashcore M (hash_clk, din & allones[i], shift, 3'd7, nonce_out,		// Fixed 111 prefix in SIM to match genesis block
							slave_nonces[i*32+31:i*32], gn_match, loadnonce);
`else							
			hashcore M (hash_clk, din & allones[i], shift, 
					{nonce_chip, nonce_core}, 
					nonce_out,
					slave_nonces[i*32+31:i*32], 
					gn_match, 
					loadnonce);
`endif				
			// Synchronise across clock domains from hash_clk to uart_clk for: assign new_nonces[i] = gn_match;
			reg gn_match_toggle = 1'b0;		// hash_clk domain
			always @ (posedge hash_clk)
				gn_match_toggle <= gn_match_toggle ^ gn_match;

			reg gn_match_toggle_d1 = 1'b0;	// uart_clk domain
			reg gn_match_toggle_d2 = 1'b0;
			reg gn_match_toggle_d3 = 1'b0;

			assign new_nonces[i] = gn_match_toggle_d3 ^ gn_match_toggle_d2;

			always @ (posedge uart_clk)
			begin
				gn_match_toggle_d1 <= gn_match_toggle;
				gn_match_toggle_d2 <= gn_match_toggle_d1;
				gn_match_toggle_d3 <= gn_match_toggle_d2;
			end
			// End of clock domain sync
		end // for
	endgenerate

	assign LED[7] = ~RxD;

`define FLASHCLOCK		// Gives some feedback as the the actual clock speed

`ifdef FLASHCLOCK
	reg [26:0] hash_count = 0;
	reg [3:0] sync_hash_count = 0;
	always @ (posedge uart_clk)
		if (rx_done)
			sync_hash_count[0] <= ~sync_hash_count[0];
	always @ (posedge hash_clk)
	begin
		sync_hash_count[3:1] <= sync_hash_count[2:0];
		hash_count <= hash_count + 1'b1;
		if (sync_hash_count[3] != sync_hash_count[2])
			hash_count <= 0;
	end
	assign LED[6] = hash_count[26];
`else
	assign LED[6] = ~TxD;
`endif


reg [31:0]gold = 0;
reg [7:0]gn_cnt = 0;

always @(posedge hash_clk)
	//if( loadnonce )
		//gold <= 0; //start work
	//else
	if(serial_send)
	begin
		gold <= golden_nonce;
		if(golden_nonce!=32'hFFFFFFFF)
			gn_cnt<=gn_cnt+1;
	end
assign LED[5:0] = gn_cnt[5:0];

`ifdef HEX_INDICATOR
function  [7:0]seg7;
input [3:0]a;
begin
	case(a)
	0: seg7 = 63;
	1: seg7 = 6;
	2: seg7 = 91;
	3: seg7 = 79;
	4: seg7 = 102;
	5: seg7 = 109;
	6: seg7 = 125;
	7: seg7 = 7;
	8: seg7 = 127;
	9: seg7 = 111;
	10: seg7 = 119;
	11: seg7 = 124;
	12: seg7 = 57;
	13: seg7 = 94;
	14: seg7 = 121;
	15: seg7 = 113;
	endcase
end
endfunction

assign HEX0 = seg7(gold[3 :0])^8'hFF;
assign HEX1 = seg7(gold[7 :4])^8'hFF;
assign HEX2 = seg7(gold[11:8])^8'hFF;
assign HEX3 = seg7(gold[15:12])^8'hFF;
assign HEX4 = seg7(gold[19:16])^8'hFF;
assign HEX5 = seg7(gold[23:20])^8'hFF;
`endif

endmodule
