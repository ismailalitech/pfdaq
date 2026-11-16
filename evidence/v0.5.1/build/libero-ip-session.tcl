# Microsemi Tcl Script
# libero
# Date: Sun Nov 15 23:21:25 2026
# Directory /tmp/claude-1000/ip
# File /tmp/claude-1000/ip/session.tcl


open_project -file {./demo/demo/demo.prjx} -do_backup_on_convert 1 -backup_file {./demo/demo.zip} 
import_files \
         -convert_EDN_to_HDL 0 \
         -library {work} \
         -hdl_source {./pfdaq_demo_regs.v} 
build_design_hierarchy 
create_hdl_core -file {./demo/demo/hdl/pfdaq_demo_regs.v} -module {pfdaq_demo_regs} -library {work} -package {} 
hdl_core_add_bif -hdl_core_name {pfdaq_demo_regs} -bif_definition {APB:AMBA:AMBA2:slave} -bif_name {BIF_1} -signal_map {} 
hdl_core_assign_bif_signal -hdl_core_name {pfdaq_demo_regs} -bif_name {BIF_1} -bif_signal_name {PADDR} -core_signal_name {PADDR} 
hdl_core_assign_bif_signal -hdl_core_name {pfdaq_demo_regs} -bif_name {BIF_1} -bif_signal_name {PENABLE} -core_signal_name {PENABLE} 
hdl_core_assign_bif_signal -hdl_core_name {pfdaq_demo_regs} -bif_name {BIF_1} -bif_signal_name {PWRITE} -core_signal_name {PWRITE} 
hdl_core_assign_bif_signal -hdl_core_name {pfdaq_demo_regs} -bif_name {BIF_1} -bif_signal_name {PRDATA} -core_signal_name {PRDATA} 
hdl_core_assign_bif_signal -hdl_core_name {pfdaq_demo_regs} -bif_name {BIF_1} -bif_signal_name {PWDATA} -core_signal_name {PWDATA} 
hdl_core_assign_bif_signal -hdl_core_name {pfdaq_demo_regs} -bif_name {BIF_1} -bif_signal_name {PREADY} -core_signal_name {PREADY} 
hdl_core_assign_bif_signal -hdl_core_name {pfdaq_demo_regs} -bif_name {BIF_1} -bif_signal_name {PSLVERR} -core_signal_name {PSLVERR} 
hdl_core_assign_bif_signal -hdl_core_name {pfdaq_demo_regs} -bif_name {BIF_1} -bif_signal_name {PSELx} -core_signal_name {PSEL} 
hdl_core_rename_bif -hdl_core_name {pfdaq_demo_regs} -current_bif_name {BIF_1} -new_bif_name {APB_TARGET} 
hdl_core_add_bif -hdl_core_name {pfdaq_demo_regs} -bif_definition {AXI4Stream:AMBA:AMBA4:master} -bif_name {BIF_2} -signal_map {} 
hdl_core_assign_bif_signal -hdl_core_name {pfdaq_demo_regs} -bif_name {BIF_2} -bif_signal_name {TVALID} -core_signal_name {TVALID} 
hdl_core_assign_bif_signal -hdl_core_name {pfdaq_demo_regs} -bif_name {BIF_2} -bif_signal_name {TREADY} -core_signal_name {TREADY} 
hdl_core_assign_bif_signal -hdl_core_name {pfdaq_demo_regs} -bif_name {BIF_2} -bif_signal_name {TDATA} -core_signal_name {TDATA} 
hdl_core_assign_bif_signal -hdl_core_name {pfdaq_demo_regs} -bif_name {BIF_2} -bif_signal_name {TLAST} -core_signal_name {TLAST} 
hdl_core_rename_bif -hdl_core_name {pfdaq_demo_regs} -current_bif_name {BIF_2} -new_bif_name {AXIS_INITIATOR} 
create_smartdesign -sd_name {demo_top} 
sd_instantiate_hdl_core -sd_name {demo_top} -hdl_core_name {pfdaq_demo_regs} -instance_name {} 
create_and_configure_core -core_vlnv {Actel:DirectCore:CoreAPB3:4.2.100} -component_name {CoreAPB3_C0} -params {"APBSLOT0ENABLE:true" "APBSLOT10ENABLE:false" "APBSLOT11ENABLE:false" "APBSLOT12ENABLE:false" "APBSLOT13ENABLE:false" "APBSLOT14ENABLE:false" "APBSLOT15ENABLE:false" "APBSLOT1ENABLE:false" "APBSLOT2ENABLE:false" "APBSLOT3ENABLE:false" "APBSLOT4ENABLE:false" "APBSLOT5ENABLE:false" "APBSLOT6ENABLE:false" "APBSLOT7ENABLE:false" "APBSLOT8ENABLE:false" "APBSLOT9ENABLE:false" "APB_DWIDTH:32" "IADDR_OPTION:0" "MADDR_BITS:28" "SC_0:false" "SC_10:false" "SC_11:false" "SC_12:false" "SC_13:false" "SC_14:false" "SC_15:false" "SC_1:false" "SC_2:false" "SC_3:false" "SC_4:false" "SC_5:false" "SC_6:false" "SC_7:false" "SC_8:false" "SC_9:false" "UPR_NIBBLE_POSN:6"} 
sd_instantiate_component -sd_name {demo_top} -component_name {CoreAPB3_C0} -instance_name {} 
sd_connect_pins -sd_name {demo_top} -pin_names {"CoreAPB3_C0_0:APBmslave0" "pfdaq_demo_regs_0:APB_TARGET"} 
sd_connect_pin_to_port -sd_name {demo_top} -pin_name {CoreAPB3_C0_0:APB3mmaster} -port_name {} 
sd_connect_pin_to_port -sd_name {demo_top} -pin_name {pfdaq_demo_regs_0:AXIS_INITIATOR} -port_name {} 
sd_connect_pin_to_port -sd_name {demo_top} -pin_name {pfdaq_demo_regs_0:PCLK} -port_name {} 
sd_connect_pin_to_port -sd_name {demo_top} -pin_name {pfdaq_demo_regs_0:PRESETN} -port_name {} 
generate_component -component_name {demo_top} -recursive 0 
build_design_hierarchy 
set_root -module {demo_top::work} 
import_files \
         -convert_EDN_to_HDL 0 \
         -library {work} \
         -hdl_source {./pfdaq_demo_master.v} 
build_design_hierarchy 
create_hdl_core -file {./demo/demo/hdl/pfdaq_demo_master.v} -module {pfdaq_demo_master} -library {work} -package {} 
hdl_core_add_bif -hdl_core_name {pfdaq_demo_master} -bif_definition {APB:AMBA:AMBA2:master} -bif_name {BIF_1} -signal_map {} 
hdl_core_assign_bif_signal -hdl_core_name {pfdaq_demo_master} -bif_name {BIF_1} -bif_signal_name {PADDR} -core_signal_name {PADDR} 
hdl_core_assign_bif_signal -hdl_core_name {pfdaq_demo_master} -bif_name {BIF_1} -bif_signal_name {PENABLE} -core_signal_name {PENABLE} 
hdl_core_assign_bif_signal -hdl_core_name {pfdaq_demo_master} -bif_name {BIF_1} -bif_signal_name {PWRITE} -core_signal_name {PWRITE} 
hdl_core_assign_bif_signal -hdl_core_name {pfdaq_demo_master} -bif_name {BIF_1} -bif_signal_name {PRDATA} -core_signal_name {PRDATA} 
hdl_core_assign_bif_signal -hdl_core_name {pfdaq_demo_master} -bif_name {BIF_1} -bif_signal_name {PWDATA} -core_signal_name {PWDATA} 
hdl_core_assign_bif_signal -hdl_core_name {pfdaq_demo_master} -bif_name {BIF_1} -bif_signal_name {PREADY} -core_signal_name {PREADY} 
hdl_core_assign_bif_signal -hdl_core_name {pfdaq_demo_master} -bif_name {BIF_1} -bif_signal_name {PSLVERR} -core_signal_name {PSLVERR} 
hdl_core_rename_bif -hdl_core_name {pfdaq_demo_master} -current_bif_name {BIF_1} -new_bif_name {APB_INITIATOR} 
hdl_core_assign_bif_signal -hdl_core_name {pfdaq_demo_master} -bif_name {APB_INITIATOR} -bif_signal_name {PSELx} -core_signal_name {PSEL} 
set_root -module {pfdaq_demo_master::work} 
sd_instantiate_hdl_core -sd_name {demo_top} -hdl_core_name {pfdaq_demo_master} -instance_name {} 
sd_delete_ports -sd_name {demo_top} -port_names {APB3mmaster} 
sd_connect_pins -sd_name {demo_top} -pin_names {"CoreAPB3_C0_0:APB3mmaster" "pfdaq_demo_master_0:APB_INITIATOR"} 
sd_connect_pins -sd_name {demo_top} -pin_names {"PCLK" "pfdaq_demo_master_0:PCLK"} 
sd_connect_pins -sd_name {demo_top} -pin_names {"PRESETN" "pfdaq_demo_master_0:PRESETN"} 
generate_component -component_name {demo_top} -recursive 0 
set_root -module {demo_top::work} 
update_and_run_tool -name {SYNTHESIZE} 
