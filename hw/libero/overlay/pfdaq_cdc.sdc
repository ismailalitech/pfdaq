# pfdaq clock-domain crossings (project constraints, see rtl/pfdaq_stream_src.v header).
# PCLK = FIC3 clock (50 MHz), ACLK = FIC0 clock (125 MHz); both come from the same PLL,
# so without these lines SmartTime times every crossing as if it were synchronous.
# Every path listed here is either quasi-static (written only while the source is idle,
# or held stable while a synchronised toggle crosses) or the first stage of a 2-FF
# synchroniser. Nothing else between the two domains is excluded.
set pf FIC_0_PERIPHERALS_0/pfdaq_core_0/u_src

# ACLK snapshot registers -> PCLK copies (copied only after the synchronised toggle)
set_false_path -from [ get_cells "$pf/snap_*" ] -to [ get_cells "$pf/snap_*_p*" ]
set_false_path -from [ get_cells "$pf/snap_is_done*" ] -to [ get_cells "$pf/done_p*" ]

# PCLK configuration -> ACLK (sampled on the synchronised START; CREDIT value held during its toggle)
set_false_path -from [ get_cells { FIC_0_PERIPHERALS_0/pfdaq_core_0/u_src/tlast_per_frame* FIC_0_PERIPHERALS_0/pfdaq_core_0/u_src/payload_words* FIC_0_PERIPHERALS_0/pfdaq_core_0/u_src/frame_count* FIC_0_PERIPHERALS_0/pfdaq_core_0/u_src/gap_cycles* FIC_0_PERIPHERALS_0/pfdaq_core_0/u_src/fault* FIC_0_PERIPHERALS_0/pfdaq_core_0/u_src/mode_* FIC_0_PERIPHERALS_0/pfdaq_core_0/u_src/rate* FIC_0_PERIPHERALS_0/pfdaq_core_0/u_src/fir_step* FIC_0_PERIPHERALS_0/pfdaq_core_0/u_src/coef_p* FIC_0_PERIPHERALS_0/pfdaq_core_0/u_src/credit_val_p* } ] -to [ get_clocks { CLOCKS_AND_RESETS_0/CCC_FIC_x_CLK/PF_CCC_C0_0/pll_inst_0/OUT0 } ]

# first stage of each 2-FF synchroniser. Bracketed register names cannot be matched here,
# so the paths are named by their endpoints: only the first stage of each chain is fed from
# the other clock domain, so these select exactly the synchroniser inputs.
set_false_path -from [ get_cells {FIC_0_PERIPHERALS_0/pfdaq_core_0/u_src/*_tgl_p*} ] -to [ get_cells {FIC_0_PERIPHERALS_0/pfdaq_core_0/u_src/*_sync_a*} ]
set_false_path -from [ get_cells {FIC_0_PERIPHERALS_0/pfdaq_core_0/u_src/busy_a* FIC_0_PERIPHERALS_0/pfdaq_core_0/u_src/snap_tgl_a*} ] -to [ get_cells {FIC_0_PERIPHERALS_0/pfdaq_core_0/u_src/*_sync_p*} ]
