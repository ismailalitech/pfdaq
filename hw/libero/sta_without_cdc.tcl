# Re-run timing verification on the placed design WITHOUT pfdaq_cdc.sdc. The violating paths
# listed afterwards must be exactly the intended clock-domain crossings and nothing else.
# Run from the Libero work directory after a successful build.
set proj [file join [pwd] PFDAQ]
open_project -file [file join $proj PFDAQ.prjx]
organize_tool_files -tool {VERIFYTIMING} \
    -file [file join $proj constraint MPFS_DISCOVERY_KIT_derived_constraints.sdc] \
    -module {MPFS_DISCOVERY_KIT::work} -input_type {constraint}
run_tool -name {VERIFYTIMING}
close_project -save 0
