# create_ip_demo.tcl - the by-hand flow of module F07 as one Libero script.
#   mkdir -p /tmp/ipdemo && cd /tmp/ipdemo
#   libero SCRIPT:<repo>/experiments/libero-ip/create_ip_demo.tcl
# Starts from an empty MPFS095T project, imports the two Verilog files, turns each into a core with
# its bus interface, wires them in a SmartDesign, generates it and runs synthesis. Every command
# is one that the Libero GUI exported (Project > Export Script File) for the same steps, with
# shorter bus interface names and one added build_design_hierarchy before the root is set.
set here [file dirname [file normalize [info script]]]
set proj_dir [file normalize ./libero-ip-demo]

new_project -location $proj_dir -name {demo} -project_description {} -block_mode 0 \
    -standalone_peripheral_initialization 0 -instantiate_in_smartdesign 1 -ondemand_build_dh 1 \
    -use_relative_path 0 -linked_files_root_dir_env {} -hdl {VERILOG} -family {PolarFireSoC} \
    -die {MPFS095T} -package {FCSG325} -speed {-1} -die_voltage {1.0} -part_range {EXT} \
    -adv_options {IO_DEFT_STD:LVCMOS 1.8V} -adv_options {TEMPR:EXT} -adv_options {VOLTR:EXT}

# 1. import and build the hierarchy
import_files -convert_EDN_to_HDL 0 -library {work} \
    -hdl_source "$here/pfdaq_demo_regs.v" -hdl_source "$here/pfdaq_demo_master.v"
build_design_hierarchy

# 2. the register block: a core with an APB target and an AXI4-Stream initiator
create_hdl_core -file "$proj_dir/hdl/pfdaq_demo_regs.v" -module {pfdaq_demo_regs} -library {work} -package {}
hdl_core_add_bif -hdl_core_name {pfdaq_demo_regs} -bif_definition {APB:AMBA:AMBA2:slave} -bif_name {APB_TARGET} -signal_map {}
foreach s {PADDR PENABLE PWRITE PRDATA PWDATA PREADY PSLVERR} {
    hdl_core_assign_bif_signal -hdl_core_name {pfdaq_demo_regs} -bif_name {APB_TARGET} -bif_signal_name $s -core_signal_name $s
}
# the standard name is PSELx, the module's port is PSEL: the one signal that is not matched by name
hdl_core_assign_bif_signal -hdl_core_name {pfdaq_demo_regs} -bif_name {APB_TARGET} -bif_signal_name {PSELx} -core_signal_name {PSEL}
hdl_core_add_bif -hdl_core_name {pfdaq_demo_regs} -bif_definition {AXI4Stream:AMBA:AMBA4:master} -bif_name {AXIS_INITIATOR} -signal_map {}
foreach s {TVALID TREADY TDATA TLAST} {
    hdl_core_assign_bif_signal -hdl_core_name {pfdaq_demo_regs} -bif_name {AXIS_INITIATOR} -bif_signal_name $s -core_signal_name $s
}

# 3. the stand-in processor: a core with an APB initiator
create_hdl_core -file "$proj_dir/hdl/pfdaq_demo_master.v" -module {pfdaq_demo_master} -library {work} -package {}
hdl_core_add_bif -hdl_core_name {pfdaq_demo_master} -bif_definition {APB:AMBA:AMBA2:master} -bif_name {APB_INITIATOR} -signal_map {}
foreach s {PADDR PENABLE PWRITE PRDATA PWDATA PREADY PSLVERR} {
    hdl_core_assign_bif_signal -hdl_core_name {pfdaq_demo_master} -bif_name {APB_INITIATOR} -bif_signal_name $s -core_signal_name $s
}
hdl_core_assign_bif_signal -hdl_core_name {pfdaq_demo_master} -bif_name {APB_INITIATOR} -bif_signal_name {PSELx} -core_signal_name {PSEL}

# 4. the SmartDesign: master -> CoreAPB3 -> registers
create_smartdesign -sd_name {demo_top}
sd_instantiate_hdl_core -sd_name {demo_top} -hdl_core_name {pfdaq_demo_regs} -instance_name {}
sd_instantiate_hdl_core -sd_name {demo_top} -hdl_core_name {pfdaq_demo_master} -instance_name {}
create_and_configure_core -core_vlnv {Actel:DirectCore:CoreAPB3:4.2.100} -component_name {CoreAPB3_C0} -params {"APBSLOT0ENABLE:true" "APBSLOT10ENABLE:false" "APBSLOT11ENABLE:false" "APBSLOT12ENABLE:false" "APBSLOT13ENABLE:false" "APBSLOT14ENABLE:false" "APBSLOT15ENABLE:false" "APBSLOT1ENABLE:false" "APBSLOT2ENABLE:false" "APBSLOT3ENABLE:false" "APBSLOT4ENABLE:false" "APBSLOT5ENABLE:false" "APBSLOT6ENABLE:false" "APBSLOT7ENABLE:false" "APBSLOT8ENABLE:false" "APBSLOT9ENABLE:false" "APB_DWIDTH:32" "IADDR_OPTION:0" "MADDR_BITS:28" "SC_0:false" "SC_10:false" "SC_11:false" "SC_12:false" "SC_13:false" "SC_14:false" "SC_15:false" "SC_1:false" "SC_2:false" "SC_3:false" "SC_4:false" "SC_5:false" "SC_6:false" "SC_7:false" "SC_8:false" "SC_9:false" "UPR_NIBBLE_POSN:6"}
sd_instantiate_component -sd_name {demo_top} -component_name {CoreAPB3_C0} -instance_name {}
sd_connect_pins -sd_name {demo_top} -pin_names {"pfdaq_demo_master_0:APB_INITIATOR" "CoreAPB3_C0_0:APB3mmaster"}
sd_connect_pins -sd_name {demo_top} -pin_names {"CoreAPB3_C0_0:APBmslave0" "pfdaq_demo_regs_0:APB_TARGET"}
sd_connect_pin_to_port -sd_name {demo_top} -pin_name {pfdaq_demo_regs_0:AXIS_INITIATOR} -port_name {}
sd_connect_pin_to_port -sd_name {demo_top} -pin_name {pfdaq_demo_regs_0:PCLK} -port_name {}
sd_connect_pin_to_port -sd_name {demo_top} -pin_name {pfdaq_demo_regs_0:PRESETN} -port_name {}
sd_connect_pins -sd_name {demo_top} -pin_names {"PCLK" "pfdaq_demo_master_0:PCLK"}
sd_connect_pins -sd_name {demo_top} -pin_names {"PRESETN" "pfdaq_demo_master_0:PRESETN"}

# 5. generate, make it the root and synthesize
generate_component -component_name {demo_top} -recursive 0
build_design_hierarchy
set_root -module {demo_top::work}
update_and_run_tool -name {SYNTHESIZE}

save_project
close_project
