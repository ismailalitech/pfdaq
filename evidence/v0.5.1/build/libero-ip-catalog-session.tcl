# Microsemi Tcl Script
# libero
# Date: Sun Nov 15 23:56:52 2026
# Directory /tmp/claude-1000/ip
# File /tmp/claude-1000/ip/session2.tcl


open_project -file {./demo2/demo2/demo2.prjx} -do_backup_on_convert 1 -backup_file {./demo2/demo2.zip} 
create_smartdesign -sd_name {demo_top} 
sd_instantiate_hdl_core -sd_name {demo_top} -hdl_core_name {pfdaq_demo_master} -instance_name {} 
create_and_configure_core -core_vlnv {pfdaq:Demo:pfdaq_demo_regs:1.0.0} -component_name {pfdaq_demo_regs_C0} -params {"DEFAULT_LEN:16"} 
sd_instantiate_component -sd_name {demo_top} -component_name {pfdaq_demo_regs_C0} -instance_name {} 
sd_connect_pin_to_port -sd_name {demo_top} -pin_name {pfdaq_demo_regs_C0_0:AXIS_INITIATOR} -port_name {} 
sd_connect_pin_to_port -sd_name {demo_top} -pin_name {pfdaq_demo_regs_C0_0:PCLK} -port_name {} 
sd_connect_pin_to_port -sd_name {demo_top} -pin_name {pfdaq_demo_regs_C0_0:PRESETN} -port_name {} 
sd_connect_pins -sd_name {demo_top} -pin_names {"PCLK" "pfdaq_demo_master_0:PCLK"} 
sd_connect_pins -sd_name {demo_top} -pin_names {"PRESETN" "pfdaq_demo_master_0:PRESETN"} 
import_core -core {./cpz/CoreAPB3-4.2.100.cpz} 
create_and_configure_core -core_vlnv {Actel:DirectCore:CoreAPB3:4.2.100} -component_name {CoreAPB3_C0} -params {"APBSLOT0ENABLE:true" "APBSLOT10ENABLE:false" "APBSLOT11ENABLE:false" "APBSLOT12ENABLE:false" "APBSLOT13ENABLE:false" "APBSLOT14ENABLE:false" "APBSLOT15ENABLE:false" "APBSLOT1ENABLE:false" "APBSLOT2ENABLE:false" "APBSLOT3ENABLE:false" "APBSLOT4ENABLE:false" "APBSLOT5ENABLE:false" "APBSLOT6ENABLE:false" "APBSLOT7ENABLE:false" "APBSLOT8ENABLE:false" "APBSLOT9ENABLE:false" "APB_DWIDTH:32" "IADDR_OPTION:0" "MADDR_BITS:28" "SC_0:false" "SC_10:false" "SC_11:false" "SC_12:false" "SC_13:false" "SC_14:false" "SC_15:false" "SC_1:false" "SC_2:false" "SC_3:false" "SC_4:false" "SC_5:false" "SC_6:false" "SC_7:false" "SC_8:false" "SC_9:false" "UPR_NIBBLE_POSN:6"} 
sd_instantiate_component -sd_name {demo_top} -component_name {CoreAPB3_C0} -instance_name {} 
sd_connect_pins -sd_name {demo_top} -pin_names {"CoreAPB3_C0_0:APB3mmaster" "pfdaq_demo_master_0:APB_INITIATOR"} 
sd_connect_pins -sd_name {demo_top} -pin_names {"CoreAPB3_C0_0:APBmslave0" "pfdaq_demo_regs_C0_0:APB_TARGET"} 
generate_component -component_name {demo_top} -recursive 0 
build_design_hierarchy 
set_root -module {demo_top::work} 
run_tool -name {SYNTHESIZE} 
