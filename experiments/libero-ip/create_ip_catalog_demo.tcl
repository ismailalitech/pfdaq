# create_ip_catalog_demo.tcl - the Catalog route of module F07 as one Libero script.
#   python3 make_cpz.py cpz
#   python3 make_cpz.py cpz --repack <vault that holds CoreAPB3> DirectCore CoreAPB3 4.2.100
#   mkdir -p /tmp/ipcat && cd /tmp/ipcat
#   IPCAT_CPZ=<repository>/experiments/libero-ip/cpz IPCAT_VAULT=/tmp/ipcat/vault \
#       libero SCRIPT:<repository>/experiments/libero-ip/create_ip_catalog_demo.tcl
# Puts the register core into a scratch vault (and CoreAPB3, because a scratch vault starts empty), then builds the
# design with the core taken from the vault, not from the Verilog. change_vault_location changes the vault of
# every Microchip tool of this user: when you are done, run it again with your old location.
set here [file dirname [file normalize [info script]]]
set cpz   [expr {[info exists ::env(IPCAT_CPZ)]   ? $::env(IPCAT_CPZ)   : "$here/cpz"}]
set vault [expr {[info exists ::env(IPCAT_VAULT)] ? $::env(IPCAT_VAULT) : [file normalize ./libero-ip-vault]}]
set proj_dir [file normalize ./libero-ip-catalog-demo]

file mkdir $vault
change_vault_location -location $vault
import_core -core "$cpz/pfdaq_demo_regs-1.0.0.cpz"
import_core -core "$cpz/CoreAPB3-4.2.100.cpz"

new_project -location $proj_dir -name {demo2} -project_description {} -block_mode 0 \
    -standalone_peripheral_initialization 0 -instantiate_in_smartdesign 1 -ondemand_build_dh 1 \
    -use_relative_path 0 -linked_files_root_dir_env {} -hdl {VERILOG} -family {PolarFireSoC} \
    -die {MPFS095T} -package {FCSG325} -speed {-1} -die_voltage {1.0} -part_range {EXT} \
    -adv_options {IO_DEFT_STD:LVCMOS 1.8V} -adv_options {TEMPR:EXT} -adv_options {VOLTR:EXT}

# the stand-in master is still an HDL core of this project
import_files -convert_EDN_to_HDL 0 -library {work} -hdl_source "$here/pfdaq_demo_master.v"
build_design_hierarchy
create_hdl_core -file "$proj_dir/hdl/pfdaq_demo_master.v" -module {pfdaq_demo_master} -library {work} -package {}
hdl_core_add_bif -hdl_core_name {pfdaq_demo_master} -bif_definition {APB:AMBA:AMBA2:master} -bif_name {APB_INITIATOR} -signal_map {}
foreach s {PADDR PENABLE PWRITE PRDATA PWDATA PREADY PSLVERR} {
    hdl_core_assign_bif_signal -hdl_core_name {pfdaq_demo_master} -bif_name {APB_INITIATOR} -bif_signal_name $s -core_signal_name $s
}
hdl_core_assign_bif_signal -hdl_core_name {pfdaq_demo_master} -bif_name {APB_INITIATOR} -bif_signal_name {PSELx} -core_signal_name {PSEL}

create_smartdesign -sd_name {demo_top}
sd_instantiate_hdl_core -sd_name {demo_top} -hdl_core_name {pfdaq_demo_master} -instance_name {}
# the register core comes from the Catalog: vendor:library:name:version, with its parameter
create_and_configure_core -core_vlnv {pfdaq:Demo:pfdaq_demo_regs:1.0.0} -component_name {pfdaq_demo_regs_C0} -params {"DEFAULT_LEN:16"}
sd_instantiate_component -sd_name {demo_top} -component_name {pfdaq_demo_regs_C0} -instance_name {}
create_and_configure_core -core_vlnv {Actel:DirectCore:CoreAPB3:4.2.100} -component_name {CoreAPB3_C0} -params {"APBSLOT0ENABLE:true" "APBSLOT10ENABLE:false" "APBSLOT11ENABLE:false" "APBSLOT12ENABLE:false" "APBSLOT13ENABLE:false" "APBSLOT14ENABLE:false" "APBSLOT15ENABLE:false" "APBSLOT1ENABLE:false" "APBSLOT2ENABLE:false" "APBSLOT3ENABLE:false" "APBSLOT4ENABLE:false" "APBSLOT5ENABLE:false" "APBSLOT6ENABLE:false" "APBSLOT7ENABLE:false" "APBSLOT8ENABLE:false" "APBSLOT9ENABLE:false" "APB_DWIDTH:32" "IADDR_OPTION:0" "MADDR_BITS:28" "SC_0:false" "SC_10:false" "SC_11:false" "SC_12:false" "SC_13:false" "SC_14:false" "SC_15:false" "SC_1:false" "SC_2:false" "SC_3:false" "SC_4:false" "SC_5:false" "SC_6:false" "SC_7:false" "SC_8:false" "SC_9:false" "UPR_NIBBLE_POSN:6"}
sd_instantiate_component -sd_name {demo_top} -component_name {CoreAPB3_C0} -instance_name {}
sd_connect_pin_to_port -sd_name {demo_top} -pin_name {pfdaq_demo_regs_C0_0:AXIS_INITIATOR} -port_name {}
sd_connect_pin_to_port -sd_name {demo_top} -pin_name {pfdaq_demo_regs_C0_0:PCLK} -port_name {}
sd_connect_pin_to_port -sd_name {demo_top} -pin_name {pfdaq_demo_regs_C0_0:PRESETN} -port_name {}
sd_connect_pins -sd_name {demo_top} -pin_names {"PCLK" "pfdaq_demo_master_0:PCLK"}
sd_connect_pins -sd_name {demo_top} -pin_names {"PRESETN" "pfdaq_demo_master_0:PRESETN"}
sd_connect_pins -sd_name {demo_top} -pin_names {"CoreAPB3_C0_0:APB3mmaster" "pfdaq_demo_master_0:APB_INITIATOR"}
sd_connect_pins -sd_name {demo_top} -pin_names {"CoreAPB3_C0_0:APBmslave0" "pfdaq_demo_regs_C0_0:APB_TARGET"}
generate_component -component_name {demo_top} -recursive 0
build_design_hierarchy
set_root -module {demo_top::work}
update_and_run_tool -name {SYNTHESIZE}
save_project
close_project
