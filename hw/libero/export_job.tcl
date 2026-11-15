# Export a FlashPro Express job with the FABRIC and SNVM components only (never eNVM: the HSS
# in the board's boot flash is left alone). Run by build.sh from the Libero work directory.
#   PFDAQ_JOB_NAME  job file name without extension
#   PFDAQ_JOB_DIR   directory the .job is written to
source script_support/additional_configurations/functions.tcl
open_project -file [file join [pwd] PFDAQ PFDAQ.prjx]
run_tool -name {GENERATEPROGRAMMINGDATA}
export_fpe_job $::env(PFDAQ_JOB_NAME) $::env(PFDAQ_JOB_DIR) "FABRIC SNVM" 0
close_project
